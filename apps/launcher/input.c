/* Keyboard and wake event dispatch. */
#include <limits.h>
#include <linux/input.h>
#include <stdio.h>

#include "keys.h"
#include "idle.h"
#include "launcher_internal.h"
#include "power.h"
#include "romdb.h"
#include "sysd.h"
#include "updates.h"
#include "wake.h"

enum { CAPTURE_MS = 5000 };
static bool menu_down, menu_combo;
bool launcher_capture_active(void) { return launcher.capture.option >= 0; }

void launcher_key(unsigned code, bool down) {
  int button = -1;
  switch (code) {
  case KEY_UP:
    button = APPCTL_UP;
    break;
  case KEY_DOWN:
    button = APPCTL_DOWN;
    break;
  case KEY_LEFT:
    button = APPCTL_LEFT;
    break;
  case KEY_RIGHT:
    button = APPCTL_RIGHT;
    break;
  case KEY_ENTER:
    button = APPCTL_A;
    break;
  case KEY_PAGEUP:
    button = APPCTL_L1;
    break;
  case KEY_PAGEDOWN:
    button = APPCTL_R1;
    break;
  case KEY_ESC:
    button = APPCTL_MENU;
    break;
  case KEY_BACKSPACE:
    button = APPCTL_B;
    break;
  case KEY_X:
    button = APPCTL_X;
    break;
  case KEY_Y:
    button = APPCTL_Y;
    break;
  case KEY_1:
    button = APPCTL_L2;
    break;
  case KEY_2:
    button = APPCTL_R2;
    break;
  case KEY_TAB:
    button = APPCTL_SELECT;
    break;
  case KEY_SPACE:
    button = APPCTL_START;
    break;
  }
  if (button >= 0)
    win_wake(UI_KEY, button << 1 | down);
}

bool launcher_input(nav *ui, int why, int value, size_t *page) {
  int button = -1;
  bool down = false, pad = false;
  switch (why) {
  case WIN_QUIT:
    home_save_page(*page);
    run_quit_all();
    return false;
  case UI_VOLUME:
    osd_show_level(false, value);
    return true;
  case UI_BRIGHTNESS:
    /* Menu + a volume key: the app's hotkey, not a pause */
    menu_combo |= menu_down;
    power_step(POWER_BRIGHTNESS, value);
    osd_show_level(true, power_brightness());
    return true;
  case UI_HASHED:
    home_rescan(*page);
    return true;
  case UI_LAUNCH:
    control_launch_path(ui);
    return true;
  case UI_RELOAD:
    romdb_retry();
    apps_cores();
    home_rescan(*page);
    return true;
  case UI_POWER_HOLD:
    if (launcher.front != NULL && !showing_pause())
      session_pause(ui);
    if (!screen_contains(launcher.visible, &power_key_screen))
      screen_push(launcher.visible, ui, &power_key_screen, 0);
    return true;
  case UI_ACTIVE:
  case UI_LID:
    idle_event(why, value);
    return true;
  case UI_REFUSED: {
    osd_show_text(bus_refused(), 3000);
    return true;
  }
  case UI_EXITED: {
    running *r = run_find(value);
    if (r != NULL)
      session_app_exited(ui, r, page);
    return true;
  }
  case UI_BUTTON:
  case UI_KEY:
    button = value >> 1;
    down = value & 1;
    pad = why == UI_BUTTON;
    break;
  case UI_UPDATES: {
    STRBUF(256) said = {0};
    const str news = sb_str(updates.pack_news);
    if (str_eq(news, S("downloading")))
      sb_set(said, S("Downloading box art"));
    else if (str_eq(news, S("done")))
      sb_set(said, S("Box art downloaded"));
    else if (str_eq(news, S("failed")))
      sb_set(said, S("No box art: "), updates_reason(sb_str(updates.pack_why)));
    updates.pack_news.n = 0;
    if (said.n > 0)
      osd_show_text(sb_str(said), RESULT_MS);
    return true;
  }
  case UI_CONFIGURED:
  case UI_DONE:
    ui_scheme();
    return true;
  default:
    return true; /* UI_LOADED */
  }
  /* Recording a binding: every button is the combo's, Menu and B too */
  if (launcher.capture.option >= 0 && showing_pause() &&
      screen_contains(launcher.visible, &pause_settings_screen)) {
    const int bit = button;
    if (bit < 0)
      return true;
    if (down) {
      if (launcher.capture.combo == 0)
        launcher.capture.first = bit;
      launcher.capture.held |= 1u << bit;
      launcher.capture.combo |= 1u << bit;
    } else if (launcher.capture.held & (1u << bit)) {
      launcher.capture.held &= ~(1u << bit);
      if (bit == launcher.capture.first) {
        launcher_set_binding(launcher.front, launcher.capture.option,
                             launcher.capture.combo);
        launcher.capture.option = -1;
      }
    }
    return true;
  }
  /* The pad's Menu acts on its release, and only if nothing was pressed while
   * it was held: Menu plus a button or a volume key is a hotkey (a key: on the
   * press) */
  if (button == APPCTL_MENU && pad) {
    const bool tapped = !down && menu_down && !menu_combo;
    menu_down = down;
    menu_combo = false;
    if (!tapped)
      return true;
  } else if (!down) {
    return true;
  }
  if (button == APPCTL_MENU) {
    if (launcher.front != NULL && !launcher.front->quitting) {
      if (showing_pause())
        resume_front(ui);
      else
        session_pause(ui);
      return true;
    }
    button = APPCTL_START;
  }
  if (menu_down && button >= 0) {
    menu_combo = true;
    return true;
  }
  if (launcher.front != NULL && !showing_pause())
    return true;
  const ui_screen_def *kind = screen_current(launcher.visible);
  if (screen_button(launcher.visible, ui, button))
    return true;
  if (button == APPCTL_B) {
    if (kind == &pause_screen)
      resume_front(ui);
    else
      screen_pop(launcher.visible, ui);
    return true;
  }
  if (button == APPCTL_START && kind == &home_screen) {
    screen_push(launcher.visible, ui, &quick_menu_screen, 0);
    return true;
  }
  if (button == APPCTL_SELECT && kind == &home_screen && *page >= 1 &&
      tiles_focus >= 0 && (size_t)tiles_focus < categories[*page - 1].count &&
      categories[*page - 1].roms.n > 0 &&
      (categories[*page - 1].core_count > 1 ||
       categories[*page - 1].pack.n > 0)) {
    sb_set(launcher.selected_rom,
           sb_str(categories[*page - 1].apps[tiles_focus].arg));
    screen_push(launcher.visible, ui, &cores_screen, 0);
    return true;
  }
  const bool list = kind != &home_screen;
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
    if (!nav_move(ui, NAV_LEFT) && kind == &home_screen)
      home_turn_page(ui, page, -1);
    break;
  case APPCTL_RIGHT:
    if (!nav_move(ui, NAV_RIGHT) && kind == &home_screen)
      home_turn_page(ui, page, 1);
    break;
  case APPCTL_L1:
    if (kind == &home_screen)
      home_turn_page(ui, page, -1);
    break;
  case APPCTL_R1:
    if (kind == &home_screen)
      home_turn_page(ui, page, 1);
    break;
  case APPCTL_A:
    nav_activate(ui);
    break;
  }
  return true;
}

bool input_capture_expire(uint64_t now) {
  if (launcher.capture.option < 0 || launcher.capture.combo != 0 ||
      now - launcher.capture.since < CAPTURE_MS)
    return false;
  launcher.capture.option = -1;
  return true;
}
