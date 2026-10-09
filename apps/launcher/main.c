/*
 * Launcher: a status bar over pages, immediate mode. Every frame is drawn
 * from the pages and the status; nav keeps focus and scrolling. A page is a
 * category of apps (apps.h) as a grid of tiles. The page shown at exit
 * (shutdown sends SIGTERM) is shown again at start.
 *
 * D-pad moves (left/right past the edge, or L1/R1, change page), A launches,
 * Start or Menu opens the quick menu (Store, Settings, Power, Sleep, Reboot, Power off), Select on a game picks
 * its core and its title and box art (any of its pack's games), on a port its readme or its page in the
 * store. The focused tile's or row's text scrolls when it doesn't fit. Power: home's clocks and the backlight
 * (power.h). Settings: Storage, Wi-Fi, Time, Updates, Licenses. Wi-Fi: on or off, the networks
 * (wifi.h), a password typed on the on-screen keyboard (keyboard.h). Time (clock.h): the zone,
 * guessed or picked; NTP, or the date and time by hand. Updates: a newer system
 * from the server, downloaded into the other slot by downloadd (updates.h); once up, the
 * launcher tells sysd ("up"), which commits a slot on trial and turns Wi-Fi on if it was. "reload" on the FIFO (downloadd,
 * a port came or went) reads the page again. Storage: Libraries adds folders on the other card to systems
 * (their games join the system's page while the card is in), and what a PC
 * gets over USB (`usb`: MTP or SMB). Licenses: what the image holds,
 * and each one's texts to scroll through (licenses.h).
 * What it launched owns the screen (Weston kiosk shell); the gamepad is read
 * straight from evdev, so input is ignored meanwhile, but for Menu: released
 * with nothing pressed along (Menu and a button are the app's hotkeys), it
 * pauses the app and shows a menu over a blur of its last frame. Home leaves
 * the app paused in the background, on the Running page. An app on the control channel (running.h, corehost)
 * also gets Save state, Load state and Settings (its options). The focus at
 * home, and in each app's pause menu, is kept meanwhile. While an app has the screen the launcher
 * sleeps: only the pad's Menu, the volume keys and an app's exit wake it (and power calibration's
 * steps). Every app's pause menu has Power (power.h): its clocks, and the backlight.
 *
 * The volume keys are the launcher's (volume_thread); a second window, "osd",
 * shows the level, and the patched kiosk shell keeps it on top without focus.
 * A third, "perf", is the performance overlay (perf.h), top left; Settings > Debug and every
 * app's pause menu (Debug) toggle it and its metrics.
 *
 * `launcher launch PATH` (a port's folder, a ROM, an executable) starts that
 * app as A on its tile does: through /run/launcher (a desktop: $XDG_RUNTIME_DIR/launcher), a FIFO the
 * running launcher reads. Only at home: the app owns the screen and the pad
 * until it exits or Menu pauses it.
 *
 * Drawn with Vulkan (gfx.h); icons come off the card in the background
 * (assets.h). Runs on a desktop too: LAUNCHER_DATA=apps/launcher/testdata.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <linux/input.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sys/inotify.h>
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
#include "cfg.h"
#include "clock.h"
#include "gfx.h"
#include "keyboard.h"
#include "keys.h"
#include "licenses.h"
#include "nav.h"
#include "pad.h"
#include "perf.h"
#include "power.h"
#include "romdb.h"
#include "running.h"
#include "sysd.h"
#include "ui.h"
#include "updates.h"
#include "wake.h"
#include "util.h"
#include "watch.h"
#include "wifi.h"

enum { BAR = 32, BAR_TEXT = 18, BAR_ICON = 18, ROW = 44, ROW_TEXT = 22, ICON_PX = 22, MARGIN = 12 };
/* A tree's step in: a row's icon under its parent's label */
enum { TREE = ICON_PX + MARGIN };
enum { TILE_W = 144, TILE_H = 160, ICON = 128, TILE_TEXT = 16, LETTER = 40,
       RADIUS = 12, GAP = 8 };
enum { OSD_W = 320, OSD_H = 40, OSD_MS = 1500, RESULT_MS = 5000 };
/* The performance overlay: a line a metric */
enum { PERF_W = 200, PERF_TEXT = 14, PERF_LINE = 17 };
enum { LICENSE_TEXT = 16 };

/* portrun's state for each port (docs/ports.md) */
#define PORT_STATE "/data/OS/saves/ports"

/* The system's palette (ui.h) */
#define BG ui_pal.bg
#define FG ui_pal.fg
#define DIM ui_pal.faint
#define FOCUS ui_pal.focus
#define TILE ui_pal.tile
#define EDGE ui_pal.edge
/* Over the paused app's blurred frame */
#define SCRIM ((gfx_color){BG.r, BG.g, BG.b, 0xb0})
#define LOW ui_pal.bad
#define CHARGING ui_pal.ok
#define RING ui_pal.accent
/* A binding being recorded */
#define RING_TEXT ui_pal.accent
/* The dot on a tile whose app is running */
#define ACTIVE ui_pal.accent
/* An option this game has its own of */
#define PER_GAME ui_pal.accent

/* What a menu row does; RUN_COMMAND: sh -c its command; POWER: its command to sysd */
/* HEADER: a section's title, not a row to choose */
enum { RUN_COMMAND, RESUME, RESTART, SAVE_STATE, LOAD_STATE, SETTINGS, HOME, QUIT, DISMISS, IGNORE, PICK_CORE,
       STORAGE, BROWSE, BROWSE_OPEN, TOGGLE_SYSTEM, POWER, LICENSES_OPEN, LICENSE_OPEN,
       WIFI, WIFI_PICK, UPDATES, UPDATE_CHECK, UPDATE_INSTALL, NOTHING, PORT_README, PORT_STORE,
       SYSTEM_SETTINGS, HOME_POWER, HEADER, ZONES, ZONE_AUTO, ZONE_REGION, ZONE_PICK, NTP, CLOCK, TIME, DEBUG, SSH, SSH_EVERYWHERE,
       SLEEP_LIGHT, DEEP_SLEEP, LIGHT_MODE, PERF, MATCH, MATCH_SEARCH, MATCH_PICK };
typedef struct {
  str label;
  str command;
  int action;
  int icon;     /* ICON_*, -1 for none */
  int category; /* SETTINGS: appctl's; BROWSE_OPEN, LICENSE_OPEN, ZONE_*: the row; TOGGLE_SYSTEM: into categories;
                 * PICK_CORE: the core; MATCH_PICK: into match.games, -1 for none */
  bool checked; /* a check mark at the end */
  int depth;    /* under the row above it at depth - 1: a tree's branch */
} entry;

/* All of it for an app on the control channel, else Resume, Power, Debug, Home and Quit.
 * The settings: an entry per category (named as appctl_category_names), for
 * those the app has options in; Power and Debug, the launcher's own, for every app. */
enum { POWER_PAGE = APPCTL_CATEGORIES, DEBUG_PAGE };
static const entry pause_entries[] = {
    {.label = S("Resume"), .action = RESUME, .icon = ICON_PLAY},
    {.label = S("Restart"), .action = RESTART, .icon = ICON_ROTATE_CCW},
    {.label = S("Save state"), .action = SAVE_STATE, .icon = ICON_SAVE_IN},
    {.label = S("Load state"), .action = LOAD_STATE, .icon = ICON_SAVE_OUT},
    {.label = S("System"), .action = SETTINGS, .icon = ICON_COG, .category = APPCTL_SYSTEM},
    {.label = S("Graphics"), .action = SETTINGS, .icon = ICON_IMAGE, .category = APPCTL_GRAPHICS},
    {.label = S("Audio"), .action = SETTINGS, .icon = ICON_SPEAKER, .category = APPCTL_AUDIO},
    {.label = S("Input"), .action = SETTINGS, .icon = ICON_DPAD, .category = APPCTL_INPUT},
    {.label = S("Power"), .action = SETTINGS, .icon = ICON_ZAP, .category = POWER_PAGE},
    {.label = S("Debug"), .action = SETTINGS, .icon = ICON_GAUGE, .category = DEBUG_PAGE},
    {.label = S("Home"), .action = HOME, .icon = ICON_HOUSE},
    {.label = S("Quit"), .action = QUIT, .icon = ICON_X},
};
/* appctl's names, measured once (main) */
static str button_names[APPCTL_BUTTONS], category_names[DEBUG_PAGE + 1];
/* sysd does them: the launcher runs unprivileged */
static const entry system_entries[] = {
    {.label = S("Store"), .command = S("store"), .action = RUN_COMMAND, .icon = ICON_SHOPPING_BAG},
    {.label = S("Settings"), .action = SYSTEM_SETTINGS, .icon = ICON_COG},
    {.label = S("Power"), .action = HOME_POWER, .icon = ICON_ZAP},
    {.label = S("Sleep"), .command = S("suspend"), .action = POWER, .icon = ICON_MOON},
    {.label = S("Reboot"), .command = S("reboot"), .action = POWER, .icon = ICON_ROTATE_CW},
    {.label = S("Power off"), .command = S("poweroff"), .action = POWER, .icon = ICON_POWER},
};
/* Sleep, Reboot, Power off: the power key held */
static const entry *const power_entries = system_entries + 3;
enum { POWER_ENTRIES = 3 };
/* Power's key at home (power.h): apps' are paths and commands */
#define HOME_KEY S("home")
/* What a PC gets over USB, by the usb script (sysd runs it): /run/usb is "on|off MODE" */
static STRBUF(64) usb_state;

/* Not time(): the coarse clock, a tick behind, would show the minute just past at a minute's turn
 * (watch.c's timer) */
static time_t wall_time(void) {
  struct timespec now;
  clock_gettime(CLOCK_REALTIME, &now);
  return now.tv_sec;
}

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

/* Word `i` of /run/usb is `word` */
static bool usb_is(int i, str word) {
  str rest = sb_str(usb_state);
  for (int k = 0; rest.n > 0; k++) {
    const str w = str_cut(&rest, ' ');
    if (k == i && str_eq(w, word))
      return true;
  }
  return false;
}

/* Libraries, then the USB settings as a tree: on, under it (while on) the modes */
enum { STORAGE_MAX = 4 };
static size_t storage_entries(entry *out) {
  static const str modes[] = {S("MTP"), S("SMB")};
  static const str mode_cmds[] = {S("usb mtp"), S("usb smb")};
  static const str mode_words[] = {S("mtp"), S("smb")};
  static const int mode_icons[] = {ICON_SMARTPHONE, ICON_NETWORK};
  const bool on = usb_is(0, S("on"));
  size_t n = 0;
  out[n++] = (entry){.label = S("Libraries"), .action = BROWSE, .icon = ICON_LIBRARY};
  out[n++] = (entry){.label = S("USB connection"), .command = on ? S("usb off") : S("usb on"), .action = POWER,
                     .icon = ICON_USB, .checked = on};
  /* Collapsed while off */
  for (size_t i = 0; on && i < COUNTOF(modes); i++)
    out[n++] = (entry){.label = modes[i], .command = mode_cmds[i], .action = POWER, .icon = mode_icons[i],
                       .checked = usb_is(1, mode_words[i]), .depth = 1};
  return n;
}
static const entry fail_launch_entries[] = {{.label = S("OK"), .action = DISMISS, .icon = ICON_CHECK},
                                            {.label = S("Ignore for this app"), .action = IGNORE, .icon = ICON_EYE_OFF}};
static const entry fail_sleep_entries[] = {{.label = S("Use light sleep"), .action = SLEEP_LIGHT, .icon = ICON_MOON},
                                           {.label = S("Keep deep sleep"), .action = DISMISS, .icon = ICON_CHECK}};

/* Running (only while something is), then the categories */
#define PAGES (category_count + 1)

static str page_name(size_t page) {
  return page == 0 ? S("Running") : categories[page - 1].name;
}

typedef struct {
  int battery; /* percent, -1 without one */
  bool charging;
  int wifi; /* wifi_signal: -1 off, 0 on no network, bars 1-4 */
} status;

