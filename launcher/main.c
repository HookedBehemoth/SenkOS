/*
 * Launcher: a status bar over pages, immediate mode. Every frame is drawn
 * from the pages and the status; nav keeps focus and scrolling. A page is a
 * category of apps (apps.h) as a grid of tiles, then the System menu. The
 * page shown at exit (shutdown sends SIGTERM) is shown again at start.
 *
 * D-pad moves (left/right past the edge, or L1/R1, change page), A launches.
 * What it launched owns the screen (Weston kiosk shell) until it exits; the
 * gamepad is read straight from evdev, so input is ignored meanwhile, but for
 * Menu: that stops the app and shows Resume/Quit over a blur of its last frame.
 * Meanwhile the launcher sleeps: no timeouts, no polling, only the pad's Menu,
 * the mixer and the app's exit wake it.
 *
 * A second window, "osd", shows the volume whenever the mixer changes; the
 * patched kiosk shell keeps it on top of everything without focus.
 *
 * Drawn with Vulkan (gfx.h); icons come off the card in the background
 * (assets.h). Runs on a desktop too: LAUNCHER_DATA=launcher/testdata.
 */
#include <alsa/asoundlib.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/pidfd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <SDL.h>

#include "apps.h"
#include "assets.h"
#include "capture.h"
#include "gfx.h"
#include "nav.h"
#include "romdb.h"
#include "text.h"

/* weston.init's cgroup for apps: memory and CPU limits that keep the UI usable */
#define APPS_CGROUP "/sys/fs/cgroup/session/apps/cgroup.procs"

enum { BAR = 32, BAR_TEXT = 18, ROW = 44, ROW_TEXT = 22, MARGIN = 12 };
enum { TILE_W = 144, TILE_H = 140, ICON = 88, TILE_TEXT = 16, LETTER = 40,
       RADIUS = 12, GAP = 12 };
enum { OSD_W = 240, OSD_H = 40, OSD_MS = 1500 };

static const SDL_Color BG = {0x14, 0x14, 0x18, 0xff};
static const SDL_Color FG = {0xee, 0xee, 0xee, 0xff};
static const SDL_Color DIM = {0x55, 0x55, 0x5c, 0xff};
static const SDL_Color FOCUS = {0x2a, 0x2e, 0x3a, 0xff};
static const SDL_Color TILE = {0x1c, 0x1d, 0x23, 0xff};
static const SDL_Color EDGE = {0x33, 0x34, 0x3c, 0xff};
/* Over the paused app's blurred frame */
static const SDL_Color SCRIM = {0x14, 0x14, 0x18, 0xb0};
static const SDL_Color LOW = {0xe0, 0x40, 0x40, 0xff};
static const SDL_Color CHARGING = {0x50, 0xc8, 0x60, 0xff};
static const ui_color RING = {0x6a, 0x9c, 0xff, 0xff};

/* A row of a menu: sh -c */
typedef struct {
  char label[64];
  char command[256];
} entry;

/* Told apart by address */
static const entry paused_entries[] = {{"Resume", ""}, {"Quit", ""}};
static const size_t paused_count = SDL_arraysize(paused_entries);
/* sleepd does them: the launcher runs unprivileged */
static const entry system_entries[] = {
    {"Sleep", "echo suspend > /run/power"},
    {"Reboot", "echo reboot > /run/power"},
    {"Power off", "echo poweroff > /run/power"},
};
static const size_t system_count = SDL_arraysize(system_entries);

/* The categories, then System */
#define PAGES (category_count + 1)

static const char *page_name(size_t page) {
  return page < category_count ? categories[page].name : "System";
}

typedef struct {
  int battery; /* percent, -1 without one */
  bool charging;
  int wifi; /* bars 0-4, -1 without a wireless interface */
} status;

static SDL_Window *window, *osd;
static gfx_target *screen, *osd_screen;
static ui_context ctx;
static pid_t child;
static char child_label[64];
/* SIGSTOPped under the pause screen; quitting: SIGTERM sent, not reaped yet */
static bool paused, quitting;
static gfx_image backdrop;
static Uint64 osd_until;
static Uint32 volume_event, exit_event, loaded_event, hashed_event;

static void rescan(size_t page) {
  if (page < category_count)
    apps_scan(&categories[page]);
}

