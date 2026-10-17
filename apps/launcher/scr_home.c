/* Home pages, running apps, and title matching. */
#include <unistd.h>

#include "assets.h"
#include "keyboard.h"
#include "launcher_internal.h"
#include "romdb.h"
#include "sysd.h"
#include "updates.h"

enum { MATCH_MAX = 100 };
typedef STRBUF(200) row_label;

static void choose_home(nav *ui, void *choice);
static void choose_match(nav *ui, void *choice);
static void choose_quick_menu(nav *ui, void *choice);
static void choose_cores(nav *ui, void *choice);

static const entry system_entries[] = {
    {.label = S("Settings"), .action = SYSTEM_SETTINGS, .icon = ICON_COG},
    {.label = S("Power"), .action = HOME_POWER, .icon = ICON_ZAP},
    {.label = S("Sleep"),
     .sysd = sysd_suspend,
     .action = POWER,
     .icon = ICON_MOON},
    {.label = S("Reboot"),
     .sysd = sysd_reboot,
     .action = POWER,
     .icon = ICON_ROTATE_CW},
    {.label = S("Power off"),
     .sysd = sysd_power_off,
     .action = POWER,
     .icon = ICON_POWER},
};
static struct {
  size_t category;
  STRBUF(128) query;
  uint32_t crcs[MATCH_MAX];
  romdb_game games[MATCH_MAX];
  size_t count;
} match;
static str page_name(size_t page) {
  return page == 0 ? S("Running") : categories[page - 1].name;
}

static app *picked_game(launcher_state *state, category *c) {
  for (size_t i = 0; i < c->count; i++)
    if (str_eq(sb_str(c->apps[i].arg), sb_str(state->selected_rom)))
      return &c->apps[i];
  return NULL;
}

static const entry *game_dialog(launcher_state *state, nav *ui, int w, int h,
                                category *c) {
  entry *rows = NAV_FRAME(ui, entry, (CORES_MAX + 1));
  const app *a = picked_game(state, c);
  if (a == NULL) {
    screen_pop(state->visible, ui);
    return NULL;
  }
  size_t n = 0;
  for (size_t i = 0; c->core_count > 1 && i < c->core_count; i++)
    rows[n++] = (entry){.label = sb_str(c->cores[i].name),
                        .action = PICK_CORE,
                        .icon = ICON_GAMEPAD_2,
                        .arg = (int)i,
                        .checked = (int)i == a->core};
  const bool have_pack = c->pack.n > 0 && access(apps_pack(c).p, F_OK) == 0;
  if (have_pack)
    rows[n++] = (entry){
        .label = S("Title and box art"), .action = MATCH, .icon = ICON_IMAGE};
  else if (c->pack.n > 0)
    rows[n++] = (entry){.label = S("Download box art"),
                        .action = GET_PACK,
                        .icon = ICON_DOWNLOAD};
  const str text =
      c->core_count > 1 ? S("Run with")
      : c->pack.n > 0 && !have_pack
          ? S("This system's titles and box art aren't on the card.")
          : S("");
  return dialog_box(ui, w, h, sb_str(a->name), text, rows, n);
}

static void match_find(launcher_state *state, nav *ui, str query) {
  sb_set(match.query, query);
  const str pack = apps_pack(&categories[match.category]);
  const size_t n = romdb_search(pack, query, match.crcs, MATCH_MAX);
  match.count = 0;
  for (size_t i = 0; i < n; i++)
    if (romdb_find(pack, match.crcs[i], &match.games[match.count]))
      match.crcs[match.count++] = match.crcs[i];
  if (screen_current(state->visible) != &match_screen)
    screen_replace(state->visible, ui, &match_screen, 0);
  const app *a = picked_game(state, &categories[match.category]);
  size_t row = match.count > 0 ? 2 : 0;
  for (size_t i = 0; a != NULL && i < match.count; i++)
    if (match.crcs[i] == a->match)
      row = i + 2;
  nav_focus(ui, menu_row(nav_str("match"), row));
}

static void match_text(ui_screen *screen, nav *ui, str value) {
  (void)screen;
  launcher_state *state = &launcher;
  match_find(state, ui, value);
}

