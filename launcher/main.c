/*
 * Launcher: a status bar over pages, immediate mode. Every frame is drawn
 * from the pages and the status; nav keeps focus and scrolling. A page is a
 * category of apps (apps.h) as a grid of tiles. The page shown at exit
 * (shutdown sends SIGTERM) is shown again at start.
 *
 * D-pad moves (left/right past the edge, or L1/R1, change page), A launches,
 * Start opens the menu (power), Select on a game picks its core.
 * What it launched owns the screen (Weston kiosk shell); the gamepad is read
 * straight from evdev, so input is ignored meanwhile, but for Menu: released
 * with nothing pressed along (Menu and a button are the app's hotkeys), it
 * pauses the app and shows a menu over a blur of its last frame. Home leaves
 * the app paused in the background, on the Running page; Menu at home brings
 * back the last one. An app on the control channel (running.h, corehost)
 * also gets Save state, Load state and Settings (its options). The focus at
 * home, and in each app's pause menu, is kept meanwhile. While an app has the screen the launcher
 * sleeps: only the pad's Menu, the mixer and an app's exit wake it.
 *
 * A second window, "osd", shows the volume whenever the mixer changes; the
 * patched kiosk shell keeps it on top of everything without focus.
 *
 * Drawn with Vulkan (gfx.h); icons come off the card in the background
 * (assets.h). Runs on a desktop too: LAUNCHER_DATA=launcher/testdata.
 */
#include <alsa/asoundlib.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <linux/input.h>
#include <poll.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <SDL.h>

#include "apps.h"
#include "assets.h"
#include "capture.h"
#include "gfx.h"
#include "icons.h"
#include "nav.h"
#include "romdb.h"
#include "running.h"
#include "text.h"

enum { BAR = 32, BAR_TEXT = 18, ROW = 44, ROW_TEXT = 22, ICON_PX = 22, MARGIN = 12 };
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
/* A binding being recorded */
static const SDL_Color RING_TEXT = {0x6a, 0x9c, 0xff, 0xff};
/* The dot on a tile whose app is running */
static const SDL_Color ACTIVE = {0x6a, 0x9c, 0xff, 0xff};
/* A binding this game has its own of */
static const SDL_Color PER_GAME = {0x6a, 0x9c, 0xff, 0xff};

/* What a menu row does; RUN_COMMAND: sh -c its command */
enum { RUN_COMMAND, RESUME, RESTART, SAVE_STATE, LOAD_STATE, SETTINGS, HOME, QUIT, DISMISS, IGNORE, PICK_CORE };
typedef struct {
  char label[64];
  char command[256];
  int action;
  int icon; /* ICON_*, -1 for none */
} entry;

/* All of it for an app on the control channel, else Resume, Home and Quit.
 * The settings: an entry per category (named as appctl_category_names), for
 * those the app has options in. */
static const entry pause_entries[] = {
    {"Resume", "", RESUME, ICON_PLAY},
    {"Restart", "", RESTART, ICON_ROTATE_CCW},
    {"Save state", "", SAVE_STATE, ICON_SAVE_IN},
    {"Load state", "", LOAD_STATE, ICON_SAVE_OUT},
    {"System", "", SETTINGS, ICON_COG},
    {"Graphics", "", SETTINGS, ICON_IMAGE},
    {"Audio", "", SETTINGS, ICON_SPEAKER},
    {"Input", "", SETTINGS, ICON_DPAD},
    {"Home", "", HOME, ICON_HOUSE},
    {"Quit", "", QUIT, ICON_X},
};

static int category_of(const entry *e) {
  for (int c = 0; c < APPCTL_CATEGORIES; c++)
    if (strcmp(e->label, appctl_category_names[c]) == 0)
      return c;
  return APPCTL_SYSTEM;
}
/* sleepd does them: the launcher runs unprivileged */
static const entry system_entries[] = {
    {"Sleep", "echo suspend > /run/power", RUN_COMMAND, -1},
    {"Reboot", "echo reboot > /run/power", RUN_COMMAND, -1},
    {"Power off", "echo poweroff > /run/power", RUN_COMMAND, -1},
};
static const size_t system_count = SDL_arraysize(system_entries);
static const entry failure_entries[] = {{"OK", "", DISMISS, -1}, {"Ignore for this app", "", IGNORE, -1}};
/* The core picker's rows: one per core, PICK_CORE */
enum { CORES_MAX = 8 };

/* Running (only while something is), then the categories */
#define PAGES (category_count + 1)

static const char *page_name(size_t page) {
  return page == 0 ? "Running" : categories[page - 1].name;
}

typedef struct {
  int battery; /* percent, -1 without one */
  bool charging;
  int wifi; /* bars 0-4, -1 without a wireless interface */
} status;

static SDL_Window *window, *osd;
static gfx_target *screen, *osd_screen;
static ui_context ctx;
/* On screen or under the pause screen; NULL: home */
static running *front;
/* What Menu brings back from home */
static running *last;
static bool paused, in_settings;
static int settings_category;
/* Recording a binding in Settings: every button pressed since the first;
 * letting go of that first one sets the combo. Nothing pressed for
 * CAPTURE_MS gives up. */
