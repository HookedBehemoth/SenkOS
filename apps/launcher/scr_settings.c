/* System settings, time, power, and debug screens. */
#include <time.h>
#include <unistd.h>

#include "clock.h"
#include "launcher_internal.h"
#include "licenses.h"
#include "power.h"
#include "sysd.h"
#include "updates.h"
#include "wifi.h"

enum { BROWSE_MAX = 256 };
typedef STRBUF(200) row_label;

static void choose_settings(nav *ui, void *choice);
static void choose_zones(nav *ui, void *choice);
static void choose_time(nav *ui, void *choice);
static void choose_debug(nav *ui, void *choice);

static time_t wall_time(void) {
  struct timespec now;
  clock_gettime(CLOCK_REALTIME, &now);
  return now.tv_sec;
}

#define SSH_OFF "/data/OS/dropbear/disabled"
#define SSH_ALL "/data/OS/dropbear/everywhere"
#define LIGHT_SLEEP "/data/OS/config/light-sleep"
/* sysd's; the PMU gets it at shutdown (save-settings) */
#define POWER_ON_MS "/data/OS/config/power-on-ms"
#define PMU_POWER_ON "/sys/bus/platform/devices/axp717-pek/startup"

/* The power key's hold to turn on: the PMU's four (the AXP717's ONLEVEL) */
static const int power_on_times[] = {128, 512, 1000, 2000};
static const str power_on_labels[] = {S("128 ms"), S("512 ms"), S("1 s"),
                                      S("2 s")};

static int read_ms(const char *path) {
  int ms = 0;
  FILE *f = fopen(path, "r");
  if (f != NULL) {
    if (fscanf(f, "%d", &ms) != 1)
      ms = 0;
    fclose(f);
  }
  return ms;
}

/* The index in use, -1 until read */
static int power_on_at = -1;

/* The one chosen, else the PMU's, else its reset value (1 s) */
static int power_on(void) {
  if (power_on_at >= 0)
    return power_on_at;
  power_on_at = 2;
  int ms = read_ms(POWER_ON_MS);
  if (ms == 0)
    ms = read_ms(PMU_POWER_ON);
  for (int i = 0; i < (int)COUNTOF(power_on_times); i++)
    if (power_on_times[i] == ms)
      power_on_at = i;
  return power_on_at;
}

static str power_on_value(void) { return power_on_labels[power_on()]; }

static void power_on_step(int by) {
  const int count = COUNTOF(power_on_times);
  power_on_at = (power_on() + by + count) % count;
  STRBUF(32) request;
  request.n = 0;
  sb_fmt(request, "power-on %d", power_on_times[power_on_at]);
  sysd(sb_str(request));
}

enum { PAGE_MAX = 8 + PERF_ROWS };
typedef STRBUF(96) page_label;

static STRBUF(32) zone_region;

enum { CLOCK_FIELDS = 5 };
static struct {
  int field[CLOCK_FIELDS];
  int at;
} clock_edit;

