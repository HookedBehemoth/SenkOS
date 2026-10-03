/*
 * Launcher: a status bar over pages, immediate mode. Every frame is drawn
 * from the pages and the status; nav keeps focus and scrolling. A page is a
 * category of apps (apps.h) as a grid of tiles. The page shown at exit
 * (shutdown sends SIGTERM) is shown again at start.
 *
 * D-pad moves (left/right past the edge, or L1/R1, change page), A launches,
 * Start or Menu opens the quick menu (Storage, power), Select on a game picks
 * its core. Storage: Libraries adds folders on the other card to systems
 * (their games join the system's page while the card is in), and what a PC
 * gets over USB (`usb`: MTP, UMS or SMB).
 * What it launched owns the screen (Weston kiosk shell); the gamepad is read
 * straight from evdev, so input is ignored meanwhile, but for Menu: released
 * with nothing pressed along (Menu and a button are the app's hotkeys), it
 * pauses the app and shows a menu over a blur of its last frame. Home leaves
 * the app paused in the background, on the Running page. An app on the control channel (running.h, corehost)
 * also gets Save state, Load state and Settings (its options). The focus at
 * home, and in each app's pause menu, is kept meanwhile. While an app has the screen the launcher
 * sleeps: only the pad's Menu, the mixer and an app's exit wake it.
 *
 * A second window, "osd", shows the volume whenever the mixer changes; the
 * patched kiosk shell keeps it on top of everything without focus.
 *
 * Drawn with Vulkan (gfx.h); icons come off the card in the background
 * (assets.h). Runs on a desktop too: LAUNCHER_DATA=apps/launcher/testdata.
 */
#include <alsa/asoundlib.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <linux/input.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>


#include "apps.h"
#include "assets.h"
#include "capture.h"
#include "gfx.h"
#include "nav.h"
#include "pad.h"
#include "romdb.h"
#include "running.h"
#include "ui.h"
#include "wake.h"
#include "util.h"

enum { BAR = 32, BAR_TEXT = 18, ROW = 44, ROW_TEXT = 22, ICON_PX = 22, MARGIN = 12 };
enum { TILE_W = 144, TILE_H = 140, ICON = 88, TILE_TEXT = 16, LETTER = 40,
       RADIUS = 12, GAP = 12 };
enum { OSD_W = 240, OSD_H = 40, OSD_MS = 1500 };

static const gfx_color BG = {0x14, 0x14, 0x18, 0xff};
static const gfx_color FG = {0xee, 0xee, 0xee, 0xff};
static const gfx_color DIM = {0x55, 0x55, 0x5c, 0xff};
static const gfx_color FOCUS = {0x2a, 0x2e, 0x3a, 0xff};
static const gfx_color TILE = {0x1c, 0x1d, 0x23, 0xff};
static const gfx_color EDGE = {0x33, 0x34, 0x3c, 0xff};
/* Over the paused app's blurred frame */
static const gfx_color SCRIM = {0x14, 0x14, 0x18, 0xb0};
static const gfx_color LOW = {0xe0, 0x40, 0x40, 0xff};
static const gfx_color CHARGING = {0x50, 0xc8, 0x60, 0xff};
static const ui_color RING = {0x6a, 0x9c, 0xff, 0xff};
/* A binding being recorded */
static const gfx_color RING_TEXT = {0x6a, 0x9c, 0xff, 0xff};
/* The dot on a tile whose app is running */
static const gfx_color ACTIVE = {0x6a, 0x9c, 0xff, 0xff};
/* A binding this game has its own of */
static const gfx_color PER_GAME = {0x6a, 0x9c, 0xff, 0xff};

/* What a menu row does; RUN_COMMAND: sh -c its command; POWER: its command to sleepd */
enum { RUN_COMMAND, RESUME, RESTART, SAVE_STATE, LOAD_STATE, SETTINGS, HOME, QUIT, DISMISS, IGNORE, PICK_CORE,
       STORAGE, BROWSE, BROWSE_OPEN, TOGGLE_SYSTEM, POWER };
typedef struct {
  str label;
  str command;
  int action;
  int icon;     /* ICON_*, -1 for none */
  int category; /* SETTINGS: appctl's; BROWSE_OPEN: the row; TOGGLE_SYSTEM: into categories */
  bool checked; /* a check mark at the end */
} entry;

/* All of it for an app on the control channel, else Resume, Home and Quit.
 * The settings: an entry per category (named as appctl_category_names), for
 * those the app has options in. */
static const entry pause_entries[] = {
    {.label = S("Resume"), .action = RESUME, .icon = ICON_PLAY},
    {.label = S("Restart"), .action = RESTART, .icon = ICON_ROTATE_CCW},
    {.label = S("Save state"), .action = SAVE_STATE, .icon = ICON_SAVE_IN},
    {.label = S("Load state"), .action = LOAD_STATE, .icon = ICON_SAVE_OUT},
    {.label = S("System"), .action = SETTINGS, .icon = ICON_COG, .category = APPCTL_SYSTEM},
    {.label = S("Graphics"), .action = SETTINGS, .icon = ICON_IMAGE, .category = APPCTL_GRAPHICS},
    {.label = S("Audio"), .action = SETTINGS, .icon = ICON_SPEAKER, .category = APPCTL_AUDIO},
    {.label = S("Input"), .action = SETTINGS, .icon = ICON_DPAD, .category = APPCTL_INPUT},
    {.label = S("Home"), .action = HOME, .icon = ICON_HOUSE},
    {.label = S("Quit"), .action = QUIT, .icon = ICON_X},
};
/* appctl's names, measured once (main) */
static str button_names[APPCTL_BUTTONS], category_names[APPCTL_CATEGORIES];
/* sleepd does them: the launcher runs unprivileged */
static const entry system_entries[] = {
    {.label = S("Storage"), .action = STORAGE, .icon = -1},
    {.label = S("Sleep"), .command = S("echo suspend > /run/power"), .action = RUN_COMMAND, .icon = -1},
    {.label = S("Reboot"), .command = S("echo reboot > /run/power"), .action = RUN_COMMAND, .icon = -1},
    {.label = S("Power off"), .command = S("echo poweroff > /run/power"), .action = RUN_COMMAND, .icon = -1},
};
/* A request to sleepd (/run/power); the menu stays, and shows what came of it */
static void power(str request) {
  const int fd = open("/run/power", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd >= 0) {
    if (write(fd, request.p, request.n) != (ssize_t)request.n)
      fprintf(stderr, "launcher: /run/power: %s\n", strerror(errno));
    close(fd);
  }
}

/* What a PC gets over USB, by the usb script (sleepd runs it): /run/usb is
 * "on|off MODE [sd] [data]", the cards UMS takes once a PC is there. /run/ums
 * lists what it has meanwhile (the ums script). */
static STRBUF(64) usb_state;
static bool ums_on;

static bool ums_active(void) { return access("/run/ums", F_OK) == 0; }

