/*
 * Launcher: a status bar over pages of menus, immediate mode. Every frame is
 * drawn from the pages and the status; nav keeps focus and scrolling.
 *
 * D-pad moves (left/right past the menu, or L1/R1, change page), A launches.
 * What it launched owns the screen (Weston kiosk shell) until it exits; the
 * gamepad is read straight from evdev, so input is ignored meanwhile, but for
 * Menu: that stops the app and shows Resume/Quit over a blur of its last frame.
 * Meanwhile the launcher sleeps: no timeouts, no polling, only the pad's Menu,
 * the mixer and the app's exit wake it.
 *
 * A second window, "osd", shows the volume whenever the mixer changes; the
 * patched kiosk shell keeps it on top of everything without focus.
 *
 * Apps page: built-ins, then every executable in /data/OS/apps.
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
#include <sys/pidfd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <SDL.h>

#include "capture.h"
#include "nav.h"
#include "text.h"

#define APPS_DIR "/data/OS/apps"

enum { BAR = 32, BAR_TEXT = 18, ROW = 44, ROW_TEXT = 22, MARGIN = 12 };
enum { OSD_W = 240, OSD_H = 40, OSD_MS = 1500 };

static const SDL_Color BG = {0x14, 0x14, 0x18, 0xff};
static const SDL_Color FG = {0xee, 0xee, 0xee, 0xff};
static const SDL_Color DIM = {0x55, 0x55, 0x5c, 0xff};
static const SDL_Color FOCUS = {0x2a, 0x2e, 0x3a, 0xff};
/* Over the paused app's blurred frame */
static const SDL_Color SCRIM = {0x14, 0x14, 0x18, 0xb0};
static const SDL_Color LOW = {0xe0, 0x40, 0x40, 0xff};
static const SDL_Color CHARGING = {0x50, 0xc8, 0x60, 0xff};
static const ui_color RING = {0x6a, 0x9c, 0xff, 0xff};

typedef struct {
  char label[64];
  /* Absolute path: exec'd as is. Anything else: sh -c. */
  char command[256];
} entry;

static const entry builtin_apps[] = {
    {"Vulkan cube", "vkcube --wsi wayland"},
};
/* Told apart by address */
static const entry paused_entries[] = {{"Resume", ""}, {"Quit", ""}};
static const size_t paused_count = SDL_arraysize(paused_entries);
static const entry system_entries[] = {
    {"Sleep", "/usr/sbin/suspend"},
    {"Reboot", "reboot"},
    {"Power off", "poweroff"},
};
static entry apps[64];
static size_t app_count;
static const size_t system_count = SDL_arraysize(system_entries);

static const struct {
  const char *name;
  const entry *entries;
  const size_t *count;
} pages[] = {
    {"Apps", apps, &app_count},
    {"System", system_entries, &system_count},
};
#define PAGES SDL_arraysize(pages)

typedef struct {
  int battery; /* percent, -1 without one */
  bool charging;
  int wifi; /* bars 0-4, -1 without a wireless interface */
} status;

static SDL_Window *window, *osd;
static SDL_Renderer *renderer, *osd_renderer;
static ui_context ctx;
static pid_t child;
static char child_label[64];
/* SIGSTOPped under the pause screen; quitting: SIGTERM sent, not reaped yet */
static bool paused, quitting;
static SDL_Texture *backdrop;
static Uint64 osd_until;
static Uint32 volume_event, exit_event;

static int by_label(const void *a, const void *b) {
  return strcmp(((const entry *)a)->label, ((const entry *)b)->label);
}