static size_t page_rows(const ui_screen_def *kind, entry *rows,
                        page_label *labels) {
  size_t n = 0;
  if (kind == &time_screen) {
    const str z = clock_zone();
    sb_set(labels[0], S("Time zone  \u00b7  "), z.n > 0 ? z : S("UTC"),
           clock_zone_chosen() ? S("") : S(" (automatic)"));
    rows[n++] = (entry){
        .label = sb_str(labels[0]), .action = ZONES, .icon = ICON_GLOBE};
    rows[n++] = (entry){.label = S("Set the time automatically"),
                        .action = NTP,
                        .icon = ICON_REFRESH_CW,
                        .checked = clock_ntp()};
    if (!clock_ntp()) {
      char text[32];
      const time_t now = wall_time();
      strftime(text, sizeof(text), "%Y-%m-%d  %H:%M", localtime(&now));
      sb_set(labels[1], S("Date and time  \u00b7  "), str_c(text));
      rows[n++] = (entry){.label = sb_str(labels[1]),
                          .action = CLOCK,
                          .icon = ICON_CALENDAR_CLOCK};
    }
  } else if (kind == &debug_screen) {
    const bool on = access(SSH_OFF, F_OK) != 0;
    rows[n++] = (entry){.label = S("SSH  \u00b7  root, no password"),
                        .action = SSH,
                        .icon = ICON_NETWORK,
                        .checked = on};
    if (on)
      rows[n++] = (entry){.label = S("On every network, not just USB"),
                          .action = SSH_EVERYWHERE,
                          .icon = ICON_TRIANGLE_ALERT,
                          .checked = access(SSH_ALL, F_OK) == 0,
                          .depth = 1};
    rows[n++] = (entry){.label = S("Deep sleep  \u00b7  saves battery"),
                        .action = DEEP_SLEEP,
                        .icon = ICON_MOON,
                        .checked = access(LIGHT_SLEEP, F_OK) != 0};
    rows[n++] = (entry){.label = S("Hold power to turn on"),
                        .action = POWER_ON,
                        .icon = ICON_POWER,
                        .value = power_on_value()};
    n += launcher_perf_rows(rows + n);
  } else {
    rows[n++] = (entry){
        .label = S("Storage"), .action = STORAGE, .icon = ICON_HARD_DRIVE};
    rows[n++] = (entry){.label = S("Wi-Fi"), .action = WIFI, .icon = ICON_WIFI};
    rows[n++] = (entry){.label = S("Time"), .action = TIME, .icon = ICON_CLOCK};
    rows[n++] = (entry){.label = S("Light mode"),
                        .action = LIGHT_MODE,
                        .icon = ICON_SUN,
                        .checked = ui_light};
    rows[n++] = (entry){
        .label = S("Updates"), .action = UPDATES, .icon = ICON_DOWNLOAD};
    rows[n++] = (entry){.label = S("Debug"), .action = DEBUG, .icon = ICON_CPU};
    rows[n++] = (entry){.label = S("Licenses"),
                        .action = LICENSES_OPEN,
                        .icon = ICON_SCROLL_TEXT};
  }
  return n;
}

static const char *page_scope(const ui_screen_def *kind) {
  return kind == &time_screen    ? "time"
         : kind == &debug_screen ? "debug"
                                 : "system";
}

static void open_page(launcher_state *state, nav *ui, const ui_screen_def *kind,
                      int action) {
  entry rows[PAGE_MAX];
  page_label *labels = NAV_FRAME(ui, page_label, 2);
  const size_t n = page_rows(kind, rows, labels);
  screen_push(state->visible, ui, kind, 0);
  for (size_t i = 0; i < n; i++)
    if (rows[i].action == action)
      nav_focus(ui, menu_row(nav_str(page_scope(kind)), i));
}

static void open_zones(launcher_state *state, nav *ui,
                       const ui_screen_def *kind) {
  const str *names;
  const str zone = clock_zone();
  const size_t n = kind == &zones_screen
                       ? clock_regions(&names)
                       : clock_zones(sb_str(zone_region), &names);
  const str want =
      kind == &zones_screen ? str_head(zone, str_find(zone, '/')) : zone;
  screen_push(state->visible, ui, kind, 0);
  for (size_t i = 0; i < n; i++)
    if (str_eq(names[i], want))
      nav_focus(ui, menu_row(nav_str(kind == &zones_screen ? "zones" : "zone"),
                             i + (kind == &zones_screen)));
}

static void clock_open(launcher_state *state, nav *ui) {
  const time_t now = wall_time();
  const struct tm *t = localtime(&now);
  clock_edit = (typeof(clock_edit)){
      {t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, t->tm_hour, t->tm_min}, 0};
  screen_push(state->visible, ui, &clock_screen, 0);
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
  const int now_days =
      mdays[f[1] - 1] +
      (f[1] == 2 && ((f[0] % 4 == 0 && f[0] % 100 != 0) || f[0] % 400 == 0));
  f[2] = MIN(f[2], now_days);
}