/* $XDG_STATE_HOME/launcher: the page's name */
static void state_path(char *out, size_t size) {
  const char *state = getenv("XDG_STATE_HOME"), *home = getenv("HOME");
  if (state != NULL && state[0] == '/')
    snprintf(out, size, "%s/launcher", state);
  else
    snprintf(out, size, "%s/.local/state/launcher", home ? home : "/tmp");
}

static size_t load_page(void) {
  char path[512], name[64];
  state_path(path, sizeof(path));
  FILE *f = fopen(path, "r");
  if (f == NULL)
    return 0;
  const bool read = fgets(name, sizeof(name), f) != NULL;
  fclose(f);
  name[strcspn(name, "\n")] = '\0';
  for (size_t i = 0; read && i < PAGES; i++)
    if (strcmp(name, page_name(i)) == 0)
      return i;
  return 0;
}

static void save_page(size_t page) {
  char path[512];
  state_path(path, sizeof(path));
  /* mkdir -p of the two levels under $HOME that may be missing */
  for (char *slash = strchr(path + 1, '/'); slash; slash = strchr(slash + 1, '/')) {
    *slash = '\0';
    mkdir(path, 0755);
    *slash = '/';
  }
  FILE *f = fopen(path, "w");
  if (f == NULL)
    return;
  fprintf(f, "%s\n", page_name(page));
  fclose(f);
}

/* Posts exit_event (code: the pid) once the app is gone: waiting on its pidfd
 * works for an adopted app too, which isn't ours to waitpid. Ours can't be
 * reaped before the event is handled, so the pid stays valid until then. */
static int exit_thread(void *data) {
  const pid_t pid = (pid_t)(intptr_t)data;
  const int fd = pidfd_open(pid, 0);
  if (fd >= 0) {
    struct pollfd p = {.fd = fd, .events = POLLIN};
    while (poll(&p, 1, -1) < 0 && errno == EINTR)
      ;
    close(fd);
  } else if (errno != ESRCH) {
    SDL_Log("pidfd_open: %s", strerror(errno));
  }
  SDL_Event ev = {.user = {.type = exit_event, .code = pid}};
  SDL_PushEvent(&ev);
  return 0;
}

static void watch_child(void) {
  SDL_DetachThread(SDL_CreateThread(exit_thread, "exit", (void *)(intptr_t)child));
}

static void set_listening(bool on);

/* In every app's environment, with its label: a launcher that restarts finds
 * the app the last one left running (adopt). */
#define APP_ENV "LAUNCHER_APP="

extern char **environ;

/* sh -c `command` with $1 = `arg`, in `dir` when there is one */
static void launch(const char *label, const char *command, const char *arg,
                   const char *dir) {
  /* Built before fork: the child of a threaded process should not malloc */
  size_t n = 0;
  while (environ[n] != NULL)
    n++;
  char **env = malloc((n + 2) * sizeof(*env));
  char marker[sizeof(APP_ENV) + sizeof(child_label)];
  if (env == NULL)
    return;
  snprintf(marker, sizeof(marker), APP_ENV "%s", label);
  memcpy(env, environ, n * sizeof(*env));
  env[n] = marker;
  env[n + 1] = NULL;
  const pid_t pid = fork();
  if (pid == 0) {
    setsid();
    /* Without the cgroup the app runs as it is, unlimited */
    const int cg = open(APPS_CGROUP, O_WRONLY | O_CLOEXEC);
    if (cg >= 0) {
      (void)!write(cg, "0", 1);
      close(cg);
    }
    if (dir[0] != '\0' && chdir(dir) != 0)
      _exit(127);
    execle("/bin/sh", "sh", "-c", command, "sh", arg, (char *)NULL, env);
    _exit(127);
  }
  free(env);
  if (pid > 0) {
    child = pid;
    snprintf(child_label, sizeof(child_label), "%s", label);
    set_listening(false);
    watch_child();
  }
}

static int read_line(const char *path, char *out, size_t size);

/* The app a previous launcher left behind: the session leader (setsid) with
 * APP_ENV. Continued, in case it was left under the pause screen. */
