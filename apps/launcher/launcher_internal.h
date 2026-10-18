/* Shared launcher state and screen interfaces. */
#pragma once

#include <stdint.h>

#include "apps.h"
#include "gfx.h"
#include "nav.h"
#include "perf.h"
#include "running.h"
#include "screen.h"
#include "ui.h"

enum {
  BAR = 32,
  BAR_TEXT = 18,
  BAR_ICON = 18,
  ROW = 44,
  ROW_TEXT = 22,
  ICON_PX = 22,
  MARGIN = 12
};
enum { TREE = ICON_PX + MARGIN };
enum {
  TILE_W = 144,
  TILE_H = 160,
  ICON = 128,
  TILE_TEXT = 16,
  LETTER = 40,
  RADIUS = 12,
  GAP = 8
};
enum { OSD_W = 320, OSD_H = 40, OSD_TOP = 40, OSD_MS = 1500, TRACK_MS = 3000, RESULT_MS = 5000 };
enum {
  SPLASH_PX = 256,
  PERF_W = 200,
  PERF_TEXT = 14,
  PERF_LINE = 17,
  LICENSE_TEXT = 16
};
enum { PERF_ROWS = PERF_METRICS + 2 };
enum { POWER_PAGE = APPCTL_CATEGORIES, DEBUG_PAGE };
enum { FAIL_LAUNCH, FAIL_SLEEP };

#define BG ui_pal.bg
#define FG ui_pal.fg
#define DIM ui_pal.faint
#define MUTED ui_pal.dim
#define FOCUS ui_pal.focus
#define TILE ui_pal.tile
#define EDGE ui_pal.edge
#define SCRIM ((gfx_color){BG.r, BG.g, BG.b, 0xb0})
#define LOW ui_pal.bad
#define CHARGING ui_pal.ok
#define RING ui_pal.accent
#define RING_TEXT ui_pal.accent
#define ACTIVE ui_pal.accent
#define PER_GAME ui_pal.accent
#define OSD_BG ((gfx_color){0x14, 0x14, 0x18, 0xc0})
#define OSD_FG ((gfx_color){0xee, 0xee, 0xee, 0xff})
#define OSD_TRACK ((gfx_color){0x55, 0x55, 0x5c, 0xff})

enum {
  RESUME,
  RESTART,
  SAVE_STATE,
  LOAD_STATE,
  SETTINGS,
  HOME,
  QUIT,
  DISMISS,
  IGNORE,
  PICK_CORE,
  STORAGE,
  BROWSE,
  BROWSE_OPEN,
  TOGGLE_SYSTEM,
  POWER,
  LICENSES_OPEN,
  LICENSE_OPEN,
  WIFI,
  WIFI_PICK,
  WIFI_ADDING,
  WIFI_ADD,
  BLUETOOTH,
  BT_TOGGLE,
  BT_PICK,
  BT_PAIRING,
  BT_PAIR,
  UPDATES,
  UPDATE_CHECK,
  UPDATE_INSTALL,
  NOTHING,
  SYSTEM_SETTINGS,
  HOME_POWER,
  HEADER,
  ZONES,
  ZONE_AUTO,
  ZONE_REGION,
  ZONE_PICK,
  NTP,
  CLOCK,
  TIME,
  DEBUG,
  SSH,
  SSH_EVERYWHERE,
  SLEEP_LIGHT,
  DEEP_SLEEP,
  LIGHT_MODE,
  POWER_ON,
  PERF,
  PERF_CORNER,
  MATCH,
  MATCH_SEARCH,
  MATCH_PICK,
  LAUNCH_APP,
  LAUNCH_RUNNING,
  CONFIRM,
  GET_PACK,
  SCREEN_SLEEP,
  IDLE_ROW,
  SETTINGS_LIST
};

typedef struct {
  str label;
  /* POWER's: a call to sysd */
  bool (*sysd)(void);
  int action;
  int icon;
  intptr_t arg;
  bool checked;
  int depth;
  /* Shown on the right as "‹ value ›": Left and Right change it */
  str value;
} entry;

typedef struct {
  int w, h;
  size_t page;
  bool calibrate;
} draw_state;

