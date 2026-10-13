#include "win.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include <wayland-client.h>

#include "util.h"
#include "xdg-shell-client-protocol.h"

struct wl_display *win_display;
struct wl_compositor *win_compositor;
struct wl_subcompositor *win_subcompositor;
struct wl_shm *win_shm;
int win_refresh_mhz;
int win_refreshes_mhz[WIN_REFRESHES_MAX], win_refresh_count;
static struct xdg_wm_base *wm_base;
static struct wl_output *output;
static struct wl_seat *seat;
static void (*on_key)(unsigned code, bool down);
static int wake_pipe[2] = {-1, -1};

/* --- wakes --------------------------------------------------------------- */

/* Eight bytes: one write, never torn (PIPE_BUF) */
void win_wake(int why, int value) {
  const int msg[2] = {why, value};
  const int saved = errno;
  const ssize_t n = write(wake_pipe[1], msg, sizeof(msg));
  (void)n;
  errno = saved;
}

bool win_woken(int *why, int *value) {
  int msg[2];
  if (read(wake_pipe[0], msg, sizeof(msg)) != sizeof(msg))
    return false;
  *why = msg[0];
  *value = msg[1];
  return true;
}

static void quit_signal(int sig) {
  (void)sig;
  win_wake(WIN_QUIT, 0);
}

bool win_wait(struct pollfd *fds, int n, int timeout_ms) {
  while (wl_display_prepare_read(win_display) != 0)
    if (wl_display_dispatch_pending(win_display) < 0)
      return false;
  wl_display_flush(win_display);
  struct pollfd all[n + 2];
  all[0] = (struct pollfd){.fd = wl_display_get_fd(win_display), .events = POLLIN};
  all[1] = (struct pollfd){.fd = wake_pipe[0], .events = POLLIN};
  memcpy(all + 2, fds, n * sizeof(*fds));
  const int ready = poll(all, n + 2, timeout_ms);
  for (int i = 0; i < n; i++)
    fds[i].revents = ready > 0 ? all[i + 2].revents : 0;
  if (ready > 0 && (all[0].revents & POLLIN))
    wl_display_read_events(win_display);
  else
    wl_display_cancel_read(win_display);
  return wl_display_dispatch_pending(win_display) >= 0;
}

/* --- toplevels ----------------------------------------------------------- */

static void xsurface_configure(void *data, struct xdg_surface *s, uint32_t serial) {
  win *w = data;
  xdg_surface_ack_configure(s, serial);
  w->configured = true;
  if (w->configure != NULL)
    w->configure(w);
}
static const struct xdg_surface_listener xsurface_listener = {xsurface_configure};

static void toplevel_configure(void *data, struct xdg_toplevel *t, int32_t width, int32_t height,
                               struct wl_array *states) {
  (void)t;
  win *w = data;
  w->activated = false;
  const uint32_t *state;
  wl_array_for_each(state, states) w->activated |= *state == XDG_TOPLEVEL_STATE_ACTIVATED;
  if (width > 0 && height > 0) {
    w->w = width;
    w->h = height;
  }
}
static void toplevel_close(void *data, struct xdg_toplevel *t) {
  (void)data, (void)t;
  win_wake(WIN_QUIT, 0);
}
static const struct xdg_toplevel_listener toplevel_listener = {.configure = toplevel_configure,
                                                               .close = toplevel_close};

void win_map(win *w) {
  if (w->toplevel != NULL)
    win_unmap(w);
  if (w->surface == NULL)
    w->surface = wl_compositor_create_surface(win_compositor);
  w->xsurface = xdg_wm_base_get_xdg_surface(wm_base, w->surface);
  xdg_surface_add_listener(w->xsurface, &xsurface_listener, w);
  w->toplevel = xdg_surface_get_toplevel(w->xsurface);
  xdg_toplevel_add_listener(w->toplevel, &toplevel_listener, w);
  xdg_toplevel_set_app_id(w->toplevel, w->app_id);
  xdg_toplevel_set_title(w->toplevel, w->title);
  if (w->fullscreen)
    xdg_toplevel_set_fullscreen(w->toplevel, NULL);
  wl_surface_commit(w->surface);
  while (!w->configured)
    if (wl_display_dispatch(win_display) < 0)
      return;
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t time) {
  (void)time;
  ((win *)data)->frame_pending = false;
  wl_callback_destroy(cb);
}
static const struct wl_callback_listener frame_listener = {frame_done};

void win_frame(win *w) {
  if (w->frame_pending)
    return;
  w->frame_pending = true;
  wl_callback_add_listener(wl_surface_frame(w->surface), &frame_listener, w);
}

void win_unmap(win *w) {
  if (w->toplevel == NULL)
    return;
  xdg_toplevel_destroy(w->toplevel);
  xdg_surface_destroy(w->xsurface);
  w->toplevel = NULL;
  w->xsurface = NULL;
  w->configured = w->activated = false;
  wl_surface_attach(w->surface, NULL, 0, 0);
  wl_surface_commit(w->surface);
}

/* --- the connection ------------------------------------------------------ */

static void wm_ping(void *data, struct xdg_wm_base *b, uint32_t serial) {
  (void)data;
  xdg_wm_base_pong(b, serial);
}
static const struct xdg_wm_base_listener wm_listener = {wm_ping};