static void match_open(launcher_state *state, nav *ui, size_t category) {
  const str path = sb_str(state->selected_rom),
            name = str_at(path, str_rfind(path, '/') + 1);
  const size_t dot = str_rfind(name, '.');
  match.category = category;
  match_find(state, ui, dot > 0 ? str_head(name, dot) : name);
}

static running *running_at(size_t index) {
  for (size_t i = 0; i < RUNNING_MAX; i++)
    if (runs[i].pid != 0 && !runs[i].quitting && index-- == 0)
      return &runs[i];
  return NULL;
}

static str home_title(const ui_screen *screen) {
  (void)screen;
  launcher_state *state = &launcher;
  return page_name(state->draw.page);
}

static void home_frame(ui_screen *screen, nav *ui, ui_rect content) {
  (void)screen;
  launcher_state *state = &launcher;
  NAV_SCOPE(ui, nav_int(state->draw.page)) {
    if (state->draw.page == 0) {
      const int picked = launcher_tiles(ui, NULL, content);
      if (picked >= 0) {
        const entry action = {.action = LAUNCH_RUNNING, .arg = picked};
        nav_defer(ui, choose_home, &action, sizeof(action));
      }
    } else {
      const category *c = &categories[state->draw.page - 1];
      const int picked = launcher_tiles(ui, c, content);
      if (picked >= 0) {
        const entry action = {.action = LAUNCH_APP, .arg = picked};
        nav_defer(ui, choose_home, &action, sizeof(action));
      }
    }
  }
}

static void choose_home(nav *ui, void *choice) {
  const entry *chosen = choice;
  if (chosen->action == LAUNCH_RUNNING) {
    running *r = running_at((size_t)chosen->arg);
    if (r != NULL)
      session_start(ui, sb_str(r->label), sb_str(r->icon), sb_str(r->exec),
                    sb_str(r->arg), sb_str(r->dir), r->flags);
    return;
  }
  if (chosen->action != LAUNCH_APP || launcher.draw.page == 0)
    return;
  const category *c = &categories[launcher.draw.page - 1];
  if ((size_t)chosen->arg >= c->count)
    return;
  const app *a = &c->apps[chosen->arg];
  if (a->exec.n == 0) {
    STRBUF(96) why;
    sb_set(why,
           str_eq(c->folder, S("videos")) ? S("No player for ")
                                          : S("No emulator for "),
           c->name);
    launcher_show_failure(ui, FAIL_LAUNCH, sb_str(a->name), sb_str(why), S(""));
  } else {
    session_start(ui, sb_str(a->name), sb_str(a->icon), sb_str(a->exec),
                  sb_str(a->arg), sb_str(a->dir), a->flags);
  }
}

static void match_frame(ui_screen *screen, nav *ui, ui_rect content) {
  (void)screen;
  launcher_state *state = &launcher;
  entry *rows = NAV_FRAME(ui, entry, (MATCH_MAX + 2));
  row_label *labels = NAV_FRAME(ui, row_label, 1);

  const app *a = picked_game(state, &categories[match.category]);
  size_t n = 0;
  sb_set(labels[0], S("Search  \u00b7  "), sb_str(match.query));
  rows[n++] = (entry){
      .label = sb_str(labels[0]), .action = MATCH_SEARCH, .icon = ICON_SEARCH};
  rows[n++] = (entry){.label = S("By its contents"),
                      .action = MATCH_PICK,
                      .icon = ICON_MICROCHIP,
                      .arg = -1,
                      .checked = a != NULL && a->match == 0};
  for (size_t i = 0; i < match.count; i++)
    rows[n++] = (entry){.label = match.games[i].title,
                        .action = MATCH_PICK,
                        .icon = ICON_IMAGE,
                        .arg = (int)i,
                        .checked = a != NULL && a->match == match.crcs[i]};
  const ui_rect left = {content.x, content.y, content.w - ICON - MARGIN * 2,
                        content.h};
  const entry *selected = menu(ui, rows, n, left);
  if (selected != NULL)
    nav_defer(ui, choose_match, selected, sizeof(*selected));
  const float px = left.x + left.w, py = content.y + MARGIN;
  if (menu_focus >= 2 && (size_t)menu_focus - 2 < match.count) {
    const asset art =
        assets_get(sb_str(match.games[menu_focus - 2].image), ICON);
    if (art.slot != GFX_PLACEHOLDER) {
      const float scale = MIN((float)ICON / art.w, (float)ICON / art.h);
      gfx_push((gfx_quad){.x = px + (ICON - art.w * scale) / 2,
                          .y = py,
                          .w = art.w * scale,
                          .h = art.h * scale,
                          .u1 = 1,
                          .v1 = 1,
                          .color = 0xffffffff,
                          .tex = art.slot,
                          .radius = RADIUS - 2});
    } else {
      ui_rounded(px, py, ICON, ICON, EDGE, RADIUS - 2, 0);
    }
  }
}