static void adopt(void) {
  DIR *proc = opendir("/proc");
  if (proc == NULL)
    return;
  struct dirent *d;
  while (child == 0 && (d = readdir(proc)) != NULL) {
    const pid_t pid = atoi(d->d_name);
    char path[64], buf[4096];
    if (pid <= 0)
      continue;
    /* "pid (comm) state ppid pgrp session ...": comm may hold anything */
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    int session = 0;
    if (read_line(path, buf, sizeof(buf)) != 0 || strrchr(buf, ')') == NULL ||
        sscanf(strrchr(buf, ')') + 1, " %*c %*d %*d %d", &session) != 1 ||
        session != pid)
      continue;
    snprintf(path, sizeof(path), "/proc/%d/environ", pid);
    FILE *f = fopen(path, "r");
    if (f == NULL)
      continue;
    const size_t len = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[len] = '\0';
    /* NUL-separated; ponytail: only the first 4 KiB */
    for (const char *v = buf; v < buf + len; v += strlen(v) + 1)
      if (strncmp(v, APP_ENV, strlen(APP_ENV)) == 0) {
        child = pid;
        snprintf(child_label, sizeof(child_label), "%s", v + strlen(APP_ENV));
        kill(-pid, SIGCONT);
        break;
      }
  }
  closedir(proc);
}

/* The built-in pad, read by this thread and turned into controller events.
 * Not SDL's game controller: with a joystick open, SDL_WaitEvent polls every
 * millisecond. While the app has the screen only Menu gets through. EVIOCGRAB
 * while the app is stopped keeps the presses for the pause screen out of its
 * queue, where it would find them on SIGCONT.
 * ponytail: the built-in pad only, raw codes as it labels them. */
#define PAD_NAME "gpio-keys-gamepad"
static int pad_fd = -1;
static SDL_atomic_t listening;

static void set_listening(bool on) { SDL_AtomicSet(&listening, on); }

static int pad_thread(void *data) {
  (void)data;
  struct input_event e;
  while (read(pad_fd, &e, sizeof(e)) == sizeof(e)) {
    if (e.type != EV_KEY || e.value == 2 ||
        (e.code != BTN_MODE && !SDL_AtomicGet(&listening)))
      continue;
    int button;
    switch (e.code) {
    case BTN_DPAD_UP: button = SDL_CONTROLLER_BUTTON_DPAD_UP; break;
    case BTN_DPAD_DOWN: button = SDL_CONTROLLER_BUTTON_DPAD_DOWN; break;
    case BTN_DPAD_LEFT: button = SDL_CONTROLLER_BUTTON_DPAD_LEFT; break;
    case BTN_DPAD_RIGHT: button = SDL_CONTROLLER_BUTTON_DPAD_RIGHT; break;
    case BTN_TL: button = SDL_CONTROLLER_BUTTON_LEFTSHOULDER; break;
    case BTN_TR: button = SDL_CONTROLLER_BUTTON_RIGHTSHOULDER; break;
    case BTN_EAST: button = SDL_CONTROLLER_BUTTON_A; break;
    case BTN_SOUTH: button = SDL_CONTROLLER_BUTTON_B; break;
    case BTN_MODE: button = SDL_CONTROLLER_BUTTON_GUIDE; break;
    default: continue;
    }
    SDL_Event ev = {.cbutton = {.type = e.value ? SDL_CONTROLLERBUTTONDOWN
                                                : SDL_CONTROLLERBUTTONUP,
                                .button = (Uint8)button}};
    SDL_PushEvent(&ev);
  }
  return 0;
}

static void open_pad(void) {
  glob_t g;
  if (glob("/sys/class/input/event*/device/name", 0, NULL, &g) != 0)
    return;
  for (size_t i = 0; i < g.gl_pathc && pad_fd < 0; i++) {
    char name[64], path[64];
    if (read_line(g.gl_pathv[i], name, sizeof(name)) != 0 ||
        strncmp(name, PAD_NAME "\n", sizeof(PAD_NAME)) != 0)
      continue;
    /* /sys/class/input/eventN/device/name -> /dev/input/eventN */
    const char *ev = strstr(g.gl_pathv[i], "event");
    snprintf(path, sizeof(path), "/dev/input/%.*s", (int)strcspn(ev, "/"), ev);
    pad_fd = open(path, O_RDONLY | O_CLOEXEC);
  }
  globfree(&g);
  if (pad_fd >= 0)
    SDL_DetachThread(SDL_CreateThread(pad_thread, "pad", NULL));
}

static void grab(bool on) {
  if (pad_fd >= 0)
    ioctl(pad_fd, EVIOCGRAB, on);
}