static void clock_set(void) {
  const int *f = clock_edit.field;
  struct tm t = {.tm_year = f[0] - 1900,
                 .tm_mon = f[1] - 1,
                 .tm_mday = f[2],
                 .tm_hour = f[3],
                 .tm_min = f[4],
                 .tm_isdst = -1};
  const time_t when = mktime(&t);
  STRBUF(48) request;
  request.n = 0;
  sb_fmt(request, "clock set %lld", (long long)when);
  if (when != (time_t)-1)
    sysd(sb_str(request));
}

static void clock_dialog(int w, int h) {
  ui_fill(0, 0, w, h, SCRIM);
  static const char *const after[CLOCK_FIELDS] = {"-", "-", "", ":", ""};
  const int text = ui_text_height(ROW_TEXT),
            digits = ui_text_width(ROW_TEXT, S("00")) + MARGIN;
  const float field_w[CLOCK_FIELDS] = {ui_text_width(ROW_TEXT, S("0000")) +
                                           MARGIN,
                                       digits, digits, digits, digits};
  const float sep = ui_text_width(ROW_TEXT, S(" - ")), gap = MARGIN * 4;
  float total = gap;
  for (int i = 0; i < CLOCK_FIELDS; i++)
    total += field_w[i] + (after[i][0] ? sep : 0);
  const float dw = MAX(total + MARGIN * 4, 320),
              dh = MARGIN * 5 + text * 3 + 4 + ROW;
  const float x = (w - dw) / 2, y = (h - dh) / 2;
  ui_rounded(x, y, dw, dh, TILE, RADIUS, 0);
  ui_rounded(x, y, dw, dh, EDGE, RADIUS, 1);
  ui_text(ROW_TEXT, (int)x + MARGIN * 2, (int)y + MARGIN * 2,
          S("Date and time"), FG);
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
    ui_text(
        ROW_TEXT,
        (int)(fx + (field_w[i] - ui_text_width(ROW_TEXT, sb_str(value))) / 2),
        (int)(fy + (fh - text) / 2), sb_str(value), FG);
    fx += field_w[i];
    if (after[i][0]) {
      const str a = str_c(after[i]);
      ui_text(ROW_TEXT, (int)(fx + (sep - ui_text_width(ROW_TEXT, a)) / 2),
              (int)(fy + (fh - text) / 2), a, DIM);
      fx += sep;
    }
    if (i == 2)
      fx += gap;
  }
}

static void page_frame(ui_screen *screen, nav *ui, ui_rect content) {
  entry *rows = NAV_FRAME(ui, entry, PAGE_MAX);
  page_label *labels = NAV_FRAME(ui, page_label, 2);
  const size_t n = page_rows(screen->def, rows, labels);
  const entry *selected = menu(ui, rows, n, content);
  if (selected != NULL)
    nav_defer(ui,
              screen->def == &time_screen    ? choose_time
              : screen->def == &debug_screen ? choose_debug
                                             : choose_settings,
              selected, sizeof(*selected));
}

static void home_power_frame(ui_screen *screen, nav *ui, ui_rect content) {
  (void)screen;

  launcher_power_page(ui, content, false);
}

static void zones_frame(ui_screen *screen, nav *ui, ui_rect content) {
  (void)screen;
  entry *rows = NAV_FRAME(ui, entry, BROWSE_MAX);

  const str *regions;
  const size_t n = MIN(clock_regions(&regions), BROWSE_MAX - 1);
  rows[0] = (entry){.label = S("Automatic"),
                    .action = ZONE_AUTO,
                    .icon = ICON_GLOBE,
                    .checked = !clock_zone_chosen()};
  for (size_t i = 0; i < n; i++)
    rows[i + 1] = (entry){
        .label = regions[i],
        .action = ZONE_REGION,
        .icon = ICON_FOLDER,
        .arg = (int)i,
        .checked = clock_zone_chosen() &&
                   str_eq(regions[i],
                          str_head(clock_zone(), str_find(clock_zone(), '/')))};
  const entry *selected = menu(ui, rows, n + 1, content);
  if (selected != NULL)
    nav_defer(ui, choose_zones, selected, sizeof(*selected));
}