/* Whether it changed */
static bool read_usb(void) {
  char line[64] = "";
  FILE *f = fopen("/run/usb", "r");
  if (f != NULL) {
    if (fgets(line, sizeof(line), f) == NULL)
      line[0] = '\0';
    fclose(f);
  }
  line[strcspn(line, "\n")] = '\0';
  const bool changed = !str_eq(sb_str(usb_state), str_c(line));
  sb_set(usb_state, str_c(line));
  return changed;
}

/* Word `i` of /run/usb is `word`, or any word after it for i < 0 */
static bool usb_is(int i, str word) {
  str rest = sb_str(usb_state);
  for (int k = 0; rest.n > 0; k++) {
    const str w = str_cut(&rest, ' ');
    if ((i < 0 ? k >= 2 : k == i) && str_eq(w, word))
      return true;
  }
  return false;
}

/* Libraries, then the USB settings: on, the mode, and for UMS the cards and
 * Stop while a PC has them */
enum { STORAGE_MAX = 8 };
static size_t storage_entries(entry *out) {
  static const str modes[] = {S("MTP"), S("UMS"), S("SMB")};
  static const str mode_cmds[] = {S("usb mtp\n"), S("usb ums\n"), S("usb smb\n")};
  static const str mode_words[] = {S("mtp"), S("ums"), S("smb")};
  const bool on = usb_is(0, S("on"));
  size_t n = 0;
  out[n++] = (entry){.label = S("Libraries"), .action = BROWSE, .icon = -1};
  out[n++] = (entry){.label = S("Enabled"), .command = on ? S("usb off\n") : S("usb on\n"), .action = POWER,
                     .icon = -1, .checked = on};
  for (size_t i = 0; i < COUNTOF(modes); i++)
    out[n++] = (entry){.label = modes[i], .command = mode_cmds[i], .action = POWER, .icon = -1,
                       .checked = usb_is(1, mode_words[i])};
  if (usb_is(1, S("ums"))) {
    out[n++] = (entry){.label = S("SD card"), .command = S("usb sd\n"), .action = POWER, .icon = -1,
                       .checked = usb_is(-1, S("sd"))};
    out[n++] = (entry){.label = S("Main card"), .command = S("usb data\n"), .action = POWER, .icon = -1,
                       .checked = usb_is(-1, S("data"))};
    if (ums_on)
      out[n++] = (entry){.label = S("Stop"), .command = S("ums stop\n"), .action = POWER, .icon = -1};
  }
  return n;
}
static const entry failure_entries[] = {{.label = S("OK"), .action = DISMISS, .icon = -1},
                                        {.label = S("Ignore for this app"), .action = IGNORE, .icon = -1}};
/* The core picker's rows: one per core, PICK_CORE (CORES_MAX: apps.h) */

/* Running (only while something is), then the categories */
#define PAGES (category_count + 1)

static str page_name(size_t page) {
  return page == 0 ? S("Running") : categories[page - 1].name;
}

typedef struct {
  int battery; /* percent, -1 without one */
  bool charging;
  int wifi; /* bars 0-4, -1 without a wireless interface */
} status;

static void redraw(win *w);
static win window = {.app_id = "launcher", .title = "Launcher", .w = 640, .h = 480, .configure = redraw};
/* The kiosk shell knows it by app_id and title, and keeps it on top */
static win osd = {.app_id = "launcher", .title = "osd", .w = OSD_W, .h = OSD_H};
static gfx_target *screen, *osd_screen;
static ui_context ctx;
/* On screen or under the pause screen; NULL: home */
static running *front;
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
  uint64_t since;
} capture = {.option = -1};
/* Menu is down while the app has the screen: the release pauses, unless
 * another button made it a hotkey */
static bool menu_down, menu_combo;
/* Where the focus was at home, for coming back */
static nav_id home_focus;
/* Under the pause menu: how the last action went */
static STRBUF(128) notice;
static gfx_image backdrop;
/* An app ended badly: a dialog over home until dismissed. `key` names the
 * app in the ignore list; `back` is the focus under the dialog. */
static struct {
  bool shown;
  STRBUF(96) title;
  STRBUF(96) why;
  STRBUF(1300) key;
  nav_id back;
} failure;
/* Over home: the quick menu, or Select on a game whose system has more than
 * one core: which runs it. Instead of home: Storage, Libraries, and a
 * folder's systems. */
enum { MODAL_NONE, MODAL_MENU, MODAL_CORES, MODAL_STORAGE, MODAL_BROWSE, MODAL_SYSTEMS };
static struct {
  int kind;
  STRBUF(512) rom; /* MODAL_CORES: the game's arg, as a rescan reorders */
  nav_id back;
} modal;
/* The tile focused on the page, the row in the last menu; -1: none */
static int tiles_focus = -1, menu_focus = -1;
static uint64_t osd_until;


static void rescan(size_t page) {
  if (page >= 1 && page <= category_count)
    apps_scan(&categories[page - 1]);
}

/* state_file(""): the page's name; "-ignored": the apps whose failures are
 * not shown, a line each */
static bool ignored(str key) {
  str lines = str_read(state_file("-ignored", "r"));
  const char *text = lines.p;
  bool found = false;
  while (lines.n > 0 && !found)
    found = str_eq(str_cut(&lines, '\n'), key);
  free((char *)text);
  return found;
}

static void ignore(str key) {
  FILE *f = state_file("-ignored", "a");
  if (f != NULL) {
    str_fput(f, key, S("\n"));
    fclose(f);
  }
}

static size_t load_page(void) {
  str text = str_read(state_file("", "r")), rest = text;
  const str name = str_cut(&rest, '\n');
  size_t page = 1;
  /* Nothing runs at start */
  for (size_t i = 1; page == 1 && i < PAGES; i++)
    if (str_eq(name, page_name(i)) && apps_present(&categories[i - 1]))
      page = i;
  free((char *)text.p);
  return page;
}

static void save_page(size_t page) {
  FILE *f = state_file("", "w");
  if (f == NULL)
    return;
  str_fput(f, page_name(page), S("\n"));
  fclose(f);
}

static void set_listening(bool on);

/* The built-in pad (pad.h), read by this thread and turned into UI_BUTTON
 * wakes. While the app has the screen only Menu gets through, and what is
 * pressed while Menu is held. EVIOCGRAB while the app is stopped keeps the
 * presses for the pause screen out of its queue, where it would find them on
 * SIGCONT. A held d-pad repeats. */
enum { REPEAT_DELAY = 350, REPEAT_MS = 80 };
static int pad_fd = -1;
static atomic_bool listening;

static void set_listening(bool on) { atomic_store(&listening, on); }

static void push_button(int button, bool down) { win_wake(UI_BUTTON, button << 1 | down); }