/* setsid made the child a group leader: signals go to all it started. Stopped
 * first, so the frame blurred is the one it stays on. The pad is grabbed once
 * Menu is released (input). */
static void drop_backdrop(void) {
  gfx_bind(GFX_BACKDROP, NULL);
  gfx_release(&backdrop);
}

static void pause_child(nav *ui) {
  kill(-child, SIGSTOP);
  drop_backdrop();
  int w, h;
  uint8_t *rgba = capture_blurred(&w, &h);
  if (rgba != NULL && gfx_upload(&backdrop, rgba, w, h))
    gfx_bind(GFX_BACKDROP, &backdrop);
  free(rgba);
  paused = true;
  set_listening(true);
  nav_focus(ui, 0);
  /* kiosk-shell puts a toplevel on top when it maps: map again */
  SDL_HideWindow(window);
  SDL_ShowWindow(window);
}

static void resume_child(void) {
  paused = false;
  set_listening(false);
  grab(false);
  SDL_HideWindow(window);
  drop_backdrop();
  kill(-child, SIGCONT);
}

/* SDL apps turn SIGTERM into SDL_QUIT and exit cleanly (mGBA flushes its
 * save). The pause screen stays up over the app until it has exited. */
static void quit_child(void) {
  paused = false;
  quitting = true;
  set_listening(false);
  grab(false);
  kill(-child, SIGTERM);
  kill(-child, SIGCONT);
}

/* Line Out on hw:0, which weston's volume-keys module sets. Posts the level in
 * percent, 0 when muted, whenever it changes. */
static int volume_thread(void *data) {
  (void)data;
  snd_mixer_t *mixer;
  snd_mixer_selem_id_t *id;
  snd_mixer_selem_id_alloca(&id);
  snd_mixer_selem_id_set_name(id, "Line Out");
  snd_mixer_elem_t *elem;
  long min, max;
  if (snd_mixer_open(&mixer, 0) < 0)
    return 0;
  if (snd_mixer_attach(mixer, "hw:0") < 0 ||
      snd_mixer_selem_register(mixer, NULL, NULL) < 0 ||
      snd_mixer_load(mixer) < 0 ||
      (elem = snd_mixer_find_selem(mixer, id)) == NULL ||
      snd_mixer_selem_get_playback_volume_range(elem, &min, &max) < 0 ||
      max <= min) {
    snd_mixer_close(mixer);
    return 0;
  }
  int last = -1;
  for (;;) {
    long raw = min;
    int on = 1;
    snd_mixer_selem_get_playback_volume(elem, SND_MIXER_SCHN_FRONT_LEFT, &raw);
    snd_mixer_selem_get_playback_switch(elem, SND_MIXER_SCHN_FRONT_LEFT, &on);
    const int percent = on ? (int)((raw - min) * 100 / (max - min)) : 0;
    if (last >= 0 && percent != last) {
      SDL_Event ev = {.user = {.type = volume_event, .code = percent}};
      SDL_PushEvent(&ev);
    }
    last = percent;
    if (snd_mixer_wait(mixer, -1) < 0 || snd_mixer_handle_events(mixer) < 0)
      break;
  }
  snd_mixer_close(mixer);
  return 0;
}

static int read_line(const char *path, char *out, size_t size) {
  FILE *f = fopen(path, "r");
  if (f == NULL)
    return -1;
  const bool ok = fgets(out, (int)size, f) != NULL;
  fclose(f);
  return ok ? 0 : -1;
}

static void read_status(status *s) {
  char line[256];
  s->battery = -1;
  s->charging = false;
  glob_t g;
  if (glob("/sys/class/power_supply/*/capacity", 0, NULL, &g) == 0) {
    if (read_line(g.gl_pathv[0], line, sizeof(line)) == 0)
      s->battery = atoi(line);
    char path[512];
    snprintf(path, sizeof(path), "%.*s/status",
             (int)(strrchr(g.gl_pathv[0], '/') - g.gl_pathv[0]),
             g.gl_pathv[0]);
    s->charging = read_line(path, line, sizeof(line)) == 0 &&
                  strncmp(line, "Charging", 8) == 0;
    globfree(&g);
  }
  /* Two header lines, then "wlan0: 0000   54.  -56. ...": link quality of 70. */
  s->wifi = -1;
  FILE *f = fopen("/proc/net/wireless", "r");
  if (f != NULL) {
    float quality;
    if (fgets(line, sizeof(line), f) && fgets(line, sizeof(line), f) &&
        fgets(line, sizeof(line), f) &&
        sscanf(line, "%*[^:]: %*x %f", &quality) == 1)
      s->wifi = SDL_clamp((int)(quality * 4 / 70 + 0.5f), 0, 4);
    fclose(f);
  }
}