static void kb_keymap(void *d, struct wl_keyboard *k, uint32_t format, int fd, uint32_t size) {
  (void)d, (void)k, (void)format, (void)size;
  close(fd);
}
static void kb_enter(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *f, struct wl_array *keys) {
  (void)d, (void)k, (void)s, (void)f, (void)keys;
}
static void kb_leave(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *f) {
  (void)d, (void)k, (void)s, (void)f;
}
static void kb_key(void *d, struct wl_keyboard *k, uint32_t s, uint32_t t, uint32_t key, uint32_t state) {
  (void)d, (void)k, (void)s, (void)t;
  if (on_key != NULL)
    on_key(key, state == WL_KEYBOARD_KEY_STATE_PRESSED);
}
static void kb_modifiers(void *d, struct wl_keyboard *k, uint32_t s, uint32_t a, uint32_t b, uint32_t c, uint32_t e) {
  (void)d, (void)k, (void)s, (void)a, (void)b, (void)c, (void)e;
}
static void kb_repeat(void *d, struct wl_keyboard *k, int32_t r, int32_t delay) {
  (void)d, (void)k, (void)r, (void)delay;
}
static const struct wl_keyboard_listener kb_listener = {kb_keymap, kb_enter, kb_leave, kb_key,
                                                        kb_modifiers, kb_repeat};

static void seat_caps(void *data, struct wl_seat *s, uint32_t caps) {
  (void)data;
  static struct wl_keyboard *kb;
  if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && kb == NULL) {
    kb = wl_seat_get_keyboard(s);
    wl_keyboard_add_listener(kb, &kb_listener, NULL);
  }
}
static void seat_name(void *data, struct wl_seat *s, const char *name) { (void)data, (void)s, (void)name; }
static const struct wl_seat_listener seat_listener = {seat_caps, seat_name};

/* Every mode at bind, then the current one again on each switch */
static struct {
  int w, h, mhz;
} modes[16];
static int mode_count, current_w, current_h;

static void output_mode(void *data, struct wl_output *o, uint32_t flags, int32_t w, int32_t h, int32_t refresh) {
  (void)data, (void)o;
  if (flags & WL_OUTPUT_MODE_CURRENT) {
    win_refresh_mhz = refresh;
    current_w = w;
    current_h = h;
  }
  for (int i = 0; i < mode_count; i++)
    if (modes[i].w == w && modes[i].h == h && modes[i].mhz == refresh)
      return;
  if (mode_count < (int)COUNTOF(modes))
    modes[mode_count++] = (typeof(modes[0])){w, h, refresh};
}
static void output_geometry(void *data, struct wl_output *o, int32_t x, int32_t y, int32_t pw, int32_t ph,
                            int32_t subpixel, const char *make, const char *model, int32_t transform) {
  (void)data, (void)o, (void)x, (void)y, (void)pw, (void)ph, (void)subpixel, (void)make, (void)model, (void)transform;
}
static void output_done(void *data, struct wl_output *o) {
  (void)data, (void)o;
  win_refresh_count = 0;
  for (int i = 0; i < mode_count; i++) {
    if (modes[i].w != current_w || modes[i].h != current_h || win_refresh_count == WIN_REFRESHES_MAX)
      continue;
    int at = win_refresh_count++;
    for (; at > 0 && win_refreshes_mhz[at - 1] > modes[i].mhz; at--)
      win_refreshes_mhz[at] = win_refreshes_mhz[at - 1];
    win_refreshes_mhz[at] = modes[i].mhz;
  }
}
static void output_scale(void *data, struct wl_output *o, int32_t factor) { (void)data, (void)o, (void)factor; }
static const struct wl_output_listener output_listener = {
    .geometry = output_geometry, .mode = output_mode, .done = output_done, .scale = output_scale};

static void global(void *data, struct wl_registry *r, uint32_t name, const char *iface, uint32_t version) {
  (void)data;
  if (strcmp(iface, wl_compositor_interface.name) == 0)
    win_compositor = wl_registry_bind(r, name, &wl_compositor_interface, 4);
  else if (strcmp(iface, wl_subcompositor_interface.name) == 0)
    win_subcompositor = wl_registry_bind(r, name, &wl_subcompositor_interface, 1);
  else if (strcmp(iface, wl_shm_interface.name) == 0)
    win_shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
  else if (strcmp(iface, xdg_wm_base_interface.name) == 0) {
    wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
    xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);
  } else if (strcmp(iface, wl_output_interface.name) == 0 && output == NULL) {
    /* The first: a handheld has the one */
    output = wl_registry_bind(r, name, &wl_output_interface, MIN(version, 2u));
    wl_output_add_listener(output, &output_listener, NULL);
  } else if (strcmp(iface, wl_seat_interface.name) == 0 && seat == NULL) {
    seat = wl_registry_bind(r, name, &wl_seat_interface, MIN(version, 5u));
    wl_seat_add_listener(seat, &seat_listener, NULL);
  }
}
static void global_remove(void *data, struct wl_registry *r, uint32_t name) { (void)data, (void)r, (void)name; }
static const struct wl_registry_listener registry_listener = {global, global_remove};

bool win_connect(void (*key)(unsigned code, bool down)) {
  on_key = key;
  if (pipe2(wake_pipe, O_CLOEXEC | O_NONBLOCK) != 0)
    return false;
  /* Not SA_RESTART: a wait in progress wakes either way, through the pipe */
  struct sigaction sa = {.sa_handler = quit_signal};
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGINT, &sa, NULL);
  if ((win_display = wl_display_connect(NULL)) == NULL)
    return false;
  wl_registry_add_listener(wl_display_get_registry(win_display), &registry_listener, NULL);
  /* The globals, then what they tell when bound (the output's mode) */
  wl_display_roundtrip(win_display);
  wl_display_roundtrip(win_display);
  return win_compositor != NULL && win_subcompositor != NULL && win_shm != NULL && wm_base != NULL;
}
