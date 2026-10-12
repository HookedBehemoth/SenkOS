/* Shared launcher drawing widgets. */
#include <stdlib.h>

#include "assets.h"
#include "launcher_internal.h"
#include "power.h"

int tiles_focus = -1, menu_focus = -1;
enum {
  MARQUEE_PX = 30 /* a second */,
  MARQUEE_HOLD = 1500,
  MARQUEE_PASSES = 3
};
static uint64_t marquee_due, marquee_since, frame_now;
static uint64_t marquee_of;
static bool marquee_drawn;
static void marquee(int px, float x, int y, float room, str text,
                    gfx_color color, ui_rect view) {
  uint64_t key = 1469598103934665603u;
  for (size_t i = 0; i < text.n; i++)
    key = (key ^ (uint8_t)text.p[i]) * 1099511628211u;
  if (key != marquee_of) {
    marquee_of = key;
    marquee_since = frame_now;
  }
  marquee_drawn = true;
  const int over = ui_text_width(px, text) - (int)room;
  const uint64_t scroll = (uint64_t)over * 1000 / MARQUEE_PX,
                 cycle = MARQUEE_HOLD * 2 + scroll;
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

const entry *menu(nav *ui, const entry *entries, size_t count, ui_rect view) {
  if (count == 0) {
    ui_text(ROW_TEXT, view.x + MARGIN * 2, view.y + MARGIN, S("Nothing here"),
            DIM);
    return NULL;
  }
  const entry *chosen = NULL;
  menu_focus = -1;
  const float offset = nav_scroll_begin(ui, nav_str("menu"), view, UI_VERTICAL,
                                        count * ROW + MARGIN * 2, ROW, 0);
  nav_fill(ui, FOCUS, 6);
  gfx_clip(view.x, view.y, view.w, view.h);
  for (size_t i = 0; i < count; i++) {
    const int depth = entries[i].depth;
    const float top = view.y + MARGIN + i * ROW - offset;
    const ui_rect r = {view.x + MARGIN + depth * TREE, top,
                       view.w - MARGIN * 2 - depth * TREE, ROW - 4};
    const bool shown = r.y < view.y + view.h && r.y + r.h > view.y;
    if (entries[i].action == HEADER) {
      if (shown) {
        ui_icon(entries[i].icon, ICON_PX, (int)r.x + MARGIN,
                (int)r.y + ((int)r.h - ICON_PX) / 2, DIM);
        ui_text(ROW_TEXT, (int)r.x + MARGIN * 2 + ICON_PX,
                (int)r.y + ((int)r.h - ui_text_height(ROW_TEXT)) / 2,
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
    const float mid = r.y + r.h / 2;
    for (int k = 1; k <= depth; k++) {
      const float lx =
          view.x + MARGIN + (k - 1) * TREE + MARGIN + ICON_PX / 2 - 1;
      bool on = false;
      for (size_t j = i + 1; j < count && entries[j].depth >= k && !on; j++)
        on = entries[j].depth == k;
      if (k < depth) {
        if (on)
          ui_fill(lx, r.y - 4, 2, ROW, DIM);
        continue;
      }
      const float from = i > 0 && entries[i - 1].depth == k - 1
                             ? r.y - 4 - (ROW - 4) / 2 + ICON_PX / 2 + 3
                             : r.y - 4;
      ui_fill(lx, from, 2, (on ? r.y + r.h : mid + 1) - from, DIM);
      ui_fill(lx, mid - 1, r.x - 2 - lx, 2, DIM);
    }
    int x = (int)r.x + MARGIN;
    if (entries[i].icon >= 0) {
      ui_icon(entries[i].icon, ICON_PX, x, (int)r.y + ((int)r.h - ICON_PX) / 2,
              FG);
      x += ICON_PX + MARGIN;
    }
    const float room =
        r.x + r.w - x - MARGIN - (entries[i].checked ? ICON_PX + MARGIN : 0);
    const int ty = (int)r.y + ((int)r.h - ui_text_height(ROW_TEXT)) / 2;
    if ((flags & NAV_FOCUSED) &&
        ui_text_width(ROW_TEXT, entries[i].label) > room)
      marquee(ROW_TEXT, x, ty, room, entries[i].label, FG, view);
    else
      ui_text(ROW_TEXT, x, ty,
              ui_text_fit(ROW_TEXT, entries[i].label, room, NULL), FG);
    if (entries[i].checked)
      ui_icon(ICON_CHECK, ICON_PX, (int)(r.x + r.w) - MARGIN - ICON_PX,
              (int)r.y + ((int)r.h - ICON_PX) / 2, FG);
  }
  gfx_noclip();
  nav_scroll_end(ui);
  return chosen;
}

static void tile(str name, str icon_name, bool active, ui_rect r, bool focused,
                 ui_rect view) {
  const float ix = r.x + (r.w - ICON) / 2, iy = r.y + 8;
  const asset icon = assets_get(icon_name, ICON);
  if (icon.slot == GFX_PLACEHOLDER) {
    ui_rounded(ix, iy, ICON, ICON, EDGE, RADIUS - 2, 0);
    str initial = str_head(name, 1);
    while (initial.n < name.n && (name.p[initial.n] & 0xc0) == 0x80)
      initial.n++;
    ui_text(LETTER, (int)(ix + (ICON - ui_text_width(LETTER, initial)) / 2),
            (int)(iy + (ICON - ui_text_height(LETTER)) / 2), initial, DIM);
  } else {
    const float scale = MIN((float)ICON / icon.w, (float)ICON / icon.h);
    const float w = icon.w * scale, h = icon.h * scale;
    gfx_push((gfx_quad){.x = ix + (ICON - w) / 2,
                        .y = iy + (ICON - h) / 2,
                        .w = w,
                        .h = h,
                        .u1 = 1,
                        .v1 = 1,
                        .color = 0xffffffff,
                        .tex = icon.slot,
                        .radius = RADIUS - 2});
  }
  if (active) {
    ui_rounded(r.x + r.w - 20, r.y + 4, 14, 14, TILE, 7, 0);
    ui_rounded(r.x + r.w - 17, r.y + 7, 8, 8, ACTIVE, 4, 0);
  }
  const int ty = (int)(iy + ICON +
                       (r.y + r.h - iy - ICON - ui_text_height(TILE_TEXT)) / 2);
  const float room = r.w - MARGIN;
  if (focused && ui_text_width(TILE_TEXT, name) > room) {
    marquee(TILE_TEXT, r.x + MARGIN / 2, ty, room, name, FG, view);
    return;
  }
  int nw;
  const str fitted = ui_text_fit(TILE_TEXT, name, room, &nw);
  ui_text(TILE_TEXT, (int)(r.x + (r.w - nw) / 2), ty, fitted, FG);
}

int launcher_tiles(nav *ui, const category *category, ui_rect view) {
  size_t count = category != NULL ? category->count : 0;
  if (category == NULL)
    for (size_t i = 0; i < RUNNING_MAX; i++)
      count += runs[i].pid != 0 && !runs[i].quitting;
  if (count == 0) {
    ui_text(ROW_TEXT, view.x + MARGIN * 2, view.y + MARGIN, S("Nothing here"),
            DIM);
    return -1;
  }
  const int cols = MAX(1, (int)((view.w - GAP) / (TILE_W + GAP)));
  const size_t rows = (count + cols - 1) / cols;
  const float left = view.x + (view.w - cols * (TILE_W + GAP) + GAP) / 2;
  const float offset = nav_scroll_begin(ui, nav_str("tiles"), view, UI_VERTICAL,
                                        rows * (TILE_H + GAP) + GAP, GAP, 0);
  int chosen = -1;
  tiles_focus = -1;
  gfx_clip(view.x, view.y, view.w, view.h);
  for (size_t i = 0; i < count; i++) {
    const ui_rect r = {left + (i % cols) * (TILE_W + GAP),
                       view.y + GAP + (i / cols) * (TILE_H + GAP) - offset,
                       TILE_W, TILE_H};
    if (r.y < view.y + view.h && r.y + r.h > view.y) {
      ui_rounded(r.x, r.y, r.w, r.h, TILE, RADIUS, 0);
      ui_rounded(r.x, r.y, r.w, r.h, EDGE, RADIUS, 1);
    }
  }
  nav_fill(ui, FOCUS, RADIUS);
  size_t run_index = 0;
  for (size_t i = 0; i < count; i++) {
    const ui_rect r = {left + (i % cols) * (TILE_W + GAP),
                       view.y + GAP + (i / cols) * (TILE_H + GAP) - offset,
                       TILE_W, TILE_H};
    const unsigned flags = nav_item(ui, nav_int(i), r, view);
    nav_ring(ui, r, RADIUS + 2);
    if (flags & NAV_FOCUSED)
      tiles_focus = (int)i;
    const app *a = category != NULL ? &category->apps[i] : NULL;
    const running *run = NULL;
    if (category == NULL) {
      while (run_index < RUNNING_MAX &&
             (runs[run_index].pid == 0 || runs[run_index].quitting))
        run_index++;
      run = &runs[run_index++];
    }
    if (r.y < view.y + view.h && r.y + r.h > view.y)
      tile(a != NULL ? sb_str(a->name) : sb_str(run->label),
           a != NULL ? sb_str(a->icon) : sb_str(run->icon),
           a != NULL ? run_same(sb_str(a->exec), sb_str(a->arg),
                                sb_str(a->dir)) != NULL
                     : false,
           r, flags & NAV_FOCUSED, view);
    if (flags & NAV_ACTIVATED)
      chosen = (int)i;
  }
  gfx_noclip();
  nav_scroll_end(ui);
  return chosen;
}

const entry *dialog_box(nav *ui, int w, int h, str title, str text,
                        const entry *entries, size_t count) {
  ui_fill(0, 0, w, h, SCRIM);
  const float dw = MIN(w - MARGIN * 4, 480),
              line = ui_text_height(ROW_TEXT) + 6;
  const float tw = dw - MARGIN * 4;
  str wrapped[8];
  size_t n = 0;
  for (str rest = text; rest.n > 0 && n < COUNTOF(wrapped); n++) {
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
  const float list =
      MIN(ROW * count + MARGIN * 2, h - MARGIN * 8 - line * lines);
  const float dh = MARGIN * 2 + line * lines + list + MARGIN;
  const float x = (w - dw) / 2, y = (h - dh) / 2;
  ui_rounded(x, y, dw, dh, TILE, RADIUS, 0);
  ui_rounded(x, y, dw, dh, EDGE, RADIUS, 1);
  ui_text(ROW_TEXT, (int)x + MARGIN * 2, (int)(y + MARGIN * 2),
          ui_text_fit(ROW_TEXT, title, dw - MARGIN * 4, NULL), FG);
  for (size_t i = 0; i < n; i++)
    ui_text(ROW_TEXT, (int)x + MARGIN * 2,
            (int)(y + MARGIN * 2 + line * ((title.n > 0) + i)),
            ui_text_fit(ROW_TEXT, wrapped[i], tw, NULL), MUTED);
  const ui_rect view = {x + MARGIN, y + MARGIN + line * lines + MARGIN,
                        dw - MARGIN * 2, list};
  return menu(ui, entries, count, view);
}

nav_id menu_row(nav_id scope, size_t i) {
  return nav_child(nav_child(scope, nav_str("menu")), nav_int(i));
}

int focused_row(const nav *ui, nav_id scope, size_t count) {
  for (size_t i = 0; i < count; i++)
    if (ui->focus == menu_row(scope, i))
      return (int)i;
  return -1;
}

size_t launcher_perf_rows(entry *rows) {
  static const int icons[PERF_METRICS] = {
      ICON_GAUGE,   ICON_CPU,        ICON_MICROCHIP, ICON_PLAY,
      ICON_LIBRARY, ICON_HARD_DRIVE, ICON_SUN};
  static STRBUF(64) corner;
  size_t n = 0;
  rows[n++] = (entry){.label = S("Performance overlay"),
                      .action = PERF,
                      .icon = ICON_GAUGE,
                      .arg = -1,
                      .checked = perf_on()};
  sb_set(corner, S("Corner  \u00b7  "), perf_corner_labels[perf_corner()]);
  if (perf_on())
    rows[n++] = (entry){.label = sb_str(corner),
                        .action = PERF_CORNER,
                        .icon = ICON_IMAGE,
                        .depth = 1};
  for (int i = 0; perf_on() && i < PERF_METRICS; i++)
    rows[n++] = (entry){.label = perf_labels[i],
                        .action = PERF,
                        .icon = icons[i],
                        .arg = i,
                        .checked = perf_shows(i),
                        .depth = 1};
  return n;
}

int launcher_power_focus = -1;

bool launcher_power_page(nav *ui, ui_rect view, bool calibration) {
  static const str labels[POWER_ROWS] = {S("Automatic calibration"), S("CPU"),
                                         S("GPU"), S("Brightness")};
  static const int icons[POWER_ROWS] = {ICON_GAUGE, ICON_CPU, ICON_MICROCHIP,
                                        ICON_SUN};
  bool calibrate = false;
  launcher_power_focus = -1;
  const int first = calibration ? 0 : POWER_CPU;
  const float offset =
      nav_scroll_begin(ui, nav_str("power"), view, UI_VERTICAL,
                       (POWER_ROWS - first) * ROW + MARGIN * 2, ROW, 0);
  nav_fill(ui, FOCUS, 6);
  gfx_clip(view.x, view.y, view.w, view.h);
  for (int i = first; i < POWER_ROWS; i++) {
    const ui_rect row = {view.x + MARGIN,
                         view.y + MARGIN + (i - first) * ROW - offset,
                         view.w - MARGIN * 2, ROW - 4};
    const unsigned flags = nav_item(ui, nav_int(i), row, view);
    nav_ring(ui, row, 8);
    if (flags & NAV_FOCUSED)
      launcher_power_focus = i;
    if ((flags & NAV_ACTIVATED) && i == POWER_CALIBRATE)
      calibrate = true;
    else if (flags & NAV_ACTIVATED)
      power_step(i, 1);
    STRBUF(64) value;
    sb_set(value, S("\u2039 "), power_value(i), S(" \u203a"));
    const int y = (int)row.y + ((int)row.h - ui_text_height(ROW_TEXT)) / 2;
    const int vw = ui_text_width(ROW_TEXT, sb_str(value)),
              lx = (int)row.x + MARGIN * 2 + ICON_PX;
    ui_icon(icons[i], ICON_PX, (int)row.x + MARGIN,
            (int)row.y + ((int)row.h - ICON_PX) / 2, FG);
    ui_text(ROW_TEXT, lx, y,
            ui_text_fit(ROW_TEXT, labels[i],
                        row.x + row.w - lx - vw - MARGIN * 2, NULL),
            FG);
    ui_text(ROW_TEXT, (int)(row.x + row.w) - MARGIN - vw, y, sb_str(value),
            flags & NAV_FOCUSED ? FG : MUTED);
  }
  gfx_noclip();
  nav_scroll_end(ui);
  return calibrate;
}

void widgets_frame_begin(uint64_t now) {
  frame_now = now;
  marquee_due = 0;
  marquee_drawn = false;
}

void widgets_frame_end(void) {
  if (!marquee_drawn)
    marquee_of = 0;
}

uint64_t widgets_due(void) { return marquee_due; }