enum { CAPTURE_MS = 5000 };
static struct {
  int option; /* -1: not recording */
  uint16_t held, combo;
  int first;
  Uint64 since;
} capture = {.option = -1};
/* Menu is down while the app has the screen: the release pauses, unless
 * another button made it a hotkey */
static bool menu_down, menu_combo;
/* Where the focus was at home, for coming back */
static nav_id home_focus;
/* Under the pause menu: how the last action went */
static char notice[128];
static gfx_image backdrop;
/* An app ended badly: a dialog over home until dismissed. `key` names the
 * app in the ignore list; `back` is the focus under the dialog. */
static struct {
  bool shown;
  char label[64], why[96], key[1300];
  nav_id back;
} failure;
/* Over home: Start's menu, or Select on a game whose system has more than
 * one core: which runs it */
enum { MODAL_NONE, MODAL_MENU, MODAL_CORES };
static struct {
  int kind;
  char rom[512]; /* MODAL_CORES: the game's arg, as a rescan reorders */
  nav_id back;
} modal;
/* The tile focused on the page, -1: none */
static int tiles_focus = -1;
static Uint64 osd_until;
static Uint32 volume_event, exit_event, loaded_event, hashed_event;

static void rescan(size_t page) {
  if (page >= 1 && page <= category_count)
    apps_scan(&categories[page - 1]);
}

/* state_file(""): the page's name; "-ignored": the apps whose failures are
 * not shown, a line each */
static bool ignored(const char *key) {
  FILE *f = state_file("-ignored", "r");
  char line[sizeof(failure.key) + 2];
  bool found = false;
  while (f != NULL && !found && fgets(line, sizeof(line), f) != NULL) {
    line[strcspn(line, "\n")] = '\0';
    found = strcmp(line, key) == 0;
  }
  if (f != NULL)
    fclose(f);
  return found;
}

static void ignore(const char *key) {
  FILE *f = state_file("-ignored", "a");
  if (f != NULL) {
    fprintf(f, "%s\n", key);
    fclose(f);
  }
}

static size_t load_page(void) {
  char name[64];
  FILE *f = state_file("", "r");
  if (f == NULL)
    return 1;
  const bool read = fgets(name, sizeof(name), f) != NULL;
  fclose(f);
  name[strcspn(name, "\n")] = '\0';
  /* Nothing runs at start */
  for (size_t i = 1; read && i < PAGES; i++)
    if (strcmp(name, page_name(i)) == 0 && apps_present(&categories[i - 1]))
      return i;
  return 1;
}

static void save_page(size_t page) {
  FILE *f = state_file("", "w");
  if (f == NULL)
    return;
  fprintf(f, "%s\n", page_name(page));
  fclose(f);
}

static void set_listening(bool on);
static int read_line(const char *path, char *out, size_t size);

/* The built-in pad, read by this thread and turned into controller events.
 * Not SDL's game controller: with a joystick open, SDL_WaitEvent polls every
 * millisecond. While the app has the screen only Menu gets through, and what
 * is pressed while Menu is held. EVIOCGRAB while the app is stopped keeps the
 * presses for the pause screen out of its queue, where it would find them on
 * SIGCONT. A held d-pad repeats.
 * ponytail: the built-in pad only, raw codes as it labels them. */
#define PAD_NAME "gpio-keys-gamepad"
enum { REPEAT_DELAY = 350, REPEAT_MS = 80 };
static int pad_fd = -1;
static SDL_atomic_t listening;

static void set_listening(bool on) { SDL_AtomicSet(&listening, on); }

static void push_button(int button, bool down) {
  SDL_Event ev = {.cbutton = {.type = down ? SDL_CONTROLLERBUTTONDOWN : SDL_CONTROLLERBUTTONUP,
                              .button = (Uint8)button}};
  SDL_PushEvent(&ev);
}