static void *pad_thread(void *data) {
  (void)data;
  struct input_event e;
  bool menu = false;
  int held = -1; /* the d-pad button repeating */
  uint64_t next = 0;
  for (;;) {
    struct pollfd p = {.fd = pad_fd, .events = POLLIN};
    const int wait = held < 0 ? -1 : (int)(next - MIN(next, ms_now()));
    const int ready = poll(&p, 1, wait);
    if (ready == 0) {
      if (atomic_load(&listening))
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
    else if (!menu && !atomic_load(&listening))
      continue;
    const int button = pad_button(e.code);
    if (button < 0)
      continue;
    if (button <= APPCTL_RIGHT && button >= APPCTL_UP) {
      if (e.value) {
        held = button;
        next = ms_now() + REPEAT_DELAY;
      } else if (held == button) {
        held = -1;
      }
    }
    push_button(button, e.value);
  }
  return NULL;
}

static void open_pad(void) {
  if ((pad_fd = pad_open(0)) >= 0)
    spawn("pad", pad_thread, NULL);
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
  if (!window.configured)
    win_map(&window);
  nav_focus(ui, home_focus);
}

static void hand_over(nav *ui, running *r) {
  if (front == NULL)
    home_focus = ui->focus;
  front = r;
  paused = in_settings = false;
  set_listening(false);
  grab(false);
}

/* The same app again comes back instead of starting twice (two of a game
 * would share its save) */
static void start(nav *ui, str label, str icon, str exec, str arg, str dir) {
  running *r = run_same(exec, arg, dir);
  if (r != NULL) {
    hand_over(ui, r);
    run_resume(r, true);
    win_unmap(&window);
    return;
  }
  r = run_launch(label, icon, exec, arg, dir);
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
  notice.n = 0;
  set_listening(true);
  grab(true);
  nav_focus(ui, in_settings ? front->settings_focus[settings_category] : front->menu_focus);
  /* kiosk-shell puts a toplevel on top when it maps: map again */
  win_map(&window);
}

static void resume_front(void) {
  paused = in_settings = false;
  set_listening(false);
  grab(false);
  win_unmap(&window);
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
static str combo_text(uint16_t combo) {
  static STRBUF(160) out;
  out.n = 0;
  for (int b = 0; b < APPCTL_BUTTONS; b++)
    if (combo & (1u << b))
      sb_add(out, out.n ? S(" + ") : S(""), button_names[b]);
  return out.n ? sb_str(out) : S("None");
}

static void set_binding(running *r, int i, uint16_t combo) {
  atomic_store(&r->shm->options[i].buttons, combo);
  run_request(r, APPCTL_OPTIONS, 0);
}

static void step_option(running *r, int i, int by) {
  appctl_option *o = &r->shm->options[i];
  const int n = MIN(o->count, APPCTL_VALUES);
  if (n == 0 || o->kind != APPCTL_CHOICE)
    return;
  atomic_store(&o->value, (uint8_t)((atomic_load(&o->value) % n + n + by) % n));
  run_request(r, APPCTL_OPTIONS, 0);
}

/* Line Out on hw:0, which weston's volume-keys module sets. Posts the level in
 * percent, 0 when muted, whenever it changes. */
static void *volume_thread(void *data) {
  (void)data;
  snd_mixer_t *mixer;
  snd_mixer_selem_id_t *id;
  snd_mixer_selem_id_alloca(&id);
  snd_mixer_selem_id_set_name(id, "Line Out");
  snd_mixer_elem_t *elem;
  long min, max;
  if (snd_mixer_open(&mixer, 0) < 0)
    return NULL;
  if (snd_mixer_attach(mixer, "hw:0") < 0 ||
      snd_mixer_selem_register(mixer, NULL, NULL) < 0 ||
      snd_mixer_load(mixer) < 0 ||
      (elem = snd_mixer_find_selem(mixer, id)) == NULL ||
      snd_mixer_selem_get_playback_volume_range(elem, &min, &max) < 0 ||
      max <= min) {
    snd_mixer_close(mixer);
    return NULL;
  }
  int last = -1;
  for (;;) {
    long raw = min;
    int on = 1;
    snd_mixer_selem_get_playback_volume(elem, SND_MIXER_SCHN_FRONT_LEFT, &raw);
    snd_mixer_selem_get_playback_switch(elem, SND_MIXER_SCHN_FRONT_LEFT, &on);
    const int percent = on ? (int)((raw - min) * 100 / (max - min)) : 0;
    if (last >= 0 && percent != last) {
      win_wake(UI_VOLUME, percent);
    }
    last = percent;
    if (snd_mixer_wait(mixer, -1) < 0 || snd_mixer_handle_events(mixer) < 0)
      break;
  }
  snd_mixer_close(mixer);
  return NULL;
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
      s->wifi = CLAMP((int)(quality * 4 / 70 + 0.5f), 0, 4);
    fclose(f);
  }
}

/* Speaker, then the level as a bar. Created on first use: a second
 * swapchain at startup delays the first frame. */
static void show_volume(int percent) {
  if (!osd.configured)
    win_map(&osd);
  if (osd_screen == NULL)
    osd_screen = gfx_target_create(&osd, true);
  if (osd_screen == NULL)
    return;
  int w, h;
  if (!gfx_begin(osd_screen, &w, &h))
    return;
  ui_fill(0, 0, w, h, (gfx_color){BG.r, BG.g, BG.b, SCRIM.a});
  ui_fill(MARGIN, OSD_H / 2 - 5, 6, 10, FG);
  /* The cone, a column a pixel */
  const float x0 = MARGIN + 6, x1 = x0 + 8, y = OSD_H / 2;
  for (float x = x0; x < x1; x++) {
    const float half = 5 + (x - x0) * 6 / (x1 - x0);
    ui_fill(x, y - half, 1, half * 2, FG);
  }
  const float bx = x1 + MARGIN, bw = OSD_W - bx - MARGIN * 1.5f;
  ui_rounded(bx, y - 3, bw, 6, DIM, 3, 0);
  ui_rounded(bx, y - 3, bw * percent / 100, 6, FG, 3, 0);
  gfx_end(osd_screen);
  osd_until = ms_now() + OSD_MS;
}

/* "[time]      [page]         [wifi bars][battery%][battery symbol]" */
static void top_bar(int width, str page, const status *s) {
  char clock[8];
  const time_t now = time(NULL);
  const str hhmm = {clock, strftime(clock, sizeof(clock), "%H:%M", localtime(&now))};
  const int y = (BAR - ui_text_height(BAR_TEXT)) / 2;
  ui_text(BAR_TEXT, MARGIN, y, hhmm, FG);
  ui_text(BAR_TEXT, (width - ui_text_width(BAR_TEXT, page)) / 2, y, page, FG);

  float x = width - MARGIN;
  if (s->battery >= 0) {
    const gfx_color level = s->charging         ? CHARGING
                            : s->battery <= 15 ? LOW
                                               : FG;
    ui_rounded(x -= 2, BAR / 2 - 3, 2, 6, FG, 1, 0);
    x -= 24;
    ui_rounded(x, BAR / 2 - 6, 24, 12, FG, 3, 1);
    ui_rounded(x + 2, BAR / 2 - 4, 20 * CLAMP(s->battery, 0, 100) / 100.0f, 8,
            level, 1.5f, 0);
    STRBUF(8) percent = {0};
    sb_fmt(percent, "%d%%", s->battery);
    x -= 6 + ui_text_width(BAR_TEXT, sb_str(percent));
    ui_text(BAR_TEXT, (int)x, y, sb_str(percent), FG);
  }
  x -= 10 + 4 * 5;
  for (int i = 0; i < 4; i++) {
    const float h = 4 + 3 * i;
    ui_rounded(x + i * 5, BAR / 2 + 7 - h, 3, h, i < s->wifi ? FG : DIM, 1, 0);
  }
  ui_fill(0, BAR, width, 1, DIM);
}

/* Returns the entry activated this frame, if any. */
static const entry *menu(nav *ui, const entry *entries, size_t count,
                         ui_rect view) {
  if (count == 0) {
    ui_text(ROW_TEXT, view.x + MARGIN * 2, view.y + MARGIN, S("Nothing here"),
              DIM);
    return NULL;
  }
  const entry *chosen = NULL;
  menu_focus = -1;
  const float offset =
      nav_scroll_begin(ui, nav_str("menu"), view, UI_VERTICAL,
                       count * ROW + MARGIN * 2, ROW, 0);
  gfx_clip(view.x, view.y, view.w, view.h);
  for (size_t i = 0; i < count; i++) {
    const ui_rect r = {view.x + MARGIN, view.y + MARGIN + i * ROW - offset,
                       view.w - MARGIN * 2, ROW - 4};
    const unsigned flags = nav_item(ui, nav_int(i), r, view);
    nav_ring(ui, r, 8);
    if (flags & NAV_FOCUSED) {
      ui_rounded(r.x, r.y, r.w, r.h, FOCUS, 6, 0);
      menu_focus = (int)i;
    }
    int x = (int)r.x + MARGIN;
    if (entries[i].icon >= 0) {
      ui_icon(entries[i].icon, ICON_PX, x, (int)r.y + ((int)r.h - ICON_PX) / 2, FG);
      x += ICON_PX + MARGIN;
    }
    ui_text(ROW_TEXT, x, (int)r.y + ((int)r.h - ui_text_height(ROW_TEXT)) / 2,
              entries[i].label, FG);
    if (entries[i].checked)
      ui_icon(ICON_CHECK, ICON_PX, (int)(r.x + r.w) - MARGIN - ICON_PX, (int)r.y + ((int)r.h - ICON_PX) / 2, FG);
    if (flags & NAV_ACTIVATED)
      chosen = &entries[i];
  }
  gfx_noclip();
  nav_scroll_end(ui);
  return chosen;
}

static void tile(const app *a, ui_rect r, unsigned flags) {
  ui_rounded(r.x, r.y, r.w, r.h, flags & NAV_FOCUSED ? FOCUS : TILE, RADIUS, 0);
  ui_rounded(r.x, r.y, r.w, r.h, EDGE, RADIUS, 1);
  if (run_same(sb_str(a->exec), sb_str(a->arg), sb_str(a->dir)) != NULL)
    ui_rounded(r.x + r.w - 16, r.y + 8, 8, 8, ACTIVE, 4, 0);
  const float ix = r.x + (r.w - ICON) / 2, iy = r.y + 12;
  const asset icon = assets_get(sb_str(a->icon));
  if (icon.slot == GFX_PLACEHOLDER) {
    /* Until it loads, or without one: the initial */
    ui_rounded(ix, iy, ICON, ICON, EDGE, RADIUS - 2, 0);
    str initial = str_head(sb_str(a->name), 1);
    while (initial.n < a->name.n && (a->name.p[initial.n] & 0xc0) == 0x80)
      initial.n++;
    ui_text(LETTER, (int)(ix + (ICON - ui_text_width(LETTER, initial)) / 2),
              (int)(iy + (ICON - ui_text_height(LETTER)) / 2), initial, DIM);
  } else {
    /* Fit, aspect kept */
    const float scale = MIN((float)ICON / icon.w, (float)ICON / icon.h);
    const float w = icon.w * scale, h = icon.h * scale;
    gfx_push((gfx_quad){.x = ix + (ICON - w) / 2, .y = iy + (ICON - h) / 2,
                        .w = w, .h = h, .u1 = 1, .v1 = 1,
                        .color = 0xffffffff, .tex = icon.slot,
                        .radius = RADIUS - 2});
  }
  int nw;
  const str name = ui_text_fit(TILE_TEXT, sb_str(a->name), r.w - MARGIN, &nw);
  ui_text(TILE_TEXT, (int)(r.x + (r.w - nw) / 2),
            (int)(iy + ICON + (r.y + r.h - iy - ICON - ui_text_height(TILE_TEXT)) / 2),
            name, FG);
}

/* A grid, scrolled by rows. Returns the app activated this frame, if any. */
static const app *tiles(nav *ui, const category *c, ui_rect view) {
  if (c->count == 0) {
    ui_text(ROW_TEXT, view.x + MARGIN * 2, view.y + MARGIN, S("Nothing here"),
              DIM);
    return NULL;
  }
  const int cols = MAX(1, (int)((view.w - GAP) / (TILE_W + GAP)));
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
  const uint32_t n = MIN(atomic_load(&r->shm->option_count), APPCTL_OPTIONS_MAX);
  for (uint32_t i = 0; i < n; i++)
    if (r->shm->options[i].category == category)
      return true;
  return false;
}

/* settings_category's options */
static void settings(nav *ui, running *r, ui_rect view) {
  settings_focus = -1;
  const uint32_t n = MIN(atomic_load(&r->shm->option_count), APPCTL_OPTIONS_MAX);
  uint32_t rows = 0;
  for (uint32_t i = 0; i < n; i++)
    rows += r->shm->options[i].category == settings_category;
  if (rows == 0) {
    ui_text(ROW_TEXT, view.x + MARGIN * 2, view.y + MARGIN, S("No settings"), DIM);
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
      ui_rounded(row.x, row.y, row.w, row.h, FOCUS, 6, 0);
      settings_focus = (int)i;
    }
    /* The app wrote these: bounded here */
    const appctl_option *o = &r->shm->options[i];
    const bool binding = o->kind == APPCTL_BINDING;
    if ((flags & NAV_ACTIVATED) && binding)
      capture = (typeof(capture)){.option = (int)i, .since = ms_now()};
    else if (flags & NAV_ACTIVATED)
      step_option(r, (int)i, 1);
    const str label = {o->label, strnlen(o->label, APPCTL_TEXT)};
    STRBUF(160) choice;
    str value;
    if (binding && capture.option == (int)i && capture.combo == 0) {
      value = S("Press\u2026");
    } else if (binding) {
      value = combo_text(capture.option == (int)i ? capture.combo : atomic_load(&o->buttons));
    } else {
      const char *v = o->count ? o->values[atomic_load(&o->value) % MIN(o->count, APPCTL_VALUES)] : "";
      sb_set(choice, S("\u2039 "), (str){v, strnlen(v, APPCTL_TEXT)}, S(" \u203a"));
      value = sb_str(choice);
    }
    const int y = (int)row.y + ((int)row.h - ui_text_height(ROW_TEXT)) / 2;
    const int vw = ui_text_width(ROW_TEXT, value);
    const bool per_game = binding && atomic_load(&o->value);
    ui_text(ROW_TEXT, (int)row.x + MARGIN, y,
              ui_text_fit(ROW_TEXT, label, row.w - vw - MARGIN * 3, NULL), per_game ? PER_GAME : FG);
    ui_text(ROW_TEXT, (int)(row.x + row.w) - MARGIN - vw, y, value,
              capture.option == (int)i ? RING_TEXT : flags & NAV_FOCUSED ? FG : DIM);
  }
  gfx_noclip();
  nav_scroll_end(ui);
}

