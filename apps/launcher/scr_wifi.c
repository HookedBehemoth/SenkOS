/* Wi-Fi selection and shared text entry. */
#include <stdio.h>

#include "keyboard.h"
#include "launcher_internal.h"
#include "sysd.h"
#include "wifi.h"

typedef STRBUF(200) row_label;

static void choose_wifi(nav *ui, void *choice);

static struct {
  STRBUF(160) ssid;
} wifi_pick;

static const ui_screen_def *keyboard_parent(const ui_screen *screen) {
  (void)screen;
  launcher_state *state = &launcher;
  return state->visible && state->visible->count > 1
             ? state->visible->items[state->visible->count - 2].def
             : &home_screen;
}

static void keyboard_submit(ui_screen *screen, nav *ui) {
  (void)screen;
  launcher_state *state = &launcher;
  const str value = keyboard_text();
  screen_pop(state->visible, ui);
  ui_screen *parent = screen_top(state->visible);
  if (parent != NULL && parent->def->text != NULL)
    parent->def->text(parent, ui, value);
}

static void wifi_text(ui_screen *screen, nav *ui, str value) {
  (void)screen;
  (void)ui;
  if (!wifi_connect(sb_str(wifi_pick.ssid), true, value))
    fprintf(stderr, "launcher: wifi: couldn't add %s\n", wifi_pick.ssid.p);
}

static void wifi_frame(ui_screen *screen, nav *ui, ui_rect content) {
  (void)screen;
  launcher_state *state = &launcher;
  entry *rows = NAV_FRAME(ui, entry, (WIFI_MAX + 1));
  row_label *labels = NAV_FRAME(ui, row_label, WIFI_MAX);

  const bool on = wifi_on();
  size_t n = 0;
  rows[n++] = (entry){.label = S("Wi-Fi"),
                      .sysd = on ? sysd_wifi_off : sysd_wifi_on,
                      .action = POWER,
                      .icon = on ? ICON_WIFI : ICON_WIFI_OFF,
                      .checked = on};
  for (size_t i = 0; i < wifi_count; i++) {
    const wifi_network *net = &wifi_networks[i];
    sb_set(labels[i], sb_str(net->ssid),
           net->saved      ? S("  \u00b7  saved")
           : !net->secured ? S("  \u00b7  open")
                           : S(""));
    static const int arcs[] = {ICON_WIFI_ZERO, ICON_WIFI_LOW, ICON_WIFI_HIGH,
                               ICON_WIFI, ICON_WIFI};
    rows[n++] = (entry){.label = sb_str(labels[i]),
                        .action = WIFI_PICK,
                        .icon = arcs[CLAMP(net->bars, 0, 4)],
                        .arg = (int)i,
                        .checked = net->current};
  }
  const int line = ui_text_height(ROW_TEXT) + MARGIN;
  ui_text(
      ROW_TEXT, MARGIN * 2, (int)content.y + MARGIN,
      ui_text_fit(ROW_TEXT, wifi_status(), state->draw.w - MARGIN * 4, NULL),
      DIM);
  const ui_rect below = {content.x, content.y + line, content.w,
                         content.h - line};
  const entry *selected = menu(ui, rows, n, below);
  if (selected != NULL)
    nav_defer(ui, choose_wifi, selected, sizeof(*selected));
}

static void keyboard_frame(ui_screen *screen, nav *ui, ui_rect content) {
  const bool search = keyboard_parent(screen) == &match_screen;
  STRBUF(200) label;
  if (search)
    sb_set(label, S("Search "), categories[screen->arg].name);
  else
    sb_set(label, S("Password for "), sb_str(wifi_pick.ssid));
  if (keyboard(ui, content, sb_str(label)))
    keyboard_submit(screen, ui);
}

static str wifi_hint(const ui_screen *screen, const nav *ui) {
  (void)screen;
  const int row = focused_row(ui, nav_str("wifi"), wifi_count + 1);
  return row > 0 && wifi_networks[row - 1].saved ? S("X: forget") : S("");
}
static str keyboard_title(const ui_screen *screen) {
  return keyboard_parent(screen) == &match_screen ? S("Title and box art")
                                                  : S("Wi-Fi");
}
static str keyboard_hint(const ui_screen *screen, const nav *ui) {
  (void)screen;
  (void)ui;
  return S("X: delete    Y: shift    Start: done");
}

static bool keyboard_button(ui_screen *screen, nav *ui, int button) {
  (void)screen;
  if (button >= APPCTL_UP && button <= APPCTL_RIGHT) {
    static const nav_dir dirs[] = {[APPCTL_UP] = NAV_UP,
                                   [APPCTL_DOWN] = NAV_DOWN,
                                   [APPCTL_LEFT] = NAV_LEFT,
                                   [APPCTL_RIGHT] = NAV_RIGHT};
    if (!nav_move(ui, dirs[button]))
      nav_wrap_around(ui, dirs[button]);
    return true;
  }
  switch (button) {
  case APPCTL_X:
    keyboard_delete();
    return true;
  case APPCTL_Y:
    keyboard_shift();
    return true;
  case APPCTL_START:
    keyboard_submit(screen, ui);
    return true;
  default:
    return false;
  }
}

static bool wifi_button(ui_screen *screen, nav *ui, int button) {
  (void)screen;
  if (button != APPCTL_X)
    return false;
  const int row = focused_row(ui, nav_str("wifi"), wifi_count + 1);
  if (row > 0 && wifi_networks[row - 1].saved) {
    wifi_forget(sb_str(wifi_networks[row - 1].ssid));
    wifi_poll();
  }
  return true;
}

static void choose_wifi(nav *ui, void *choice) {
  launcher_state *state = &launcher;
  const entry *chosen = choice;
  switch (chosen->action) {
  case POWER:
    chosen->sysd();
    break;
  case WIFI_PICK:
    if ((size_t)chosen->arg < wifi_count) {
      const wifi_network *net = &wifi_networks[chosen->arg];
      if (net->saved || !net->secured) {
        wifi_connect(sb_str(net->ssid), net->secured, S(""));
      } else {
        sb_set(wifi_pick.ssid, sb_str(net->ssid));
        keyboard_open(S(""));
        screen_push(state->visible, ui, &keyboard_screen, 0);
      }
    }
    break;
  default:
    break;
  }
}

const ui_screen_def wifi_screen = {.name = "wifi",
                                   .frame = wifi_frame,
                                   .button = wifi_button,
                                   .title_text = S("Wi-Fi"),
                                   .hint = wifi_hint,
                                   .text = wifi_text};
const ui_screen_def keyboard_screen = {.name = "keyboard",
                                       .frame = keyboard_frame,
                                       .button = keyboard_button,
                                       .title = keyboard_title,
                                       .hint = keyboard_hint};