static void scan_apps(void) {
  app_count = 0;
  for (size_t i = 0; i < SDL_arraysize(builtin_apps); i++)
    apps[app_count++] = builtin_apps[i];
  DIR *dir = opendir(APPS_DIR);
  if (dir == NULL)
    return;
  const size_t first = app_count;
  struct dirent *d;
  while ((d = readdir(dir)) != NULL && app_count < SDL_arraysize(apps)) {
    entry *e = &apps[app_count];
    if (d->d_name[0] == '.' ||
        snprintf(e->command, sizeof(e->command), APPS_DIR "/%s", d->d_name) >=
            (int)sizeof(e->command) ||
        access(e->command, X_OK) != 0)
      continue;
    snprintf(e->label, sizeof(e->label), "%.63s", d->d_name);
    app_count++;
  }
  closedir(dir);
  qsort(apps + first, app_count - first, sizeof(*apps), by_label);
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

static void launch(const entry *e) {
  const char *command = e->command;
  /* Built before fork: the child of a threaded process should not malloc */
  size_t n = 0;
  while (environ[n] != NULL)
    n++;
  char **env = malloc((n + 2) * sizeof(*env));
  char marker[sizeof(APP_ENV) + sizeof(e->label)];
  if (env == NULL)
    return;
  snprintf(marker, sizeof(marker), APP_ENV "%s", e->label);
  memcpy(env, environ, n * sizeof(*env));
  env[n] = marker;
  env[n + 1] = NULL;
  const pid_t pid = fork();
  if (pid == 0) {
    setsid();
    if (command[0] == '/')
      execle(command, command, (char *)NULL, env);
    else
      execle("/bin/sh", "sh", "-c", command, (char *)NULL, env);
    _exit(127);
  }
  free(env);
  if (pid > 0) {
    child = pid;
    snprintf(child_label, sizeof(child_label), "%s", e->label);
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
static void pause_child(nav *ui) {
  kill(-child, SIGSTOP);
  backdrop = capture_blurred(renderer);
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

static void fill(float x, float y, float w, float h, SDL_Color c) {
  SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, c.a);
  SDL_RenderFillRectF(renderer, &(SDL_FRect){x, y, w, h});
}

/* Speaker, then the level as a bar. Its own renderer, so no text. */
static void show_volume(int percent) {
  if (osd_renderer == NULL)
    return;
  SDL_ShowWindow(osd);
  SDL_Renderer *r = osd_renderer;
  /* Wayland wants premultiplied alpha */
  SDL_SetRenderDrawColor(r, BG.r * SCRIM.a / 255, BG.g * SCRIM.a / 255,
                         BG.b * SCRIM.a / 255, SCRIM.a);
  SDL_RenderClear(r);
  SDL_SetRenderDrawColor(r, FG.r, FG.g, FG.b, FG.a);
  SDL_RenderFillRectF(r, &(SDL_FRect){MARGIN, OSD_H / 2 - 5, 6, 10});
  const float x0 = MARGIN + 6, x1 = x0 + 8, y = OSD_H / 2;
  const SDL_Vertex cone[] = {
      {{x0, y - 5}, FG, {0, 0}}, {{x1, y - 11}, FG, {0, 0}}, {{x1, y + 11}, FG, {0, 0}},
      {{x0, y - 5}, FG, {0, 0}}, {{x1, y + 11}, FG, {0, 0}}, {{x0, y + 5}, FG, {0, 0}},
  };
  SDL_RenderGeometry(r, NULL, cone, SDL_arraysize(cone), NULL, 0);
  const float bx = x1 + MARGIN, bw = OSD_W - bx - MARGIN * 1.5f;
  SDL_SetRenderDrawColor(r, DIM.r, DIM.g, DIM.b, DIM.a);
  SDL_RenderFillRectF(r, &(SDL_FRect){bx, y - 3, bw, 6});
  SDL_SetRenderDrawColor(r, FG.r, FG.g, FG.b, FG.a);
  SDL_RenderFillRectF(r, &(SDL_FRect){bx, y - 3, bw * percent / 100, 6});
  SDL_RenderPresent(r);
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
    fill(x -= 2, BAR / 2 - 3, 2, 6, FG);
    x -= 24;
    ui_stroke(&ctx, (ui_rect){x, BAR / 2 - 6, 24, 12}, NULL,
              (ui_color){FG.r, FG.g, FG.b, FG.a}, 1, 0);
    fill(x + 2, BAR / 2 - 4, 20 * SDL_clamp(s->battery, 0, 100) / 100.0f, 8,
         level);
    char percent[8];
    snprintf(percent, sizeof(percent), "%d%%", s->battery);
    x -= 6 + text_width(BAR_TEXT, percent);
    text_draw(BAR_TEXT, (int)x, y, percent, FG);
  }
  x -= 10 + 4 * 5;
  for (int i = 0; i < 4; i++) {
    const float h = 4 + 3 * i;
    fill(x + i * 5, BAR / 2 + 7 - h, 3, h, i < s->wifi ? FG : DIM);
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
  SDL_RenderSetClipRect(renderer, &(SDL_Rect){(int)view.x, (int)view.y,
                                              (int)view.w, (int)view.h});
  for (size_t i = 0; i < count; i++) {
    const ui_rect r = {view.x + MARGIN, view.y + MARGIN + i * ROW - offset,
                       view.w - MARGIN * 2, ROW - 4};
    const unsigned flags = nav_item(ui, nav_int(i), r, view);
    nav_ring(ui, r, 0);
    if (flags & NAV_FOCUSED)
      fill(r.x, r.y, r.w, r.h, FOCUS);
    text_draw(ROW_TEXT, (int)r.x + MARGIN,
              (int)r.y + ((int)r.h - text_height(ROW_TEXT)) / 2,
              entries[i].label, FG);
    if (flags & NAV_ACTIVATED)
      chosen = &entries[i];
  }
  SDL_RenderSetClipRect(renderer, NULL);
  nav_scroll_end(ui);
  return chosen;
}

static void turn_page(size_t *page, int by) {
  *page = (*page + PAGES + by) % PAGES;
  if (*page == 0)
    scan_apps();
}

/* Back to the menu. Reaped here if it is ours; an adopted one is init's. */
static void child_exited(void) {
  waitpid(child, NULL, WNOHANG);
  if (paused)
    grab(false);
  child = 0;
  paused = quitting = false;
  set_listening(true);
  SDL_DestroyTexture(backdrop);
  backdrop = NULL;
  SDL_ShowWindow(window);
  scan_apps();
}

static bool input(nav *ui, const SDL_Event *ev, size_t *page) {
  int button = -1;
  if (ev->type == volume_event) {
    show_volume(ev->user.code);
    return true;
  }
  if (ev->type == exit_event) {
    if (ev->user.code == child)
      child_exited();
    return true;
  }
  switch (ev->type) {
  case SDL_QUIT:
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
  /* An alpha channel for the OSD, and no opaque region */
  SDL_SetHint(SDL_HINT_VIDEO_EGL_ALLOW_TRANSPARENCY, "1");
  /* The kiosk shell knows the OSD by app_id and title; apps get their own */
  SDL_setenv("SDL_VIDEO_WAYLAND_WMCLASS", "launcher", 1);
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    SDL_Log("SDL_Init: %s", SDL_GetError());
    return 1;
  }
  unsetenv("SDL_VIDEO_WAYLAND_WMCLASS");
  window = SDL_CreateWindow("Launcher", SDL_WINDOWPOS_UNDEFINED,
                            SDL_WINDOWPOS_UNDEFINED, 640, 480,
                            SDL_WINDOW_RESIZABLE);
  /* No vsync: Mesa waits for the last frame's callback before the next
   * swap, and a window hidden right after a frame (Resume) may never get it.
   * Weston composites on vblank anyway; the loop paces animation. */
  renderer = window ? SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED)
                    : NULL;
  if (renderer == NULL || !text_init(renderer, "sans-serif")) {
    SDL_Log("launcher: %s", SDL_GetError());
    return 1;
  }
  ctx.renderer = renderer;
  /* The window has alpha now: blend, so it stays opaque under the scrim */
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  /* Hidden until there is something to show */
  osd = SDL_CreateWindow("osd", SDL_WINDOWPOS_UNDEFINED,
                         SDL_WINDOWPOS_UNDEFINED, OSD_W, OSD_H,
                         SDL_WINDOW_HIDDEN);
  osd_renderer =
      osd ? SDL_CreateRenderer(osd, -1, SDL_RENDERER_ACCELERATED) : NULL;
  volume_event = SDL_RegisterEvents(2);
  exit_event = volume_event + 1;
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
  size_t page = 0;
  scan_apps();
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

    int w, h;
    SDL_GetRendererOutputSize(renderer, &w, &h);
    SDL_SetRenderDrawColor(renderer, BG.r, BG.g, BG.b, BG.a);
    SDL_RenderClear(renderer);
    if (paused && backdrop != NULL) {
      SDL_RenderCopy(renderer, backdrop, NULL, NULL);
      fill(0, 0, w, h, SCRIM);
    }
    nav_begin(&ui, &ctx, now);
    const entry *chosen = NULL;
    if (paused) {
      top_bar(w, child_label, &st);
      NAV_SCOPE(&ui, nav_str("paused")) {
        chosen = menu(&ui, paused_entries, paused_count,
                      (ui_rect){0, BAR + 1, w, h - BAR - 1});
      }
    } else {
      top_bar(w, pages[page].name, &st);
      NAV_SCOPE(&ui, nav_int(page)) {
        chosen = menu(&ui, pages[page].entries, *pages[page].count,
                      (ui_rect){0, BAR + 1, w, h - BAR - 1});
      }
    }
    animating = nav_end(&ui, RING, 2);
    SDL_RenderPresent(renderer);
    if (chosen == &paused_entries[0])
      resume_child();
    else if (chosen == &paused_entries[1])
      quit_child();
    else if (chosen != NULL)
      launch(chosen);
  }
}