/* Over home: a title, a line under it if any, then the choices */
static const entry *dialog_box(nav *ui, int w, int h, str title, str line2,
                           const entry *entries, size_t count) {
  ui_fill(0, 0, w, h, SCRIM);
  const float dw = MIN(w - MARGIN * 4, 480), line = ui_text_height(ROW_TEXT) + 6;
  const float lines = line2.n > 0 ? 2 : 1;
  const float list = MIN(ROW * count + MARGIN * 2, h - MARGIN * 8 - line * lines);
  const float dh = MARGIN * 2 + line * lines + list + MARGIN;
  const float x = (w - dw) / 2, y = (h - dh) / 2;
  ui_rounded(x, y, dw, dh, TILE, RADIUS, 0);
  ui_rounded(x, y, dw, dh, EDGE, RADIUS, 1);
  ui_text(ROW_TEXT, (int)x + MARGIN * 2, (int)(y + MARGIN * 2), ui_text_fit(ROW_TEXT, title, dw - MARGIN * 4, NULL), FG);
  if (line2.n > 0)
    ui_text(ROW_TEXT, (int)x + MARGIN * 2, (int)(y + MARGIN * 2 + line),
              ui_text_fit(ROW_TEXT, line2, dw - MARGIN * 4, NULL), DIM);
  const ui_rect view = {x + MARGIN, y + MARGIN + line * lines + MARGIN, dw - MARGIN * 2, list};
  return menu(ui, entries, count, view);
}

