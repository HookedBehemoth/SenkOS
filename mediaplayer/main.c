/*
 * mediaplayer: plays a video, with the pad. The launcher drives it through
 * appctl.h (pause, settings) like corehost; without the launcher it just plays.
 *
 *   mediaplayer FILE
 *
 * GStreamer's playbin3 decodes: on Cedrus through v4l2codecs, whose dmabufs
 * waylandsink hands to Weston as they are. If decoding fails (a codec profile
 * the engine lacks), the file is opened again with software decoders.
 *
 * Our toplevel is black; waylandsink draws into a subsurface of it, and the
 * OSD (title, time, progress) is a CPU-drawn subsurface above that.
 *
 * A, Start: pause    Left/Right: -/+10 s    L1/R1: -/+60 s
 * Up/Down: previous/next video in the folder    X, Y, Select: OSD    B: quit
 *
 * Where each video was left is kept in $HOME/.local/state/mediaplayer/resume
 * and playback starts there. At the end the next video in the folder plays.
 */
#include <errno.h>
#include <dirent.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <SDL.h>
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <gst/gst.h>
#include <gst/video/videooverlay.h>
#define GST_USE_UNSTABLE_API
#include <gst/wayland/wayland.h>
#include <wayland-client.h>

#include "../launcher/appctl.h"
#include "xdg-shell-client-protocol.h"

#define OSD_H 72
#define OSD_SHOWN_MS 3000
#define SEEK_SHORT (10 * GST_SECOND)
#define SEEK_LONG (60 * GST_SECOND)
#define REPEAT_DELAY_MS 400
#define REPEAT_MS 150
static const char *const video_exts[] = {".mp4", ".mkv", ".webm", ".mov", ".m4v", ".avi"};

static struct wl_display *dpy;
static struct wl_compositor *compositor;
static struct wl_subcompositor *subcompositor;
static struct wl_shm *shm_global;
static struct xdg_wm_base *wm_base;
static struct wl_seat *seat;
static struct wl_surface *top, *video_surface, *osd_surface;
static struct wl_subsurface *video_sub, *osd_sub;
static struct xdg_surface *xsurface;
static struct xdg_toplevel *toplevel;
static int win_w = 640, win_h = 480;
static bool configured;

static GstElement *pipe_, *vsink;
static char path[1024];
static bool software, finished, quit;
static GstClockTime duration = GST_CLOCK_TIME_NONE;
/* Where a seek goes, until it is done: held buttons add up from here */
static GstClockTime seek_target = GST_CLOCK_TIME_NONE;
static GstClockTime start_at;
static bool user_paused, ctl_paused, prerolled;

static Uint64 osd_until;
static char message[128];
static Uint64 message_until;

static int ctl = -1;
static appctl_shm *shm;
enum { OPT_SCALING, OPT_AUDIO };
static char opt_path[512], state_path[512];

/* --- shm buffers ------------------------------------------------------- */

typedef struct {
  struct wl_buffer *buffer;
  uint32_t *pixels;
  int w, h;
  bool busy;
} shm_buffer;

static void buffer_release(void *data, struct wl_buffer *b) {
  (void)b;
  ((shm_buffer *)data)->busy = false;
}
static const struct wl_buffer_listener buffer_listener = {buffer_release};

static bool shm_buffer_make(shm_buffer *sb, int w, int h, uint32_t format) {
  if (sb->buffer != NULL) {
    wl_buffer_destroy(sb->buffer);
    munmap(sb->pixels, (size_t)sb->w * sb->h * 4);
    sb->buffer = NULL;
  }
  const size_t size = (size_t)w * h * 4;
  const int fd = memfd_create("mediaplayer", MFD_CLOEXEC);
  if (fd < 0 || ftruncate(fd, (off_t)size) != 0) {
    if (fd >= 0)
      close(fd);
    return false;
  }
  sb->pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  struct wl_shm_pool *pool = wl_shm_create_pool(shm_global, fd, (int32_t)size);
  close(fd);
  sb->buffer = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4, format);
  wl_shm_pool_destroy(pool);
  wl_buffer_add_listener(sb->buffer, &buffer_listener, sb);
  sb->w = w;
  sb->h = h;
  sb->busy = false;
  return sb->pixels != MAP_FAILED;
}

/* --- text ---------------------------------------------------------------- */

static FT_Library ft;
static FT_Face face;

static bool font_init(void) {
  if (!FcInit() || FT_Init_FreeType(&ft) != 0)
    return false;
  FcPattern *wanted = FcNameParse((const FcChar8 *)"sans-serif");
  FcConfigSubstitute(NULL, wanted, FcMatchPattern);
  FcDefaultSubstitute(wanted);
  FcResult result;
  FcPattern *match = FcFontMatch(NULL, wanted, &result);
  FcPatternDestroy(wanted);
  FcChar8 *file;
  int index = 0;
  if (match == NULL || FcPatternGetString(match, FC_FILE, 0, &file) != FcResultMatch)
    return false;
  FcPatternGetInteger(match, FC_INDEX, 0, &index);
  const bool ok = FT_New_Face(ft, (const char *)file, index, &face) == 0;
  FcPatternDestroy(match);
  return ok;
}

