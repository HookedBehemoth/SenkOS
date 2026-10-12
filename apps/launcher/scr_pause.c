/* Pause menus, dialogs, and app settings choices. */
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "launcher_internal.h"
#include "power.h"
#include "sysd.h"

static void choose_pause(nav *ui, void *choice);
static void choose_pause_list(nav *ui, void *choice);
static void choose_pause_settings(nav *ui, void *choice);
static void choose_confirm(nav *ui, void *choice);
static void choose_failure(nav *ui, void *choice);
static void choose_power_key(nav *ui, void *choice);

static int settings_category(launcher_state *state) {
  const ui_screen *screen = screen_find(state->visible, &pause_settings_screen);
  return screen != NULL ? (int)screen->arg : -1;
}

static const entry power_entries[] = {
    {.label = S("Sleep"),
     .command = S("suspend"),
     .action = POWER,
     .icon = ICON_MOON},
    {.label = S("Reboot"),
     .command = S("reboot"),
     .action = POWER,
     .icon = ICON_ROTATE_CW},
    {.label = S("Power off"),
     .command = S("poweroff"),
     .action = POWER,
     .icon = ICON_POWER},
};

static const entry pause_entries[] = {
    {.label = S("Resume"), .action = RESUME, .icon = ICON_PLAY},
    {.label = S("Save state"), .action = SAVE_STATE, .icon = ICON_SAVE_IN},
    {.label = S("Load state"),
     .action = CONFIRM,
     .icon = ICON_SAVE_OUT,
     .arg = LOAD_STATE},
    {.label = S("Settings"), .action = SETTINGS_LIST, .icon = ICON_COG},
    {.label = S("Restart"),
     .action = CONFIRM,
     .icon = ICON_ROTATE_CCW,
     .arg = RESTART},
    {.label = S("Home"), .action = HOME, .icon = ICON_HOUSE},
    {.label = S("Quit"), .action = CONFIRM, .icon = ICON_X, .arg = QUIT},
};
static const entry settings_entries[] = {
    {.label = S("System"),
     .action = SETTINGS,
     .icon = ICON_COG,
     .arg = APPCTL_SYSTEM},
    {.label = S("Graphics"),
     .action = SETTINGS,
     .icon = ICON_IMAGE,
     .arg = APPCTL_GRAPHICS},
    {.label = S("Audio"),
     .action = SETTINGS,
     .icon = ICON_SPEAKER,
     .arg = APPCTL_AUDIO},
    {.label = S("Input"),
     .action = SETTINGS,
     .icon = ICON_DPAD,
     .arg = APPCTL_INPUT},
    {.label = S("Power"),
     .action = SETTINGS,
     .icon = ICON_ZAP,
     .arg = POWER_PAGE},
    {.label = S("Debug"),
     .action = SETTINGS,
     .icon = ICON_GAUGE,
     .arg = DEBUG_PAGE},
};
static const entry fail_launch_entries[] = {
    {.label = S("OK"), .action = DISMISS, .icon = ICON_CHECK},
    {.label = S("Ignore for this app"),
     .action = IGNORE,
     .icon = ICON_EYE_OFF}};
static const entry fail_sleep_entries[] = {
    {.label = S("Use light sleep"), .action = SLEEP_LIGHT, .icon = ICON_MOON},
    {.label = S("Keep deep sleep"), .action = DISMISS, .icon = ICON_CHECK}};

static struct {
  int kind;
  STRBUF(96) title;
  STRBUF(640) why;
  STRBUF(1300) key;
} failure;
static int settings_focus = -1;

static str combo_text(uint16_t combo) {
  static STRBUF(160) out;
  out.n = 0;
  for (int b = 0; b < APPCTL_BUTTONS; b++)
    if (combo & (1u << b))
      sb_add(out, out.n ? S(" + ") : S(""), button_names[b]);
  return out.n ? sb_str(out) : S("None");
}