static void rounded(float x, float y, float w, float h, SDL_Color c,
                    float radius, float border) {
  gfx_push((gfx_quad){.x = x, .y = y, .w = w, .h = h, .u1 = 1, .v1 = 1,
                      .color = gfx_rgba(c), .tex = GFX_WHITE,
                      .radius = radius, .border = border});
}

static void fill(float x, float y, float w, float h, SDL_Color c) {
  rounded(x, y, w, h, c, 0, 0);
}

/* Speaker, then the level as a bar. Created on first use: a second
 * swapchain at startup delays the first frame. */
static void show_volume(int percent) {
  if (osd == NULL)
    osd = SDL_CreateWindow("osd", SDL_WINDOWPOS_UNDEFINED,
                           SDL_WINDOWPOS_UNDEFINED, OSD_W, OSD_H,
                           SDL_WINDOW_HIDDEN | SDL_WINDOW_VULKAN);
  if (osd != NULL && osd_screen == NULL)
    osd_screen = gfx_target_create(osd, true);
  if (osd_screen == NULL)
    return;
  SDL_ShowWindow(osd);
  int w, h;
  if (!gfx_begin(osd_screen, &w, &h))
    return;
  fill(0, 0, w, h, (SDL_Color){BG.r, BG.g, BG.b, SCRIM.a});
  fill(MARGIN, OSD_H / 2 - 5, 6, 10, FG);
  /* The cone, a column a pixel */
  const float x0 = MARGIN + 6, x1 = x0 + 8, y = OSD_H / 2;
  for (float x = x0; x < x1; x++) {
    const float half = 5 + (x - x0) * 6 / (x1 - x0);
    fill(x, y - half, 1, half * 2, FG);
  }
  const float bx = x1 + MARGIN, bw = OSD_W - bx - MARGIN * 1.5f;
  rounded(bx, y - 3, bw, 6, DIM, 3, 0);
  rounded(bx, y - 3, bw * percent / 100, 6, FG, 3, 0);
  gfx_end(osd_screen);
  osd_until = SDL_GetTicks64() + OSD_MS;
}

/* "[time]      [page]         [wifi bars][battery%][battery symbol]" */
static void top_bar(int width, const char *page, const status *s) {
  char clock[8];
  const time_t now = time(NULL);
  strftime(clock, sizeof(clock), "%H:%M", localtime(&now));
  const int y = (BAR - text_height(BAR_TEXT)) / 2;
  text_draw(BAR_TEXT, MARGIN, y, clock, FG);
  text_draw(BAR_TEXT, (width - text_width(BAR_TEXT, page)) / 2, y, page, FG);

  float x = width - MARGIN;
  if (s->battery >= 0) {
    const SDL_Color level = s->charging         ? CHARGING
                            : s->battery <= 15 ? LOW
                                               : FG;
    rounded(x -= 2, BAR / 2 - 3, 2, 6, FG, 1, 0);
    x -= 24;
    rounded(x, BAR / 2 - 6, 24, 12, FG, 3, 1);
    rounded(x + 2, BAR / 2 - 4, 20 * SDL_clamp(s->battery, 0, 100) / 100.0f, 8,
            level, 1.5f, 0);
    char percent[8];
    snprintf(percent, sizeof(percent), "%d%%", s->battery);
    x -= 6 + text_width(BAR_TEXT, percent);
    text_draw(BAR_TEXT, (int)x, y, percent, FG);
  }
  x -= 10 + 4 * 5;
  for (int i = 0; i < 4; i++) {
    const float h = 4 + 3 * i;
    rounded(x + i * 5, BAR / 2 + 7 - h, 3, h, i < s->wifi ? FG : DIM, 1, 0);
  }
  fill(0, BAR, width, 1, DIM);
}