static uint32_t utf8_next(const char **s) {
  const unsigned char *p = (const unsigned char *)*s;
  uint32_t c = *p++;
  int extra = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
  c &= extra == 3 ? 0x07 : extra == 2 ? 0x0f : extra == 1 ? 0x1f : 0x7f;
  while (extra-- > 0 && (*p & 0xc0) == 0x80)
    c = c << 6 | (*p++ & 0x3f);
  *s = (const char *)p;
  return c;
}

/* ponytail: glyph by glyph, no shaping; titles in complex scripts would want
 * HarfBuzz like the launcher's text.c */
static int text_width(const char *s, int px) {
  FT_Set_Pixel_Sizes(face, 0, px);
  int w = 0;
  while (*s)
    if (FT_Load_Char(face, utf8_next(&s), FT_LOAD_DEFAULT) == 0)
      w += face->glyph->advance.x >> 6;
  return w;
}

/* Premultiplied ARGB; returns the pen's x after the text */
static int text_draw(shm_buffer *b, int x, int baseline, const char *s, int px, uint32_t rgb,
                     int max_x) {
  FT_Set_Pixel_Sizes(face, 0, px);
  while (*s) {
    if (FT_Load_Char(face, utf8_next(&s), FT_LOAD_RENDER) != 0)
      continue;
    const FT_GlyphSlot g = face->glyph;
    if (x + (int)(g->advance.x >> 6) > max_x)
      break;
    for (unsigned row = 0; row < g->bitmap.rows; row++) {
      const int y = baseline - g->bitmap_top + (int)row;
      if (y < 0 || y >= b->h)
        continue;
      for (unsigned col = 0; col < g->bitmap.width; col++) {
        const int px_x = x + g->bitmap_left + (int)col;
        const uint32_t a = g->bitmap.buffer[row * g->bitmap.pitch + col];
        if (px_x < 0 || px_x >= b->w || a == 0)
          continue;
        uint32_t *d = &b->pixels[y * b->w + px_x], out = 0;
        for (int shift = 0; shift < 32; shift += 8) {
          const uint32_t src = shift == 24 ? 255 : (rgb >> shift) & 0xff;
          const uint32_t dst = (*d >> shift) & 0xff;
          out |= ((src * a + dst * (255 - a)) / 255) << shift;
        }
        *d = out;
      }
    }
    x += g->advance.x >> 6;
  }
  return x;
}

/* Cut to fit with an ellipsis */
static void text_fit(shm_buffer *b, int x, int baseline, const char *s, int px, uint32_t rgb,
                     int max_x) {
  if (x + text_width(s, px) <= max_x) {
    text_draw(b, x, baseline, s, px, rgb, max_x);
    return;
  }
  const int dots = text_width("…", px);
  x = text_draw(b, x, baseline, s, px, rgb, max_x - dots);
  text_draw(b, x, baseline, "…", px, rgb, max_x);
}

static void fill(shm_buffer *b, int x, int y, int w, int h, uint32_t argb) {
  for (int j = SDL_max(y, 0); j < SDL_min(y + h, b->h); j++)
    for (int i = SDL_max(x, 0); i < SDL_min(x + w, b->w); i++)
      b->pixels[j * b->w + i] = argb;
}

/* --- OSD ----------------------------------------------------------------- */

static shm_buffer osd_buffers[2];
static bool osd_mapped;

static void format_time(char *out, size_t size, GstClockTime t) {
  if (!GST_CLOCK_TIME_IS_VALID(t)) {
    snprintf(out, size, "--:--");
    return;
  }
  const unsigned s = (unsigned)(t / GST_SECOND);
  if (s >= 3600)
    snprintf(out, size, "%u:%02u:%02u", s / 3600, s / 60 % 60, s % 60);
  else
    snprintf(out, size, "%u:%02u", s / 60, s % 60);
}

static GstClockTime position(void) {
  gint64 pos;
  if (GST_CLOCK_TIME_IS_VALID(seek_target))
    return seek_target;
  if (pipe_ != NULL && gst_element_query_position(pipe_, GST_FORMAT_TIME, &pos))
    return (GstClockTime)pos;
  return GST_CLOCK_TIME_NONE;
}

static void show(const char *text) {
  snprintf(message, sizeof(message), "%s", text);
  message_until = SDL_GetTicks64() + OSD_SHOWN_MS;
  osd_until = message_until;
}

static void osd_draw(void) {
  const Uint64 now = SDL_GetTicks64();
  const bool wanted = now < osd_until || user_paused || finished;
  if (!configured)
    return;
  if (!wanted) {
    if (osd_mapped) {
      wl_surface_attach(osd_surface, NULL, 0, 0);
      wl_surface_commit(osd_surface);
      osd_mapped = false;
    }
    return;
  }
  shm_buffer *b = !osd_buffers[0].busy ? &osd_buffers[0] : !osd_buffers[1].busy ? &osd_buffers[1] : NULL;
  if (b == NULL)
    return;
  if ((b->w != win_w || b->buffer == NULL) && !shm_buffer_make(b, win_w, OSD_H, WL_SHM_FORMAT_ARGB8888))
    return;
  const int m = 16, w = b->w;
  fill(b, 0, 0, w, OSD_H, 0xd0101014);
  int x = m;
  if (user_paused) {
    fill(b, x, 10, 4, 16, 0xffffffff);
    fill(b, x + 7, 10, 4, 16, 0xffffffff);
    x += 20;
  }
  const char *name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
  const bool msg = now < message_until || finished;
  text_fit(b, x, 25, msg ? message : name, 18, msg ? 0xffd070 : 0xffffff, w - m);

  const GstClockTime pos = position();
  fill(b, m, 36, w - 2 * m, 6, 0xff3a3b44);
  if (GST_CLOCK_TIME_IS_VALID(pos) && GST_CLOCK_TIME_IS_VALID(duration) && duration > 0)
    fill(b, m, 36, (int)((w - 2 * m) * (double)SDL_min(pos, duration) / duration), 6, 0xff6a9cff);
  char t[32];
  format_time(t, sizeof(t), pos);
  text_draw(b, m, 63, t, 15, 0xc8c8d0, w);
  format_time(t, sizeof(t), duration);
  text_draw(b, w - m - text_width(t, 15), 63, t, 15, 0xc8c8d0, w);

  wl_surface_attach(osd_surface, b->buffer, 0, 0);
  wl_surface_damage(osd_surface, 0, 0, w, OSD_H);
  wl_surface_commit(osd_surface);
  b->busy = true;
  osd_mapped = true;
}

