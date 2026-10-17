/* Wi-Fi selection and shared text entry. */
#include <stdio.h>

#include "keyboard.h"
#include "launcher_internal.h"
#include "sysd.h"
#include "wifi.h"

typedef STRBUF(200) row_label;

static void choose_wifi(nav *ui, void *data);

/* A choice, with its network: the list may change before it's run */
typedef struct {
  int action;
  bool checked;
  STRBUF(64) path;
  wifi_found net;
} choice;

/* The network the keyboard asks the password of */
static wifi_found wifi_pick;

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

static void status_line(ui_rect *content) {
  char status[160];
  const int line = ui_text_height(ROW_TEXT) + MARGIN;
  ui_text(ROW_TEXT, MARGIN * 2, (int)content->y + MARGIN,
          ui_text_fit(ROW_TEXT, wifi_status(status, sizeof(status)), launcher.draw.w - MARGIN * 4, NULL), DIM);
  content->y += line;
  content->h -= line;
}

static void pick(nav *ui, const entry *rows, size_t n, ui_rect content, const choice *choices) {
  const entry *selected = menu(ui, rows, n, content);
  if (selected != NULL)
    nav_defer(ui, choose_wifi, &choices[selected - rows], sizeof(choice));
}

static void wifi_frame(ui_screen *screen, nav *ui, ui_rect content) {
  (void)screen;
  entry *rows = NAV_FRAME(ui, entry, WIFI_MAX + 2);
  choice *choices = NAV_FRAME(ui, choice, WIFI_MAX + 2);
  wifi_saved *saved = NAV_FRAME(ui, wifi_saved, WIFI_MAX);
  row_label *labels = NAV_FRAME(ui, row_label, WIFI_MAX);

  const bool on = wifi_on();
  size_t n = 0;
  choices[n] = (choice){.action = POWER, .checked = on};
  rows[n++] = (entry){.label = S("Wi-Fi"), .sysd = on ? sysd_wifi_off : sysd_wifi_on, .action = POWER,
                      .icon = on ? ICON_WIFI : ICON_WIFI_OFF, .checked = on};
  const size_t count = on ? wifi_saved_list(saved, WIFI_MAX) : 0;
  const int bars = wifi_signal();
  static const int arcs[] = {ICON_WIFI_ZERO, ICON_WIFI_LOW, ICON_WIFI_HIGH, ICON_WIFI, ICON_WIFI};
  for (size_t i = 0; i < count; i++) {
    const wifi_saved *w = &saved[i];
    sb_set(labels[i], sb_str(w->name),
           w->connected ? S("  ·  connected") : w->current ? S("  ·  …") : S(""));
    choices[n] = (choice){.action = WIFI_PICK};
    sb_set(choices[n].path, sb_str(w->path));
    rows[n++] = (entry){.label = sb_str(labels[i]), .action = WIFI_PICK,
                        .icon = w->connected ? arcs[CLAMP(bars, 0, 4)] : ICON_WIFI, .checked = w->connected};
  }
  if (on) {
    choices[n] = (choice){.action = WIFI_ADDING};
    rows[n++] = (entry){.label = S("Add network…"), .action = WIFI_ADDING, .icon = ICON_SEARCH};
  }
  status_line(&content);
  pick(ui, rows, n, content, choices);
}

/* The focused saved network */
static bool focused_saved(const nav *ui, wifi_saved *out) {
  wifi_saved saved[WIFI_MAX];
  const size_t count = wifi_on() ? wifi_saved_list(saved, WIFI_MAX) : 0;
  const int row = focused_row(ui, nav_str("wifi"), count + 2) - 1;
  if (row < 0 || (size_t)row >= count)
    return false;
  *out = saved[row];
  return true;
}

static str wifi_hint(const ui_screen *screen, const nav *ui) {
  (void)screen;
  wifi_saved w;
  return focused_saved(ui, &w) ? S("X: forget") : S("");
}