static void quick_menu_frame(ui_screen *screen, nav *ui, ui_rect content) {
  (void)screen;
  launcher_state *state = &launcher;
  (void)content;
  const entry *selected =
      dialog_box(ui, state->draw.w, state->draw.h, S(""), S(""), system_entries,
                 COUNTOF(system_entries));
  if (selected != NULL)
    nav_defer(ui, choose_quick_menu, selected, sizeof(*selected));
}

static void cores_frame(ui_screen *screen, nav *ui, ui_rect content) {
  (void)screen;
  launcher_state *state = &launcher;
  (void)content;
  if (state->draw.page >= 1) {
    const entry *selected = game_dialog(state, ui, state->draw.w, state->draw.h,
                                        &categories[state->draw.page - 1]);
    if (selected != NULL)
      nav_defer(ui, choose_cores, selected, sizeof(*selected));
  }
}

static bool quick_menu_button(ui_screen *screen, nav *ui, int button) {
  (void)screen;
  launcher_state *state = &launcher;
  if (button != APPCTL_START)
    return false;
  screen_pop(state->visible, ui);
  return true;
}

static void choose_quick_menu(nav *ui, void *choice) {
  launcher_state *state = &launcher;
  const entry *chosen = choice;
  switch (chosen->action) {
  case SYSTEM_SETTINGS:
    screen_replace(state->visible, ui, &settings_screen, 0);
    break;
  case HOME_POWER:
    screen_replace(state->visible, ui, &power_screen, 0);
    break;
  case POWER:
    chosen->sysd();
    break;
  default:
    break;
  }
}

static void choose_cores(nav *ui, void *choice) {
  launcher_state *state = &launcher;
  const entry *chosen = choice;
  const size_t page = launcher.draw.page;
  switch (chosen->action) {
  case PICK_CORE: {
    app *a = picked_game(state, &categories[page - 1]);
    if (a != NULL)
      apps_set_core(&categories[page - 1], a, chosen->arg);
    screen_pop(state->visible, ui);
    break;
  }
  case MATCH:
    match_open(state, ui, page - 1);
    break;
  case GET_PACK:
    if (page >= 1)
      updates_pack(sb_str(categories[page - 1].pack));
    screen_pop(state->visible, ui);
    break;
  default:
    break;
  }
}

static void choose_match(nav *ui, void *choice) {
  launcher_state *state = &launcher;
  const entry *chosen = choice;
  const size_t page = launcher.draw.page;
  switch (chosen->action) {
  case MATCH_SEARCH:
    keyboard_open(sb_str(match.query));
    screen_push(state->visible, ui, &keyboard_screen, match.category);
    break;
  case MATCH_PICK: {
    category *c = &categories[match.category];
    app *a = picked_game(state, c);
    if (a != NULL)
      apps_set_match(a, chosen->arg >= 0 ? match.crcs[chosen->arg] : 0);
    apps_scan(c);
    screen_pop(state->visible, ui);
    if ((a = picked_game(state, c)) != NULL)
      nav_focus(ui,
                nav_child(nav_child(nav_child(nav_str("home"), nav_int(page)),
                                    nav_str("tiles")),
                          nav_int(a - c->apps)));
    break;
  }
  default:
    break;
  }
}

const ui_screen_def home_screen = {
    .name = "home", .frame = home_frame, .title = home_title};
const ui_screen_def quick_menu_screen = {
    .name = "start",
    .overlay = true,
    .frame = quick_menu_frame,
    .button = quick_menu_button,
};
const ui_screen_def cores_screen = {
    .name = "cores",
    .overlay = true,
    .frame = cores_frame,
};

const ui_screen_def match_screen = {.name = "match",
                                    .frame = match_frame,
                                    .title_text = S("Title and box art"),
                                    .text = match_text};