static void osd_poke(void) { osd_until = SDL_GetTicks64() + OSD_SHOWN_MS; }

/* --- window -------------------------------------------------------------- */

static shm_buffer black, clear;

static void attach_background(void) {
  if (black.w != win_w || black.h != win_h)
    shm_buffer_make(&black, win_w, win_h, WL_SHM_FORMAT_XRGB8888);
  wl_subsurface_set_position(osd_sub, 0, win_h - OSD_H);
  wl_surface_attach(top, black.buffer, 0, 0);
  wl_surface_damage(top, 0, 0, win_w, win_h);
  wl_surface_commit(top);
  if (vsink != NULL)
    gst_video_overlay_set_render_rectangle(GST_VIDEO_OVERLAY(vsink), 0, 0, win_w, win_h);
}

static void xsurface_configure(void *data, struct xdg_surface *s, uint32_t serial) {
  (void)data;
  xdg_surface_ack_configure(s, serial);
  configured = true;
  attach_background();
  osd_draw();
}
static const struct xdg_surface_listener xsurface_listener = {xsurface_configure};

static void toplevel_configure(void *data, struct xdg_toplevel *t, int32_t w, int32_t h,
                               struct wl_array *states) {
  (void)data, (void)t, (void)states;
  if (w > 0 && h > 0) {
    win_w = w;
    win_h = h;
  }
}
static void toplevel_close(void *data, struct xdg_toplevel *t) {
  (void)data, (void)t;
  quit = true;
}
static const struct xdg_toplevel_listener toplevel_listener = {.configure = toplevel_configure,
                                                               .close = toplevel_close};

static void wm_ping(void *data, struct xdg_wm_base *b, uint32_t serial) {
  (void)data;
  xdg_wm_base_pong(b, serial);
}
static const struct xdg_wm_base_listener wm_listener = {wm_ping};

static void map_toplevel(void) {
  xsurface = xdg_wm_base_get_xdg_surface(wm_base, top);
  xdg_surface_add_listener(xsurface, &xsurface_listener, NULL);
  toplevel = xdg_surface_get_toplevel(xsurface);
  xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
  xdg_toplevel_set_app_id(toplevel, "mediaplayer");
  xdg_toplevel_set_title(toplevel, "Media player");
  xdg_toplevel_set_fullscreen(toplevel, NULL);
  configured = false;
  wl_surface_commit(top);
}

/* Mapped again: the kiosk shell puts what maps on top */
static void raise_window(void) {
  xdg_toplevel_destroy(toplevel);
  xdg_surface_destroy(xsurface);
  wl_surface_attach(top, NULL, 0, 0);
  wl_surface_commit(top);
  map_toplevel();
}

/* --- keyboard (testing on a desktop) ------------------------------------ */

static void press(SDL_GameControllerButton b);
static void release(SDL_GameControllerButton b);

static SDL_GameControllerButton key_button(uint32_t key) {
  switch (key) {
  case KEY_SPACE: case KEY_ENTER: return SDL_CONTROLLER_BUTTON_A;
  case KEY_ESC: case KEY_BACKSPACE: return SDL_CONTROLLER_BUTTON_B;
  case KEY_TAB: return SDL_CONTROLLER_BUTTON_X;
  case KEY_LEFT: return SDL_CONTROLLER_BUTTON_DPAD_LEFT;
  case KEY_RIGHT: return SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
  case KEY_UP: return SDL_CONTROLLER_BUTTON_DPAD_UP;
  case KEY_DOWN: return SDL_CONTROLLER_BUTTON_DPAD_DOWN;
  case KEY_PAGEUP: return SDL_CONTROLLER_BUTTON_LEFTSHOULDER;
  case KEY_PAGEDOWN: return SDL_CONTROLLER_BUTTON_RIGHTSHOULDER;
  default: return SDL_CONTROLLER_BUTTON_INVALID;
  }
}

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
  const SDL_GameControllerButton b = key_button(key);
  if (b == SDL_CONTROLLER_BUTTON_INVALID)
    return;
  if (state == WL_KEYBOARD_KEY_STATE_PRESSED)
    press(b);
  else
    release(b);
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