static void redraw(win *w);
static win window = {.app_id = "launcher", .title = "Launcher", .w = 640, .h = 480, .configure = redraw};
/* The kiosk shell knows it by app_id and title, and keeps it on top */
static win osd = {.app_id = "launcher", .title = "osd", .w = OSD_W, .h = OSD_H};
/* The same, top left */
static win perf = {.app_id = "launcher", .title = "perf", .w = PERF_W, .h = PERF_METRICS * PERF_LINE + 8};
static gfx_target *screen, *osd_screen, *perf_screen;
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
/* An app ended badly, or a deep sleep didn't resume: a dialog over home until
 * dismissed. `key` names the app in the ignore list; `back` is the focus under
 * the dialog. */
enum { FAIL_LAUNCH, FAIL_SLEEP };
static struct {
  bool shown;
  int kind;
  STRBUF(96) title;
  STRBUF(640) why;
  STRBUF(1300) key;
  nav_id back;
} failure;
/* Over home: the quick menu, or Select on a game: which core runs it, its title and box art. Instead of home: Storage, Libraries, and a
 * folder's systems; Licenses, and one's texts. */
enum { MODAL_NONE, MODAL_MENU, MODAL_CORES, MODAL_PORT, MODAL_STORAGE, MODAL_BROWSE, MODAL_SYSTEMS, MODAL_LICENSES,
       MODAL_LICENSE, MODAL_WIFI, MODAL_KEYBOARD, MODAL_UPDATES, MODAL_README, MODAL_SETTINGS, MODAL_POWER,
       MODAL_ZONES, MODAL_ZONE, MODAL_CLOCK, MODAL_TIME, MODAL_DEBUG, MODAL_MATCH };
static struct {
  int kind;
  STRBUF(512) rom; /* MODAL_CORES, MODAL_MATCH: the game's arg, as a rescan reorders; MODAL_PORT, MODAL_README: its folder */
  STRBUF(64) name; /* MODAL_PORT, MODAL_README: the port's */
  nav_id back;
} modal;
/* The tile focused on the page, the row in the last menu; -1: none */
static int tiles_focus = -1, menu_focus = -1;
/* MODAL_LICENSE: the row read */
static size_t license_shown;
static uint64_t osd_until;
/* The power options, over whatever is shown (an app: paused first); `back`: the focus under */
static struct {
  bool shown;
  nav_id back;
} power_menu;


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

/* What a new app shows until its window takes the focus: a port's splash, else the app's icon,
 * over this line (none: not shown) */
static str starting;
/* The image to show for it */
static STRBUF(600) starting_image;

/* The launcher has the screen again, at the focus it left */
static void go_home(nav *ui) {
  front = NULL;
  power_use(HOME_KEY);
  starting = S("");
  paused = in_settings = false;
  capture.option = -1;
  set_listening(true);
  drop_backdrop();
  if (!window.configured)
    win_map(&window);
  nav_focus(ui, home_focus);
}

/* Its power settings' key: the ROM, else the port's folder, else the command */
static str power_key(const running *r) {
  return r->arg.n > 0 ? sb_str(r->arg) : r->dir.n > 0 ? sb_str(r->dir) : sb_str(r->exec);
}

static void hand_over(nav *ui, running *r) {
  if (front == NULL)
    home_focus = ui->focus;
  front = r;
  power_use(power_key(r));
  paused = in_settings = false;
  set_listening(false);
  grab(false);
}

/* A port that runs its [Setup] or [Patch] first: portrun hasn't left its setup.done yet */
static bool first_start(str dir) {
  STRBUF(600) path;
  sb_set(path, dir, S("/port.ini"));
  const str text = str_read(fopen(path.p, "r"));
  if (text.p == NULL)
    return false;
  bool setup = false;
  cfg_reader r = cfg_init(text);
  for (cfg_event e; (e = cfg_next(&r)) != CFG_END;)
    setup |= e == CFG_VALUE && (str_eq(r.section, S("Setup")) || str_eq(r.section, S("Patch")));
  free((char *)text.p);
  const str id = str_at(dir, str_rfind(dir, '/') + 1);
  return setup && sb_set(path, S(PORT_STATE "/"), id, S("/setup.done")) && access(path.p, F_OK) != 0;
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
  if (r == NULL)
    return;
  hand_over(ui, r);
  starting = first_start(dir) ? S("Running initial setup\u2026") : S("Loading\u2026");
  sb_set(starting_image, dir, S("/splash.png"));
  if (dir.n == 0 || access(starting_image.p, F_OK) != 0)
    sb_set(starting_image, icon);
}

/* Paused first, so the frame blurred is the one it stays on. Menu has been
 * released (input): the app saw all of it, and nothing more until it resumes. */
