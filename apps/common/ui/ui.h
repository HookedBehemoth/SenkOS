/*
 * Drawing for immediate-mode screens, straight to gfx quads: boxes, text,
 * icons; and the part of webos-native's loom that nav.c needs.
 *
 * Text: fontconfig picks the font, HarfBuzz shapes, FreeType rasterizes.
 * Glyphs are bitmaps, rendered once per pixel size and packed into one
 * texture (skyline, slot GFX_TEXT), so every size is crisp and nothing is
 * scaled. Icons: strokes from SVGs (apps/common/ui/icons, tools/icon-paths ->
 * icons.inc), stroked by FreeType at the size they're drawn and packed into
 * the same atlas, so they're as crisp as the text beside them and tinted
 * like it. All of it is clipped like any other quad.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "gfx.h"
#include "skyline.h"
#include "str.h"
#include "util.h"

#include "icons.inc"

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

void ui_rounded(float x, float y, float w, float h, gfx_color c, float radius, float border);
void ui_fill(float x, float y, float w, float h, gfx_color c);

/* After win_connect. `font` is a fontconfig
 * pattern, e.g. "sans-serif:weight=medium". */
bool ui_init(const char *font);
/* Height of a line at `px`: ascender to descender. */
int ui_text_height(int px);
int ui_text_width(int px, str utf8);
/* `y` is the top of the line box. Returns the advance. */
int ui_text(int px, int x, int y, str utf8, gfx_color color);
/* `utf8` cut to fit `max` pixels, with an ellipsis; its width in `*w` (or
 * NULL). The cut one lives until the next call. */
str ui_text_fit(int px, str utf8, float max, int *w);

/* `icon` (ICON_*) as a `px` square, its top left at x, y */
void ui_icon(int icon, int px, int x, int y, gfx_color color);

/* Other coverage bitmaps drawn like glyphs (icons.c): packed into the same
 * texture, then drawn tinted, a texel a pixel */
bool ui_atlas_add(const uint8_t *coverage, int w, int h, int pitch, skyline_region *out);
void ui_atlas_draw(skyline_region r, int x, int y, gfx_color color);

/* The library's reasons to win_wake (win.h), then the app's own from
 * UI_WAKE_APP */
enum {
  UI_LOADED = WIN_WAKE_APP, /* an image is in (assets.h): a frame is all it wants */
  UI_WAKE_APP,
};

typedef struct {
  ui_mask mask;
} ui_context;

static inline bool ui_contains(ui_rect r, float px, float py) {
  return px >= r.x && py >= r.y && px < r.x + r.w && py < r.y + r.h;
}

static inline ui_rect ui_inset(ui_rect r, float n) {
  return (ui_rect){r.x + n, r.y + n, MAX(0, r.w - n * 2),
                     MAX(0, r.h - n * 2)};
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