static void global(void *data, struct wl_registry *r, uint32_t name, const char *iface, uint32_t version) {
  (void)data;
  if (strcmp(iface, wl_compositor_interface.name) == 0)
    compositor = wl_registry_bind(r, name, &wl_compositor_interface, 4);
  else if (strcmp(iface, wl_subcompositor_interface.name) == 0)
    subcompositor = wl_registry_bind(r, name, &wl_subcompositor_interface, 1);
  else if (strcmp(iface, wl_shm_interface.name) == 0)
    shm_global = wl_registry_bind(r, name, &wl_shm_interface, 1);
  else if (strcmp(iface, xdg_wm_base_interface.name) == 0) {
    wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
    xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);
  } else if (strcmp(iface, wl_seat_interface.name) == 0 && seat == NULL) {
    seat = wl_registry_bind(r, name, &wl_seat_interface, SDL_min(version, 5u));
    wl_seat_add_listener(seat, &seat_listener, NULL);
  }
}
static void global_remove(void *data, struct wl_registry *r, uint32_t name) { (void)data, (void)r, (void)name; }
static const struct wl_registry_listener registry_listener = {global, global_remove};

static bool window_init(void) {
  if ((dpy = wl_display_connect(NULL)) == NULL)
    return false;
  wl_registry_add_listener(wl_display_get_registry(dpy), &registry_listener, NULL);
  wl_display_roundtrip(dpy);
  if (compositor == NULL || subcompositor == NULL || shm_global == NULL || wm_base == NULL)
    return false;
  top = wl_compositor_create_surface(compositor);
  /* Video first, OSD second: a new subsurface goes on top of its siblings,
   * and waylandsink's own surfaces stay inside the video's */
  video_surface = wl_compositor_create_surface(compositor);
  video_sub = wl_subcompositor_get_subsurface(subcompositor, video_surface, top);
  osd_surface = wl_compositor_create_surface(compositor);
  osd_sub = wl_subcompositor_get_subsurface(subcompositor, osd_surface, top);
  wl_subsurface_set_desync(video_sub);
  wl_subsurface_set_desync(osd_sub);
  /* An unmapped parent hides its children: the video's gets a clear pixel */
  shm_buffer_make(&clear, 1, 1, WL_SHM_FORMAT_ARGB8888);
  wl_surface_attach(video_surface, clear.buffer, 0, 0);
  wl_surface_commit(video_surface);
  /* Pass input to... nothing: the OSD and video never take it */
  struct wl_region *none = wl_compositor_create_region(compositor);
  wl_surface_set_input_region(osd_surface, none);
  wl_surface_set_input_region(video_surface, none);
  wl_region_destroy(none);
  map_toplevel();
  return true;
}

/* --- state --------------------------------------------------------------- */

static void mkdirs(char *p) {
  for (char *slash = strchr(p + 1, '/'); slash; slash = strchr(slash + 1, '/')) {
    *slash = '\0';
    mkdir(p, 0755);
    *slash = '/';
  }
}

/* One "SECONDS PATH" line per video, the latest last.
 * ponytail: rewritten whole each time; a few hundred lines is nothing */
#define RESUME_MAX 500
static long resume_get(const char *file) {
  FILE *f = fopen(state_path, "r");
  char line[1200];
  long found = 0, s;
  int n;
  while (f != NULL && fgets(line, sizeof(line), f)) {
    line[strcspn(line, "\n")] = '\0';
    if (sscanf(line, "%ld %n", &s, &n) == 1 && strcmp(line + n, file) == 0)
      found = s;
  }
  if (f != NULL)
    fclose(f);
  return found;
}

/* seconds < 0: forget it */
static void resume_set(const char *file, long seconds) {
  char tmp[600];
  snprintf(tmp, sizeof(tmp), "%s.new", state_path);
  FILE *in = fopen(state_path, "r"), *out = fopen(tmp, "w");
  if (out == NULL) {
    if (in != NULL)
      fclose(in);
    return;
  }
  char line[1200];
  long s;
  int n, total = 0, skip = 0;
  while (in != NULL && fgets(line, sizeof(line), in))
    total++;
  if (in != NULL)
    rewind(in);
  skip = total >= RESUME_MAX ? total - RESUME_MAX + 1 : 0;
  while (in != NULL && fgets(line, sizeof(line), in)) {
    line[strcspn(line, "\n")] = '\0';
    if (skip > 0 && skip--)
      continue;
    if (sscanf(line, "%ld %n", &s, &n) == 1 && strcmp(line + n, file) != 0)
      fprintf(out, "%s\n", line);
  }
  if (seconds >= 0)
    fprintf(out, "%ld %s\n", seconds, file);
  if (in != NULL)
    fclose(in);
  if (fclose(out) == 0)
    rename(tmp, state_path);
}

static void remember(void) {
  const GstClockTime pos = position();
  if (pipe_ == NULL || !GST_CLOCK_TIME_IS_VALID(pos))
    return;
  /* The credits count as seen */
  const bool done = finished || (GST_CLOCK_TIME_IS_VALID(duration) && pos > duration / 100 * 95);
  resume_set(path, done ? -1 : (long)(pos / GST_SECOND));
}

/* --- options (appctl) ----------------------------------------------------- */

static void set_option(int i, uint8_t category, const char *key, const char *label,
                       const char *const *values, int count) {
  appctl_option *o = &shm->options[i];
  o->category = category;
  snprintf(o->key, sizeof(o->key), "%s", key);
  snprintf(o->label, sizeof(o->label), "%s", label);
  o->kind = APPCTL_CHOICE;
  o->count = (uint8_t)SDL_min(count, APPCTL_VALUES);
  for (int v = 0; v < o->count; v++)
    snprintf(o->values[v], APPCTL_TEXT, "%s", values[v]);
}