/* Returns the entry activated this frame, if any. */
static const entry *menu(nav *ui, const entry *entries, size_t count,
                         ui_rect view) {
  if (count == 0) {
    text_draw(ROW_TEXT, view.x + MARGIN * 2, view.y + MARGIN, "Nothing here",
              DIM);
    return NULL;
  }
  const entry *chosen = NULL;
  const float offset =
      nav_scroll_begin(ui, nav_str("menu"), view, UI_VERTICAL,
                       count * ROW + MARGIN * 2, ROW, 0);
  gfx_clip(view.x, view.y, view.w, view.h);
  for (size_t i = 0; i < count; i++) {
    const ui_rect r = {view.x + MARGIN, view.y + MARGIN + i * ROW - offset,
                       view.w - MARGIN * 2, ROW - 4};
    const unsigned flags = nav_item(ui, nav_int(i), r, view);
    nav_ring(ui, r, 8);
    if (flags & NAV_FOCUSED)
      rounded(r.x, r.y, r.w, r.h, FOCUS, 6, 0);
    text_draw(ROW_TEXT, (int)r.x + MARGIN,
              (int)r.y + ((int)r.h - text_height(ROW_TEXT)) / 2,
              entries[i].label, FG);
    if (flags & NAV_ACTIVATED)
      chosen = &entries[i];
  }
  gfx_noclip();
  nav_scroll_end(ui);
  return chosen;
}

/* `utf8` cut to fit `max` pixels, with an ellipsis */
static const char *fit(int px, const char *utf8, float max) {
  static char out[80];
  size_t n = snprintf(out, sizeof(out) - 4, "%s", utf8);
  n = SDL_min(n, sizeof(out) - 5);
  while (n > 0 && text_width(px, out) > max) {
    do
      n--;
    while (n > 0 && (out[n] & 0xc0) == 0x80);
    memcpy(out + n, "\u2026", 4);
  }
  return out;
}

static void tile(const app *a, ui_rect r, unsigned flags) {
  rounded(r.x, r.y, r.w, r.h, flags & NAV_FOCUSED ? FOCUS : TILE, RADIUS, 0);
  rounded(r.x, r.y, r.w, r.h, EDGE, RADIUS, 1);
  const float ix = r.x + (r.w - ICON) / 2, iy = r.y + 12;
  const asset icon = assets_get(a->icon);
  if (icon.slot == GFX_PLACEHOLDER) {
    /* Until it loads, or without one: the initial */
    rounded(ix, iy, ICON, ICON, EDGE, RADIUS - 2, 0);
    const char initial[2] = {a->name[0], '\0'};
    text_draw(LETTER, (int)(ix + (ICON - text_width(LETTER, initial)) / 2),
              (int)(iy + (ICON - text_height(LETTER)) / 2), initial, DIM);
  } else {
    /* Fit, aspect kept */
    const float scale = SDL_min((float)ICON / icon.w, (float)ICON / icon.h);
    const float w = icon.w * scale, h = icon.h * scale;
    gfx_push((gfx_quad){.x = ix + (ICON - w) / 2, .y = iy + (ICON - h) / 2,
                        .w = w, .h = h, .u1 = 1, .v1 = 1,
                        .color = 0xffffffff, .tex = icon.slot,
                        .radius = RADIUS - 2});
  }
  const char *name = fit(TILE_TEXT, a->name, r.w - MARGIN);
  text_draw(TILE_TEXT, (int)(r.x + (r.w - text_width(TILE_TEXT, name)) / 2),
            (int)(iy + ICON + (r.y + r.h - iy - ICON - text_height(TILE_TEXT)) / 2),
            name, FG);
}

/* A grid, scrolled by rows. Returns the app activated this frame, if any. */
static const app *tiles(nav *ui, const category *c, ui_rect view) {
  if (c->count == 0) {
    text_draw(ROW_TEXT, view.x + MARGIN * 2, view.y + MARGIN, "Nothing here",
              DIM);
    return NULL;
  }
  const int cols = SDL_max(1, (int)((view.w - GAP) / (TILE_W + GAP)));
  const size_t rows = (c->count + cols - 1) / cols;
  const float left = view.x + (view.w - cols * (TILE_W + GAP) + GAP) / 2;
  const float offset =
      nav_scroll_begin(ui, nav_str("tiles"), view, UI_VERTICAL,
                       rows * (TILE_H + GAP) + GAP, GAP, 0);
  const app *chosen = NULL;
  gfx_clip(view.x, view.y, view.w, view.h);
  for (size_t i = 0; i < c->count; i++) {
    const ui_rect r = {left + (i % cols) * (TILE_W + GAP),
                       view.y + GAP + (i / cols) * (TILE_H + GAP) - offset,
                       TILE_W, TILE_H};
    /* Every tile is there to move to; only those in view are drawn, and only
     * their icons are asked for */
    const unsigned flags = nav_item(ui, nav_int(i), r, view);
    nav_ring(ui, r, RADIUS + 2);
    if (r.y < view.y + view.h && r.y + r.h > view.y)
      tile(&c->apps[i], r, flags);
    if (flags & NAV_ACTIVATED)
      chosen = &c->apps[i];
  }
  gfx_noclip();
  nav_scroll_end(ui);
  return chosen;
}

