/*
 * The part of webos-native's loom that nav.c needs, drawn straight with gfx
 * quads instead of through a command list.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <SDL.h>

#include "gfx.h"

typedef struct {
  float x, y, w, h;
} ui_rect;
typedef uint8_t ui_color[4];
/* nav.c carries it around; nothing here draws with it. */
typedef struct {
  float edge[4];
  float width[4];
} ui_mask;
typedef enum { UI_HORIZONTAL, UI_VERTICAL } ui_axis;

typedef struct {
  ui_mask mask;
} ui_context;

static inline bool ui_contains(ui_rect r, float px, float py) {
  return px >= r.x && py >= r.y && px < r.x + r.w && py < r.y + r.h;
}

static inline ui_rect ui_inset(ui_rect r, float n) {
  return (ui_rect){r.x + n, r.y + n, SDL_max(0, r.w - n * 2),
                     SDL_max(0, r.h - n * 2)};
}

static inline void ui_stroke(ui_context *ctx, ui_rect r,
                             const ui_rect *clip, const ui_color color,
                             float width, float radius) {
  (void)ctx;
  if (clip != NULL)
    gfx_clip(clip->x, clip->y, clip->w, clip->h);
  gfx_push((gfx_quad){
      .x = r.x, .y = r.y, .w = r.w, .h = r.h,
      .u1 = 1, .v1 = 1,
      .color = color[0] | color[1] << 8 | color[2] << 16 | (uint32_t)color[3] << 24,
      .tex = GFX_WHITE,
      .radius = radius,
      .border = width,
  });
  gfx_noclip();
}