static void zone_frame(ui_screen *screen, nav *ui, ui_rect content) {
  (void)screen;
  entry *rows = NAV_FRAME(ui, entry, BROWSE_MAX);
  row_label *labels = NAV_FRAME(ui, row_label, BROWSE_MAX);

  const str *zones;
  const size_t n = MIN(clock_zones(sb_str(zone_region), &zones), BROWSE_MAX);
  for (size_t i = 0; i < n; i++) {
    labels[i].n = 0;
    const str place = str_at(zones[i], zone_region.n + 1);
    for (size_t k = 0; k < place.n; k++)
      sb_add(labels[i], place.p[k] == '_'   ? S(" ")
                        : place.p[k] == '/' ? S(" / ")
                                            : str_head(str_at(place, k), 1));
    rows[i] = (entry){.label = sb_str(labels[i]),
                      .action = ZONE_PICK,
                      .icon = ICON_CLOCK,
                      .arg = (int)i,
                      .checked = str_eq(zones[i], clock_zone())};
  }
  const entry *selected = menu(ui, rows, n, content);
  if (selected != NULL)
    nav_defer(ui, choose_zones, selected, sizeof(*selected));
}

static void clock_frame(ui_screen *screen, nav *ui, ui_rect content) {
  (void)screen;
  launcher_state *state = &launcher;
  (void)ui;
  (void)content;
  clock_dialog(state->draw.w, state->draw.h);
}

static bool clock_button(ui_screen *screen, nav *ui, int button) {
  (void)screen;
  launcher_state *state = &launcher;
  if (button == APPCTL_B)
    return false;
  switch (button) {
  case APPCTL_LEFT:
    clock_edit.at = MAX(clock_edit.at - 1, 0);
    break;
  case APPCTL_RIGHT:
    clock_edit.at = MIN(clock_edit.at + 1, CLOCK_FIELDS - 1);
    break;
  case APPCTL_UP:
    clock_step(1);
    break;
  case APPCTL_DOWN:
    clock_step(-1);
    break;
  case APPCTL_A:
    clock_set();
    screen_pop(state->visible, ui);
    break;
  }
  return true;
}

static bool power_button(ui_screen *screen, nav *ui, int button) {
  (void)screen;
  (void)ui;
  if (launcher_power_focus < 0 ||
      (button != APPCTL_LEFT && button != APPCTL_RIGHT))
    return false;
  power_step(launcher_power_focus, button == APPCTL_LEFT ? -1 : 1);
  return true;
}

static bool debug_button(ui_screen *screen, nav *ui, int button) {
  entry rows[PAGE_MAX];
  page_label labels[2];
  (void)ui;
  if (menu_focus < 0 || (button != APPCTL_LEFT && button != APPCTL_RIGHT) ||
      (size_t)menu_focus >= page_rows(screen->def, rows, labels) ||
      rows[menu_focus].action != POWER_ON)
    return false;
  power_on_step(button == APPCTL_LEFT ? -1 : 1);
  return true;
}

static void choose_settings(nav *ui, void *choice) {
  launcher_state *state = &launcher;
  const entry *chosen = choice;
  switch (chosen->action) {
  case STORAGE:
    screen_push(state->visible, ui, &storage_screen, 0);
    break;
  case WIFI:
    screen_push(state->visible, ui, &wifi_screen, 0);
    wifi_poll();
    break;
  case TIME:
    open_page(state, ui, &time_screen, 0);
    break;
  case LIGHT_MODE:
    sysd(ui_light ? S("theme dark") : S("theme light"));
    break;
  case UPDATES:
    screen_push(state->visible, ui, &updates_screen, 0);
    updates_check();
    break;
  case DEBUG:
    open_page(state, ui, &debug_screen, 0);
    break;
  case LICENSES_OPEN:
    licenses_load();
    screen_push(state->visible, ui, &licenses_screen, 0);
    break;
  default:
    break;
  }
}