/* After an app ended badly: what and how, then OK / Ignore. No key: nothing
 * to ignore (no emulator for a game). */
static const entry *failure_dialog(nav *ui, int w, int h) {
  return dialog_box(ui, w, h, sb_str(failure.title), sb_str(failure.why), failure_entries,
                    failure.key.n > 0 ? COUNTOF(failure_entries) : 1);
}

static void show_failure(nav *ui, str title, str why, str key) {
  if (!failure.shown)
    failure.back = ui->focus;
  failure.shown = true;
  sb_set(failure.title, title);
  sb_set(failure.why, why);
  sb_set(failure.key, key);
  nav_focus(ui, 0);
}

static app *picked_game(category *c) {
  for (size_t i = 0; i < c->count; i++)
    if (str_eq(sb_str(c->apps[i].arg), sb_str(modal.rom)))
      return &c->apps[i];
  return NULL;
}

/* The game's system's cores, the one it runs with checked */
static entry core_rows[CORES_MAX];
static const entry *core_dialog(nav *ui, int w, int h, category *c) {
  const app *a = picked_game(c);
  if (a == NULL) {
    modal.kind = MODAL_NONE;
    return NULL;
  }
  const size_t n = c->core_count;
  for (size_t i = 0; i < n; i++) {
    core_rows[i] = (entry){.label = sb_str(c->cores[i].name), .action = PICK_CORE, .icon = -1, .checked = (int)i == a->core};
  }
  return dialog_box(ui, w, h, sb_str(a->name), S("Run with"), core_rows, n);
}

/* Libraries: the card's partitions (where sdcard.rules mounts them)
 * and every source at the top, browse_dir "", then a folder's folders. B goes
 * up. */
#define MEDIA "/run/media"
enum { BROWSE_MAX = 256 };
static STRBUF(256) browse_dir;
static struct {
  STRBUF(256) path;
  STRBUF(96) name;
} browse_rows[BROWSE_MAX];
static size_t browse_count;
/* MODAL_SYSTEMS: the folder, and the focus in Libraries to go back to */
static STRBUF(256) pick_path;
static nav_id pick_back;

/* A partition by its label (udev's links), else its name.
 * ponytail: udev writes a space in a label as \x20; shown as is. */
static str card_label(str dev) {
  static STRBUF(64) label;
  sb_set(label, dev);
  DIR *dir = opendir("/dev/disk/by-label");
  for (struct dirent *d; dir != NULL && (d = readdir(dir)) != NULL;) {
    char link[300], target[64];
    snprintf(link, sizeof(link), "/dev/disk/by-label/%s", d->d_name);
    const ssize_t n = readlink(link, target, sizeof(target));
    const str t = {target, n > 0 ? (size_t)n : 0};
    if (d->d_name[0] != '.' && str_eq(str_at(t, str_rfind(t, '/') + 1), dev)) {
      sb_set(label, str_c(d->d_name));
      break;
    }
  }
  if (dir != NULL)
    closedir(dir);
  return sb_str(label);
}

/* "/run/media/mmcblk1p2/roms" as "STORAGE/roms" */
static str shown(str path) {
  static STRBUF(300) out;
  const str media = S(MEDIA "/");
  if (path.n <= media.n || memcmp(path.p, media.p, media.n) != 0)
    return path;
  str rest = str_at(path, media.n);
  const str dev = str_cut(&rest, '/');
  sb_set(out, card_label(dev), rest.n > 0 ? S("/") : S(""), rest);
  return sb_str(out);
}

/* Tabs and newlines would break launcher-folders' lines */
static int is_folder(const struct dirent *d) {
  return d->d_type == DT_DIR && d->d_name[0] != '.' && strpbrk(d->d_name, "\t\n") == NULL;
}

static void browse_add(str path, str name) {
  for (size_t i = 0; i < browse_count; i++)
    if (str_eq(sb_str(browse_rows[i].path), path))
      return;
  if (browse_count < BROWSE_MAX && sb_set(browse_rows[browse_count].path, path)) {
    sb_set(browse_rows[browse_count].name, name);
    browse_count++;
  }
}

/* ponytail: BROWSE_MAX rows; a folder with more wants a virtual list */
static void browse(nav *ui, str dir) {
  sb_set(browse_dir, dir);
  browse_count = 0;
  STRBUF(256) base;
  sb_set(base, dir.n > 0 ? dir : S(MEDIA));
  struct dirent **list = NULL;
  const int n = scandir(base.p, &list, is_folder, alphasort);
  for (int i = 0; i < n; i++) {
    STRBUF(256) path;
    const str name = str_c(list[i]->d_name);
    if (sb_set(path, sb_str(base), S("/"), name))
      browse_add(sb_str(path), dir.n > 0 ? name : shown(sb_str(path)));
    free(list[i]);
  }
  free(list);
  for (size_t i = 0; dir.n == 0 && i < source_count; i++)
    browse_add(sb_str(sources[i].path), shown(sb_str(sources[i].path)));
  nav_focus(ui, 0);
}

/* Row `i` of menu() in the top-level scope `scope` */
static nav_id menu_row(nav_id scope, size_t i) {
  return nav_child(nav_child(scope, nav_str("menu")), nav_int(i));
}