static void turn_page(size_t *page, int by) {
  *page = (*page + PAGES + by) % PAGES;
  rescan(*page);
}

/* Back to the menu. Reaped here if it is ours; an adopted one is init's. */
static void child_exited(size_t page) {
  waitpid(child, NULL, WNOHANG);
  if (paused)
    grab(false);
  child = 0;
  paused = quitting = false;
  set_listening(true);
  drop_backdrop();
  SDL_ShowWindow(window);
  rescan(page);
}

static bool input(nav *ui, const SDL_Event *ev, size_t *page) {
  int button = -1;
  if (ev->type == volume_event) {
    show_volume(ev->user.code);
    return true;
  }
  /* A ROM's CRC: its title and box art may be known now */
  if (ev->type == hashed_event) {
    rescan(*page);
    return true;
  }
  if (ev->type == exit_event) {
    if (ev->user.code == child)
      child_exited(*page);
    return true;
  }
  switch (ev->type) {
  case SDL_QUIT:
    save_page(*page);
    return false;
  /* Menu's release, which reaches the app queue first: it sees the whole
   * press, and nothing after it until it is resumed. */
  case SDL_CONTROLLERBUTTONUP:
    if (ev->cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE && paused)
      grab(true);
    return true;
  case SDL_CONTROLLERBUTTONDOWN:
    /* Menu toggles the pause screen, on the press */
    if (ev->cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE) {
      if (child != 0 && !quitting) {
        if (paused)
          resume_child();
        else
          pause_child(ui);
      }
      return true;
    }
    button = ev->cbutton.button;
    break;
  case SDL_KEYDOWN: /* a keyboard, or the desktop */
    switch (ev->key.keysym.sym) {
    case SDLK_UP: button = SDL_CONTROLLER_BUTTON_DPAD_UP; break;
    case SDLK_DOWN: button = SDL_CONTROLLER_BUTTON_DPAD_DOWN; break;
    case SDLK_LEFT: button = SDL_CONTROLLER_BUTTON_DPAD_LEFT; break;
    case SDLK_RIGHT: button = SDL_CONTROLLER_BUTTON_DPAD_RIGHT; break;
    case SDLK_RETURN: button = SDL_CONTROLLER_BUTTON_A; break;
    case SDLK_PAGEUP: button = SDL_CONTROLLER_BUTTON_LEFTSHOULDER; break;
    case SDLK_PAGEDOWN: button = SDL_CONTROLLER_BUTTON_RIGHTSHOULDER; break;
    }
    break;
  }
  if (child != 0 && !paused)
    return true;
  if (paused && button == SDL_CONTROLLER_BUTTON_B) {
    resume_child();
    return true;
  }
  switch (button) {
  case SDL_CONTROLLER_BUTTON_DPAD_UP: nav_move(ui, NAV_UP); break;
  case SDL_CONTROLLER_BUTTON_DPAD_DOWN: nav_move(ui, NAV_DOWN); break;
  case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
    if (!nav_move(ui, NAV_LEFT) && !paused)
      turn_page(page, -1);
    break;
  case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
    if (!nav_move(ui, NAV_RIGHT) && !paused)
      turn_page(page, 1);
    break;
  case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
    if (!paused)
      turn_page(page, -1);
    break;
  case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
    if (!paused)
      turn_page(page, 1);
    break;
  /* A by label: SDL_GAMECONTROLLERCONFIG (/etc/profile.d/wayland.sh) maps the pad */
  case SDL_CONTROLLER_BUTTON_A: nav_activate(ui); break;
  }
  return true;
}

