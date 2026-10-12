/* License list and text screens. */
#include "launcher_internal.h"
#include "licenses.h"

static void choose_licenses(nav *ui, void *choice);

static size_t license_shown;

static str license_title(const ui_screen *screen) {
  (void)screen;
  return licenses[license_shown].name;
}

static void license_text_frame(nav *ui, ui_rect view) {
  const str *lines;
  const size_t n =
      license_text(license_shown, LICENSE_TEXT, view.w - MARGIN * 4, &lines);
  const int line = ui_text_height(LICENSE_TEXT) + 2;
  const float offset =
      nav_scroll_begin(ui, nav_str("license"), view, UI_VERTICAL,
                       n * line + MARGIN * 2, 0, line);
  gfx_clip(view.x, view.y, view.w, view.h);
  for (size_t i = (size_t)MAX(0, (offset - MARGIN) / line); i < n; i++) {
    const float y = view.y + MARGIN + i * line - offset;
    if (y >= view.y + view.h)
      break;
    ui_text(LICENSE_TEXT, (int)view.x + MARGIN * 2, (int)y, lines[i], FG);
  }
  gfx_noclip();
  nav_scroll_end(ui);
}

static void licenses_frame(ui_screen *screen, nav *ui, ui_rect content) {
  (void)screen;
  entry *rows = NAV_FRAME(ui, entry, LICENSES_MAX);

  for (size_t i = 0; i < license_count; i++)
    rows[i] = (entry){.label = licenses[i].label,
                      .action = LICENSE_OPEN,
                      .icon = ICON_FILE_TEXT,
                      .arg = (int)i};
  const entry *selected = menu(ui, rows, license_count, content);
  if (selected != NULL)
    nav_defer(ui, choose_licenses, selected, sizeof(*selected));
}

static void license_frame(ui_screen *screen, nav *ui, ui_rect content) {
  (void)screen;

  license_text_frame(ui, content);
}

static bool license_button(ui_screen *screen, nav *ui, int button) {
  (void)screen;
  launcher_state *state = &launcher;
  if (button == APPCTL_B)
    return false;
  const int lines =
      (state->draw.h - BAR - 1) / (ui_text_height(LICENSE_TEXT) + 2);
  switch (button) {
  case APPCTL_UP:
    nav_step(ui, -1);
    break;
  case APPCTL_DOWN:
    nav_step(ui, 1);
    break;
  case APPCTL_LEFT:
  case APPCTL_L1:
    nav_step(ui, -lines);
    break;
  case APPCTL_RIGHT:
  case APPCTL_R1:
    nav_step(ui, lines);
    break;
  }
  return true;
}

static void choose_licenses(nav *ui, void *choice) {
  launcher_state *state = &launcher;
  const entry *chosen = choice;
  switch (chosen->action) {
  case LICENSE_OPEN:
    license_shown = (size_t)chosen->arg;
    screen_push(state->visible, ui, &license_screen, 0);
    nav_forget(ui, nav_child(nav_str("license"), nav_str("license")));
    break;
  default:
    break;
  }
}

const ui_screen_def licenses_screen = {
    .name = "licenses",
    .title_text = S("Licenses"),
    .frame = licenses_frame,
};
const ui_screen_def license_screen = {.name = "license",
                                      .title = license_title,
                                      .frame = license_frame,
                                      .button = license_button};