static void pause_front(nav *ui) {
  run_pause(front);
  power_cancel();
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
  power_menu.shown = false;
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
  s->wifi = wifi_signal();
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
  ui_fill(0, 0, w, h, SCRIM);
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

/* A line on the OSD, for `ms` */
static void show_text(str text, int ms) {
  if (!osd.configured)
    win_map(&osd);
  if (osd_screen == NULL)
    osd_screen = gfx_target_create(&osd, true);
  int w, h;
  if (osd_screen == NULL || !gfx_begin(osd_screen, &w, &h))
    return;
  ui_fill(0, 0, w, h, SCRIM);
  int tw;
  text = ui_text_fit(BAR_TEXT, text, w - MARGIN * 2, &tw);
  ui_text(BAR_TEXT, (w - tw) / 2, (h - ui_text_height(BAR_TEXT)) / 2, text, FG);
  gfx_end(osd_screen);
  osd_until = ms_now() + ms;
}

/* The overlay's lines, a box behind each; created on first use, as the OSD */
static void show_perf(void) {
  if (!perf.configured)
    win_map(&perf);
  if (perf_screen == NULL)
    perf_screen = gfx_target_create(&perf, true);
  int w, h;
  if (perf_screen == NULL || !gfx_begin(perf_screen, &w, &h))
    return;
  str lines[PERF_METRICS];
  const size_t n = perf_sample(lines);
  for (size_t i = 0; i < n; i++) {
    const int y = 4 + (int)i * PERF_LINE;
    ui_fill(0, y - 1, ui_text_width(PERF_TEXT, lines[i]) + 12, PERF_LINE, (gfx_color){0, 0, 0, 0xc0});
    ui_text(PERF_TEXT, 6, y + (PERF_LINE - ui_text_height(PERF_TEXT)) / 2 - 1, lines[i], FG);
  }
  gfx_end(perf_screen);
}

/* The overlay on or off, its rows: under it (while on) each metric */
static size_t perf_rows(entry *rows) {
  static const int icons[PERF_METRICS] = {ICON_GAUGE,   ICON_CPU,        ICON_MICROCHIP, ICON_PLAY,
                                          ICON_LIBRARY, ICON_HARD_DRIVE, ICON_SUN};
  size_t n = 0;
  rows[n++] = (entry){.label = S("Performance overlay"), .action = PERF, .icon = ICON_GAUGE, .category = -1,
                      .checked = perf_on()};
  for (int i = 0; perf_on() && i < PERF_METRICS; i++)
    rows[n++] = (entry){.label = perf_labels[i], .action = PERF, .icon = icons[i], .category = i,
                        .checked = perf_shows(i), .depth = 1};
  return n;
}

/* "[time]      [page]         [wifi][battery%][battery symbol]"; no Wi-Fi symbol while it's off */
static void top_bar(int width, str page, const status *s) {
  char clock[8];
  const time_t now = wall_time();
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
  if (s->wifi >= 0) {
    static const int arcs[] = {ICON_WIFI_OFF, ICON_WIFI_LOW, ICON_WIFI_HIGH, ICON_WIFI, ICON_WIFI};
    x -= 10 + BAR_ICON;
    ui_icon(arcs[CLAMP(s->wifi, 0, 4)], BAR_ICON, (int)x, (BAR - BAR_ICON) / 2, s->wifi > 0 ? FG : DIM);
  }
  ui_fill(0, BAR, width, 1, DIM);
}

/* The focused tile's or row's text, when it doesn't fit: held, scrolled to its end, held, back
 * to its start; MARQUEE_PASSES times, then it rests at its start (no frames while idle).
 * marquee_due: the frame it next moves in, 0 for none. */
enum { MARQUEE_PX = 30 /* a second */, MARQUEE_HOLD = 1500, MARQUEE_PASSES = 3 };
static uint64_t marquee_due, marquee_since, frame_now;
/* The text scrolling (a hash), 0 once a frame had none: focused again, it starts over */
static uint64_t marquee_of;
static bool marquee_drawn;

/* `text` from x on, in `room`; clipped to that and `view`, the list's clip, which is put back */
static void marquee(int px, float x, int y, float room, str text, gfx_color color, ui_rect view) {
  /* Another text: from its start */
  uint64_t key = 1469598103934665603u;
  for (size_t i = 0; i < text.n; i++)
    key = (key ^ (uint8_t)text.p[i]) * 1099511628211u;
  if (key != marquee_of) {
    marquee_of = key;
    marquee_since = frame_now;
  }
  marquee_drawn = true;
  const int over = ui_text_width(px, text) - (int)room;
  const uint64_t scroll = (uint64_t)over * 1000 / MARQUEE_PX, cycle = MARQUEE_HOLD * 2 + scroll;
  const uint64_t t = (frame_now - marquee_since) % cycle;
  const bool done = (frame_now - marquee_since) / cycle >= MARQUEE_PASSES;
  int offset = 0;
  uint64_t next = MARQUEE_HOLD - MIN(t, MARQUEE_HOLD);
  if (t >= MARQUEE_HOLD + scroll) {
    offset = over;
    next = cycle - t;
  } else if (t >= MARQUEE_HOLD) {
    offset = (int)((t - MARQUEE_HOLD) * MARQUEE_PX / 1000);
    next = 1000 / MARQUEE_PX;
  }
  if (done)
    offset = 0;
  else
    marquee_due = frame_now + MAX(next, 1);
  const float x0 = MAX(x, view.x), x1 = MIN(x + room, view.x + view.w);
  gfx_clip(x0, view.y, MAX(0, x1 - x0), view.h);
  ui_text(px, (int)x - offset, y, text, color);
  gfx_clip(view.x, view.y, view.w, view.h);
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
  nav_fill(ui, FOCUS, 6);
  gfx_clip(view.x, view.y, view.w, view.h);
  for (size_t i = 0; i < count; i++) {
    const int depth = entries[i].depth;
    const float top = view.y + MARGIN + i * ROW - offset;
    const ui_rect r = {view.x + MARGIN + depth * TREE, top, view.w - MARGIN * 2 - depth * TREE, ROW - 4};
    const bool shown = r.y < view.y + view.h && r.y + r.h > view.y;
    if (entries[i].action == HEADER) {
      if (shown) {
        ui_icon(entries[i].icon, ICON_PX, (int)r.x + MARGIN, (int)r.y + ((int)r.h - ICON_PX) / 2, DIM);
        ui_text(ROW_TEXT, (int)r.x + MARGIN * 2 + ICON_PX, (int)r.y + ((int)r.h - ui_text_height(ROW_TEXT)) / 2,
                entries[i].label, DIM);
      }
      continue;
    }
    const unsigned flags = nav_item(ui, nav_int(i), r, view);
    nav_ring(ui, r, 8);
    if (flags & NAV_FOCUSED)
      menu_focus = (int)i;
    if (flags & NAV_ACTIVATED)
      chosen = &entries[i];
    if (!shown)
      continue;
    /* The tree: down from the parent's icon (or the sibling above), across to this one's,
     * and on down past it to the next sibling; an ancestor's line passes by */
    const float mid = r.y + r.h / 2;
    for (int k = 1; k <= depth; k++) {
      const float lx = view.x + MARGIN + (k - 1) * TREE + MARGIN + ICON_PX / 2 - 1;
      bool on = false;
      for (size_t j = i + 1; j < count && entries[j].depth >= k && !on; j++)
        on = entries[j].depth == k;
      if (k < depth) {
        if (on)
          ui_fill(lx, r.y - 4, 2, ROW, DIM);
        continue;
      }
      const float from = i > 0 && entries[i - 1].depth == k - 1 ? r.y - 4 - (ROW - 4) / 2 + ICON_PX / 2 + 3 : r.y - 4;
      ui_fill(lx, from, 2, (on ? r.y + r.h : mid + 1) - from, DIM);
      ui_fill(lx, mid - 1, r.x - 2 - lx, 2, DIM);
    }
    int x = (int)r.x + MARGIN;
    if (entries[i].icon >= 0) {
      ui_icon(entries[i].icon, ICON_PX, x, (int)r.y + ((int)r.h - ICON_PX) / 2, FG);
      x += ICON_PX + MARGIN;
    }
    const float room = r.x + r.w - x - MARGIN - (entries[i].checked ? ICON_PX + MARGIN : 0);
    const int ty = (int)r.y + ((int)r.h - ui_text_height(ROW_TEXT)) / 2;
    if ((flags & NAV_FOCUSED) && ui_text_width(ROW_TEXT, entries[i].label) > room)
      marquee(ROW_TEXT, x, ty, room, entries[i].label, FG, view);
    else
      ui_text(ROW_TEXT, x, ty, ui_text_fit(ROW_TEXT, entries[i].label, room, NULL), FG);
    if (entries[i].checked)
      ui_icon(ICON_CHECK, ICON_PX, (int)(r.x + r.w) - MARGIN - ICON_PX, (int)r.y + ((int)r.h - ICON_PX) / 2, FG);
  }
  gfx_noclip();
  nav_scroll_end(ui);
  return chosen;
}

/* Over its background (tiles), in `view` */
static void tile(const app *a, ui_rect r, bool focused, ui_rect view) {
  if (run_same(sb_str(a->exec), sb_str(a->arg), sb_str(a->dir)) != NULL)
    ui_rounded(r.x + r.w - 16, r.y + 8, 8, 8, ACTIVE, 4, 0);
  const float ix = r.x + (r.w - ICON) / 2, iy = r.y + 8;
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
  const int ty = (int)(iy + ICON + (r.y + r.h - iy - ICON - ui_text_height(TILE_TEXT)) / 2);
  const float room = r.w - MARGIN;
  if (focused && ui_text_width(TILE_TEXT, sb_str(a->name)) > room) {
    marquee(TILE_TEXT, r.x + MARGIN / 2, ty, room, sb_str(a->name), FG, view);
    return;
  }
  int nw;
  const str name = ui_text_fit(TILE_TEXT, sb_str(a->name), room, &nw);
  ui_text(TILE_TEXT, (int)(r.x + (r.w - nw) / 2), ty, name, FG);
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
  /* The backgrounds first: the focus's fill goes over them, under the icons */
  for (size_t i = 0; i < c->count; i++) {
    const ui_rect r = {left + (i % cols) * (TILE_W + GAP), view.y + GAP + (i / cols) * (TILE_H + GAP) - offset,
                       TILE_W, TILE_H};
    if (r.y < view.y + view.h && r.y + r.h > view.y) {
      ui_rounded(r.x, r.y, r.w, r.h, TILE, RADIUS, 0);
      ui_rounded(r.x, r.y, r.w, r.h, EDGE, RADIUS, 1);
    }
  }
  nav_fill(ui, FOCUS, RADIUS);
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
      tile(&c->apps[i], r, flags & NAV_FOCUSED, view);
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
  nav_fill(ui, FOCUS, 6);
  gfx_clip(view.x, view.y, view.w, view.h);
  for (uint32_t i = 0, at = 0; i < n; i++) {
    if (r->shm->options[i].category != settings_category)
      continue;
    const ui_rect row = {view.x + MARGIN, view.y + MARGIN + at++ * ROW - offset,
                         view.w - MARGIN * 2, ROW - 4};
    const unsigned flags = nav_item(ui, nav_int(i), row, view);
    nav_ring(ui, row, 8);
    if (flags & NAV_FOCUSED)
      settings_focus = (int)i;
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
    const bool per_game = atomic_load(&o->own);
    ui_text(ROW_TEXT, (int)row.x + MARGIN, y,
              ui_text_fit(ROW_TEXT, label, row.w - vw - MARGIN * 3, NULL), per_game ? PER_GAME : FG);
    ui_text(ROW_TEXT, (int)(row.x + row.w) - MARGIN - vw, y, value,
              capture.option == (int)i ? RING_TEXT : flags & NAV_FOCUSED ? FG : DIM);
  }
  gfx_noclip();
  nav_scroll_end(ui);
}

/* Power (power.h): left/right step a row; A on calibration starts it (an app's: home has
 * none). Returns whether to start it. */
static int power_focus = -1;

static bool power_page(nav *ui, ui_rect view, bool calibration) {
  static const str labels[POWER_ROWS] = {S("Automatic calibration"), S("CPU"), S("GPU"), S("Brightness")};
  static const int icons[POWER_ROWS] = {ICON_GAUGE, ICON_CPU, ICON_MICROCHIP, ICON_SUN};
  bool calibrate = false;
  settings_focus = power_focus = -1;
  const int first = calibration ? 0 : POWER_CPU;
  const float offset =
      nav_scroll_begin(ui, nav_str("power"), view, UI_VERTICAL, (POWER_ROWS - first) * ROW + MARGIN * 2, ROW, 0);
  nav_fill(ui, FOCUS, 6);
  gfx_clip(view.x, view.y, view.w, view.h);
  for (int i = first; i < POWER_ROWS; i++) {
    const ui_rect row = {view.x + MARGIN, view.y + MARGIN + (i - first) * ROW - offset, view.w - MARGIN * 2, ROW - 4};
    const unsigned flags = nav_item(ui, nav_int(i), row, view);
    nav_ring(ui, row, 8);
    if (flags & NAV_FOCUSED)
      power_focus = i;
    if ((flags & NAV_ACTIVATED) && i == POWER_CALIBRATE)
      calibrate = true;
    else if (flags & NAV_ACTIVATED)
      power_step(i, 1);
    STRBUF(64) value;
    sb_set(value, S("\u2039 "), power_value(i), S(" \u203a"));
    const int y = (int)row.y + ((int)row.h - ui_text_height(ROW_TEXT)) / 2;
    const int vw = ui_text_width(ROW_TEXT, sb_str(value)), lx = (int)row.x + MARGIN * 2 + ICON_PX;
    ui_icon(icons[i], ICON_PX, (int)row.x + MARGIN, (int)row.y + ((int)row.h - ICON_PX) / 2, FG);
    ui_text(ROW_TEXT, lx, y, ui_text_fit(ROW_TEXT, labels[i], row.x + row.w - lx - vw - MARGIN * 2, NULL), FG);
    ui_text(ROW_TEXT, (int)(row.x + row.w) - MARGIN - vw, y, sb_str(value), flags & NAV_FOCUSED ? FG : DIM);
  }
  gfx_noclip();
  nav_scroll_end(ui);
  return calibrate;
}

/* Over home: a title and text under it, if any (wrapped, up to 8 lines), then
 * the choices */
static const entry *dialog_box(nav *ui, int w, int h, str title, str text,
                           const entry *entries, size_t count) {
  ui_fill(0, 0, w, h, SCRIM);
  const float dw = MIN(w - MARGIN * 4, 480), line = ui_text_height(ROW_TEXT) + 6;
  const float tw = dw - MARGIN * 4;
  str wrapped[8];
  size_t n = 0;
  for (str rest = text; rest.n > 0 && n < COUNTOF(wrapped); n++) {
    /* Words while they fit; the last line takes the rest, cut */
    size_t end = rest.n;
    if (n + 1 < COUNTOF(wrapped))
      for (size_t i = 0, fit = 0; i <= rest.n; i++)
        if (i == rest.n || rest.p[i] == ' ') {
          if (fit > 0 && ui_text_width(ROW_TEXT, str_head(rest, i)) > tw) {
            end = fit;
            break;
          }
          fit = i;
        }
    wrapped[n] = str_head(rest, end);
    rest = str_at(rest, end);
    while (rest.n > 0 && rest.p[0] == ' ')
      rest = str_at(rest, 1);
  }
  const float lines = (title.n > 0) + n;
  const float list = MIN(ROW * count + MARGIN * 2, h - MARGIN * 8 - line * lines);
  const float dh = MARGIN * 2 + line * lines + list + MARGIN;
  const float x = (w - dw) / 2, y = (h - dh) / 2;
  ui_rounded(x, y, dw, dh, TILE, RADIUS, 0);
  ui_rounded(x, y, dw, dh, EDGE, RADIUS, 1);
  ui_text(ROW_TEXT, (int)x + MARGIN * 2, (int)(y + MARGIN * 2), ui_text_fit(ROW_TEXT, title, dw - MARGIN * 4, NULL), FG);
  for (size_t i = 0; i < n; i++)
    ui_text(ROW_TEXT, (int)x + MARGIN * 2, (int)(y + MARGIN * 2 + line * ((title.n > 0) + i)),
              ui_text_fit(ROW_TEXT, wrapped[i], tw, NULL), DIM);
  const ui_rect view = {x + MARGIN, y + MARGIN + line * lines + MARGIN, dw - MARGIN * 2, list};
  return menu(ui, entries, count, view);
}

/* What and how, then: an app's OK / Ignore (no key: nothing to ignore, no
 * emulator for a game); a sleep's light / deep */
static const entry *failure_dialog(nav *ui, int w, int h) {
  if (failure.kind == FAIL_SLEEP)
    return dialog_box(ui, w, h, sb_str(failure.title), sb_str(failure.why), fail_sleep_entries,
                      COUNTOF(fail_sleep_entries));
  return dialog_box(ui, w, h, sb_str(failure.title), sb_str(failure.why), fail_launch_entries,
                    failure.key.n > 0 ? COUNTOF(fail_launch_entries) : 1);
}

static void show_failure(nav *ui, int kind, str title, str why, str key) {
  if (!failure.shown)
    failure.back = ui->focus;
  failure.shown = true;
  sb_set(failure.title, title);
  sb_set(failure.why, why);
  sb_set(failure.key, key);
  failure.kind = kind;
  nav_focus(ui, 0);
}

/* Row `i` of menu() in the top-level scope `scope` */
static nav_id menu_row(nav_id scope, size_t i) {
  return nav_child(nav_child(scope, nav_str("menu")), nav_int(i));
}

static app *picked_game(category *c) {
  for (size_t i = 0; i < c->count; i++)
    if (str_eq(sb_str(c->apps[i].arg), sb_str(modal.rom)))
      return &c->apps[i];
  return NULL;
}

/* Select on a game: its system's cores when there's a choice, the one it runs with checked;
 * its title and box art when its system has a pack */
static const entry *game_dialog(nav *ui, int w, int h, category *c) {
  static entry rows[CORES_MAX + 1];
  const app *a = picked_game(c);
  if (a == NULL) {
    modal.kind = MODAL_NONE;
    return NULL;
  }
  size_t n = 0;
  for (size_t i = 0; c->core_count > 1 && i < c->core_count; i++)
    rows[n++] = (entry){.label = sb_str(c->cores[i].name), .action = PICK_CORE, .icon = ICON_GAMEPAD_2,
                        .category = (int)i, .checked = (int)i == a->core};
  if (c->pack.n > 0)
    rows[n++] = (entry){.label = S("Title and box art"), .action = MATCH, .icon = ICON_IMAGE};
  return dialog_box(ui, w, h, sb_str(a->name), c->core_count > 1 ? S("Run with") : S(""), rows, n);
}

/* MODAL_MATCH: the pack's games that the game (modal.rom) may be, for its title and box art;
 * found by its file's name, or by what was typed */
enum { MATCH_MAX = 100 };
static struct {
  size_t category;
  STRBUF(128) query;
  uint32_t crcs[MATCH_MAX];
  romdb_game games[MATCH_MAX];
  size_t count;
} match;
/* MODAL_KEYBOARD: what it types for, MODAL_WIFI (a password) or MODAL_MATCH (a search) */
static int keyboard_for = MODAL_WIFI;

static void match_find(nav *ui, str query) {
  sb_set(match.query, query);
  const str pack = apps_pack(&categories[match.category]);
  const size_t n = romdb_search(pack, query, match.crcs, MATCH_MAX);
  match.count = 0;
  for (size_t i = 0; i < n; i++)
    if (romdb_find(pack, match.crcs[i], &match.games[match.count]))
      match.crcs[match.count++] = match.crcs[i];
  modal.kind = MODAL_MATCH;
  /* The one picked, else the best: after Search and By its contents */
  const app *a = picked_game(&categories[match.category]);
  size_t row = match.count > 0 ? 2 : 0;
  for (size_t i = 0; a != NULL && i < match.count; i++)
    if (match.crcs[i] == a->match)
      row = i + 2;
  nav_focus(ui, menu_row(nav_str("match"), row));
}

/* By the ROM's name: "Super Mario Land 2 (Rev 1).gb" looks for "super mario land 2" */
static void match_open(nav *ui, size_t category) {
  const str path = sb_str(modal.rom), name = str_at(path, str_rfind(path, '/') + 1);
  const size_t dot = str_rfind(name, '.');
  match.category = category;
  match_find(ui, dot > 0 ? str_head(name, dot) : name);
}

/* Select on a port: its readme, its page in the store */
static const entry *port_dialog(nav *ui, int w, int h) {
  static entry rows[2];
  size_t n = 0;
  STRBUF(600) md;
  sb_set(md, sb_str(modal.rom), S("/port.md"));
  if (access(md.p, F_OK) == 0)
    rows[n++] = (entry){.label = S("Readme"), .action = PORT_README, .icon = ICON_BOOK_OPEN};
  rows[n++] = (entry){.label = S("Store page"), .action = PORT_STORE, .icon = ICON_SHOPPING_BAG};
  return dialog_box(ui, w, h, sb_str(modal.name), S(""), rows, n);
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

/* licenses[license_shown]'s texts, a line a step */
static void license_screen(nav *ui, ui_rect view) {
  const str *lines;
  const size_t n = license_text(license_shown, LICENSE_TEXT, view.w - MARGIN * 4, &lines);
  const int line = ui_text_height(LICENSE_TEXT) + 2;
  const float offset = nav_scroll_begin(ui, nav_str("license"), view, UI_VERTICAL,
                                        n * line + MARGIN * 2, 0, line);
  gfx_clip(view.x, view.y, view.w, view.h);
  /* Only those in view */
  for (size_t i = (size_t)MAX(0, (offset - MARGIN) / line); i < n; i++) {
    const float y = view.y + MARGIN + i * line - offset;
    if (y >= view.y + view.h)
      break;
    ui_text(LICENSE_TEXT, (int)view.x + MARGIN * 2, (int)y, lines[i], FG);
  }
  gfx_noclip();
  nav_scroll_end(ui);
}

/* MODAL_README: the port's port.md as text, wrapped to the width it was last shown at */
static str readme_text;
static ui_lines readme_lines;
static float readme_width;

static void readme_load(void) {
  STRBUF(600) path;
  sb_set(path, sb_str(modal.rom), S("/port.md"));
  const str md = str_read(fopen(path.p, "r"));
  free((char *)readme_text.p);
  readme_text = md.p != NULL ? ui_markdown(md) : (str){0};
  free((char *)md.p);
  readme_width = 0;
}

static void readme_screen(nav *ui, ui_rect view) {
  if (readme_width != view.w) {
    readme_width = view.w;
    readme_lines.count = 0;
    ui_wrap_text(&readme_lines, readme_text, LICENSE_TEXT, view.w - MARGIN * 4);
  }
  const int line = ui_text_height(LICENSE_TEXT) + 2;
  const float offset = nav_scroll_begin(ui, nav_str("readme"), view, UI_VERTICAL,
                                        readme_lines.count * line + MARGIN * 2, 0, line);
  gfx_clip(view.x, view.y, view.w, view.h);
  for (size_t i = (size_t)MAX(0, (offset - MARGIN) / line); i < readme_lines.count; i++) {
    const float y = view.y + MARGIN + i * line - offset;
    if (y >= view.y + view.h)
      break;
    ui_text(LICENSE_TEXT, (int)view.x + MARGIN * 2, (int)y, readme_lines.lines[i], FG);
  }
  gfx_noclip();
  nav_scroll_end(ui);
}

/* MODAL_KEYBOARD: the network the password is for */
static struct {
  STRBUF(160) ssid;
} wifi_pick;

static void keyboard_submit(nav *ui) {
  if (keyboard_for == MODAL_MATCH) {
    match_find(ui, keyboard_text());
    return;
  }
  if (!wifi_connect(sb_str(wifi_pick.ssid), true, keyboard_text()))
    fprintf(stderr, "launcher: wifi: couldn't add %s\n", wifi_pick.ssid.p);
  modal.kind = MODAL_WIFI;
  nav_focus(ui, 0);
}

/* "142 MB" */
static str megabytes(unsigned long long bytes) {
  static STRBUF(32) out;
  out.n = 0;
  sb_fmt(out, "%llu MB", (bytes + (1 << 19)) >> 20);
  return sb_str(out);
}

/* downloadd's "https://…/v1/os?…: Couldn't resolve host name" as its reason alone */
static str without_url(str why) {
  const size_t scheme = str_find(why, ':');
  if (scheme + 2 < why.n && why.p[scheme + 1] == '/' && why.p[scheme + 2] == '/')
    for (size_t i = scheme + 3; i + 1 < why.n; i++)
      if (why.p[i] == ':' && why.p[i + 1] == ' ')
        return str_at(why, i + 2);
  return why;
}

/* Updates: what the server has, then what can be done about it */
static size_t update_rows(entry *rows, str *said) {
  static STRBUF(64) labels[3];
  static STRBUF(700) text;
  pthread_mutex_lock(&updates_lock);
  const update_state u = updates;
  pthread_mutex_unlock(&updates_lock);
  size_t n = 0;
  sb_set(text, S("This system: "), updates_build(), S(". "));
  if (!u.connected)
    sb_add(text, S("The download service isn't running."));
  else if (u.job == UPDATE_IDLE && str_eq(sb_str(u.job_state), S("failed")))
    sb_add(text, S("The update failed: "), sb_str(u.job_message));
  else if (u.check == UPDATE_CHECKING)
    sb_add(text, S("Checking\u2026"));
  else if (u.check == UPDATE_CURRENT)
    sb_add(text, S("It's up to date."));
  else if (u.check == UPDATE_AVAILABLE)
    sb_add(text, S("Build "), sb_str(u.build), S(" is out. "), sb_str(u.notes));
  else if (u.check == UPDATE_FAILED)
    sb_add(text, S("Couldn't check: "), without_url(sb_str(u.notes)));
  *said = sb_str(text);
  if (u.job == UPDATE_READY) {
    rows[n++] = (entry){.label = S("Restart to finish the update"), .command = S("reboot"), .action = POWER,
                        .icon = ICON_ROTATE_CW};
    return n;
  }
  if (u.job == UPDATE_RUNNING) {
    labels[0].n = 0;
    if (str_eq(sb_str(u.job_state), S("downloading")))
      sb_fmt(labels[0], "Downloading %d%%", u.total ? (int)(u.done * 100 / u.total) : 0);
    else if (str_eq(sb_str(u.job_state), S("installing")))
      sb_set(labels[0], S("Installing\u2026"));
    else
      sb_set(labels[0], S("Waiting\u2026"));
    rows[n++] = (entry){.label = sb_str(labels[0]), .action = NOTHING, .icon = ICON_HOURGLASS};
    return n;
  }
  if (u.check == UPDATE_AVAILABLE) {
    sb_set(labels[1], S("Download and install ("), megabytes(u.size), S(")"));
    rows[n++] = (entry){.label = sb_str(labels[1]), .action = UPDATE_INSTALL, .icon = ICON_DOWNLOAD};
  }
  rows[n++] = (entry){.label = S("Check for updates"), .action = UPDATE_CHECK, .icon = ICON_REFRESH_CW};
  return n;
}

/* SSH (dropbear: root, no password) is off while this is there, and on the USB cable's network
 * alone unless SSH_ALL is; sysd's "ssh on|off|all|usb" */
#define SSH_OFF "/data/OS/dropbear/disabled"
#define SSH_ALL "/data/OS/dropbear/everywhere"
/* Sleep never powers the SoC off while this is there; sysd's "sleep light|deep" */
#define LIGHT_SLEEP "/data/OS/config/light-sleep"
/* The last deep sleep didn't resume (why, for the log: senkos.init); emptied once asked */
#define SLEEP_FAILED "/run/sleep-failed"

/* Settings, Time and Debug: what each row does */
enum { PAGE_MAX = 8 + PERF_METRICS + 1 };
static size_t page_rows(int kind, entry *rows) {
  static STRBUF(96) zone, when;
  size_t n = 0;
  if (kind == MODAL_TIME) {
    const str z = clock_zone();
    sb_set(zone, S("Time zone  \u00b7  "), z.n > 0 ? z : S("UTC"), clock_zone_chosen() ? S("") : S(" (automatic)"));
    rows[n++] = (entry){.label = sb_str(zone), .action = ZONES, .icon = ICON_GLOBE};
    rows[n++] = (entry){.label = S("Set the time automatically"), .action = NTP, .icon = ICON_REFRESH_CW,
                        .checked = clock_ntp()};
    if (!clock_ntp()) {
      char text[32];
      const time_t now = wall_time();
      strftime(text, sizeof(text), "%Y-%m-%d  %H:%M", localtime(&now));
      sb_set(when, S("Date and time  \u00b7  "), str_c(text));
      rows[n++] = (entry){.label = sb_str(when), .action = CLOCK, .icon = ICON_CALENDAR_CLOCK};
    }
  } else if (kind == MODAL_DEBUG) {
    const bool on = access(SSH_OFF, F_OK) != 0;
    rows[n++] = (entry){.label = S("SSH  \u00b7  root, no password"), .action = SSH, .icon = ICON_NETWORK,
                        .checked = on};
    if (on)
      rows[n++] = (entry){.label = S("On every network, not just USB"), .action = SSH_EVERYWHERE,
                          .icon = ICON_TRIANGLE_ALERT, .checked = access(SSH_ALL, F_OK) == 0, .depth = 1};
    rows[n++] = (entry){.label = S("Deep sleep  \u00b7  saves battery"), .action = DEEP_SLEEP, .icon = ICON_MOON,
                        .checked = access(LIGHT_SLEEP, F_OK) != 0};
    n += perf_rows(rows + n);
  } else {
    rows[n++] = (entry){.label = S("Storage"), .action = STORAGE, .icon = ICON_HARD_DRIVE};
    rows[n++] = (entry){.label = S("Wi-Fi"), .action = WIFI, .icon = ICON_WIFI};
    rows[n++] = (entry){.label = S("Time"), .action = TIME, .icon = ICON_CLOCK};
    rows[n++] = (entry){.label = S("Light mode"), .action = LIGHT_MODE, .icon = ICON_SUN,
                        .checked = ui_light};
    rows[n++] = (entry){.label = S("Updates"), .action = UPDATES, .icon = ICON_DOWNLOAD};
    rows[n++] = (entry){.label = S("Debug"), .action = DEBUG, .icon = ICON_CPU};
    rows[n++] = (entry){.label = S("Licenses"), .action = LICENSES_OPEN, .icon = ICON_SCROLL_TEXT};
  }
  return n;
}

static const char *page_scope(int kind) {
  return kind == MODAL_TIME ? "time" : kind == MODAL_DEBUG ? "debug" : "system";
}

/* Settings, Time or Debug, on the row that does `action` (none: the top) */
static void open_page(nav *ui, int kind, int action) {
  entry rows[PAGE_MAX];
  const size_t n = page_rows(kind, rows);
  modal.kind = kind;
  nav_focus(ui, 0);
  for (size_t i = 0; i < n; i++)
    if (rows[i].action == action)
      nav_focus(ui, menu_row(nav_str(page_scope(kind)), i));
}

/* MODAL_ZONE: the region whose zones are shown */
static STRBUF(32) zone_region;

/* MODAL_ZONES on the zone's region, MODAL_ZONE on the zone */
static void open_zones(nav *ui, int kind) {
  const str *names;
  const str zone = clock_zone();
  const size_t n = kind == MODAL_ZONES ? clock_regions(&names) : clock_zones(sb_str(zone_region), &names);
  const str want = kind == MODAL_ZONES ? str_head(zone, str_find(zone, '/')) : zone;
  modal.kind = kind;
  nav_focus(ui, 0);
  for (size_t i = 0; i < n; i++)
    if (str_eq(names[i], want))
      nav_focus(ui, menu_row(nav_str(kind == MODAL_ZONES ? "zones" : "zone"), i + (kind == MODAL_ZONES)));
}

/* MODAL_CLOCK: year, month, day, hour, minute; the field changed by up and down */
enum { CLOCK_FIELDS = 5 };
static struct {
  int field[CLOCK_FIELDS];
  int at;
} clock_edit;

static void clock_open(void) {
  const time_t now = wall_time();
  const struct tm *t = localtime(&now);
  clock_edit = (typeof(clock_edit)){{t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, t->tm_hour, t->tm_min}, 0};
  modal.kind = MODAL_CLOCK;
}

static void clock_step(int by) {
  static const int mdays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int *f = clock_edit.field;
  const int y = f[0], leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
  const int days = mdays[f[1] - 1] + (f[1] == 2 && leap);
  const int low[] = {2020, 1, 1, 0, 0}, high[] = {2099, 12, days, 23, 59};
  const int i = clock_edit.at, span = high[i] - low[i] + 1;
  f[i] = low[i] + ((f[i] - low[i] + by) % span + span) % span;
  /* A shorter month: its last day */
  const int now_days = mdays[f[1] - 1] + (f[1] == 2 && ((f[0] % 4 == 0 && f[0] % 100 != 0) || f[0] % 400 == 0));
  f[2] = MIN(f[2], now_days);
}

/* To sysd, as seconds: the fields are this zone's time */
static void clock_set(void) {
  const int *f = clock_edit.field;
  struct tm t = {.tm_year = f[0] - 1900, .tm_mon = f[1] - 1, .tm_mday = f[2], .tm_hour = f[3], .tm_min = f[4],
                 .tm_isdst = -1};
  const time_t when = mktime(&t);
  STRBUF(48) request;
  request.n = 0;
  sb_fmt(request, "clock set %lld", (long long)when);
  if (when != (time_t)-1)
    sysd(sb_str(request));
}

/* Over Settings: the date's fields, then the time's, the one changing ringed */
static void clock_dialog(int w, int h) {
  ui_fill(0, 0, w, h, SCRIM);
  /* "2026 - 10 - 05     18 : 30" */
  static const char *const after[CLOCK_FIELDS] = {"-", "-", "", ":", ""};
  const int text = ui_text_height(ROW_TEXT), digits = ui_text_width(ROW_TEXT, S("00")) + MARGIN;
  const float field_w[CLOCK_FIELDS] = {ui_text_width(ROW_TEXT, S("0000")) + MARGIN, digits, digits, digits, digits};
  const float sep = ui_text_width(ROW_TEXT, S(" - ")), gap = MARGIN * 4;
  float total = gap;
  for (int i = 0; i < CLOCK_FIELDS; i++)
    total += field_w[i] + (after[i][0] ? sep : 0);
  const float dw = MAX(total + MARGIN * 4, 320), dh = MARGIN * 5 + text * 3 + 4 + ROW;
  const float x = (w - dw) / 2, y = (h - dh) / 2;
  ui_rounded(x, y, dw, dh, TILE, RADIUS, 0);
  ui_rounded(x, y, dw, dh, EDGE, RADIUS, 1);
  ui_text(ROW_TEXT, (int)x + MARGIN * 2, (int)y + MARGIN * 2, S("Date and time"), FG);
  float fx = x + (dw - total) / 2;
  const float ly = y + MARGIN * 3 + text, fy = ly + text + 4, fh = ROW;
  for (int i = 0; i < CLOCK_FIELDS; i++) {
    if (i == 0 || i == 3)
      ui_text(ROW_TEXT, (int)fx, (int)ly, i == 0 ? S("Date") : S("Time"), DIM);
    ui_rounded(fx, fy, field_w[i], fh, i == clock_edit.at ? FOCUS : BG, 6, 0);
    if (i == clock_edit.at)
      ui_rounded(fx - 2, fy - 2, field_w[i] + 4, fh + 4, RING, 8, 2);
    STRBUF(8) value;
    value.n = 0;
    sb_fmt(value, i == 0 ? "%04d" : "%02d", clock_edit.field[i]);
    ui_text(ROW_TEXT, (int)(fx + (field_w[i] - ui_text_width(ROW_TEXT, sb_str(value))) / 2), (int)(fy + (fh - text) / 2),
            sb_str(value), FG);
    fx += field_w[i];
    if (after[i][0]) {
      const str a = str_c(after[i]);
      ui_text(ROW_TEXT, (int)(fx + (sep - ui_text_width(ROW_TEXT, a)) / 2), (int)(fy + (fh - text) / 2), a, DIM);
      fx += sep;
    }
    if (i == 2)
      fx += gap;
  }
}

/* Instead of home: Storage; a folder's rows, each with the systems it's a
 * source of; or that folder's systems, its own checked; Licenses, or one's
 * texts */
static const entry *folders_screen(nav *ui, int w, int h, ui_rect view, const status *st) {
  static entry rows[MAX(BROWSE_MAX, LICENSES_MAX)]; /* Storage's STORAGE_MAX too */
  static STRBUF(200) labels[BROWSE_MAX];
  const ui_rect list = {view.x, view.y, view.w, view.h - ROW};
  const entry *chosen = NULL;
  str hint;
  if (modal.kind == MODAL_STORAGE) {
    top_bar(w, S("Storage"), st);
    const size_t n = storage_entries(rows);
    NAV_SCOPE(ui, nav_str("storage")) { chosen = menu(ui, rows, n, list); }
    /* Windows doesn't find the share by itself */
    hint = usb_is(1, S("smb")) ? S("On a PC: \\\\10.55.0.1, user player") : S("");
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
      rows[i] = (entry){.label = sb_str(labels[i]), .action = BROWSE_OPEN, .icon = ICON_FOLDER, .category = (int)i};
    }
    NAV_SCOPE(ui, nav_str(browse_dir.p)) { chosen = menu(ui, rows, browse_count, list); }
    hint = browse_count > 0 ? S("X: systems") : S("");
  } else if (modal.kind == MODAL_LICENSES) {
    top_bar(w, S("Licenses"), st);
    for (size_t i = 0; i < license_count; i++)
      rows[i] = (entry){.label = licenses[i].label, .action = LICENSE_OPEN, .icon = ICON_FILE_TEXT, .category = (int)i};
    NAV_SCOPE(ui, nav_str("licenses")) { chosen = menu(ui, rows, license_count, list); }
    hint = S("");
  } else if (modal.kind == MODAL_LICENSE) {
    top_bar(w, licenses[license_shown].name, st);
    license_screen(ui, list);
    hint = S("Left, right: page");
  } else if (modal.kind == MODAL_README) {
    top_bar(w, sb_str(modal.name), st);
    readme_screen(ui, list);
    hint = S("Left, right: page");
  } else if (modal.kind == MODAL_WIFI) {
    top_bar(w, S("Wi-Fi"), st);
    const bool on = wifi_on();
    size_t n = 0;
    rows[n++] = (entry){.label = S("Wi-Fi"), .command = on ? S("wifi off") : S("wifi on"), .action = POWER,
                        .icon = on ? ICON_WIFI : ICON_WIFI_OFF, .checked = on};
    for (size_t i = 0; i < wifi_count; i++) {
      const wifi_network *net = &wifi_networks[i];
      /* The signal is the icon's */
      sb_set(labels[i], sb_str(net->ssid), net->saved ? S("  \u00b7  saved") : !net->secured ? S("  \u00b7  open") : S(""));
      static const int arcs[] = {ICON_WIFI_ZERO, ICON_WIFI_LOW, ICON_WIFI_HIGH, ICON_WIFI, ICON_WIFI};
      rows[n++] = (entry){.label = sb_str(labels[i]), .action = WIFI_PICK, .icon = arcs[CLAMP(net->bars, 0, 4)],
                          .category = (int)i,
                          .checked = net->current};
    }
    const int line = ui_text_height(ROW_TEXT) + MARGIN;
    ui_text(ROW_TEXT, MARGIN * 2, (int)view.y + MARGIN, ui_text_fit(ROW_TEXT, wifi_status(), w - MARGIN * 4, NULL), DIM);
    const ui_rect below = {list.x, list.y + line, list.w, list.h - line};
    NAV_SCOPE(ui, nav_str("wifi")) { chosen = menu(ui, rows, n, below); }
    hint = menu_focus > 0 && wifi_networks[menu_focus - 1].saved ? S("X: forget") : S("");
  } else if (modal.kind == MODAL_KEYBOARD) {
    const bool search = keyboard_for == MODAL_MATCH;
    top_bar(w, search ? S("Title and box art") : S("Wi-Fi"), st);
    STRBUF(200) label;
    if (search)
      sb_set(label, S("Search "), categories[match.category].name);
    else
      sb_set(label, S("Password for "), sb_str(wifi_pick.ssid));
    bool done = false;
    NAV_SCOPE(ui, nav_str("keyboard")) { done = keyboard(ui, list, sb_str(label)); }
    if (done)
      keyboard_submit(ui);
    hint = S("X: delete    Y: shift    Start: done");
  } else if (modal.kind == MODAL_UPDATES) {
    top_bar(w, S("Updates"), st);
    str said;
    const size_t n = update_rows(rows, &said);
    ui_lines text = {0};
    ui_wrap_text(&text, said, ROW_TEXT, w - MARGIN * 4);
    const int line = ui_text_height(ROW_TEXT) + 4;
    const size_t shown_lines = MIN(text.count, 4);
    for (size_t i = 0; i < shown_lines; i++)
      ui_text(ROW_TEXT, MARGIN * 2, (int)view.y + MARGIN + (int)i * line, text.lines[i], DIM);
    free(text.lines);
    const float top = MARGIN + shown_lines * line;
    const ui_rect below = {list.x, list.y + top, list.w, list.h - top};
    NAV_SCOPE(ui, nav_str("updates")) { chosen = menu(ui, rows, n, below); }
    hint = S("");
  } else if (modal.kind == MODAL_SETTINGS || modal.kind == MODAL_TIME || modal.kind == MODAL_DEBUG ||
             modal.kind == MODAL_CLOCK) {
    /* The clock's popup is over Time */
    const int kind = modal.kind == MODAL_CLOCK ? MODAL_TIME : modal.kind;
    top_bar(w, kind == MODAL_TIME ? S("Time") : kind == MODAL_DEBUG ? S("Debug") : S("Settings"), st);
    const size_t n = page_rows(kind, rows);
    nav_set_inert(ui, modal.kind == MODAL_CLOCK);
    NAV_SCOPE(ui, nav_str(page_scope(kind))) { chosen = menu(ui, rows, n, list); }
    nav_set_inert(ui, false);
    hint = S("");
    if (modal.kind == MODAL_CLOCK) {
      clock_dialog(w, h);
      hint = S("Up, down: change    Left, right: move");
    }
  } else if (modal.kind == MODAL_MATCH) {
    /* Search, None, then what was found; the focused one's box art beside them */
    top_bar(w, S("Title and box art"), st);
    const app *a = picked_game(&categories[match.category]);
    size_t n = 0;
    sb_set(labels[0], S("Search  \u00b7  "), sb_str(match.query));
    rows[n++] = (entry){.label = sb_str(labels[0]), .action = MATCH_SEARCH, .icon = ICON_SEARCH};
    rows[n++] = (entry){.label = S("By its contents"), .action = MATCH_PICK, .icon = ICON_MICROCHIP, .category = -1,
                        .checked = a != NULL && a->match == 0};
    for (size_t i = 0; i < match.count; i++)
      rows[n++] = (entry){.label = match.games[i].title, .action = MATCH_PICK, .icon = ICON_IMAGE, .category = (int)i,
                          .checked = a != NULL && a->match == match.crcs[i]};
    const ui_rect left = {list.x, list.y, list.w - ICON - MARGIN * 2, list.h};
    NAV_SCOPE(ui, nav_str("match")) { chosen = menu(ui, rows, n, left); }
    const float px = left.x + left.w, py = list.y + MARGIN;
    if (menu_focus >= 2 && (size_t)menu_focus - 2 < match.count) {
      const asset art = assets_get(sb_str(match.games[menu_focus - 2].image));
      if (art.slot != GFX_PLACEHOLDER) {
        const float scale = MIN((float)ICON / art.w, (float)ICON / art.h);
        gfx_push((gfx_quad){.x = px + (ICON - art.w * scale) / 2, .y = py, .w = art.w * scale, .h = art.h * scale,
                            .u1 = 1, .v1 = 1, .color = 0xffffffff, .tex = art.slot, .radius = RADIUS - 2});
      } else {
        ui_rounded(px, py, ICON, ICON, EDGE, RADIUS - 2, 0);
      }
    }
    hint = S("");
  } else if (modal.kind == MODAL_POWER) {
    top_bar(w, S("Power"), st);
    NAV_SCOPE(ui, nav_str("home power")) { power_page(ui, list, false); }
    hint = S("Left, right: change");
  } else if (modal.kind == MODAL_ZONES) {
    top_bar(w, S("Time zone"), st);
    const str *regions;
    const size_t n = MIN(clock_regions(&regions), BROWSE_MAX - 1);
    rows[0] = (entry){.label = S("Automatic"), .action = ZONE_AUTO, .icon = ICON_GLOBE, .checked = !clock_zone_chosen()};
    for (size_t i = 0; i < n; i++)
      rows[i + 1] = (entry){.label = regions[i], .action = ZONE_REGION, .icon = ICON_FOLDER, .category = (int)i,
                            .checked = clock_zone_chosen() && str_eq(regions[i], str_head(clock_zone(), str_find(clock_zone(), '/')))};
    NAV_SCOPE(ui, nav_str("zones")) { chosen = menu(ui, rows, n + 1, list); }
    hint = S("");
  } else if (modal.kind == MODAL_ZONE) {
    top_bar(w, sb_str(zone_region), st);
    const str *zones;
    const size_t n = MIN(clock_zones(sb_str(zone_region), &zones), BROWSE_MAX);
    for (size_t i = 0; i < n; i++) {
      /* "America/Argentina/Buenos_Aires" as "Argentina / Buenos Aires" */
      labels[i].n = 0;
      const str place = str_at(zones[i], zone_region.n + 1);
      for (size_t k = 0; k < place.n; k++)
        sb_add(labels[i], place.p[k] == '_' ? S(" ") : place.p[k] == '/' ? S(" / ") : str_head(str_at(place, k), 1));
      rows[i] = (entry){.label = sb_str(labels[i]), .action = ZONE_PICK, .icon = ICON_CLOCK, .category = (int)i,
                        .checked = str_eq(zones[i], clock_zone())};
    }
    NAV_SCOPE(ui, nav_str("zone")) { chosen = menu(ui, rows, n, list); }
    hint = S("");
  } else {
    top_bar(w, shown(sb_str(pick_path)), st);
    size_t n = 0;
    for (size_t i = 0; i < category_count && n < BROWSE_MAX; i++) {
      if (categories[i].roms.n == 0)
        continue;
      rows[n++] = (entry){.label = categories[i].name, .action = TOGGLE_SYSTEM, .icon = ICON_GAMEPAD_2,
                          .category = (int)i, .checked = sources_has(sb_str(pick_path), i)};
    }
    NAV_SCOPE(ui, nav_str("systems")) { chosen = menu(ui, rows, n, list); }
    hint = S("");
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
/* A port's [Port] Instructions (docs/ports.md), until the next call */
static bool port_instructions(const char *ini, str *out) {
  static STRBUF(640) text;
  const str file = str_read(fopen(ini, "r"));
  if (file.p == NULL)
    return false;
  sb_set(text, S(""));
  cfg_reader r = cfg_init(file);
  for (cfg_event e; (e = cfg_next(&r)) != CFG_END;)
    if (e == CFG_VALUE && str_eq(r.section, S("Port")) && str_eq(r.key, S("Instructions")))
      sb_set(text, r.value);
  free((char *)file.p);
  if (text.n == 0)
    sb_set(text, S("Its files aren't there yet: see its port.md."));
  *out = sb_str(text);
  return true;
}

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
  STRBUF(sizeof(r->dir.p) + 16) ini;
  sb_set(ini, sb_str(r->dir), S("/port.ini"));
  str why;
  const bool ok = run_forget(r, &why);
  if (had_screen)
    go_home(ui);
  STRBUF(96) title;
  if (str_eq(why, S("exited with status 3")) && port_instructions(ini.p, &why)) {
    /* portrun: files it Needs aren't there. Always said, never ignored. */
    sb_set(title, sb_str(label), S(" needs files"));
    show_failure(ui, FAIL_LAUNCH, sb_str(title), why, S(""));
  } else if (!ok && !ignored(sb_str(key))) {
    sb_set(title, sb_str(label), S(" stopped"));
    show_failure(ui, FAIL_LAUNCH, sb_str(title), why, sb_str(key));
  }
  if (*page == 0 && run_count() == 0)
    turn_page(page, 1);
  rescan(*page);
}

/* A frame is all it wants */
static void redraw(win *w) {
  (void)w;
  win_wake(UI_CONFIGURED, 0);
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

/* `launcher launch`'s FIFO; the last path asked for, until the main loop takes it */
static char ctl_fifo[256];
static pthread_mutex_t ctl_lock = PTHREAD_MUTEX_INITIALIZER;
static STRBUF(512) ctl_path;

static void *ctl_thread(void *arg) {
  (void)arg;
  /* Read-write: never at EOF when a writer leaves. Not a FILE: exit() would wait for its lock,
   * held by a read that never returns. */
  const int fd = open(ctl_fifo, O_RDWR | O_CLOEXEC);
  char line[512];
  size_t n = 0;
  for (ssize_t got; fd >= 0 && (got = read(fd, line + n, sizeof(line) - n)) > 0;) {
    n += (size_t)got;
    for (char *end; (end = memchr(line, '\n', n)) != NULL;) {
      *end = '\0';
      if (strcmp(line, "reload") == 0) {
        win_wake(UI_RELOAD, 0);
      } else {
        pthread_mutex_lock(&ctl_lock);
        sb_set(ctl_path, str_c(line));
        pthread_mutex_unlock(&ctl_lock);
        win_wake(UI_LAUNCH, 0);
      }
      n -= (size_t)(end + 1 - line);
      memmove(line, end + 1, n);
    }
    /* A line too long: dropped */
    if (n == sizeof(line))
      n = 0;
  }
  return NULL;
}

/* /run/launcher, which weston.init makes (shell's; the player may write to it too: launch,
 * reload), else $XDG_RUNTIME_DIR/launcher (on a desktop) */
#define CTL_FIFO "/run/launcher"
static bool ctl_name(void) {
  const char *run = getenv("XDG_RUNTIME_DIR");
  if (access(CTL_FIFO, F_OK) == 0)
    snprintf(ctl_fifo, sizeof(ctl_fifo), "%s", CTL_FIFO);
  else if (run != NULL)
    snprintf(ctl_fifo, sizeof(ctl_fifo), "%s/launcher", run);
  return run != NULL || ctl_fifo[0] != '\0';
}

static bool open_ctl(void) {
  return ctl_name() && (mkfifo(ctl_fifo, 0600) == 0 || errno == EEXIST) && spawn("ctl", ctl_thread, NULL);
}

/* The app whose tile `path` is (its ROM, folder or program), started as A on it does */
static void launch_path(nav *ui) {
  STRBUF(512) path;
  pthread_mutex_lock(&ctl_lock);
  sb_set(path, sb_str(ctl_path));
  pthread_mutex_unlock(&ctl_lock);
  if (front != NULL || modal.kind != MODAL_NONE) {
    fprintf(stderr, "launcher: launch %s: not at home\n", path.p);
    return;
  }
  /* What's loaded, then each category read again: pages are read when shown */
  for (int pass = 0; pass < 2; pass++)
    for (size_t i = 0; i < category_count; i++) {
      if (pass == 1)
        apps_scan(&categories[i]);
      for (size_t j = 0; j < categories[i].count; j++) {
        const app *a = &categories[i].apps[j];
        if (a->exec.n > 0 && (str_eq(sb_str(a->arg), sb_str(path)) || str_eq(sb_str(a->dir), sb_str(path)))) {
          start(ui, sb_str(a->name), sb_str(a->icon), sb_str(a->exec), sb_str(a->arg), sb_str(a->dir));
          return;
        }
      }
    }
  fprintf(stderr, "launcher: launch %s: no such app\n", path.p);
}

/* `launcher launch PATH`: to the running launcher */
static int request_launch(const char *arg) {
  char path[PATH_MAX];
  if (realpath(arg, path) == NULL || !ctl_name()) {
    fprintf(stderr, "launcher: %s: %s\n", arg, ctl_fifo[0] == '\0' ? "no XDG_RUNTIME_DIR" : strerror(errno));
    return 1;
  }
  const int fd = open(ctl_fifo, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0 || dprintf(fd, "%s\n", path) < 0) {
    fprintf(stderr, "launcher: no launcher running (%s)\n", ctl_fifo);
    return 1;
  }
  close(fd);
  return 0;
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
  case UI_LAUNCH: launch_path(ui); return true;
  case UI_RELOAD:
    apps_cores();
    rescan(*page);
    return true;
  case UI_POWER_HOLD:
    if (front != NULL && !paused)
      pause_front(ui);
    if (!power_menu.shown)
      power_menu.back = ui->focus;
    power_menu.shown = true;
    nav_focus(ui, 0);
    return true;
  case UI_REFUSED: {
    char text[128];
    show_text(sysd_refused(text, sizeof(text)), 3000);
    return true;
  }
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
  case UI_CONFIGURED:
  case UI_DONE: ui_scheme(); return true;
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
  if (power_menu.shown) {
    if (button == APPCTL_B) {
      power_menu.shown = false;
      nav_focus(ui, power_menu.back);
    } else if (button == APPCTL_UP || button == APPCTL_DOWN) {
      const nav_dir dir = button == APPCTL_UP ? NAV_UP : NAV_DOWN;
      if (!nav_move(ui, dir))
        nav_wrap(ui, dir);
    } else if (button == APPCTL_A) {
      nav_activate(ui);
    }
    return true;
  }
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
  if (button == APPCTL_B && modal.kind == MODAL_LICENSE) {
    modal.kind = MODAL_LICENSES;
    nav_focus(ui, menu_row(nav_str("licenses"), license_shown));
    return true;
  }
  if (modal.kind == MODAL_KEYBOARD && !paused) {
    switch (button) {
    case APPCTL_B:
      modal.kind = keyboard_for;
      nav_focus(ui, keyboard_for == MODAL_MATCH ? menu_row(nav_str("match"), 0) : 0);
      return true;
    case APPCTL_X: keyboard_delete(); return true;
    case APPCTL_Y: keyboard_shift(); return true;
    case APPCTL_START: keyboard_submit(ui); return true;
    }
  }
  if (button == APPCTL_X && modal.kind == MODAL_WIFI && !paused && menu_focus > 0 &&
      (size_t)menu_focus <= wifi_count) {
    wifi_forget(sb_str(wifi_networks[menu_focus - 1].ssid));
    wifi_poll();
    return true;
  }
  /* Nothing to focus: the text scrolls, by lines or by pages */
  if ((modal.kind == MODAL_LICENSE || (modal.kind == MODAL_README && button != APPCTL_B)) && !paused) {
    const int page_lines = (window.h - BAR - ROW * 2) / (ui_text_height(LICENSE_TEXT) + 2);
    switch (button) {
    case APPCTL_UP: nav_step(ui, -1); break;
    case APPCTL_DOWN: nav_step(ui, 1); break;
    case APPCTL_LEFT: case APPCTL_L1: nav_step(ui, -page_lines); break;
    case APPCTL_RIGHT: case APPCTL_R1: nav_step(ui, page_lines); break;
    }
    return true;
  }
  if (button == APPCTL_B && modal.kind == MODAL_BROWSE) {
    modal.kind = MODAL_STORAGE;
    nav_focus(ui, menu_row(nav_str("storage"), 0));
    return true;
  }
  /* Settings' screens: back to it */
  if (button == APPCTL_B && !paused) {
    /* The screen, the page it's on, the row there that opened it */
    static const int opened_by[][3] = {
        {MODAL_STORAGE, MODAL_SETTINGS, STORAGE}, {MODAL_WIFI, MODAL_SETTINGS, WIFI},
        {MODAL_UPDATES, MODAL_SETTINGS, UPDATES}, {MODAL_LICENSES, MODAL_SETTINGS, LICENSES_OPEN},
        {MODAL_TIME, MODAL_SETTINGS, TIME},       {MODAL_DEBUG, MODAL_SETTINGS, DEBUG},
        {MODAL_ZONES, MODAL_TIME, ZONES},         {MODAL_CLOCK, MODAL_TIME, CLOCK}};
    for (size_t i = 0; i < COUNTOF(opened_by); i++)
      if (modal.kind == opened_by[i][0]) {
        open_page(ui, opened_by[i][1], opened_by[i][2]);
        return true;
      }
    if (modal.kind == MODAL_ZONE) {
      open_zones(ui, MODAL_ZONES);
      return true;
    }
  }
  if (modal.kind == MODAL_CLOCK && !paused) {
    switch (button) {
    case APPCTL_LEFT: clock_edit.at = MAX(clock_edit.at - 1, 0); break;
    case APPCTL_RIGHT: clock_edit.at = MIN(clock_edit.at + 1, CLOCK_FIELDS - 1); break;
    case APPCTL_UP: clock_step(1); break;
    case APPCTL_DOWN: clock_step(-1); break;
    case APPCTL_A:
      clock_set();
      open_page(ui, MODAL_TIME, CLOCK);
      break;
    }
    return true;
  }
  if (modal.kind == MODAL_POWER && !paused && power_focus >= 0 && (button == APPCTL_LEFT || button == APPCTL_RIGHT)) {
    power_step(power_focus, button == APPCTL_LEFT ? -1 : 1);
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
  /* Select on a port: its readme, its store page */
  if (button == APPCTL_SELECT && !paused && !dialog && *page >= 1 && tiles_focus >= 0 &&
      (size_t)tiles_focus < categories[*page - 1].count) {
    const app *a = &categories[*page - 1].apps[tiles_focus];
    STRBUF(600) ini;
    sb_set(ini, sb_str(a->dir), S("/port.ini"));
    if (a->dir.n > 0 && access(ini.p, F_OK) == 0) {
      modal.kind = MODAL_PORT;
      modal.back = ui->focus;
      sb_set(modal.rom, sb_str(a->dir));
      sb_set(modal.name, sb_str(a->name));
      nav_focus(ui, 0);
      return true;
    }
  }
  /* Select on a game: which core runs it, when there's a choice; its title and box art */
  if (button == APPCTL_SELECT && !paused && !dialog && *page >= 1 && tiles_focus >= 0 &&
      (size_t)tiles_focus < categories[*page - 1].count && categories[*page - 1].roms.n > 0 &&
      (categories[*page - 1].core_count > 1 || categories[*page - 1].pack.n > 0)) {
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
  if (in_settings && settings_focus >= 0 && button == APPCTL_Y && (front->shm->flags & APPCTL_OWN_OPTIONS)) {
    appctl_option *o = &front->shm->options[settings_focus];
    atomic_store(&o->own, !atomic_load(&o->own));
    run_request(front, APPCTL_OPTIONS, 0);
    return true;
  }
  if (in_settings && settings_category == POWER_PAGE && power_focus >= 0 &&
      (button == APPCTL_LEFT || button == APPCTL_RIGHT)) {
    power_step(power_focus, button == APPCTL_LEFT ? -1 : 1);
    return true;
  }
  if (in_settings && settings_focus >= 0 &&
      (button == APPCTL_LEFT || button == APPCTL_RIGHT)) {
    step_option(front, settings_focus, button == APPCTL_LEFT ? -1 : 1);
    return true;
  }
  /* Lists go round; the grids stop at their edges, but for the keyboard's */
  const bool list = paused || dialog;
  if (modal.kind == MODAL_KEYBOARD && button >= APPCTL_UP && button <= APPCTL_RIGHT) {
    static const nav_dir dirs[] = {[APPCTL_UP] = NAV_UP, [APPCTL_DOWN] = NAV_DOWN, [APPCTL_LEFT] = NAV_LEFT,
                                   [APPCTL_RIGHT] = NAV_RIGHT};
    if (!nav_move(ui, dirs[button]))
      nav_wrap_around(ui, dirs[button]);
    return true;
  }
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

/* Weston's socket, which may not be there yet: Weston and the launcher start together. Watched
 * before it's looked for, so its creation can't slip in between. */
static void wait_for_display(void) {
  const char *display = getenv("WAYLAND_DISPLAY"), *run = getenv("XDG_RUNTIME_DIR");
  char path[PATH_MAX];
  if (display == NULL)
    display = "wayland-0";
  if (display[0] == '/')
    snprintf(path, sizeof(path), "%s", display);
  else if (run != NULL)
    snprintf(path, sizeof(path), "%s/%s", run, display);
  else
    return;
  char dir[PATH_MAX];
  snprintf(dir, sizeof(dir), "%.*s", (int)(strrchr(path, '/') - path), path);
  const int fd = inotify_init1(IN_CLOEXEC);
  if (fd < 0 || inotify_add_watch(fd, dir, IN_CREATE) < 0) {
    if (fd >= 0)
      close(fd);
    return;
  }
  char events[1024];
  while (access(path, F_OK) != 0 && read(fd, events, sizeof(events)) > 0)
    ;
  close(fd);
}

int main(int argc, char **argv) {
  if (argc == 3 && strcmp(argv[1], "launch") == 0)
    return request_launch(argv[2]);
  if (argc == 3 && strcmp(argv[1], "screenshot") == 0)
    return capture_ppm(argv[2]) ? 0 : 1;
  if (argc != 1) {
    fprintf(stderr, "usage: launcher [launch PATH]\n");
    return 2;
  }
  if (!run_drop_root()) {
    fprintf(stderr, "launcher: can't become shell: %s\n", strerror(errno));
    return 1;
  }
  /* Vulkan, fonts and assets while Weston starts: none of it needs the display */
  if (!gfx_init(window.title) || !ui_init("sans-serif")) {
    fprintf(stderr, "launcher: can't draw\n");
    return 1;
  }
  assets_init();
  wait_for_display();
  if (!win_connect(key)) {
    fprintf(stderr, "launcher: no Wayland display\n");
    return 1;
  }
  win_map(&window);
  if ((screen = gfx_target_create(&window, false)) == NULL) {
    fprintf(stderr, "launcher: can't draw\n");
    return 1;
  }
  romdb_init(); /* its wakes need the connection */
  for (int b = 0; b < APPCTL_BUTTONS; b++)
    button_names[b] = str_c(appctl_button_names[b]);
  for (int c = 0; c < APPCTL_CATEGORIES; c++)
    category_names[c] = str_c(appctl_category_names[c]);
  category_names[POWER_PAGE] = S("Power");
  category_names[DEBUG_PAGE] = S("Debug");
  set_listening(true);
  open_pad();
  power_init();
  perf_init();
  power_use(HOME_KEY);
  clock_refresh();
  const int volume_keys = input_open(S("gpio-keys-volume"), 0);
  if (volume_keys >= 0)
    spawn("volume", volume_thread, (void *)(intptr_t)volume_keys);
  const int power_key = input_open(S("axp20x-pek"), 0);
  if (power_key >= 0)
    spawn("power key", power_key_thread, (void *)(intptr_t)power_key);
  if (!open_ctl())
    fprintf(stderr, "launcher: no launch FIFO\n");
  static nav ui = {.animate = true, .duration = 150};
  apps_init();
  apps_watch();
  sources_load();
  uint64_t present = sources_present();
  size_t page = load_page();
  rescan(page);
  status st;
  read_status(&st);
  uint64_t status_at = ms_now();
  read_usb();
  clock_refresh();
  watch_init();
  /* What the status shows changed (watch.h), kept while it can't show */
  unsigned changed = 0;
  /* Up: a slot on trial (an update) works, Wi-Fi may come on. Once sysd is there:
   * the first frame comes before rcS has started it. */
  bool told_up = false;
  bool animating = true;

  for (;;) {
    /* A frame's time while animating. Idle: only events, but once a second until sysd is up
     * and while the Wi-Fi screen lists networks. Under the app: only events. And to hide
     * the OSD. */
    /* The new app's window took the focus: no more splash */
    if (starting.n > 0 && window.configured && !window.activated && front != NULL)
      starting = S("");
    const bool seconds = !told_up || modal.kind == MODAL_WIFI;
    int wait = front != NULL && !paused && starting.n == 0 ? -1 : animating ? 16 : seconds ? 1000 : -1;
    /* Calibrating: its next step; what it found on the OSD */
    str said = S("");
    const int step = power_tick(ms_now(), &said);
    /* Up until calibration is done, then its result for a while */
    if (said.n > 0)
      show_text(said, power_calibrating() ? INT_MAX / 2 : RESULT_MS);
    /* Calibration given up (a pause, Home, the app gone): its line goes */
    if (osd_until > ms_now() + RESULT_MS && !power_calibrating())
      osd_until = ms_now();
    if (step >= 0 && (wait < 0 || step < wait))
      wait = step;
    /* The overlay: once a second */
    static uint64_t perf_due;
    if (perf_on()) {
      const int left = (int)(perf_due - MIN(perf_due, ms_now()));
      if (wait < 0 || left < wait)
        wait = left;
    }
    if (osd_until != 0) {
      const int left = (int)(osd_until - MIN(osd_until, ms_now()));
      if (wait < 0 || left < wait)
        wait = left;
    }
    /* A text scrolling: its next step (none under the app) */
    if (marquee_due != 0 && (front == NULL || paused)) {
      const int left = (int)(marquee_due - MIN(marquee_due, ms_now()));
      if (wait < 0 || left < wait)
        wait = left;
    }
    if (!win_wait(watch_fds, WATCH_FDS, wait))
      return 1;
    changed |= watch_changed();
    for (int why, value; win_woken(&why, &value); animating = true)
      if (!input(&ui, why, value, &page))
        return 0;
    const uint64_t now = ms_now();
    if (osd_until != 0 && now >= osd_until) {
      win_unmap(&osd);
      osd_until = 0;
    }
    if (perf_on() && now >= perf_due) {
      show_perf();
      perf_due = now + 1000;
    }
    /* Under the app, or hidden: nothing would show */
    if (front != NULL && !paused && starting.n == 0) {
      animating = false;
      continue;
    }
    /* Nothing pressed: not recording after all */
    if (capture.option >= 0 && capture.combo == 0 && now - capture.since >= CAPTURE_MS) {
      capture.option = -1;
      animating = true;
    }
    /* Events (input, an icon loaded, an app gone) count as changes */
    bool dirty = animating || (marquee_due != 0 && now >= marquee_due);
    const bool second = seconds && now - status_at >= 1000;
    if (second || changed != 0) {
      if (second && !told_up && (told_up = sysd(S("up")))) {
        power_apply();
        const str why = str_read(fopen(SLEEP_FAILED, "r"));
        if (why.n > 0) {
          show_failure(&ui, FAIL_SLEEP, S("Couldn't wake up"),
                       S("The handheld had to restart, so anything unsaved was lost. Light sleep avoids this "
                         "but uses more battery."),
                       S(""));
          FILE *f = fopen(SLEEP_FAILED, "w");
          if (f != NULL)
            fclose(f);
          dirty = true;
        }
        free((char *)why.p);
      }
      if (second)
        status_at = now;
      /* The minute: the clock; and the battery level and the signal, which nothing tells */
      if (changed & (WATCH_MINUTE | WATCH_POWER | WATCH_WIFI)) {
        const status was = st;
        read_status(&st);
        dirty |= (changed & WATCH_MINUTE) || was.battery != st.battery || was.charging != st.charging ||
                 was.wifi != st.wifi;
      }
      if (changed & WATCH_USB)
        dirty |= read_usb();
      /* Time's rows: the zone and NTP as `clock` left them */
      if (changed & WATCH_TIME) {
        clock_refresh();
        dirty = true;
      }
      if (second && modal.kind == MODAL_WIFI) {
        wifi_poll();
        dirty = true;
      }
      /* A card came or went, or a source was added: the page again, or
       * another if it has nothing left */
      const uint64_t now_present = changed & WATCH_MOUNTS ? sources_present() : present;
      if (now_present != present) {
        present = now_present;
        if (page >= 1 && !apps_present(&categories[page - 1]))
          turn_page(&page, 1);
        rescan(page);
        dirty = true;
      }
    }
    changed = 0;
    if (!dirty)
      continue;

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
    frame_now = now;
    marquee_due = 0;
    marquee_drawn = false;
    const entry *chosen = NULL;
    const app *picked = NULL;
    bool calibrate = false;
    const ui_rect view = {0, BAR + 1, w, h - BAR - 1};
    /* Under the power options: they take the input */
    nav_set_inert(&ui, power_menu.shown);
    if (front != NULL && !paused) {
      const asset splash = starting_image.n > 0 ? assets_get(sb_str(starting_image)) : (asset){.slot = GFX_PLACEHOLDER};
      if (splash.slot != GFX_PLACEHOLDER) {
        const float scale = MIN((float)w / splash.w, (float)h / splash.h);
        gfx_push((gfx_quad){.x = (w - splash.w * scale) / 2, .y = (h - splash.h * scale) / 2,
                            .w = splash.w * scale, .h = splash.h * scale, .u1 = 1, .v1 = 1,
                            .color = 0xffffffff, .tex = splash.slot});
      }
      ui_fill(0, h - ROW, w, ROW, SCRIM);
      ui_text(ROW_TEXT, (w - ui_text_width(ROW_TEXT, starting)) / 2, h - (ROW + ui_text_height(ROW_TEXT)) / 2,
              starting, FG);
    } else if (!paused && modal.kind >= MODAL_STORAGE) {
      chosen = folders_screen(&ui, w, h, view, &st);
    } else if (paused) {
      STRBUF(128) title;
      sb_set(title, sb_str(front->label), S(" \u00b7 "), category_names[settings_category]);
      top_bar(w, in_settings ? sb_str(title) : sb_str(front->label), &st);
      if (in_settings) {
        /* The bottom row is the hint's */
        const ui_rect list = {view.x, view.y, view.w, view.h - ROW};
        if (settings_category == POWER_PAGE)
          NAV_SCOPE(&ui, nav_int(settings_category)) { calibrate = power_page(&ui, list, true); }
        else if (settings_category == DEBUG_PAGE)
          NAV_SCOPE(&ui, nav_int(settings_category)) {
            static entry rows[PERF_METRICS + 1];
            settings_focus = power_focus = -1;
            chosen = menu(&ui, rows, perf_rows(rows), list);
          }
        else
          NAV_SCOPE(&ui, nav_int(settings_category)) { settings(&ui, front, list); }
        const bool on_binding = settings_focus >= 0 &&
                                front->shm->options[settings_focus].kind == APPCTL_BINDING;
        const bool own = settings_focus >= 0 && (front->shm->flags & APPCTL_OWN_OPTIONS);
        const str hint = capture.option >= 0 ? S("Hold the buttons, then let go of the first")
                          : on_binding && own ? S("A: record    X: clear    Y: this game")
                          : on_binding        ? S("A: record    X: clear")
                          : own               ? S("Y: this game")
                          : settings_category == POWER_PAGE && power_focus == POWER_CALIBRATE
                                              ? S("A: find the lowest clocks for this frame rate")
                                              : S("");
        if (hint.n > 0)
          ui_text(ROW_TEXT, MARGIN * 2, h - MARGIN - ui_text_height(ROW_TEXT), hint, DIM);
      } else {
        static entry items[COUNTOF(pause_entries)];
        size_t n = 0;
        for (size_t i = 0; i < COUNTOF(pause_entries); i++) {
          const int a = pause_entries[i].action;
          if (a == SETTINGS ? pause_entries[i].category >= POWER_PAGE ||
                                  (run_controlled(front) && has_settings(front, pause_entries[i].category))
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
      nav_set_inert(&ui, power_menu.shown || failure.shown || modal.kind != MODAL_NONE);
      NAV_SCOPE(&ui, nav_int(page)) {
        if (page == 0) {
          list_running();
          picked = tiles(&ui, &running_page, view);
        } else {
          picked = tiles(&ui, &categories[page - 1], view);
        }
      }
      /* Under a failure: it takes the input */
      nav_set_inert(&ui, power_menu.shown || failure.shown);
      if (modal.kind == MODAL_MENU)
        NAV_SCOPE(&ui, nav_str("start")) {
          chosen = dialog_box(&ui, w, h, S(""), S(""), system_entries, COUNTOF(system_entries));
        }
      if (modal.kind == MODAL_PORT)
        NAV_SCOPE(&ui, nav_str("port")) { chosen = port_dialog(&ui, w, h); }
      if (modal.kind == MODAL_CORES && page >= 1)
        NAV_SCOPE(&ui, nav_str("cores")) { chosen = game_dialog(&ui, w, h, &categories[page - 1]); }
      nav_set_inert(&ui, power_menu.shown);
      if (failure.shown)
        NAV_SCOPE(&ui, nav_str("failure")) { chosen = failure_dialog(&ui, w, h); }
    }
    nav_set_inert(&ui, false);
    if (power_menu.shown)
      NAV_SCOPE(&ui, nav_str("power key")) {
        chosen = dialog_box(&ui, w, h, S(""), S(""), power_entries, POWER_ENTRIES);
      }
    animating = nav_end(&ui, RING, 2);
    if (!marquee_drawn)
      marquee_of = 0;
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
    if (calibrate) {
      /* In the game, at its pace: the frames counted are its */
      resume_front();
      power_calibrate();
      show_text(power_calibrating() ? power_progress() : S("No frame counter"), power_calibrating() ? INT_MAX / 2 : RESULT_MS);
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
        apps_set_core(&categories[page - 1], a, chosen->category);
      modal.kind = MODAL_NONE;
      nav_focus(&ui, modal.back);
      break;
    }
    case MATCH: match_open(&ui, page - 1); break;
    /* From what was looked for last: the file's name, or what was typed */
    case MATCH_SEARCH:
      keyboard_open(sb_str(match.query));
      keyboard_for = MODAL_MATCH;
      modal.kind = MODAL_KEYBOARD;
      nav_focus(&ui, 0);
      break;
    /* Its new title may sort it elsewhere: the focus goes with it */
    case MATCH_PICK: {
      category *c = &categories[match.category];
      app *a = picked_game(c);
      if (a != NULL)
        apps_set_match(a, chosen->category >= 0 ? match.crcs[chosen->category] : 0);
      apps_scan(c);
      modal.kind = MODAL_NONE;
      nav_focus(&ui, modal.back);
      if ((a = picked_game(c)) != NULL)
        nav_focus(&ui, nav_child(nav_child(nav_int(page), nav_str("tiles")), nav_int(a - c->apps)));
      break;
    }
    /* The quick menu stays: closed, its last frame would show until the wake-up */
    case POWER: sysd(chosen->command); break;
    case STORAGE:
      modal.kind = MODAL_STORAGE;
      nav_focus(&ui, 0);
      break;
    case SYSTEM_SETTINGS:
      modal.kind = MODAL_SETTINGS;
      nav_focus(&ui, 0);
      break;
    case HOME_POWER:
      modal.kind = MODAL_POWER;
      nav_focus(&ui, 0);
      break;
    case ZONES: open_zones(&ui, MODAL_ZONES); break;
    case ZONE_REGION: {
      const str *regions;
      if ((size_t)chosen->category < clock_regions(&regions))
        sb_set(zone_region, regions[chosen->category]);
      open_zones(&ui, MODAL_ZONE);
      break;
    }
    case ZONE_AUTO:
    case ZONE_PICK: {
      const str *zones;
      STRBUF(96) request = {0};
      if (chosen->action == ZONE_AUTO)
        sb_set(request, S("clock zone auto"));
      else if ((size_t)chosen->category < clock_zones(sb_str(zone_region), &zones))
        sb_set(request, S("clock zone "), zones[chosen->category]);
      sysd(sb_str(request));
      open_page(&ui, MODAL_TIME, ZONES);
      break;
    }
    case NTP: sysd(clock_ntp() ? S("clock ntp off") : S("clock ntp on")); break;
    case CLOCK: clock_open(); break;
    case TIME: open_page(&ui, MODAL_TIME, 0); break;
    case DEBUG: open_page(&ui, MODAL_DEBUG, 0); break;
    case SSH: sysd(access(SSH_OFF, F_OK) != 0 ? S("ssh off") : S("ssh on")); break;
    case SSH_EVERYWHERE: sysd(access(SSH_ALL, F_OK) == 0 ? S("ssh usb") : S("ssh all")); break;
    case DEEP_SLEEP: sysd(access(LIGHT_SLEEP, F_OK) == 0 ? S("sleep deep") : S("sleep light")); break;
    case LIGHT_MODE: sysd(ui_light ? S("theme dark") : S("theme light")); break;
    case PERF:
      perf_toggle(chosen->category);
      if (perf_on())
        show_perf();
      else if (perf.configured)
        win_unmap(&perf);
      break;
    case WIFI:
      modal.kind = MODAL_WIFI;
      wifi_poll();
      nav_focus(&ui, 0);
      break;
    case WIFI_PICK:
      if ((size_t)chosen->category < wifi_count) {
        const wifi_network *net = &wifi_networks[chosen->category];
        if (net->saved || !net->secured) {
          wifi_connect(sb_str(net->ssid), net->secured, S(""));
        } else {
          sb_set(wifi_pick.ssid, sb_str(net->ssid));
          keyboard_open(S(""));
          keyboard_for = MODAL_WIFI;
          modal.kind = MODAL_KEYBOARD;
          nav_focus(&ui, 0);
        }
      }
      break;
    case UPDATES:
      modal.kind = MODAL_UPDATES;
      updates_check();
      nav_focus(&ui, 0);
      break;
    case UPDATE_CHECK: updates_check(); break;
    case UPDATE_INSTALL: updates_install(); break;
    case PORT_README:
      readme_load();
      modal.kind = MODAL_README;
      nav_forget(&ui, nav_str("readme"));
      break;
    case PORT_STORE: {
      const str dir = sb_str(modal.rom), id = str_at(dir, str_rfind(dir, '/') + 1);
      modal.kind = MODAL_NONE;
      nav_focus(&ui, modal.back);
      start(&ui, S("Store"), S(""), S("store \"$1\""), id, S(""));
      break;
    }
    case LICENSES_OPEN:
      licenses_load();
      modal.kind = MODAL_LICENSES;
      nav_focus(&ui, 0);
      break;
    case LICENSE_OPEN:
      license_shown = (size_t)chosen->category;
      modal.kind = MODAL_LICENSE;
      nav_forget(&ui, nav_str("license"));
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
    case TOGGLE_SYSTEM:
      sources_toggle(sb_str(pick_path), (size_t)chosen->category);
      changed |= WATCH_MOUNTS; /* sources_present again */
      break;
    case IGNORE: ignore(sb_str(failure.key)); /* fall through */
    case SLEEP_LIGHT:
      if (chosen->action == SLEEP_LIGHT)
        sysd(S("sleep light"));
      /* fall through */
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
      show_failure(&ui, FAIL_LAUNCH, sb_str(picked->name), sb_str(why), S(""));
    } else if (picked != NULL) {
      start(&ui, sb_str(picked->name), sb_str(picked->icon), sb_str(picked->exec),
            sb_str(picked->arg), sb_str(picked->dir));
    }
  }
}
