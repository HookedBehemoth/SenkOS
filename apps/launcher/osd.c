/* Volume, brightness, text, and performance overlays. */
#include <limits.h>

#include "launcher_internal.h"
#include "layer.h"
#include "perf.h"
#include "util.h"
#include "wake.h"

static win osd = {.app_id = "launcher", .title = "osd", .w = OSD_W, .h = OSD_H};
static win perf = {.app_id = "launcher",
                   .title = "perf",
                   .w = PERF_W,
                   .h = PERF_METRICS * PERF_LINE + 8};
static gfx_target *osd_screen, *perf_screen;
static uint64_t osd_until;
static bool perf_pending, perf_move;

/* Top centre */
static void map_osd(void) {
  if (osd.configured)
    return;
  layer_overlay(&osd, LAYER_TOP, OSD_TOP);
  win_map(&osd);
}

void osd_show_level(bool brightness, int percent) {
  map_osd();
  if (osd_screen == NULL)
    osd_screen = gfx_target_create(&osd, true);
  if (osd_screen == NULL)
    return;
  int w, h;
  if (!gfx_begin(osd_screen, &w, &h))
    return;
  ui_fill(0, 0, w, h, OSD_BG);
  const float x0 = MARGIN + 6, x1 = x0 + 8, y = OSD_H / 2;
  if (brightness) {
    ui_icon(ICON_SUN, BAR_ICON, MARGIN - 2, (OSD_H - BAR_ICON) / 2, OSD_FG);
  } else {
    ui_fill(MARGIN, OSD_H / 2 - 5, 6, 10, OSD_FG);
    for (float x = x0; x < x1; x++) {
      const float half = 5 + (x - x0) * 6 / (x1 - x0);
      ui_fill(x, y - half, 1, half * 2, OSD_FG);
    }
  }
  const float bx = x1 + MARGIN, bw = OSD_W - bx - MARGIN * 1.5f;
  ui_rounded(bx, y - 3, bw, 6, OSD_TRACK, 3, 0);
  ui_rounded(bx, y - 3, bw * CLAMP(percent, 0, 100) / 100, 6, OSD_FG, 3, 0);
  gfx_end(osd_screen);
  osd_until = ms_now() + OSD_MS;
}

void osd_show_text(str text, int ms) {
  map_osd();
  if (osd_screen == NULL)
    osd_screen = gfx_target_create(&osd, true);
  int w, h;
  if (osd_screen == NULL || !gfx_begin(osd_screen, &w, &h))
    return;
  ui_fill(0, 0, w, h, OSD_BG);
  int tw;
  text = ui_text_fit(BAR_TEXT, text, w - MARGIN * 2, &tw);
  ui_text(BAR_TEXT, (w - tw) / 2, (h - ui_text_height(BAR_TEXT)) / 2, text,
          OSD_FG);
  gfx_end(osd_screen);
  osd_until = ms_now() + ms;
}

static const unsigned corner_anchors[PERF_CORNERS] = {
    [PERF_TOP_LEFT] = LAYER_TOP | LAYER_LEFT,
    [PERF_TOP_RIGHT] = LAYER_TOP | LAYER_RIGHT,
    [PERF_BOTTOM_LEFT] = LAYER_BOTTOM | LAYER_LEFT,
    [PERF_BOTTOM_RIGHT] = LAYER_BOTTOM | LAYER_RIGHT,
};

void show_perf(void) {
  if (!perf.configured) {
    layer_overlay(&perf, corner_anchors[perf_corner()], 0);
    win_map(&perf);
  }
  if (perf_screen == NULL)
    perf_screen = gfx_target_create(&perf, true);
  int w, h;
  if (perf_screen == NULL || !gfx_begin(perf_screen, &w, &h))
    return;
  str lines[PERF_METRICS];
  const size_t n = perf_sample(lines);
  const int corner = perf_corner();
  const bool right = corner == PERF_TOP_RIGHT || corner == PERF_BOTTOM_RIGHT;
  const int top = corner >= PERF_BOTTOM_LEFT ? h - 4 - (int)n * PERF_LINE : 4;
  for (size_t i = 0; i < n; i++) {
    const int y = top + (int)i * PERF_LINE,
              bw = ui_text_width(PERF_TEXT, lines[i]) + 12,
              x = right ? w - bw : 0;
    ui_fill(x, y - 1, bw, PERF_LINE, OSD_BG);
    ui_text(PERF_TEXT, x + 6,
            y + (PERF_LINE - ui_text_height(PERF_TEXT)) / 2 - 1, lines[i],
            OSD_FG);
  }
  gfx_end(perf_screen);
}

void launcher_perf_action(int action, intptr_t arg) {
  if (action == PERF_CORNER) {
    perf_step_corner((int)arg);
    perf_move = true;
  } else if (action == PERF) {
    perf_toggle(arg);
  }
  perf_pending = true;
}

void osd_flush(void) {
  if (!perf_pending)
    return;
  perf_pending = false;
  if (perf_move) {
    win_unmap(&perf);
    perf_move = false;
  }
  if (perf_on())
    show_perf();
  else if (perf.configured)
    win_unmap(&perf);
}

uint64_t osd_due(void) { return osd_until; }

void osd_tick(uint64_t now) {
  if (osd_until != 0 && now >= osd_until) {
    win_unmap(&osd);
    osd_until = 0;
  }
}

void osd_limit_notice(void) {
  if (osd_until > ms_now() + RESULT_MS)
    osd_until = ms_now();
}