static int pad_thread(void *data) {
  (void)data;
  struct input_event e;
  bool menu = false;
  int held = -1; /* the d-pad button repeating */
  Uint64 next = 0;
  for (;;) {
    struct pollfd p = {.fd = pad_fd, .events = POLLIN};
    const int wait = held < 0 ? -1 : (int)(next - SDL_min(next, SDL_GetTicks64()));
    const int ready = poll(&p, 1, wait);
    if (ready == 0) {
      if (SDL_AtomicGet(&listening))
        push_button(held, true);
      next += REPEAT_MS;
      continue;
    }
    if (ready < 0 && errno == EINTR)
      continue;
    if (ready < 0 || read(pad_fd, &e, sizeof(e)) != sizeof(e))
      break;
    if (e.type != EV_KEY || e.value == 2)
      continue;
    if (e.code == BTN_MODE)
      menu = e.value;
    else if (!menu && !SDL_AtomicGet(&listening))
      continue;
    int button;
    switch (e.code) {
    case BTN_DPAD_UP: button = SDL_CONTROLLER_BUTTON_DPAD_UP; break;
    case BTN_DPAD_DOWN: button = SDL_CONTROLLER_BUTTON_DPAD_DOWN; break;
    case BTN_DPAD_LEFT: button = SDL_CONTROLLER_BUTTON_DPAD_LEFT; break;
    case BTN_DPAD_RIGHT: button = SDL_CONTROLLER_BUTTON_DPAD_RIGHT; break;
    case BTN_TL: button = SDL_CONTROLLER_BUTTON_LEFTSHOULDER; break;
    case BTN_TR: button = SDL_CONTROLLER_BUTTON_RIGHTSHOULDER; break;
    /* SDL's game controller has no L2/R2 buttons: the paddles stand in */
    case BTN_TL2: button = SDL_CONTROLLER_BUTTON_PADDLE1; break;
    case BTN_TR2: button = SDL_CONTROLLER_BUTTON_PADDLE2; break;
    case BTN_SELECT: button = SDL_CONTROLLER_BUTTON_BACK; break;
    case BTN_START: button = SDL_CONTROLLER_BUTTON_START; break;
    case BTN_EAST: button = SDL_CONTROLLER_BUTTON_A; break;
    case BTN_SOUTH: button = SDL_CONTROLLER_BUTTON_B; break;
    case BTN_NORTH: button = SDL_CONTROLLER_BUTTON_X; break;
    case BTN_WEST: button = SDL_CONTROLLER_BUTTON_Y; break;
    case BTN_MODE: button = SDL_CONTROLLER_BUTTON_GUIDE; break;
    default: continue;
    }
    if (button <= SDL_CONTROLLER_BUTTON_DPAD_RIGHT && button >= SDL_CONTROLLER_BUTTON_DPAD_UP) {
      if (e.value) {
        held = button;
        next = SDL_GetTicks64() + REPEAT_DELAY;
      } else if (held == button) {
        held = -1;
      }
    }
    push_button(button, e.value);
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

static void drop_backdrop(void) {
  gfx_bind(GFX_BACKDROP, NULL);
  gfx_release(&backdrop);
}

/* The launcher has the screen again, at the focus it left */
static void go_home(nav *ui) {
  front = NULL;
  paused = in_settings = false;
  capture.option = -1;
  set_listening(true);
  drop_backdrop();
  SDL_ShowWindow(window);
  nav_focus(ui, home_focus);
}

static void hand_over(nav *ui, running *r) {
  if (front == NULL)
    home_focus = ui->focus;
  front = last = r;
  paused = in_settings = false;
  set_listening(false);
  grab(false);
}

/* The same app again comes back instead of starting twice (two of a game
 * would share its save) */
static void start(nav *ui, const char *label, const char *icon,
                  const char *exec, const char *arg, const char *dir) {
  running *r = run_same(exec, arg, dir);
  if (r != NULL) {
    hand_over(ui, r);
    run_resume(r, true);
    SDL_HideWindow(window);
    return;
  }
  r = run_launch(label, icon, exec, arg, dir, exit_event);
  if (r != NULL)
    hand_over(ui, r);
}

/* Paused first, so the frame blurred is the one it stays on. Menu has been
 * released (input): the app saw all of it, and nothing more until it resumes. */
static void pause_front(nav *ui) {
  run_pause(front);
  drop_backdrop();
  int w, h;
  uint8_t *rgba = capture_blurred(&w, &h);
  if (rgba != NULL && gfx_upload(&backdrop, rgba, w, h))
    gfx_bind(GFX_BACKDROP, &backdrop);
  free(rgba);
  paused = true;
  in_settings = front->in_settings;
  settings_category = front->settings_category;
  capture.option = -1;
  notice[0] = '\0';
  set_listening(true);
  grab(true);
  nav_focus(ui, in_settings ? front->settings_focus[settings_category] : front->menu_focus);
  /* kiosk-shell puts a toplevel on top when it maps: map again */
  SDL_HideWindow(window);
  SDL_ShowWindow(window);
}

static void resume_front(void) {
  paused = in_settings = false;
  set_listening(false);
  grab(false);
  SDL_HideWindow(window);
  drop_backdrop();
  run_resume(front, false);
}

/* The pause screen stays up over the app until it has exited */
static void quit_front(void) {
  paused = in_settings = false;
  set_listening(false);
  grab(false);
  run_quit(front);
}

/* "R2 + Start + A", in appctl's order */
static void combo_text(uint16_t combo, char *out, size_t size) {
  size_t n = 0;
  out[0] = '\0';
  for (int b = 0; b < APPCTL_BUTTONS; b++)
    if (combo & (1u << b))
      n += snprintf(out + n, n < size ? size - n : 0, "%s%s", n ? " + " : "",
                    appctl_button_names[b]);
  if (n == 0)
    snprintf(out, size, "None");
}

/* The appctl bit of a button as input() has it */
static int pad_bit(int button) {
  switch (button) {
  case SDL_CONTROLLER_BUTTON_GUIDE: return APPCTL_MENU;
  case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return APPCTL_L1;
  case SDL_CONTROLLER_BUTTON_PADDLE1: return APPCTL_L2;
  case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return APPCTL_R1;
  case SDL_CONTROLLER_BUTTON_PADDLE2: return APPCTL_R2;
  case SDL_CONTROLLER_BUTTON_DPAD_UP: return APPCTL_UP;
  case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return APPCTL_DOWN;
  case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return APPCTL_LEFT;
  case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return APPCTL_RIGHT;
  case SDL_CONTROLLER_BUTTON_START: return APPCTL_START;
  case SDL_CONTROLLER_BUTTON_BACK: return APPCTL_SELECT;
  case SDL_CONTROLLER_BUTTON_A: return APPCTL_A;
  case SDL_CONTROLLER_BUTTON_B: return APPCTL_B;
  case SDL_CONTROLLER_BUTTON_X: return APPCTL_X;
  case SDL_CONTROLLER_BUTTON_Y: return APPCTL_Y;
  default: return -1;
  }
}

static void set_binding(running *r, int i, uint16_t combo) {
  atomic_store(&r->shm->options[i].buttons, combo);
  run_request(r, APPCTL_OPTIONS, 0);
}

static void step_option(running *r, int i, int by) {
  appctl_option *o = &r->shm->options[i];
  const int n = SDL_min(o->count, APPCTL_VALUES);
  if (n == 0 || o->kind != APPCTL_CHOICE)
    return;
  atomic_store(&o->value, (uint8_t)((atomic_load(&o->value) % n + n + by) % n));
  run_request(r, APPCTL_OPTIONS, 0);
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
    int x = (int)r.x + MARGIN;
    if (entries[i].icon >= 0) {
      icon_draw(entries[i].icon, ICON_PX, x, (int)r.y + ((int)r.h - ICON_PX) / 2, FG);
      x += ICON_PX + MARGIN;
    }
    text_draw(ROW_TEXT, x, (int)r.y + ((int)r.h - text_height(ROW_TEXT)) / 2,
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
  if (run_same(a->exec, a->arg, a->dir) != NULL)
    rounded(r.x + r.w - 16, r.y + 8, 8, 8, ACTIVE, 4, 0);
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
  tiles_focus = -1;
  gfx_clip(view.x, view.y, view.w, view.h);
  for (size_t i = 0; i < c->count; i++) {
    const ui_rect r = {left + (i % cols) * (TILE_W + GAP),
                       view.y + GAP + (i / cols) * (TILE_H + GAP) - offset,
                       TILE_W, TILE_H};
    /* Every tile is there to move to; only those in view are drawn, and only
     * their icons are asked for */
    const unsigned flags = nav_item(ui, nav_int(i), r, view);
    nav_ring(ui, r, RADIUS + 2);
    if (flags & NAV_FOCUSED)
      tiles_focus = (int)i;
    if (r.y < view.y + view.h && r.y + r.h > view.y)
      tile(&c->apps[i], r, flags);
    if (flags & NAV_ACTIVATED)
      chosen = &c->apps[i];
  }
  gfx_noclip();
  nav_scroll_end(ui);
  return chosen;
}

/* The app's options, a row each: left/right (or A) step a choice; A records
 * a binding, X clears it, Y makes it this game's own (or the core's again) */
static int settings_focus = -1;

static bool has_settings(const running *r, int category) {
  const uint32_t n = SDL_min(atomic_load(&r->shm->option_count), APPCTL_OPTIONS_MAX);
  for (uint32_t i = 0; i < n; i++)
    if (r->shm->options[i].category == category)
      return true;
  return false;
}

/* settings_category's options */
static void settings(nav *ui, running *r, ui_rect view) {
  settings_focus = -1;
  const uint32_t n = SDL_min(atomic_load(&r->shm->option_count), APPCTL_OPTIONS_MAX);
  uint32_t rows = 0;
  for (uint32_t i = 0; i < n; i++)
    rows += r->shm->options[i].category == settings_category;
  if (rows == 0) {
    text_draw(ROW_TEXT, view.x + MARGIN * 2, view.y + MARGIN, "No settings", DIM);
    return;
  }
  const float offset = nav_scroll_begin(ui, nav_str("settings"), view, UI_VERTICAL,
                                        rows * ROW + MARGIN * 2, ROW, 0);
  gfx_clip(view.x, view.y, view.w, view.h);
  for (uint32_t i = 0, at = 0; i < n; i++) {
    if (r->shm->options[i].category != settings_category)
      continue;
    const ui_rect row = {view.x + MARGIN, view.y + MARGIN + at++ * ROW - offset,
                         view.w - MARGIN * 2, ROW - 4};
    const unsigned flags = nav_item(ui, nav_int(i), row, view);
    nav_ring(ui, row, 8);
    if (flags & NAV_FOCUSED) {
      rounded(row.x, row.y, row.w, row.h, FOCUS, 6, 0);
      settings_focus = (int)i;
    }
    /* The app wrote these: bounded and terminated here */
    const appctl_option *o = &r->shm->options[i];
    const bool binding = o->kind == APPCTL_BINDING;
    if ((flags & NAV_ACTIVATED) && binding)
      capture = (typeof(capture)){.option = (int)i, .since = SDL_GetTicks64()};
    else if (flags & NAV_ACTIVATED)
      step_option(r, (int)i, 1);
    char label[APPCTL_TEXT + 1], value[160];
    snprintf(label, sizeof(label), "%.*s", APPCTL_TEXT, o->label);
    if (binding && capture.option == (int)i && capture.combo == 0)
      snprintf(value, sizeof(value), "Press\u2026");
    else if (binding)
      combo_text(capture.option == (int)i ? capture.combo : atomic_load(&o->buttons), value, sizeof(value));
    else
      snprintf(value, sizeof(value), "\u2039 %.*s \u203a", APPCTL_TEXT,
               o->count ? o->values[atomic_load(&o->value) % SDL_min(o->count, APPCTL_VALUES)] : "");
    const int y = (int)row.y + ((int)row.h - text_height(ROW_TEXT)) / 2;
    const int vw = text_width(ROW_TEXT, value);
    const bool per_game = binding && atomic_load(&o->value);
    text_draw(ROW_TEXT, (int)row.x + MARGIN, y,
              fit(ROW_TEXT, label, row.w - vw - MARGIN * 3), per_game ? PER_GAME : FG);
    text_draw(ROW_TEXT, (int)(row.x + row.w) - MARGIN - vw, y, value,
              capture.option == (int)i ? RING_TEXT : flags & NAV_FOCUSED ? FG : DIM);
  }
  gfx_noclip();
  nav_scroll_end(ui);
}

/* Over home: a title, a line under it if any, then the choices */
static const entry *dialog_box(nav *ui, int w, int h, const char *title, const char *line2,
                           const entry *entries, size_t count) {
  fill(0, 0, w, h, SCRIM);
  const float dw = SDL_min(w - MARGIN * 4, 480), line = text_height(ROW_TEXT) + 6;
  const float lines = line2 != NULL ? 2 : 1;
  const float list = SDL_min(ROW * count + MARGIN * 2, h - MARGIN * 8 - line * lines);
  const float dh = MARGIN * 2 + line * lines + list + MARGIN;
  const float x = (w - dw) / 2, y = (h - dh) / 2;
  rounded(x, y, dw, dh, TILE, RADIUS, 0);
  rounded(x, y, dw, dh, EDGE, RADIUS, 1);
  text_draw(ROW_TEXT, (int)x + MARGIN * 2, (int)(y + MARGIN * 2), fit(ROW_TEXT, title, dw - MARGIN * 4), FG);
  if (line2 != NULL)
    text_draw(ROW_TEXT, (int)x + MARGIN * 2, (int)(y + MARGIN * 2 + line),
              fit(ROW_TEXT, line2, dw - MARGIN * 4), DIM);
  const ui_rect view = {x + MARGIN, y + MARGIN + line * lines + MARGIN, dw - MARGIN * 2, list};
  return menu(ui, entries, count, view);
}

/* After an app ended badly: what and how, then OK / Ignore */
static const entry *failure_dialog(nav *ui, int w, int h) {
  char title[96];
  snprintf(title, sizeof(title), "%s stopped", failure.label);
  return dialog_box(ui, w, h, title, failure.why, failure_entries, SDL_arraysize(failure_entries));
}

static app *picked_game(category *c) {
  for (size_t i = 0; i < c->count; i++)
    if (strcmp(c->apps[i].arg, modal.rom) == 0)
      return &c->apps[i];
  return NULL;
}

/* The game's system's cores, the one it runs with ticked */
static entry core_rows[CORES_MAX];
static const entry *core_dialog(nav *ui, int w, int h, category *c) {
  const app *a = picked_game(c);
  if (a == NULL) {
    modal.kind = MODAL_NONE;
    return NULL;
  }
  const size_t n = SDL_min(apps_core_count(c), CORES_MAX);
  for (size_t i = 0; i < n; i++) {
    core_rows[i] = (entry){.action = PICK_CORE, .icon = -1};
    snprintf(core_rows[i].label, sizeof(core_rows[i].label), "%s%s", c->cores[i].name,
             (int)i == a->core ? "  \u2713" : "");
  }
  return dialog_box(ui, w, h, a->name, "Run with", core_rows, n);
}

/* The Running page: a category of what runs, built each frame */
static app running_apps[RUNNING_MAX];
static running *running_of[RUNNING_MAX];
static category running_page = {.name = "Running", .apps = running_apps};

static void list_running(void) {
  running_page.count = 0;
  for (size_t i = 0; i < RUNNING_MAX; i++) {
    if (runs[i].pid == 0 || runs[i].quitting)
      continue;
    app *a = &running_apps[running_page.count];
    snprintf(a->name, sizeof(a->name), "%s", runs[i].label);
    snprintf(a->icon, sizeof(a->icon), "%s", runs[i].icon);
    running_of[running_page.count++] = &runs[i];
  }
}

/* Running only while something does */
static void turn_page(size_t *page, int by) {
  do
    *page = (*page + PAGES + by) % PAGES;
  while ((*page == 0 && run_count() == 0) ||
         (*page >= 1 && !apps_present(&categories[*page - 1])));
  modal.kind = MODAL_NONE;
  rescan(*page);
}

/* Reaped and forgotten; if it had the screen, home again. Ending badly
 * shows the dialog, unless the app is on the ignore list. */
static void app_exited(nav *ui, running *r, size_t *page) {
  const bool had_screen = r == front;
  if (had_screen && paused)
    grab(false);
  if (r == last)
    last = NULL;
  char label[64], key[sizeof(failure.key)], why[sizeof(failure.why)];
  snprintf(label, sizeof(label), "%s", r->label);
  /* Tabs: no field has one */
  snprintf(key, sizeof(key), "%s\t%s\t%s", r->dir, r->exec, r->arg);
  key[strcspn(key, "\n")] = '\0';
  const bool ok = run_forget(r, why, sizeof(why));
  if (had_screen)
    go_home(ui);
  if (!ok && !ignored(key)) {
    if (!failure.shown)
      failure.back = ui->focus;
    failure.shown = true;
    snprintf(failure.label, sizeof(failure.label), "%s", label);
    snprintf(failure.why, sizeof(failure.why), "%s", why);
    snprintf(failure.key, sizeof(failure.key), "%s", key);
    nav_focus(ui, 0);
  }
  if (*page == 0 && run_count() == 0)
    turn_page(page, 1);
  rescan(*page);
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
    running *r = run_find(ev->user.code);
    if (r != NULL)
      app_exited(ui, r, page);
    return true;
  }
  switch (ev->type) {
  case SDL_QUIT:
    save_page(*page);
    run_quit_all();
    return false;
  case SDL_CONTROLLERBUTTONUP:
  case SDL_CONTROLLERBUTTONDOWN:
    button = ev->cbutton.button;
    break;
  case SDL_KEYUP:
  case SDL_KEYDOWN: /* a keyboard, or the desktop */
    if (ev->key.repeat && capture.option >= 0)
      return true;
    switch (ev->key.keysym.sym) {
    case SDLK_UP: button = SDL_CONTROLLER_BUTTON_DPAD_UP; break;
    case SDLK_DOWN: button = SDL_CONTROLLER_BUTTON_DPAD_DOWN; break;
    case SDLK_LEFT: button = SDL_CONTROLLER_BUTTON_DPAD_LEFT; break;
    case SDLK_RIGHT: button = SDL_CONTROLLER_BUTTON_DPAD_RIGHT; break;
    case SDLK_RETURN: button = SDL_CONTROLLER_BUTTON_A; break;
    case SDLK_PAGEUP: button = SDL_CONTROLLER_BUTTON_LEFTSHOULDER; break;
    case SDLK_PAGEDOWN: button = SDL_CONTROLLER_BUTTON_RIGHTSHOULDER; break;
    case SDLK_ESCAPE: button = SDL_CONTROLLER_BUTTON_GUIDE; break;
    case SDLK_BACKSPACE: button = SDL_CONTROLLER_BUTTON_B; break;
    case SDLK_x: button = SDL_CONTROLLER_BUTTON_X; break;
    case SDLK_y: button = SDL_CONTROLLER_BUTTON_Y; break;
    case SDLK_1: button = SDL_CONTROLLER_BUTTON_PADDLE1; break;
    case SDLK_2: button = SDL_CONTROLLER_BUTTON_PADDLE2; break;
    case SDLK_TAB: button = SDL_CONTROLLER_BUTTON_BACK; break;
    case SDLK_SPACE: button = SDL_CONTROLLER_BUTTON_START; break;
    }
    break;
  }
  const bool down = ev->type == SDL_CONTROLLERBUTTONDOWN || ev->type == SDL_KEYDOWN;
  /* Recording a binding: every button is the combo's, Menu and B too */
  if (capture.option >= 0 && paused && in_settings) {
    const int bit = pad_bit(button);
    if (bit < 0)
      return true;
    if (down) {
      if (capture.combo == 0)
        capture.first = bit;
      capture.held |= 1u << bit;
      capture.combo |= 1u << bit;
    } else if (capture.held & (1u << bit)) {
      capture.held &= ~(1u << bit);
      if (bit == capture.first) {
        set_binding(front, capture.option, capture.combo);
        capture.option = -1;
      }
    }
    return true;
  }
  /* Menu's release, which reaches the app's queue first */
  if (!down) {
    if (button == SDL_CONTROLLER_BUTTON_GUIDE && menu_down) {
      menu_down = false;
      if (!menu_combo && front != NULL && !front->quitting && !paused)
        pause_front(ui);
    }
    return true;
  }
  /* Menu resumes on the press, pauses on the release (a key: the press); at
   * home it brings back the last app */
  if (button == SDL_CONTROLLER_BUTTON_GUIDE) {
    if (front != NULL && !front->quitting) {
      if (paused) {
        resume_front();
      } else if (ev->type == SDL_CONTROLLERBUTTONDOWN) {
        menu_down = true;
        menu_combo = false;
      } else {
        pause_front(ui);
      }
    } else if (front == NULL && last != NULL) {
      start(ui, last->label, last->icon, last->exec, last->arg, last->dir);
    }
    return true;
  }
  /* A hotkey: the app's */
  if (menu_down && button >= 0) {
    menu_combo = true;
    return true;
  }
  if (front != NULL && !paused)
    return true;
  if (button == SDL_CONTROLLER_BUTTON_B && in_settings) {
    in_settings = false;
    nav_focus(ui, front->menu_focus);
    return true;
  }
  if (button == SDL_CONTROLLER_BUTTON_B && paused) {
    resume_front();
    return true;
  }
  const bool dialog = (failure.shown || modal.kind != MODAL_NONE) && !paused;
  if (button == SDL_CONTROLLER_BUTTON_B && failure.shown && !paused) {
    failure.shown = false;
    nav_focus(ui, failure.back);
    return true;
  }
  /* B, or Start again on the menu */
  if (dialog && (button == SDL_CONTROLLER_BUTTON_B ||
                 (button == SDL_CONTROLLER_BUTTON_START && modal.kind == MODAL_MENU))) {
    modal.kind = MODAL_NONE;
    nav_focus(ui, modal.back);
    return true;
  }
  if (button == SDL_CONTROLLER_BUTTON_START && !paused && !dialog) {
    modal.kind = MODAL_MENU;
    modal.back = ui->focus;
    nav_focus(ui, 0);
    return true;
  }
  /* Select on a game: which core runs it, when there's a choice */
  if (button == SDL_CONTROLLER_BUTTON_BACK && !paused && !dialog && *page >= 1 && tiles_focus >= 0 &&
      (size_t)tiles_focus < categories[*page - 1].count &&
      apps_core_count(&categories[*page - 1]) > 1) {
    modal.kind = MODAL_CORES;
    modal.back = ui->focus;
    snprintf(modal.rom, sizeof(modal.rom), "%s", categories[*page - 1].apps[tiles_focus].arg);
    nav_focus(ui, 0);
    return true;
  }
  if (in_settings && settings_focus >= 0 && button == SDL_CONTROLLER_BUTTON_X &&
      front->shm->options[settings_focus].kind == APPCTL_BINDING) {
    set_binding(front, settings_focus, 0);
    return true;
  }
  if (in_settings && settings_focus >= 0 && button == SDL_CONTROLLER_BUTTON_Y &&
      front->shm->options[settings_focus].kind == APPCTL_BINDING) {
    appctl_option *o = &front->shm->options[settings_focus];
    atomic_store(&o->value, !atomic_load(&o->value));
    run_request(front, APPCTL_OPTIONS, 0);
    return true;
  }
  if (in_settings && settings_focus >= 0 &&
      (button == SDL_CONTROLLER_BUTTON_DPAD_LEFT || button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) {
    step_option(front, settings_focus, button == SDL_CONTROLLER_BUTTON_DPAD_LEFT ? -1 : 1);
    return true;
  }
  /* Lists go round; the grids stop at their edges */
  const bool list = paused || dialog;
  switch (button) {
  case SDL_CONTROLLER_BUTTON_DPAD_UP:
    if (!nav_move(ui, NAV_UP) && list)
      nav_wrap(ui, NAV_UP);
    break;
  case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
    if (!nav_move(ui, NAV_DOWN) && list)
      nav_wrap(ui, NAV_DOWN);
    break;
  case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
    if (!nav_move(ui, NAV_LEFT) && !paused && !dialog)
      turn_page(page, -1);
    break;
  case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
    if (!nav_move(ui, NAV_RIGHT) && !paused && !dialog)
      turn_page(page, 1);
    break;
  case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
    if (!paused && !dialog)
      turn_page(page, -1);
    break;
  case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
    if (!paused && !dialog)
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
  SDL_DetachThread(SDL_CreateThread(volume_thread, "volume", NULL));
  static nav ui = {.animate = true, .duration = 150};
  size_t page = load_page();
  rescan(page);
  status st;
  read_status(&st);
  Uint64 status_at = SDL_GetTicks64();
  /* The clock shows minutes: a frame when one passes, not every second */
  time_t shown_minute = 0;
  bool animating = true;

  for (;;) {
    SDL_Event ev;
    /* A frame's time while animating. Idle: wake once a second for the
     * clock and the status, and draw only if either changed. Under the app:
     * only events. And to hide the OSD. */
    int wait = front != NULL && !paused ? -1 : animating ? 16 : 1000;
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
    if (front != NULL && !paused) {
      animating = false;
      continue;
    }
    /* Nothing pressed: not recording after all */
    if (capture.option >= 0 && capture.combo == 0 && now - capture.since >= CAPTURE_MS) {
      capture.option = -1;
      animating = true;
    }
    /* Events (input, an icon loaded, an app gone) count as changes */
    bool dirty = animating;
    if (now - status_at >= 1000) {
      const status was = st;
      read_status(&st);
      status_at = now;
      dirty |= was.battery != st.battery || was.charging != st.charging ||
               was.wifi != st.wifi || time(NULL) / 60 != shown_minute;
    }
    if (!dirty)
      continue;
    shown_minute = time(NULL) / 60;

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
      char title[128];
      snprintf(title, sizeof(title), "%s \u00b7 %s", front->label,
               appctl_category_names[settings_category]);
      top_bar(w, in_settings ? title : front->label, &st);
      if (in_settings) {
        /* The bottom row is the hint's */
        const ui_rect list = {view.x, view.y, view.w, view.h - ROW};
        NAV_SCOPE(&ui, nav_int(settings_category)) { settings(&ui, front, list); }
        const bool on_binding = settings_focus >= 0 &&
                                front->shm->options[settings_focus].kind == APPCTL_BINDING;
        const char *hint = capture.option >= 0 ? "Hold the buttons, then let go of the first"
                           : on_binding        ? "A: record    X: clear    Y: this game"
                                               : NULL;
        if (hint != NULL)
          text_draw(ROW_TEXT, MARGIN * 2, h - MARGIN - text_height(ROW_TEXT), hint, DIM);
      } else {
        static entry items[SDL_arraysize(pause_entries)];
        size_t n = 0;
        for (size_t i = 0; i < SDL_arraysize(pause_entries); i++) {
          const int a = pause_entries[i].action;
          if (a == SETTINGS ? run_controlled(front) && has_settings(front, category_of(&pause_entries[i]))
              : a == SAVE_STATE || a == LOAD_STATE || a == RESTART ? run_controlled(front) && front->shm->slots > 0
              : run_controlled(front) || a == RESUME || a == HOME || a == QUIT)
            items[n++] = pause_entries[i];
        }
        /* The bottom row is the notice's */
        const ui_rect list = {view.x, view.y, view.w, view.h - (notice[0] ? ROW : 0)};
        NAV_SCOPE(&ui, nav_str("paused")) { chosen = menu(&ui, items, n, list); }
        if (notice[0] != '\0')
          text_draw(ROW_TEXT, MARGIN * 2, h - MARGIN - text_height(ROW_TEXT), notice, DIM);
      }
    } else {
      top_bar(w, page_name(page), &st);
      nav_set_inert(&ui, failure.shown || modal.kind != MODAL_NONE);
      NAV_SCOPE(&ui, nav_int(page)) {
        if (page == 0) {
          list_running();
          picked = tiles(&ui, &running_page, view);
        } else {
          picked = tiles(&ui, &categories[page - 1], view);
        }
      }
      /* Under a failure: it takes the input */
      nav_set_inert(&ui, failure.shown);
      if (modal.kind == MODAL_MENU)
        NAV_SCOPE(&ui, nav_str("start")) {
          chosen = dialog_box(&ui, w, h, "Menu", NULL, system_entries, system_count);
        }
      if (modal.kind == MODAL_CORES && page >= 1)
        NAV_SCOPE(&ui, nav_str("cores")) { chosen = core_dialog(&ui, w, h, &categories[page - 1]); }
      nav_set_inert(&ui, false);
      if (failure.shown)
        NAV_SCOPE(&ui, nav_str("failure")) { chosen = failure_dialog(&ui, w, h); }
    }
    animating = nav_end(&ui, RING, 2);
    if (paused) {
      front->in_settings = in_settings;
      front->settings_category = settings_category;
      *(in_settings ? &front->settings_focus[settings_category] : &front->menu_focus) = ui.focus;
    }
    gfx_end(screen);
    static bool presented;
    if (!presented) {
      /* boot-time measurements: seconds since kernel start */
      struct timespec ts;
      clock_gettime(CLOCK_BOOTTIME, &ts);
      SDL_Log("first frame at %.3f s", ts.tv_sec + ts.tv_nsec / 1e9);
      presented = true;
    }
    int result = 0;
    switch (chosen ? chosen->action : -1) {
    case RESUME: resume_front(); break;
    case RESTART:
      run_request(front, APPCTL_RESET, 0);
      resume_front();
      break;
    case SAVE_STATE:
    case LOAD_STATE:
      result = run_request(front, chosen->action == SAVE_STATE ? APPCTL_SAVE_STATE : APPCTL_LOAD_STATE, 0);
      if (result == 0)
        resume_front();
      else
        snprintf(notice, sizeof(notice), "%s failed: %s", chosen->label,
                 result == -ENOENT ? "nothing saved yet" : strerror(-result));
      break;
    case SETTINGS:
      in_settings = true;
      settings_category = category_of(chosen);
      settings_focus = -1;
      /* Where this category was left; none yet: the top */
      nav_focus(&ui, front->settings_focus[settings_category]);
      break;
    /* Paused it stays, in the background */
    case HOME: go_home(&ui); break;
    case QUIT: quit_front(); break;
    case RUN_COMMAND:
      modal.kind = MODAL_NONE;
      nav_focus(&ui, modal.back);
      start(&ui, chosen->label, "", chosen->command, "", "");
      break;
    case PICK_CORE: {
      app *a = picked_game(&categories[page - 1]);
      if (a != NULL)
        apps_set_core(&categories[page - 1], a, (int)(chosen - core_rows));
      modal.kind = MODAL_NONE;
      nav_focus(&ui, modal.back);
      break;
    }
    case IGNORE: ignore(failure.key); /* fall through */
    case DISMISS:
      failure.shown = false;
      nav_focus(&ui, failure.back);
      break;
    }
    if (picked != NULL && page == 0) {
      const running *r = running_of[picked - running_apps];
      start(&ui, r->label, r->icon, r->exec, r->arg, r->dir);
    } else if (picked != NULL) {
      start(&ui, picked->name, picked->icon, picked->exec, picked->arg, picked->dir);
    }
  }
}