/* To the folder above, on the one we were in; the card's top is the top's */
static void browse_up(nav *ui) {
  STRBUF(256) from, parent;
  sb_set(from, sb_str(browse_dir));
  str up = str_head(sb_str(from), str_rfind(sb_str(from), '/'));
  sb_set(parent, str_eq(up, S(MEDIA)) ? S("") : up);
  browse(ui, sb_str(parent));
  for (size_t i = 0; i < browse_count; i++)
    if (str_eq(sb_str(browse_rows[i].path), sb_str(from)))
      nav_focus(ui, menu_row(nav_str(browse_dir.p), i));
}

/* The systems' rows, on the system the folder's name says if any */
static void pick_systems(nav *ui, str path) {
  sb_set(pick_path, path);
  pick_back = ui->focus;
  modal.kind = MODAL_SYSTEMS;
  const int guess = apps_guess(path);
  size_t row = 0;
  for (int i = 0; i < guess; i++)
    row += categories[i].roms.n > 0;
  nav_focus(ui, guess >= 0 ? menu_row(nav_str("systems"), row) : 0);
}

/* Instead of home: Storage; a folder's rows, each with the systems it's a
 * source of; or that folder's systems, its own checked */
static const entry *folders_screen(nav *ui, int w, int h, ui_rect view, const status *st) {
  static entry rows[BROWSE_MAX]; /* Storage's STORAGE_MAX too */
  static STRBUF(200) labels[BROWSE_MAX];
  const ui_rect list = {view.x, view.y, view.w, view.h - ROW};
  const entry *chosen = NULL;
  str hint;
  if (modal.kind == MODAL_STORAGE) {
    top_bar(w, S("Storage"), st);
    const size_t n = storage_entries(rows);
    NAV_SCOPE(ui, nav_str("storage")) { chosen = menu(ui, rows, n, list); }
    /* Windows doesn't find the share by itself */
    hint = usb_is(1, S("smb")) ? S("A: choose    B: back    On a PC: \\\\10.55.0.1, user player")
                               : S("A: choose    B: back");
  } else if (modal.kind == MODAL_BROWSE) {
    top_bar(w, browse_dir.n > 0 ? shown(sb_str(browse_dir)) : S("Libraries"), st);
    for (size_t i = 0; i < browse_count; i++) {
      sb_set(labels[i], sb_str(browse_rows[i].name));
      bool first = true;
      for (size_t k = 0; k < source_count; k++)
        if (str_eq(sb_str(sources[k].path), sb_str(browse_rows[i].path))) {
          sb_add(labels[i], first ? S("  \u00b7  ") : S(", "), categories[sources[k].category].name);
          first = false;
        }
      rows[i] = (entry){.label = sb_str(labels[i]), .action = BROWSE_OPEN, .icon = -1, .category = (int)i};
    }
    NAV_SCOPE(ui, nav_str(browse_dir.p)) { chosen = menu(ui, rows, browse_count, list); }
    hint = browse_count > 0 ? S("A: open    X: systems    B: back") : S("B: back");
  } else {
    top_bar(w, shown(sb_str(pick_path)), st);
    size_t n = 0;
    for (size_t i = 0; i < category_count && n < BROWSE_MAX; i++) {
      if (categories[i].roms.n == 0)
        continue;
      rows[n++] = (entry){.label = categories[i].name, .action = TOGGLE_SYSTEM, .icon = -1,
                          .category = (int)i, .checked = sources_has(sb_str(pick_path), i)};
    }
    NAV_SCOPE(ui, nav_str("systems")) { chosen = menu(ui, rows, n, list); }
    hint = S("A: add or take out    B: done");
  }
  ui_text(ROW_TEXT, MARGIN * 2, h - MARGIN - ui_text_height(ROW_TEXT), hint, DIM);
  return chosen;
}

/* The Running page: a category of what runs, built each frame */
static app running_apps[RUNNING_MAX];
static running *running_of[RUNNING_MAX];
static category running_page = {.name = S("Running"), .apps = running_apps};

static void list_running(void) {
  running_page.count = 0;
  for (size_t i = 0; i < RUNNING_MAX; i++) {
    if (runs[i].pid == 0 || runs[i].quitting)
      continue;
    app *a = &running_apps[running_page.count];
    sb_set(a->name, sb_str(runs[i].label));
    sb_set(a->icon, sb_str(runs[i].icon));
    running_of[running_page.count++] = &runs[i];
  }
}

/* Running only while something does. Storage stays open. */
static void turn_page(size_t *page, int by) {
  do
    *page = (*page + PAGES + by) % PAGES;
  while ((*page == 0 && run_count() == 0) ||
         (*page >= 1 && !apps_present(&categories[*page - 1])));
  if (modal.kind < MODAL_STORAGE)
    modal.kind = MODAL_NONE;
  rescan(*page);
}

/* Reaped and forgotten; if it had the screen, home again. Ending badly
 * shows the dialog, unless the app is on the ignore list. */
static void app_exited(nav *ui, running *r, size_t *page) {
  const bool had_screen = r == front;
  if (had_screen && paused)
    grab(false);
  STRBUF(sizeof(r->label.p)) label;
  STRBUF(sizeof(failure.key.p)) key;
  sb_set(label, sb_str(r->label));
  /* Tabs: no field has one. One line, for the ignore list. */
  sb_set(key, sb_str(r->dir), S("\t"), sb_str(r->exec), S("\t"), sb_str(r->arg));
  key.p[key.n = str_find(sb_str(key), '\n')] = '\0';
  str why;
  const bool ok = run_forget(r, &why);
  if (had_screen)
    go_home(ui);
  if (!ok && !ignored(sb_str(key))) {
    STRBUF(96) title;
    sb_set(title, sb_str(label), S(" stopped"));
    show_failure(ui, sb_str(title), why, sb_str(key));
  }
  if (*page == 0 && run_count() == 0)
    turn_page(page, 1);
  rescan(*page);
}

/* A frame is all it wants */
static void redraw(win *w) {
  (void)w;
  win_wake(UI_LOADED, 0);
}

static void key(unsigned code, bool down) {
  int button = -1;
  switch (code) {
  case KEY_UP: button = APPCTL_UP; break;
  case KEY_DOWN: button = APPCTL_DOWN; break;
  case KEY_LEFT: button = APPCTL_LEFT; break;
  case KEY_RIGHT: button = APPCTL_RIGHT; break;
  case KEY_ENTER: button = APPCTL_A; break;
  case KEY_PAGEUP: button = APPCTL_L1; break;
  case KEY_PAGEDOWN: button = APPCTL_R1; break;
  case KEY_ESC: button = APPCTL_MENU; break;
  case KEY_BACKSPACE: button = APPCTL_B; break;
  case KEY_X: button = APPCTL_X; break;
  case KEY_Y: button = APPCTL_Y; break;
  case KEY_1: button = APPCTL_L2; break;
  case KEY_2: button = APPCTL_R2; break;
  case KEY_TAB: button = APPCTL_SELECT; break;
  case KEY_SPACE: button = APPCTL_START; break;
  }
  if (button >= 0)
    win_wake(UI_KEY, button << 1 | down);
}

