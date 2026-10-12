/* Storage, library browsing, and source selection. */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "launcher_internal.h"
#include "sysd.h"
#include "watch.h"

enum { STORAGE_MAX = 4 };
typedef STRBUF(200) row_label;

static void choose_storage(nav *ui, void *choice);
static void choose_browse(nav *ui, void *choice);
static void choose_systems(nav *ui, void *choice);

static STRBUF(64) usb_state;

bool storage_refresh_usb(void) {
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

static bool usb_is(int i, str word) {
  str rest = sb_str(usb_state);
  for (int k = 0; rest.n > 0; k++) {
    const str w = str_cut(&rest, ' ');
    if (k == i && str_eq(w, word))
      return true;
  }
  return false;
}

static size_t storage_entries(entry *out) {
  static const str modes[] = {S("MTP"), S("SMB")};
  static const str mode_cmds[] = {S("usb mtp"), S("usb smb")};
  static const str mode_words[] = {S("mtp"), S("smb")};
  static const int mode_icons[] = {ICON_SMARTPHONE, ICON_NETWORK};
  const bool on = usb_is(0, S("on"));
  size_t n = 0;
  out[n++] =
      (entry){.label = S("Libraries"), .action = BROWSE, .icon = ICON_LIBRARY};
  out[n++] = (entry){.label = S("USB connection"),
                     .command = on ? S("usb off") : S("usb on"),
                     .action = POWER,
                     .icon = ICON_USB,
                     .checked = on};
  for (size_t i = 0; on && i < COUNTOF(modes); i++)
    out[n++] = (entry){.label = modes[i],
                       .command = mode_cmds[i],
                       .action = POWER,
                       .icon = mode_icons[i],
                       .checked = usb_is(1, mode_words[i]),
                       .depth = 1};
  return n;
}

#define MEDIA "/run/media"
enum { BROWSE_MAX = 256 };
static STRBUF(256) browse_dir;
static struct {
  STRBUF(256) path;
  STRBUF(96) name;
} browse_rows[BROWSE_MAX];
static size_t browse_count;
static STRBUF(256) pick_path;

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

static int is_folder(const struct dirent *d) {
  return d->d_type == DT_DIR && d->d_name[0] != '.' &&
         strpbrk(d->d_name, "\t\n") == NULL;
}

static void browse_add(str path, str name) {
  for (size_t i = 0; i < browse_count; i++)
    if (str_eq(sb_str(browse_rows[i].path), path))
      return;
  if (browse_count < BROWSE_MAX &&
      sb_set(browse_rows[browse_count].path, path)) {
    sb_set(browse_rows[browse_count].name, name);
    browse_count++;
  }
}

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

static void browse_up(nav *ui) {
  STRBUF(256) from, parent;
  sb_set(from, sb_str(browse_dir));
  str up = str_head(sb_str(from), str_rfind(sb_str(from), '/'));
  sb_set(parent, str_eq(up, S(MEDIA)) ? S("") : up);
  browse(ui, sb_str(parent));
  for (size_t i = 0; i < browse_count; i++)
    if (str_eq(sb_str(browse_rows[i].path), sb_str(from)))
      nav_focus(
          ui, menu_row(nav_child(nav_str("browse"), nav_str(browse_dir.p)), i));
}

static void pick_systems(launcher_state *state, nav *ui, str path) {
  sb_set(pick_path, path);
  screen_push(state->visible, ui, &systems_screen, 0);
  const int guess = apps_guess(path);
  size_t row = 0;
  for (int i = 0; i < guess; i++)
    row += categories[i].roms.n > 0;
  nav_focus(ui, guess >= 0 ? menu_row(nav_str("systems"), row) : 0);
}

static void storage_frame(ui_screen *screen, nav *ui, ui_rect content) {
  (void)screen;
  entry *rows = NAV_FRAME(ui, entry, STORAGE_MAX);

  const size_t n = storage_entries(rows);
  const entry *selected = menu(ui, rows, n, content);
  if (selected != NULL)
    nav_defer(ui, choose_storage, selected, sizeof(*selected));
}

static void browse_frame(ui_screen *screen, nav *ui, ui_rect content) {
  (void)screen;
  entry *rows = NAV_FRAME(ui, entry, BROWSE_MAX);
  row_label *labels = NAV_FRAME(ui, row_label, BROWSE_MAX);

  for (size_t i = 0; i < browse_count; i++) {
    sb_set(labels[i], sb_str(browse_rows[i].name));
    bool first = true;
    for (size_t k = 0; k < source_count; k++)
      if (str_eq(sb_str(sources[k].path), sb_str(browse_rows[i].path))) {
        sb_add(labels[i], first ? S("  \u00b7  ") : S(", "),
               categories[sources[k].category].name);
        first = false;
      }
    rows[i] = (entry){.label = sb_str(labels[i]),
                      .action = BROWSE_OPEN,
                      .icon = ICON_FOLDER,
                      .arg = (int)i};
  }
  NAV_SCOPE(ui, nav_str(browse_dir.p)) {
    const entry *selected = menu(ui, rows, browse_count, content);
    if (selected != NULL)
      nav_defer(ui, choose_browse, selected, sizeof(*selected));
  }
}

static void systems_frame(ui_screen *screen, nav *ui, ui_rect content) {
  (void)screen;
  entry *rows = NAV_FRAME(ui, entry, BROWSE_MAX);

  size_t n = 0;
  for (size_t i = 0; i < category_count && n < BROWSE_MAX; i++) {
    if (categories[i].roms.n == 0)
      continue;
    rows[n++] = (entry){.label = categories[i].name,
                        .action = TOGGLE_SYSTEM,
                        .icon = ICON_GAMEPAD_2,
                        .arg = (int)i,
                        .checked = sources_has(sb_str(pick_path), i)};
  }
  const entry *selected = menu(ui, rows, n, content);
  if (selected != NULL)
    nav_defer(ui, choose_systems, selected, sizeof(*selected));
}

static bool browse_button(ui_screen *screen, nav *ui, int button) {
  (void)screen;
  launcher_state *state = &launcher;
  if (button == APPCTL_B && browse_dir.n > 0) {
    browse_up(ui);
    return true;
  }
  if (button != APPCTL_X)
    return false;
  const int row = focused_row(
      ui, nav_child(nav_str("browse"), nav_str(browse_dir.p)), browse_count);
  if (row >= 0)
    pick_systems(state, ui, sb_str(browse_rows[row].path));
  return true;
}

static void choose_storage(nav *ui, void *choice) {
  launcher_state *state = &launcher;
  const entry *chosen = choice;
  switch (chosen->action) {
  case BROWSE:
    screen_push(state->visible, ui, &browse_screen, 0);
    browse(ui, S(""));
    break;
  case POWER:
    sysd(chosen->command);
    break;
  default:
    break;
  }
}

static void choose_browse(nav *ui, void *choice) {
  const entry *chosen = choice;
  switch (chosen->action) {
  case BROWSE_OPEN: {
    STRBUF(256) dir;
    sb_set(dir, sb_str(browse_rows[chosen->arg].path));
    browse(ui, sb_str(dir));
    break;
  }
  default:
    break;
  }
}

static void choose_systems(nav *ui, void *choice) {
  (void)ui;
  const entry *chosen = choice;
  unsigned *changed = launcher.changed;
  switch (chosen->action) {
  case TOGGLE_SYSTEM:
    sources_toggle(sb_str(pick_path), (size_t)chosen->arg);
    *changed |= WATCH_MOUNTS;
    break;
  default:
    break;
  }
}

static str storage_hint(const ui_screen *screen, const nav *ui) {
  (void)screen;
  (void)ui;
  return usb_is(1, S("smb")) ? S("On a PC: \\\\10.55.0.1, user player") : S("");
}
static str browse_title(const ui_screen *screen) {
  (void)screen;
  return browse_dir.n > 0 ? shown(sb_str(browse_dir)) : S("Libraries");
}
static str browse_hint(const ui_screen *screen, const nav *ui) {
  (void)screen;
  (void)ui;
  return browse_count > 0 ? S("X: systems") : S("");
}
static str systems_title(const ui_screen *screen) {
  (void)screen;
  return shown(sb_str(pick_path));
}

const ui_screen_def storage_screen = {.name = "storage",
                                      .frame = storage_frame,
                                      .title_text = S("Storage"),
                                      .hint = storage_hint};
const ui_screen_def browse_screen = {.name = "browse",
                                     .frame = browse_frame,
                                     .button = browse_button,
                                     .title = browse_title,
                                     .hint = browse_hint};
const ui_screen_def systems_screen = {
    .name = "systems", .frame = systems_frame, .title = systems_title};