static void launcher_settings(launcher_state *state, nav *ui, running *r,
                              ui_rect view) {
  settings_focus = -1;
  const uint32_t n =
      MIN(atomic_load(&r->shm->option_count), APPCTL_OPTIONS_MAX);
  uint32_t rows = 0;
  for (uint32_t i = 0; i < n; i++)
    rows += r->shm->options[i].category == settings_category(state);
  if (rows == 0) {
    ui_text(ROW_TEXT, view.x + MARGIN * 2, view.y + MARGIN, S("No settings"),
            MUTED);
    return;
  }
  const float offset =
      nav_scroll_begin(ui, nav_str("settings"), view, UI_VERTICAL,
                       rows * ROW + MARGIN * 2, ROW, 0);
  nav_fill(ui, FOCUS, 6);
  gfx_clip(view.x, view.y, view.w, view.h);
  for (uint32_t i = 0, at = 0; i < n; i++) {
    if (r->shm->options[i].category != settings_category(state))
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
      state->capture =
          (typeof(state->capture)){.option = (int)i, .since = ms_now()};
    else if (flags & NAV_ACTIVATED)
      launcher_step_option(r, (int)i, 1);
    const str label = {o->label, strnlen(o->label, APPCTL_TEXT)};
    STRBUF(160) choice;
    str value;
    if (binding && state->capture.option == (int)i &&
        state->capture.combo == 0) {
      value = S("Press\u2026");
    } else if (binding) {
      value = combo_text(state->capture.option == (int)i
                             ? state->capture.combo
                             : atomic_load(&o->buttons));
    } else {
      const char *v =
          o->count
              ? o->values[atomic_load(&o->value) % MIN(o->count, APPCTL_VALUES)]
              : "";
      sb_set(choice, S("\u2039 "), (str){v, strnlen(v, APPCTL_TEXT)},
             S(" \u203a"));
      value = sb_str(choice);
    }
    const int y = (int)row.y + ((int)row.h - ui_text_height(ROW_TEXT)) / 2;
    const int vw = ui_text_width(ROW_TEXT, value);
    const bool per_game = atomic_load(&o->own);
    ui_text(ROW_TEXT, (int)row.x + MARGIN, y,
            ui_text_fit(ROW_TEXT, label, row.w - vw - MARGIN * 3, NULL),
            per_game ? PER_GAME : FG);
    ui_text(ROW_TEXT, (int)(row.x + row.w) - MARGIN - vw, y, value,
            state->capture.option == (int)i ? RING_TEXT
            : flags & NAV_FOCUSED           ? FG
                                            : MUTED);
  }
  gfx_noclip();
  nav_scroll_end(ui);
}

static bool has_settings(const running *r, int category) {
  const uint32_t n =
      MIN(atomic_load(&r->shm->option_count), APPCTL_OPTIONS_MAX);
  for (uint32_t i = 0; i < n; i++)
    if (r->shm->options[i].category == category)
      return true;
  return false;
}

static const entry *failure_dialog(nav *ui, int w, int h) {
  if (failure.kind == FAIL_SLEEP)
    return dialog_box(ui, w, h, sb_str(failure.title), sb_str(failure.why),
                      fail_sleep_entries, COUNTOF(fail_sleep_entries));
  return dialog_box(ui, w, h, sb_str(failure.title), sb_str(failure.why),
                    fail_launch_entries,
                    failure.key.n > 0 ? COUNTOF(fail_launch_entries) : 1);
}