/* Scaling is kept; the audio track is the file's */
static void load_options(void) {
  FILE *f = fopen(opt_path, "r");
  char line[128];
  appctl_option *o = &shm->options[OPT_SCALING];
  while (f != NULL && fgets(line, sizeof(line), f)) {
    line[strcspn(line, "\n")] = '\0';
    for (unsigned v = 0; strncmp(line, "scaling=", 8) == 0 && v < o->count; v++)
      if (strcmp(line + 8, o->values[v]) == 0)
        atomic_store(&o->value, v);
  }
  if (f != NULL)
    fclose(f);
}

static void save_options(void) {
  FILE *f = fopen(opt_path, "w");
  if (f == NULL)
    return;
  const appctl_option *o = &shm->options[OPT_SCALING];
  fprintf(f, "scaling=%s\n", o->values[atomic_load(&o->value) % o->count]);
  fclose(f);
}

/* playbin3's streams: the file's tracks, and which audio one plays */
static GstStreamCollection *collection;
static int audio_playing = -1;

/* The n-th audio stream of the collection */
static GstStream *audio_stream(int n) {
  for (guint i = 0; collection != NULL && i < gst_stream_collection_get_size(collection); i++) {
    GstStream *st = gst_stream_collection_get_stream(collection, i);
    if ((gst_stream_get_stream_type(st) & GST_STREAM_TYPE_AUDIO) && n-- == 0)
      return st;
  }
  return NULL;
}

static void audio_options(void) {
  int n = 0;
  while (n < APPCTL_VALUES && audio_stream(n) != NULL)
    n++;
  atomic_store(&shm->option_count, OPT_AUDIO);
  if (n < 2)
    return;
  char names[APPCTL_VALUES][APPCTL_TEXT];
  const char *values[APPCTL_VALUES];
  for (int i = 0; i < n; i++) {
    GstTagList *tags = gst_stream_get_tags(audio_stream(i));
    gchar *lang = NULL, *title = NULL;
    if (tags != NULL) {
      if (!gst_tag_list_get_string(tags, GST_TAG_LANGUAGE_NAME, &lang))
        gst_tag_list_get_string(tags, GST_TAG_LANGUAGE_CODE, &lang);
      gst_tag_list_get_string(tags, GST_TAG_TITLE, &title);
      gst_tag_list_unref(tags);
    }
    if (title != NULL && lang != NULL)
      snprintf(names[i], APPCTL_TEXT, "%d: %s (%s)", i + 1, title, lang);
    else
      snprintf(names[i], APPCTL_TEXT, "%d%s%s", i + 1, title || lang ? ": " : "", title ? title : lang ? lang : "");
    values[i] = names[i];
    g_free(lang);
    g_free(title);
  }
  set_option(OPT_AUDIO, APPCTL_AUDIO, "audio", "Audio track", values, n);
  atomic_store(&shm->options[OPT_AUDIO].value, (uint8_t)SDL_max(audio_playing, 0));
  atomic_store(&shm->option_count, OPT_AUDIO + 1);
}

/* What playbin3 picked, so the menu shows it */
static void streams_selected(GstMessage *m) {
  for (guint i = 0; i < gst_message_streams_selected_get_size(m); i++) {
    GstStream *st = gst_message_streams_selected_get_stream(m, i);
    for (int n = 0; (gst_stream_get_stream_type(st) & GST_STREAM_TYPE_AUDIO) && audio_stream(n); n++)
      if (audio_stream(n) == st)
        audio_playing = n;
    gst_object_unref(st);
  }
  if (atomic_load(&shm->option_count) > OPT_AUDIO)
    atomic_store(&shm->options[OPT_AUDIO].value, (uint8_t)SDL_max(audio_playing, 0));
}

/* The first video stream and the chosen audio one */
static void select_audio(int n) {
  GList *ids = NULL;
  for (guint i = 0; i < gst_stream_collection_get_size(collection); i++) {
    GstStream *st = gst_stream_collection_get_stream(collection, i);
    if (gst_stream_get_stream_type(st) & GST_STREAM_TYPE_VIDEO) {
      ids = g_list_append(ids, (gpointer)gst_stream_get_stream_id(st));
      break;
    }
  }
  ids = g_list_append(ids, (gpointer)gst_stream_get_stream_id(audio_stream(n)));
  gst_element_send_event(pipe_, gst_event_new_select_streams(ids));
  g_list_free(ids);
  audio_playing = n;
}

static void apply_options(void) {
  if (vsink != NULL)
    g_object_set(vsink, "force-aspect-ratio", atomic_load(&shm->options[OPT_SCALING].value) == 0, NULL);
  const int n = atomic_load(&shm->options[OPT_AUDIO].value);
  if (pipe_ != NULL && atomic_load(&shm->option_count) > OPT_AUDIO && n != audio_playing &&
      audio_stream(n) != NULL)
    select_audio(n);
}