/* False: quit */
static bool input(nav *ui, int why, int value, size_t *page) {
  /* APPCTL_*, from the pad or a key */
  int button = -1;
  bool down = false, pad = false;
  switch (why) {
  case WIN_QUIT:
    save_page(*page);
    run_quit_all();
    return false;
  case UI_VOLUME: show_volume(value); return true;
  case UI_HASHED: rescan(*page); return true;
  case UI_EXITED: {
    running *r = run_find(value);
    if (r != NULL)
      app_exited(ui, r, page);
    return true;
  }
  case UI_BUTTON:
  case UI_KEY:
    button = value >> 1;
    down = value & 1;
    pad = why == UI_BUTTON;
    break;
  default: return true; /* UI_LOADED */
  }
  /* Recording a binding: every button is the combo's, Menu and B too */
  if (capture.option >= 0 && paused && in_settings) {
    const int bit = button;
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
    if (button == APPCTL_MENU && menu_down) {
      menu_down = false;
      if (!menu_combo && front != NULL && !front->quitting && !paused)
        pause_front(ui);
    }
    return true;
  }
  /* Menu resumes on the press, pauses on the release (a key: the press); at
   * home it brings back the last app */
  if (button == APPCTL_MENU) {
    if (front != NULL && !front->quitting) {
      if (paused) {
        resume_front();
      } else if (pad) {
        menu_down = true;
        menu_combo = false;
      } else {
        pause_front(ui);
      }
      return true;
    }
    /* At home: the quick menu, as Start */
    button = APPCTL_START;
  }
  /* A hotkey: the app's */
  if (menu_down && button >= 0) {
    menu_combo = true;
    return true;
  }
  if (front != NULL && !paused)
    return true;
  if (button == APPCTL_B && in_settings) {
    in_settings = false;
    nav_focus(ui, front->menu_focus);
    return true;
  }
  if (button == APPCTL_B && paused) {
    resume_front();
    return true;
  }
  const bool dialog = (failure.shown || modal.kind != MODAL_NONE) && !paused;
  if (button == APPCTL_B && failure.shown && !paused) {
    failure.shown = false;
    nav_focus(ui, failure.back);
    return true;
  }
  if (button == APPCTL_B && modal.kind == MODAL_SYSTEMS) {
    modal.kind = MODAL_BROWSE;
    nav_focus(ui, pick_back);
    return true;
  }
  if (button == APPCTL_B && modal.kind == MODAL_BROWSE && browse_dir.n > 0) {
    browse_up(ui);
    return true;
  }
  if (button == APPCTL_B && modal.kind == MODAL_BROWSE) {
    modal.kind = MODAL_STORAGE;
    nav_focus(ui, menu_row(nav_str("storage"), 0));
    return true;
  }
  if (button == APPCTL_X && modal.kind == MODAL_BROWSE && menu_focus >= 0 &&
      (size_t)menu_focus < browse_count) {
    pick_systems(ui, sb_str(browse_rows[menu_focus].path));
    return true;
  }
  /* B, or Start again on the menu */
  if (dialog && (button == APPCTL_B ||
                 (button == APPCTL_START && modal.kind == MODAL_MENU))) {
    modal.kind = MODAL_NONE;
    nav_focus(ui, modal.back);
    return true;
  }
  if (button == APPCTL_START && !paused && !dialog) {
    modal.kind = MODAL_MENU;
    modal.back = ui->focus;
    nav_focus(ui, 0);
    return true;
  }
  /* Select on a game: which core runs it, when there's a choice */
  if (button == APPCTL_SELECT && !paused && !dialog && *page >= 1 && tiles_focus >= 0 &&
      (size_t)tiles_focus < categories[*page - 1].count &&
      categories[*page - 1].core_count > 1) {
    modal.kind = MODAL_CORES;
    modal.back = ui->focus;
    sb_set(modal.rom, sb_str(categories[*page - 1].apps[tiles_focus].arg));
    nav_focus(ui, 0);
    return true;
  }
  if (in_settings && settings_focus >= 0 && button == APPCTL_X &&
      front->shm->options[settings_focus].kind == APPCTL_BINDING) {
    set_binding(front, settings_focus, 0);
    return true;
  }
  if (in_settings && settings_focus >= 0 && button == APPCTL_Y &&
      front->shm->options[settings_focus].kind == APPCTL_BINDING) {
    appctl_option *o = &front->shm->options[settings_focus];
    atomic_store(&o->value, !atomic_load(&o->value));
    run_request(front, APPCTL_OPTIONS, 0);
    return true;
  }
  if (in_settings && settings_focus >= 0 &&
      (button == APPCTL_LEFT || button == APPCTL_RIGHT)) {
    step_option(front, settings_focus, button == APPCTL_LEFT ? -1 : 1);
    return true;
  }
  /* Lists go round; the grids stop at their edges */
  const bool list = paused || dialog;
  switch (button) {
  case APPCTL_UP:
    if (!nav_move(ui, NAV_UP) && list)
      nav_wrap(ui, NAV_UP);
    break;
  case APPCTL_DOWN:
    if (!nav_move(ui, NAV_DOWN) && list)
      nav_wrap(ui, NAV_DOWN);
    break;
  case APPCTL_LEFT:
    if (!nav_move(ui, NAV_LEFT) && !paused && !dialog)
      turn_page(page, -1);
    break;
  case APPCTL_RIGHT:
    if (!nav_move(ui, NAV_RIGHT) && !paused && !dialog)
      turn_page(page, 1);
    break;
  case APPCTL_L1:
    if (!paused && !dialog)
      turn_page(page, -1);
    break;
  case APPCTL_R1:
    if (!paused && !dialog)
      turn_page(page, 1);
    break;
  /* A by label: BTN_EAST (pad_thread) */
  case APPCTL_A: nav_activate(ui); break;
  }
  return true;
}