static const entry *confirm_dialog(launcher_state *state, nav *ui, int w,
                                   int h) {
  entry *rows = NAV_FRAME(ui, entry, 2);
  STRBUF(128) title;
  const int confirming = (int)screen_arg(state->visible, &confirm_screen);
  if (confirming == LOAD_STATE)
    sb_set(title, S("Load the saved state?"));
  else
    sb_set(title, confirming == QUIT ? S("Quit ") : S("Restart "),
           sb_str(state->front->label), S("?"));
  rows[0] = (entry){.label = confirming == QUIT      ? S("Quit")
                             : confirming == RESTART ? S("Restart")
                                                     : S("Load state"),
                    .action = confirming,
                    .icon = ICON_CHECK};
  rows[1] = (entry){.label = S("Cancel"), .action = NOTHING, .icon = ICON_X};
  return dialog_box(ui, w, h, sb_str(title),
                    confirming == LOAD_STATE
                        ? S("Play since it was saved is lost.")
                        : S("Anything not saved is lost."),
                    rows, 2);
}

void launcher_show_failure(nav *ui, int kind, str title, str why, str key) {
  sb_set(failure.title, title);
  sb_set(failure.why, why);
  sb_set(failure.key, key);
  failure.kind = kind;
  if (!screen_contains(launcher.visible, &failure_screen))
    screen_push(launcher.visible, ui, &failure_screen, 0);
}

static str pause_hint(launcher_state *state) {
  if (screen_base_def(state->visible) == &pause_screen)
    return sb_str(state->notice);
  if (screen_base_def(state->visible) != &pause_settings_screen)
    return S("");
  const bool binding =
      settings_focus >= 0 && settings_category(state) < POWER_PAGE &&
      state->front->shm->options[settings_focus].kind == APPCTL_BINDING;
  const bool own = settings_focus >= 0 &&
                   settings_category(state) < POWER_PAGE &&
                   (state->front->shm->flags & APPCTL_OWN_OPTIONS);
  if (launcher_capture_active())
    return S("Hold the buttons, then let go of the first");
  if (binding && own)
    return S("A: record    X: clear    Y: this game");
  if (binding)
    return S("A: record    X: clear");
  if (own)
    return S("Y: this game");
  if (settings_category(state) == POWER_PAGE &&
      launcher_power_focus == POWER_CALIBRATE)
    return S("A: find the lowest clocks for this frame rate");
  return S("");
}

static str pause_title(const ui_screen *screen) {
  launcher_state *state = &launcher;
  if (screen->def == &pause_screen)
    return sb_str(state->front->label);
  static STRBUF(128) title;
  sb_set(title, sb_str(state->front->label), S(" \u00b7 "),
         screen->def == &pause_settings_screen ? category_names[screen->arg]
                                               : S("Settings"));
  return sb_str(title);
}

static str pause_screen_hint(const ui_screen *screen, const nav *ui) {
  (void)screen;
  (void)ui;
  return pause_hint(&launcher);
}

static void pause_frame(ui_screen *screen, nav *ui, ui_rect content) {
  (void)screen;
  launcher_state *state = &launcher;
  entry *items = NAV_FRAME(ui, entry, COUNTOF(pause_entries));
  size_t n = 0;
  for (size_t i = 0; i < COUNTOF(pause_entries); i++) {
    const int action = pause_entries[i].action == CONFIRM
                           ? pause_entries[i].arg
                           : pause_entries[i].action;
    if ((action != SAVE_STATE && action != LOAD_STATE && action != RESTART) ||
        (run_controlled(state->front) && state->front->shm->slots > 0))
      items[n++] = pause_entries[i];
    if (action == QUIT && (state->front->flags & APP_QUIT_NOW))
      items[n - 1].action = QUIT;
  }
  const entry *selected = menu(ui, items, n, content);
  if (selected != NULL)
    nav_defer(ui, choose_pause, selected, sizeof(*selected));
}

static void pause_list_frame(ui_screen *screen, nav *ui, ui_rect content) {
  (void)screen;
  launcher_state *state = &launcher;
  entry *items = NAV_FRAME(ui, entry, COUNTOF(settings_entries));
  size_t n = 0;
  for (size_t i = 0; i < COUNTOF(settings_entries); i++)
    if (settings_entries[i].arg >= POWER_PAGE ||
        (run_controlled(state->front) &&
         has_settings(state->front, settings_entries[i].arg)))
      items[n++] = settings_entries[i];
  const entry *selected = menu(ui, items, n, content);
  if (selected != NULL)
    nav_defer(ui, choose_pause_list, selected, sizeof(*selected));
}