static bool open_ctl(void) {
  int sock, mem;
  const char *env = getenv(APPCTL_ENV);
  if (env != NULL && sscanf(env, "%d,%d", &sock, &mem) == 2) {
    void *map = mmap(NULL, sizeof(appctl_shm), PROT_READ | PROT_WRITE, MAP_SHARED, mem, 0);
    close(mem);
    if (map != MAP_FAILED) {
      ctl = sock;
      shm = map;
    }
  }
  unsetenv(APPCTL_ENV);
  if (shm == NULL && (shm = calloc(1, sizeof(*shm))) == NULL)
    return false;
  shm->magic = APPCTL_MAGIC;
  shm->version = APPCTL_VERSION;
  shm->slots = 0;
  static const char *const scaling[] = {"Fit", "Stretch"};
  set_option(OPT_SCALING, APPCTL_GRAPHICS, "scaling", "Scaling", scaling, 2);
  load_options();
  atomic_store(&shm->option_count, OPT_SCALING + 1);
  return true;
}

/* --- playback ------------------------------------------------------------ */

static GstBusSyncReply bus_sync(GstBus *bus, GstMessage *m, gpointer data) {
  (void)bus, (void)data;
  if (gst_is_wl_display_handle_need_context_message(m)) {
    GstContext *c = gst_wl_display_handle_context_new(dpy);
    gst_element_set_context(GST_ELEMENT(GST_MESSAGE_SRC(m)), c);
    gst_context_unref(c);
    gst_message_unref(m);
    return GST_BUS_DROP;
  }
  if (gst_is_video_overlay_prepare_window_handle_message(m)) {
    GstVideoOverlay *o = GST_VIDEO_OVERLAY(GST_MESSAGE_SRC(m));
    gst_video_overlay_set_window_handle(o, (guintptr)video_surface);
    gst_video_overlay_set_render_rectangle(o, 0, 0, win_w, win_h);
    gst_message_unref(m);
    return GST_BUS_DROP;
  }
  return GST_BUS_PASS;
}

static void close_file(void) {
  if (pipe_ == NULL)
    return;
  gst_element_set_state(pipe_, GST_STATE_NULL);
  gst_object_unref(pipe_);
  pipe_ = vsink = NULL;
  if (collection != NULL)
    gst_object_unref(collection);
  collection = NULL;
  audio_playing = -1;
  atomic_store(&shm->option_count, OPT_AUDIO);
}

/* Cedrus's decoders out of the running, for the files it can't do */
static void use_software(void) {
  GList *list = gst_registry_get_feature_list(gst_registry_get(), GST_TYPE_ELEMENT_FACTORY);
  for (GList *l = list; l != NULL; l = l->next)
    if (g_str_has_prefix(gst_plugin_feature_get_name(l->data), "v4l2sl"))
      gst_plugin_feature_set_rank(l->data, GST_RANK_NONE);
  gst_plugin_feature_list_free(list);
  software = true;
}

static bool open_file(GstClockTime at) {
  close_file();
  pipe_ = gst_element_factory_make("playbin3", NULL);
  vsink = gst_element_factory_make("waylandsink", NULL);
  GstElement *asink = gst_element_factory_make("alsasink", NULL);
  gchar *uri = gst_filename_to_uri(path, NULL);
  if (pipe_ == NULL || vsink == NULL || uri == NULL) {
    show("GStreamer is missing playbin3 or waylandsink");
    g_free(uri);
    return false;
  }
  /* playbin3: playbin's decodebin answers the decoder's allocation query
   * before the sink is linked, without VideoMeta, and v4l2codecs then refuses
   * dmabufs. Video and audio only; playsink's converter passes dmabufs
   * through and turns software's 10-bit into something waylandsink takes. */
  g_object_set(pipe_, "uri", uri, "flags", 0x1 | 0x2, "video-sink", vsink, NULL);
  if (asink != NULL)
    g_object_set(pipe_, "audio-sink", asink, NULL);
  g_free(uri);
  GstBus *bus = gst_element_get_bus(pipe_);
  gst_bus_set_sync_handler(bus, bus_sync, NULL, NULL);
  gst_object_unref(bus);
  apply_options();
  duration = GST_CLOCK_TIME_NONE;
  seek_target = GST_CLOCK_TIME_NONE;
  start_at = at;
  prerolled = finished = false;
  gst_element_set_state(pipe_, GST_STATE_PAUSED);
  return true;
}

static void play_state(void) {
  if (pipe_ != NULL && prerolled)
    gst_element_set_state(pipe_, user_paused || ctl_paused ? GST_STATE_PAUSED : GST_STATE_PLAYING);
}

static bool is_video(const char *name) {
  const char *dot = strrchr(name, '.');
  for (size_t i = 0; dot != NULL && i < SDL_arraysize(video_exts); i++)
    if (strcasecmp(dot, video_exts[i]) == 0)
      return true;
  return false;
}

/* The video before or after this one in its folder, by name like the
 * launcher sorts them; false at either end */
