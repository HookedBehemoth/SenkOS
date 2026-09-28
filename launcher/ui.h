/*
 * The part of webos-native's loom that nav.c needs, drawn straight with
 * SDL_Renderer instead of through a command list.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <SDL.h>

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
  SDL_Renderer *renderer;
  ui_mask mask;
} ui_context;

static inline bool ui_contains(ui_rect r, float px, float py) {
  return px >= r.x && py >= r.y && px < r.x + r.w && py < r.y + r.h;
}

static inline ui_rect ui_inset(ui_rect r, float n) {
  return (ui_rect){r.x + n, r.y + n, SDL_max(0, r.w - n * 2),
                     SDL_max(0, r.h - n * 2)};
}

/* ponytail: square corners, `radius` is ignored. A rounded ring wants a
 * texture or SDL_RenderGeometry. */
static inline void ui_stroke(ui_context *ctx, ui_rect r,
                               const ui_rect *clip, const ui_color color,
                               float width, float radius) {
  (void)radius;
  SDL_Renderer *ren = ctx->renderer;
  if (clip != NULL)
    SDL_RenderSetClipRect(ren, &(SDL_Rect){(int)clip->x, (int)clip->y,
                                          (int)clip->w, (int)clip->h});
  SDL_SetRenderDrawColor(ren, color[0], color[1], color[2], color[3]);
  const SDL_FRect edges[4] = {
      {r.x, r.y, r.w, width},
      {r.x, r.y + r.h - width, r.w, width},
      {r.x, r.y + width, width, r.h - width * 2},
      {r.x + r.w - width, r.y + width, width, r.h - width * 2},
  };
  SDL_RenderFillRectsF(ren, edges, 4);
  SDL_RenderSetClipRect(ren, NULL);
}