typedef STRBUF(512) selected_path;
typedef STRBUF(128) notice_buffer;
typedef struct {
  int option;
  uint16_t held, combo;
  int first;
  uint64_t since;
} capture_state;

typedef struct {
  draw_state draw;
  running *front;
  screen_stack *visible;
  selected_path selected_rom;
  notice_buffer notice;
  capture_state capture;
  unsigned *changed;
} launcher_state;

extern launcher_state launcher;
extern int tiles_focus, menu_focus;
extern str category_names[DEBUG_PAGE + 1];
extern str button_names[APPCTL_BUTTONS];
extern win window;
void widgets_frame_begin(uint64_t now);
void widgets_frame_end(void);
uint64_t widgets_due(void);
extern const ui_screen_def licenses_screen;
extern const ui_screen_def license_screen;
extern const ui_screen_def updates_screen;
extern const ui_screen_def wifi_screen;
extern const ui_screen_def wifi_adding_screen;
extern const ui_screen_def bluetooth_screen;
extern const ui_screen_def bt_pairing_screen;
extern const ui_screen_def keyboard_screen;
extern const ui_screen_def storage_screen;
extern const ui_screen_def browse_screen;
extern const ui_screen_def systems_screen;
extern const ui_screen_def settings_screen;
extern const ui_screen_def power_screen;
extern const ui_screen_def zones_screen;
extern const ui_screen_def zone_screen;
extern const ui_screen_def clock_screen;
extern const ui_screen_def time_screen;
extern const ui_screen_def debug_screen;
extern const ui_screen_def screen_sleep_screen;
extern const ui_screen_def home_screen;
extern const ui_screen_def quick_menu_screen;
extern const ui_screen_def cores_screen;
extern const ui_screen_def match_screen;
extern const ui_screen_def pause_screen;
extern const ui_screen_def pause_list_screen;
extern const ui_screen_def pause_settings_screen;
extern const ui_screen_def confirm_screen;
extern const ui_screen_def failure_screen;
extern const ui_screen_def power_key_screen;

bool storage_refresh_usb(void);
size_t launcher_perf_rows(entry *rows);
void launcher_perf_action(int action, intptr_t arg);
void osd_flush(void);
bool launcher_power_page(nav *ui, ui_rect view, bool calibration);
extern int launcher_power_focus;
void launcher_show_failure(nav *ui, int kind, str title, str why, str key);
bool launcher_capture_active(void);
void launcher_set_binding(running *r, int i, uint16_t combo);
void launcher_step_option(running *r, int i, int by);
void launcher_ignore(str key);

const entry *menu(nav *ui, const entry *entries, size_t count, ui_rect view);
int launcher_tiles(nav *ui, const category *category, ui_rect view);
const entry *dialog_box(nav *ui, int w, int h, str title, str text,
                        const entry *entries, size_t count);
nav_id menu_row(nav_id scope, size_t i);
int focused_row(const nav *ui, nav_id scope, size_t count);
void go_home(nav *ui);
void resume_front(nav *ui);
void quit_front(nav *ui);
void show_perf(void);
bool showing_pause(void);
screen_stack *session_home_screens(void);
void session_start(nav *ui, str label, str icon, str exec, str arg, str dir, unsigned flags);
void session_pause(nav *ui);
void session_app_exited(nav *ui, running *r, size_t *page);
bool session_starting(void);
void session_check_start(void);
void session_draw_backdrop(int w, int h);
void session_draw_starting(int w, int h);
void pad_listen(bool on);
void pad_start(void);
void pad_grab(bool on);
size_t home_load_page(void);
void home_save_page(size_t page);
void home_rescan(size_t page);
void home_turn_page(nav *ui, size_t *page, int by);
bool control_open(void);
int control_request_launch(const char *arg);
void control_launch_path(nav *ui);
void launcher_key(unsigned code, bool down);
bool launcher_input(nav *ui, int why, int value, size_t *page);
void osd_show_level(bool brightness, int percent);
void osd_show_text(str text, int ms);
/* A new track: its title and artist */
void osd_show_track(str title, str artist);
uint64_t osd_due(void);
void osd_tick(uint64_t now);
void osd_limit_notice(void);
bool input_capture_expire(uint64_t now);