static bool neighbour(int step) {
  char dir[1024];
  snprintf(dir, sizeof(dir), "%s", path);
  char *slash = strrchr(dir, '/');
  const char *self = slash ? slash + 1 : path;
  if (slash != NULL)
    *slash = '\0';
  DIR *d = opendir(slash ? dir : ".");
  if (d == NULL)
    return false;
  char best[256] = "";
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (e->d_name[0] == '.' || !is_video(e->d_name))
      continue;
    const int c = strcasecmp(e->d_name, self);
    if ((step > 0 && c > 0 && (!best[0] || strcasecmp(e->d_name, best) < 0)) ||
        (step < 0 && c < 0 && (!best[0] || strcasecmp(e->d_name, best) > 0)))
      snprintf(best, sizeof(best), "%s", e->d_name);
  }
  closedir(d);
  if (!best[0])
    return false;
  char next[sizeof(dir) + sizeof(best) + 1];
  snprintf(next, sizeof(next), "%s%s%s", slash ? dir : "", slash ? "/" : "", best);
  if (strlen(next) >= sizeof(path))
    return false;
  memcpy(path, next, strlen(next) + 1);
  return true;
}

static void switch_file(int step) {
  remember();
  if (!neighbour(step)) {
    show(step > 0 ? "Last video in this folder" : "First video in this folder");
    return;
  }
  open_file((GstClockTime)resume_get(path) * GST_SECOND);
  osd_poke();
}

static void seek_by(GstClockTimeDiff delta) {
  if (pipe_ == NULL || !prerolled)
    return;
  const GstClockTime pos = position();
  if (!GST_CLOCK_TIME_IS_VALID(pos))
    return;
  GstClockTimeDiff t = (GstClockTimeDiff)pos + delta;
  if (GST_CLOCK_TIME_IS_VALID(duration))
    t = SDL_min(t, (GstClockTimeDiff)duration - GST_SECOND);
  t = SDL_max(t, 0);
  /* The keyframe on the far side, so a long GOP still moves */
  const GstSeekFlags snap = delta > 0 ? GST_SEEK_FLAG_SNAP_AFTER : GST_SEEK_FLAG_SNAP_BEFORE;
  if (gst_element_seek_simple(pipe_, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT | snap, t))
    seek_target = (GstClockTime)t;
  finished = false;
  osd_poke();
}

static void bus_messages(void) {
  if (pipe_ == NULL)
    return;
  GstBus *bus = gst_element_get_bus(pipe_);
  GstMessage *m;
  while (pipe_ != NULL && (m = gst_bus_pop(bus)) != NULL) {
    switch (GST_MESSAGE_TYPE(m)) {
    case GST_MESSAGE_ASYNC_DONE:
      seek_target = GST_CLOCK_TIME_NONE;
      if (!prerolled) {
        prerolled = true;
        gint64 d;
        if (gst_element_query_duration(pipe_, GST_FORMAT_TIME, &d))
          duration = (GstClockTime)d;
        apply_options();
        if (start_at > 0 && (!GST_CLOCK_TIME_IS_VALID(duration) || start_at < duration)) {
          char t[32], line[64];
          format_time(t, sizeof(t), start_at);
          snprintf(line, sizeof(line), "Resumed at %s", t);
          show(line);
          /* Exactly there: a keyframe can be 10 s before */
          gst_element_seek_simple(pipe_, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE, start_at);
        }
        play_state();
      }
      break;
    case GST_MESSAGE_STREAM_COLLECTION:
      if (collection != NULL)
        gst_object_unref(collection);
      gst_message_parse_stream_collection(m, &collection);
      audio_options();
      break;
    case GST_MESSAGE_STREAMS_SELECTED:
      streams_selected(m);
      break;
    case GST_MESSAGE_DURATION_CHANGED: {
      gint64 d;
      if (gst_element_query_duration(pipe_, GST_FORMAT_TIME, &d))
        duration = (GstClockTime)d;
      break;
    }
    case GST_MESSAGE_EOS:
      finished = true;
      remember();
      if (neighbour(1)) {
        open_file((GstClockTime)resume_get(path) * GST_SECOND);
        osd_poke();
      } else {
        show("The end");
        quit = ctl >= 0;
      }
      break;
    case GST_MESSAGE_ERROR: {
      GError *err = NULL;
      gchar *debug = NULL;
      gst_message_parse_error(m, &err, &debug);
      fprintf(stderr, "mediaplayer: %s: %s (%s)\n", GST_OBJECT_NAME(GST_MESSAGE_SRC(m)), err->message,
              debug ? debug : "");
      const GstClockTime pos = prerolled ? position() : start_at;
      if (!software) {
        use_software();
        open_file(GST_CLOCK_TIME_IS_VALID(pos) ? pos : 0);
        show("Software decoding");
      } else {
        close_file();
        show(err->message);
        finished = true;
      }
      g_error_free(err);
      g_free(debug);
      break;
    }
    default:
      break;
    }
    gst_message_unref(m);
  }
  gst_object_unref(bus);
}

/* --- input --------------------------------------------------------------- */

static SDL_GameController *pad;
static SDL_GameControllerButton held = SDL_CONTROLLER_BUTTON_INVALID;
static Uint64 repeat_at;
static bool menu_down;

static GstClockTimeDiff seek_step(SDL_GameControllerButton b) {
  switch (b) {
  case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return -SEEK_SHORT;
  case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return SEEK_SHORT;
  case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return -SEEK_LONG;
  case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return SEEK_LONG;
  default: return 0;
  }
}