int main(void) {
  /* The kiosk shell knows the OSD by app_id and title; apps get their own */
  SDL_setenv("SDL_VIDEO_WAYLAND_WMCLASS", "launcher", 1);
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    SDL_Log("SDL_Init: %s", SDL_GetError());
    return 1;
  }
  unsetenv("SDL_VIDEO_WAYLAND_WMCLASS");
  window = SDL_CreateWindow("Launcher", SDL_WINDOWPOS_UNDEFINED,
                            SDL_WINDOWPOS_UNDEFINED, 640, 480,
                            SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN);
  if (window == NULL || !gfx_init(window) ||
      (screen = gfx_target_create(window, false)) == NULL ||
      !text_init("sans-serif")) {
    SDL_Log("launcher: %s", SDL_GetError());
    return 1;
  }
  volume_event = SDL_RegisterEvents(4);
  exit_event = volume_event + 1;
  loaded_event = volume_event + 2;
  hashed_event = volume_event + 3;
  assets_init(loaded_event);
  romdb_init(hashed_event);
  set_listening(true);
  open_pad();
  /* Mapping put this window over the adopted app: give it back the screen */
  adopt();
  if (child != 0) {
    set_listening(false);
    watch_child();
    SDL_HideWindow(window);
  }
  SDL_DetachThread(SDL_CreateThread(volume_thread, "volume", NULL));
  static nav ui = {.animate = true, .duration = 150};
  size_t page = load_page();
  rescan(page);
  status st;
  read_status(&st);
  Uint64 status_at = SDL_GetTicks64();
  bool animating = true;

  for (;;) {
    SDL_Event ev;
    /* A frame's time while animating. Idle: wake once a second for the
     * clock and the status. Under the app: only events. And to hide the OSD. */
    int wait = child != 0 && !paused ? -1 : animating ? 16 : 1000;
    if (osd_until != 0) {
      const int left = (int)(osd_until - SDL_min(osd_until, SDL_GetTicks64()));
      if (wait < 0 || left < wait)
        wait = left;
    }
    if (SDL_WaitEventTimeout(&ev, wait)) {
      do
        if (!input(&ui, &ev, &page))
          return 0;
      while (SDL_PollEvent(&ev));
      animating = true;
    }
    const Uint64 now = SDL_GetTicks64();
    if (osd_until != 0 && now >= osd_until) {
      SDL_HideWindow(osd);
      osd_until = 0;
    }
    /* Under the app, or hidden: nothing would show */
    if (child != 0 && !paused) {
      animating = false;
      continue;
    }
    if (now - status_at >= 1000) {
      read_status(&st);
      status_at = now;
    }

    assets_frame();
    int w, h;
    if (!gfx_begin(screen, &w, &h))
      continue;
    fill(0, 0, w, h, BG);
    if (paused && backdrop.image != VK_NULL_HANDLE) {
      gfx_push((gfx_quad){.w = w, .h = h, .u1 = 1, .v1 = 1,
                          .color = 0xffffffff, .tex = GFX_BACKDROP});
      fill(0, 0, w, h, SCRIM);
    }
    nav_begin(&ui, &ctx, now);
    const entry *chosen = NULL;
    const app *picked = NULL;
    const ui_rect view = {0, BAR + 1, w, h - BAR - 1};
    if (paused) {
      top_bar(w, child_label, &st);
      NAV_SCOPE(&ui, nav_str("paused")) {
        chosen = menu(&ui, paused_entries, paused_count, view);
      }
    } else {
      top_bar(w, page_name(page), &st);
      NAV_SCOPE(&ui, nav_int(page)) {
        if (page < category_count)
          picked = tiles(&ui, &categories[page], view);
        else
          chosen = menu(&ui, system_entries, system_count, view);
      }
    }
    animating = nav_end(&ui, RING, 2);
    gfx_end(screen);
    static bool presented;
    if (!presented) {
      /* boot-time measurements: seconds since kernel start */
      struct timespec ts;
      clock_gettime(CLOCK_BOOTTIME, &ts);
      SDL_Log("first frame at %.3f s", ts.tv_sec + ts.tv_nsec / 1e9);
      presented = true;
    }
    if (chosen == &paused_entries[0])
      resume_child();
    else if (chosen == &paused_entries[1])
      quit_child();
    else if (chosen != NULL)
      launch(chosen->label, chosen->command, "", "");
    else if (picked != NULL)
      launch(picked->name, picked->exec, picked->arg, picked->dir);
  }
}