static void pause_settings_frame(ui_screen *screen, nav *ui, ui_rect content) {
  launcher_state *state = &launcher;
  const int category = (int)screen->arg;
  if (category >= POWER_PAGE)
    settings_focus = -1;
  if (category == POWER_PAGE)
    NAV_SCOPE(ui, nav_int(category)) {
      state->draw.calibrate = launcher_power_page(ui, content, true);
    }
  else if (category == DEBUG_PAGE)
    NAV_SCOPE(ui, nav_int(category)) {
      entry *rows = NAV_FRAME(ui, entry, PERF_ROWS);
      settings_focus = launcher_power_focus = -1;
      const entry *selected = menu(ui, rows, launcher_perf_rows(rows), content);
      if (selected != NULL)
        nav_defer(ui, choose_pause_settings, selected, sizeof(*selected));
    }
  else
    NAV_SCOPE(ui, nav_int(category)) {
      launcher_settings(state, ui, state->front, content);
    }
}

static void confirm_frame(ui_screen *screen, nav *ui, ui_rect content) {
  (void)screen;
  launcher_state *state = &launcher;
  (void)content;
  const entry *selected =
      confirm_dialog(state, ui, state->draw.w, state->draw.h);
  if (selected != NULL)
    nav_defer(ui, choose_confirm, selected, sizeof(*selected));
}

static void failure_frame(ui_screen *screen, nav *ui, ui_rect content) {
  (void)screen;
  launcher_state *state = &launcher;
  (void)content;
  const entry *selected = failure_dialog(ui, state->draw.w, state->draw.h);
  if (selected != NULL)
    nav_defer(ui, choose_failure, selected, sizeof(*selected));
}

static void power_key_frame(ui_screen *screen, nav *ui, ui_rect content) {
  (void)screen;
  launcher_state *state = &launcher;
  (void)content;
  const entry *selected = dialog_box(ui, state->draw.w, state->draw.h, S(""),
                                     S(""), power_entries, 3);
  if (selected != NULL)
    nav_defer(ui, choose_power_key, selected, sizeof(*selected));
}

static bool pause_settings_button(ui_screen *screen, nav *ui, int button) {
  (void)screen;
  launcher_state *state = &launcher;
  (void)ui;
  if (settings_focus >= 0 && button == APPCTL_X &&
      state->front->shm->options[settings_focus].kind == APPCTL_BINDING) {
    launcher_set_binding(state->front, settings_focus, 0);
    return true;
  }
  if (settings_focus >= 0 && button == APPCTL_Y &&
      (state->front->shm->flags & APPCTL_OWN_OPTIONS)) {
    appctl_option *o = &state->front->shm->options[settings_focus];
    atomic_store(&o->own, !atomic_load(&o->own));
    run_request(state->front, APPCTL_OPTIONS, 0);
    return true;
  }
  if (settings_category(state) == POWER_PAGE && launcher_power_focus >= 0 &&
      (button == APPCTL_LEFT || button == APPCTL_RIGHT)) {
    power_step(launcher_power_focus, button == APPCTL_LEFT ? -1 : 1);
    return true;
  }
  if (settings_focus >= 0 &&
      (button == APPCTL_LEFT || button == APPCTL_RIGHT)) {
    launcher_step_option(state->front, settings_focus,
                         button == APPCTL_LEFT ? -1 : 1);
    return true;
  }
  return false;
}