static void press(SDL_GameControllerButton b) {
  /* Menu + anything is the launcher's */
  if (b == SDL_CONTROLLER_BUTTON_GUIDE)
    menu_down = true;
  if (menu_down || ctl_paused)
    return;
  if (seek_step(b) != 0) {
    seek_by(seek_step(b));
    held = b;
    repeat_at = SDL_GetTicks64() + REPEAT_DELAY_MS;
    return;
  }
  switch (b) {
  case SDL_CONTROLLER_BUTTON_A:
  case SDL_CONTROLLER_BUTTON_START:
    if (finished)
      break;
    user_paused = !user_paused;
    play_state();
    if (user_paused)
      remember();
    osd_poke();
    break;
  case SDL_CONTROLLER_BUTTON_B: quit = true; break;
  case SDL_CONTROLLER_BUTTON_DPAD_UP: switch_file(-1); break;
  case SDL_CONTROLLER_BUTTON_DPAD_DOWN: switch_file(1); break;
  case SDL_CONTROLLER_BUTTON_X:
  case SDL_CONTROLLER_BUTTON_Y:
  case SDL_CONTROLLER_BUTTON_BACK:
    osd_until = SDL_GetTicks64() < osd_until ? 0 : SDL_GetTicks64() + OSD_SHOWN_MS;
    break;
  default: break;
  }
}

static void release(SDL_GameControllerButton b) {
  if (b == SDL_CONTROLLER_BUTTON_GUIDE)
    menu_down = false;
  if (b == held)
    held = SDL_CONTROLLER_BUTTON_INVALID;
}

static void input(void) {
  SDL_Event ev;
  while (SDL_PollEvent(&ev)) {
    if (ev.type == SDL_QUIT)
      quit = true;
    else if (ev.type == SDL_CONTROLLERDEVICEADDED && pad == NULL)
      pad = SDL_GameControllerOpen(ev.cdevice.which);
    else if (ev.type == SDL_CONTROLLERBUTTONDOWN)
      press(ev.cbutton.button);
    else if (ev.type == SDL_CONTROLLERBUTTONUP)
      release(ev.cbutton.button);
  }
  if (held != SDL_CONTROLLER_BUTTON_INVALID && SDL_GetTicks64() >= repeat_at) {
    seek_by(seek_step(held));
    repeat_at = SDL_GetTicks64() + REPEAT_MS;
  }
}

/* --- launcher ------------------------------------------------------------ */

/* One request, answered; false once the launcher is gone */
static bool control(void) {
  appctl_msg m;
  const ssize_t n = recv(ctl, &m, sizeof(m), MSG_DONTWAIT);
  if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR))
    return false;
  if (n != sizeof(m))
    return true;
  m.result = 0;
  switch (m.op) {
  case APPCTL_PAUSE:
    ctl_paused = true;
    held = SDL_CONTROLLER_BUTTON_INVALID;
    menu_down = false;
    play_state();
    remember();
    break;
  case APPCTL_RESUME:
    ctl_paused = false;
    if (m.arg)
      raise_window();
    /* Presses that were the launcher's */
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
    play_state();
    break;
  case APPCTL_OPTIONS:
    apply_options();
    save_options();
    break;
  default: m.result = -ENOSYS;
  }
  send(ctl, &m, sizeof(m), MSG_NOSIGNAL);
  return true;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s FILE\n", argv[0]);
    return 2;
  }
  if (argv[1][0] == '/' || getcwd(path, sizeof(path)) == NULL)
    snprintf(path, sizeof(path), "%s", argv[1]);
  else
    snprintf(path + strlen(path), sizeof(path) - strlen(path), "/%s", argv[1]);

  const char *home = getenv("HOME"), *config = getenv("XDG_CONFIG_HOME");
  if (config != NULL && config[0] == '/')
    snprintf(opt_path, sizeof(opt_path), "%s/mediaplayer.opt", config);
  else
    snprintf(opt_path, sizeof(opt_path), "%s/.config/mediaplayer.opt", home ? home : "/tmp");
  snprintf(state_path, sizeof(state_path), "%s/.local/state/mediaplayer/resume", home ? home : "/tmp");
  mkdirs(opt_path);
  mkdirs(state_path);

  gst_init(&argc, &argv);
  if (!open_ctl() || SDL_Init(SDL_INIT_GAMECONTROLLER) != 0 || !font_init() || !window_init()) {
    fprintf(stderr, "mediaplayer: can't start (%s)\n", SDL_GetError());
    return 1;
  }
  open_file((GstClockTime)resume_get(path) * GST_SECOND);
  osd_poke();

  Uint64 saved = SDL_GetTicks64();
  while (!quit) {
    while (wl_display_prepare_read(dpy) != 0)
      wl_display_dispatch_pending(dpy);
    wl_display_flush(dpy);
    struct pollfd fds[2] = {{wl_display_get_fd(dpy), POLLIN, 0}, {ctl, POLLIN, 0}};
    /* ponytail: SDL has no fd for the pad, so it is polled at 50 Hz */
    if (poll(fds, ctl >= 0 ? 2 : 1, 20) > 0 && (fds[0].revents & POLLIN))
      wl_display_read_events(dpy);
    else
      wl_display_cancel_read(dpy);
    if (wl_display_dispatch_pending(dpy) < 0)
      break;
    if (ctl >= 0 && fds[1].revents && !control())
      break;
    input();
    bus_messages();
    osd_draw();
    /* A crash or a dead battery costs at most this much */
    if (SDL_GetTicks64() - saved > 30000 && !user_paused && !ctl_paused) {
      remember();
      saved = SDL_GetTicks64();
    }
  }
  remember();
  close_file();
  SDL_Quit();
  return 0;
}