static void choose_zones(nav *ui, void *choice) {
  launcher_state *state = &launcher;
  const entry *chosen = choice;
  switch (chosen->action) {
  case ZONE_REGION: {
    const str *regions;
    if ((size_t)chosen->arg < clock_regions(&regions))
      sb_set(zone_region, regions[chosen->arg]);
    open_zones(state, ui, &zone_screen);
    break;
  }
  case ZONE_AUTO:
  case ZONE_PICK: {
    const str *zones;
    STRBUF(96) request = {0};
    if (chosen->action == ZONE_AUTO)
      sb_set(request, S("clock zone auto"));
    else if ((size_t)chosen->arg < clock_zones(sb_str(zone_region), &zones))
      sb_set(request, S("clock zone "), zones[chosen->arg]);
    sysd(sb_str(request));
    if (screen_current(state->visible) == &zone_screen)
      screen_pop(state->visible, ui);
    screen_pop(state->visible, ui);
    break;
  }
  default:
    break;
  }
}

static void choose_time(nav *ui, void *choice) {
  launcher_state *state = &launcher;
  const entry *chosen = choice;
  switch (chosen->action) {
  case ZONES:
    open_zones(state, ui, &zones_screen);
    break;
  case NTP:
    sysd(clock_ntp() ? S("clock ntp off") : S("clock ntp on"));
    break;
  case CLOCK:
    clock_open(state, ui);
    break;
  default:
    break;
  }
}

static void choose_debug(nav *ui, void *choice) {
  (void)ui;
  const entry *chosen = choice;
  switch (chosen->action) {
  case SSH:
    sysd(access(SSH_OFF, F_OK) != 0 ? S("ssh off") : S("ssh on"));
    break;
  case SSH_EVERYWHERE:
    sysd(access(SSH_ALL, F_OK) == 0 ? S("ssh usb") : S("ssh all"));
    break;
  case DEEP_SLEEP:
    sysd(access(LIGHT_SLEEP, F_OK) == 0 ? S("sleep deep") : S("sleep light"));
    break;
  case POWER_ON:
    power_on_step(1);
    break;
  case PERF_CORNER:
    launcher_perf_action(PERF_CORNER, 0);
    break;
  case PERF:
    launcher_perf_action(PERF, chosen->arg);
    break;
  default:
    break;
  }
}

static str zone_title(const ui_screen *screen) {
  (void)screen;
  return sb_str(zone_region);
}
static str clock_hint(const ui_screen *screen, const nav *ui) {
  (void)screen;
  (void)ui;
  return S("Up, down: change    Left, right: move");
}

const ui_screen_def settings_screen = {
    .name = "system", .frame = page_frame, .title_text = S("Settings")};
const ui_screen_def power_screen = {.name = "home power",
                                    .frame = home_power_frame,
                                    .button = power_button,
                                    .title_text = S("Power")};
const ui_screen_def zones_screen = {
    .name = "zones", .frame = zones_frame, .title_text = S("Time zone")};
const ui_screen_def zone_screen = {
    .name = "zone", .frame = zone_frame, .title = zone_title};
const ui_screen_def clock_screen = {.name = "clock",
                                    .overlay = true,
                                    .frame = clock_frame,
                                    .button = clock_button,
                                    .hint = clock_hint};
const ui_screen_def time_screen = {
    .name = "time", .frame = page_frame, .title_text = S("Time")};
const ui_screen_def debug_screen = {
    .name = "debug",
    .frame = page_frame,
    .button = debug_button,
    .title_text = S("Debug")};