int main(void) {
  if (!win_connect(key)) {
    fprintf(stderr, "launcher: no Wayland display\n");
    return 1;
  }
  win_map(&window);
  if (!gfx_init(&window) || (screen = gfx_target_create(&window, false)) == NULL ||
      !ui_init("sans-serif")) {
    fprintf(stderr, "launcher: can't draw\n");
    return 1;
  }
  assets_init();
  romdb_init();
  for (int b = 0; b < APPCTL_BUTTONS; b++)
    button_names[b] = str_c(appctl_button_names[b]);
  for (int c = 0; c < APPCTL_CATEGORIES; c++)
    category_names[c] = str_c(appctl_category_names[c]);
  set_listening(true);
  open_pad();
  spawn("volume", volume_thread, NULL);
  static nav ui = {.animate = true, .duration = 150};
  apps_init();
  sources_load();
  uint64_t present = sources_present();
  size_t page = load_page();
  rescan(page);
  status st;
  read_status(&st);
  uint64_t status_at = ms_now();
  read_usb();
  /* Started again for the main card's USB storage: on the way back */
  if ((ums_on = ums_active())) {
    modal.kind = MODAL_STORAGE;
    entry rows[STORAGE_MAX];
    nav_focus(&ui, menu_row(nav_str("storage"), storage_entries(rows) - 1));
  }
  /* The clock shows minutes: a frame when one passes, not every second */
  time_t shown_minute = 0;
  bool animating = true;

  for (;;) {
    /* A frame's time while animating. Idle: wake once a second for the
     * clock and the status, and draw only if either changed. Under the app:
     * only events. And to hide the OSD. */
    int wait = front != NULL && !paused ? -1 : animating ? 16 : 1000;
    if (osd_until != 0) {
      const int left = (int)(osd_until - MIN(osd_until, ms_now()));
      if (wait < 0 || left < wait)
        wait = left;
    }
    if (!win_wait(NULL, 0, wait))
      return 1;
    for (int why, value; win_woken(&why, &value); animating = true)
      if (!input(&ui, why, value, &page))
        return 0;
    const uint64_t now = ms_now();
    if (osd_until != 0 && now >= osd_until) {
      win_unmap(&osd);
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
      const bool ums_was = ums_on;
      ums_on = ums_active();
      dirty |= was.battery != st.battery || was.charging != st.charging ||
               was.wifi != st.wifi || time(NULL) / 60 != shown_minute || ums_on != ums_was;
      dirty |= read_usb();
      /* A card came or went, or a source was added: the page again, or
       * another if it has nothing left.
       * ponytail: polled with the status; inotify on /run/media if a second is too slow. */
      const uint64_t now_present = sources_present();
      if (now_present != present) {
        present = now_present;
        if (page >= 1 && !apps_present(&categories[page - 1]))
          turn_page(&page, 1);
        rescan(page);
        dirty = true;
      }
    }
    if (!dirty)
      continue;
    shown_minute = time(NULL) / 60;

    assets_frame();
    int w, h;
    if (!gfx_begin(screen, &w, &h))
      continue;
    ui_fill(0, 0, w, h, BG);
    if (paused && backdrop.image != VK_NULL_HANDLE) {
      gfx_push((gfx_quad){.w = w, .h = h, .u1 = 1, .v1 = 1,
                          .color = 0xffffffff, .tex = GFX_BACKDROP});
      ui_fill(0, 0, w, h, SCRIM);
    }
    nav_begin(&ui, &ctx, now);
    const entry *chosen = NULL;
    const app *picked = NULL;
    const ui_rect view = {0, BAR + 1, w, h - BAR - 1};
    if (!paused && modal.kind >= MODAL_STORAGE) {
      chosen = folders_screen(&ui, w, h, view, &st);
    } else if (paused) {
      STRBUF(128) title;
      sb_set(title, sb_str(front->label), S(" \u00b7 "), category_names[settings_category]);
      top_bar(w, in_settings ? sb_str(title) : sb_str(front->label), &st);
      if (in_settings) {
        /* The bottom row is the hint's */
        const ui_rect list = {view.x, view.y, view.w, view.h - ROW};
        NAV_SCOPE(&ui, nav_int(settings_category)) { settings(&ui, front, list); }
        const bool on_binding = settings_focus >= 0 &&
                                front->shm->options[settings_focus].kind == APPCTL_BINDING;
        const str hint = capture.option >= 0 ? S("Hold the buttons, then let go of the first")
                          : on_binding        ? S("A: record    X: clear    Y: this game")
                                              : S("");
        if (hint.n > 0)
          ui_text(ROW_TEXT, MARGIN * 2, h - MARGIN - ui_text_height(ROW_TEXT), hint, DIM);
      } else {
        static entry items[COUNTOF(pause_entries)];
        size_t n = 0;
        for (size_t i = 0; i < COUNTOF(pause_entries); i++) {
          const int a = pause_entries[i].action;
          if (a == SETTINGS ? run_controlled(front) && has_settings(front, pause_entries[i].category)
              : a == SAVE_STATE || a == LOAD_STATE || a == RESTART ? run_controlled(front) && front->shm->slots > 0
              : run_controlled(front) || a == RESUME || a == HOME || a == QUIT)
            items[n++] = pause_entries[i];
        }
        /* The bottom row is the notice's */
        const ui_rect list = {view.x, view.y, view.w, view.h - (notice.n ? ROW : 0)};
        NAV_SCOPE(&ui, nav_str("paused")) { chosen = menu(&ui, items, n, list); }
        if (notice.n > 0)
          ui_text(ROW_TEXT, MARGIN * 2, h - MARGIN - ui_text_height(ROW_TEXT), sb_str(notice), DIM);
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
          chosen = dialog_box(&ui, w, h, S("Menu"), S(""), system_entries, COUNTOF(system_entries));
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
      fprintf(stderr, "first frame at %.3f s\n", ts.tv_sec + ts.tv_nsec / 1e9);
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
        sb_set(notice, chosen->label, S(" failed: "),
               result == -ENOENT ? S("nothing saved yet") : str_c(strerror(-result)));
      break;
    case SETTINGS:
      in_settings = true;
      settings_category = chosen->category;
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
      start(&ui, chosen->label, S(""), chosen->command, S(""), S(""));
      break;
    case PICK_CORE: {
      app *a = picked_game(&categories[page - 1]);
      if (a != NULL)
        apps_set_core(&categories[page - 1], a, (int)(chosen - core_rows));
      modal.kind = MODAL_NONE;
      nav_focus(&ui, modal.back);
      break;
    }
    case POWER: power(chosen->command); break;
    case STORAGE:
      modal.kind = MODAL_STORAGE;
      nav_focus(&ui, 0);
      break;
    case BROWSE:
      modal.kind = MODAL_BROWSE;
      browse(&ui, S(""));
      break;
    case BROWSE_OPEN: {
      STRBUF(256) dir;
      sb_set(dir, sb_str(browse_rows[chosen->category].path));
      browse(&ui, sb_str(dir));
      break;
    }
    /* Games join or leave once the status is next read */
    case TOGGLE_SYSTEM: sources_toggle(sb_str(pick_path), (size_t)chosen->category); break;
    case IGNORE: ignore(sb_str(failure.key)); /* fall through */
    case DISMISS:
      failure.shown = false;
      nav_focus(&ui, failure.back);
      break;
    }
    if (picked != NULL && page == 0) {
      const running *r = running_of[picked - running_apps];
      start(&ui, sb_str(r->label), sb_str(r->icon), sb_str(r->exec), sb_str(r->arg), sb_str(r->dir));
    } else if (picked != NULL && picked->exec.n == 0) {
      STRBUF(96) why;
      const category *c = &categories[page - 1];
      sb_set(why, str_eq(c->folder, S("videos")) ? S("No player for ") : S("No emulator for "), c->name);
      show_failure(&ui, sb_str(picked->name), sb_str(why), S(""));
    } else if (picked != NULL) {
      start(&ui, sb_str(picked->name), sb_str(picked->icon), sb_str(picked->exec),
            sb_str(picked->arg), sb_str(picked->dir));
    }
  }
}