static void choose_pause(nav *ui, void *choice) {
  launcher_state *state = &launcher;
  const entry *chosen = choice;
  int result = 0;
  switch (chosen->action) {
  case RESUME:
    resume_front(ui);
    break;
  case CONFIRM:
    screen_push(state->visible, ui, &confirm_screen, chosen->arg);
    break;
  case SETTINGS_LIST:
    screen_push(state->visible, ui, &pause_list_screen, 0);
    break;
  case SAVE_STATE:
  case LOAD_STATE:
    result = run_request(state->front,
                         chosen->action == SAVE_STATE ? APPCTL_SAVE_STATE
                                                      : APPCTL_LOAD_STATE,
                         0);
    if (result == 0)
      resume_front(ui);
    else
      sb_set(state->notice, chosen->label, S(" failed: "),
             result == -ENOENT ? S("nothing saved yet")
                               : str_c(strerror(-result)));
    break;
  case HOME:
    go_home(ui);
    break;
  case QUIT:
    quit_front(ui);
    break;
  default:
    break;
  }
}

static void choose_pause_list(nav *ui, void *choice) {
  launcher_state *state = &launcher;
  const entry *chosen = choice;
  switch (chosen->action) {
  case SETTINGS:
    settings_focus = -1;
    screen_push(state->visible, ui, &pause_settings_screen, chosen->arg);
    break;
  default:
    break;
  }
}

static void choose_pause_settings(nav *ui, void *choice) {
  (void)ui;
  const entry *chosen = choice;
  switch (chosen->action) {
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

static void choose_confirm(nav *ui, void *choice) {
  launcher_state *state = &launcher;
  const entry *chosen = choice;
  int result = 0;
  screen_pop(state->visible, ui);
  switch (chosen->action) {
  case RESTART:
    run_request(state->front, APPCTL_RESET, 0);
    resume_front(ui);
    break;
  case SAVE_STATE:
  case LOAD_STATE:
    result = run_request(state->front,
                         chosen->action == SAVE_STATE ? APPCTL_SAVE_STATE
                                                      : APPCTL_LOAD_STATE,
                         0);
    if (result == 0)
      resume_front(ui);
    else
      sb_set(state->notice, chosen->label, S(" failed: "),
             result == -ENOENT ? S("nothing saved yet")
                               : str_c(strerror(-result)));
    break;
  case QUIT:
    quit_front(ui);
    break;
  default:
    break;
  }
}

static void choose_failure(nav *ui, void *choice) {
  launcher_state *state = &launcher;
  const entry *chosen = choice;
  switch (chosen->action) {
  case IGNORE:
    launcher_ignore(sb_str(failure.key)); /* fall through */
  case SLEEP_LIGHT:
    if (chosen->action == SLEEP_LIGHT)
      sysd(S("sleep light"));
    /* fall through */
  case DISMISS:
    screen_pop(state->visible, ui);
    break;
  default:
    break;
  }
}

static void choose_power_key(nav *ui, void *choice) {
  (void)ui;
  const entry *chosen = choice;
  switch (chosen->action) {
  case POWER:
    sysd(chosen->command);
    break;
  default:
    break;
  }
}

const ui_screen_def pause_screen = {.name = "paused",
                                    .frame = pause_frame,
                                    .title = pause_title,
                                    .hint = pause_screen_hint};
const ui_screen_def pause_list_screen = {.name = "settings list",
                                         .frame = pause_list_frame,
                                         .title = pause_title,
                                         .hint = pause_screen_hint};
const ui_screen_def pause_settings_screen = {.name = "settings",
                                             .frame = pause_settings_frame,
                                             .button = pause_settings_button,
                                             .title = pause_title,
                                             .hint = pause_screen_hint};
const ui_screen_def confirm_screen = {
    .name = "confirm",
    .overlay = true,
    .frame = confirm_frame,
};
const ui_screen_def failure_screen = {
    .name = "failure",
    .overlay = true,
    .frame = failure_frame,
};
const ui_screen_def power_key_screen = {
    .name = "power key",
    .overlay = true,
    .frame = power_key_frame,
};