static bool wifi_button(ui_screen *screen, nav *ui, int button) {
  (void)screen;
  if (button != APPCTL_X)
    return false;
  wifi_saved w;
  if (focused_saved(ui, &w))
    wifi_forget(w.path.p);
  return true;
}

static void adding_frame(ui_screen *screen, nav *ui, ui_rect content) {
  (void)screen;
  launcher_state *state = &launcher;
  if (wifi_added_new()) {
    screen_pop(state->visible, ui);
    return;
  }
  entry *rows = NAV_FRAME(ui, entry, WIFI_MAX);
  choice *choices = NAV_FRAME(ui, choice, WIFI_MAX);
  wifi_found *found = NAV_FRAME(ui, wifi_found, WIFI_MAX);
  row_label *labels = NAV_FRAME(ui, row_label, WIFI_MAX);

  static const int arcs[] = {ICON_WIFI_ZERO, ICON_WIFI_LOW, ICON_WIFI_HIGH, ICON_WIFI, ICON_WIFI};
  const size_t count = wifi_found_list(found, WIFI_MAX);
  for (size_t i = 0; i < count; i++) {
    const wifi_found *w = &found[i];
    sb_set(labels[i], sb_str(w->name), w->gone ? S("  ·  gone") : !w->secured ? S("  ·  open") : S(""));
    choices[i] = (choice){.action = WIFI_ADD, .net = *w};
    rows[i] = (entry){.label = sb_str(labels[i]), .action = WIFI_ADD, .icon = arcs[CLAMP(w->bars, 0, 4)]};
  }
  const int line = ui_text_height(ROW_TEXT) + MARGIN;
  ui_text(ROW_TEXT, MARGIN * 2, (int)content.y + MARGIN,
          !wifi_on() ? S("Wi-Fi is off") : S("Searching…"), DIM);
  content.y += line;
  content.h -= line;
  pick(ui, rows, count, content, choices);
}

static void adding_text(ui_screen *screen, nav *ui, str value) {
  (void)screen, (void)ui;
  wifi_add(&wifi_pick, value);
}

static void keyboard_frame(ui_screen *screen, nav *ui, ui_rect content) {
  const bool search = keyboard_parent(screen) == &match_screen;
  STRBUF(200) label;
  if (search)
    sb_set(label, S("Search "), categories[screen->arg].name);
  else
    sb_set(label, S("Password for "), sb_str(wifi_pick.name));
  if (keyboard(ui, content, sb_str(label)))
    keyboard_submit(screen, ui);
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

static void choose_wifi(nav *ui, void *data) {
  launcher_state *state = &launcher;
  const choice *c = data;
  switch (c->action) {
  case POWER:
    (c->checked ? sysd_wifi_off : sysd_wifi_on)();
    break;
  case WIFI_PICK:
    wifi_select(c->path.p);
    break;
  case WIFI_ADDING:
    screen_push(state->visible, ui, &wifi_adding_screen, 0);
    break;
  case WIFI_ADD:
    if (c->net.secured) {
      wifi_pick = c->net;
      keyboard_open(S(""));
      screen_push(state->visible, ui, &keyboard_screen, 0);
    } else
      wifi_add(&c->net, S(""));
    break;
  default:
    break;
  }
}

const ui_screen_def wifi_screen = {.name = "wifi",
                                   .frame = wifi_frame,
                                   .button = wifi_button,
                                   .title_text = S("Wi-Fi"),
                                   .hint = wifi_hint};
const ui_screen_def wifi_adding_screen = {
    .name = "wifi-adding", .frame = adding_frame, .title_text = S("Add network"), .text = adding_text};
const ui_screen_def keyboard_screen = {.name = "keyboard",
                                       .frame = keyboard_frame,
                                       .button = keyboard_button,
                                       .title = keyboard_title,
                                       .hint = keyboard_hint};
