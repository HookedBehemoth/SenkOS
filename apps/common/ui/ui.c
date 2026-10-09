#include "ui.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb-ft.h>
#include <hb.h>

#include "gfx.h"
#include "skyline.h"
#include "util.h"

typedef struct {
  unsigned gid;
  skyline_region region;
  int left, top;
} glyph;

/* One FT_Face per pixel size, so sizes never re-set each other's scale. */
typedef struct {
  int px;
  FT_Face face;
  hb_font_t *hb;
  glyph *glyphs;
  size_t count, capacity;
} sized_font;

static FT_Library library;
static char *font_file;
static int font_index;
static skyline atlas;
static gfx_image texture;
static sized_font sizes[16];
static size_t size_count;

static const ui_palette dark = {
    .bg = {0x14, 0x14, 0x18, 0xff},
    .fg = {0xee, 0xee, 0xee, 0xff},
    .dim = {0x8a, 0x8a, 0x94, 0xff},
    .faint = {0x55, 0x55, 0x5c, 0xff},
    .focus = {0x2a, 0x2e, 0x3a, 0xff},
    .tile = {0x1c, 0x1d, 0x23, 0xff},
    .edge = {0x33, 0x34, 0x3c, 0xff},
    .key = {0x26, 0x27, 0x2e, 0xff},
    .key_focus = {0x3a, 0x40, 0x52, 0xff},
    .accent = {0x6a, 0x9c, 0xff, 0xff},
    .ok = {0x50, 0xc8, 0x60, 0xff},
    .bad = {0xe0, 0x60, 0x60, 0xff},
};
static const ui_palette light = {
    .bg = {0xf2, 0xf2, 0xf5, 0xff},
    .fg = {0x1c, 0x1c, 0x22, 0xff},
    .dim = {0x66, 0x66, 0x70, 0xff},
    .faint = {0xa4, 0xa4, 0xae, 0xff},
    .focus = {0xdc, 0xe4, 0xf6, 0xff},
    .tile = {0xff, 0xff, 0xff, 0xff},
    .edge = {0xd4, 0xd4, 0xdc, 0xff},
    .key = {0xe4, 0xe4, 0xea, 0xff},
    .key_focus = {0xc4, 0xd4, 0xf4, 0xff},
    .accent = {0x2a, 0x6a, 0xe0, 0xff},
    .ok = {0x2a, 0x9a, 0x40, 0xff},
    .bad = {0xd0, 0x36, 0x36, 0xff},
};
ui_palette ui_pal;
bool ui_light;

void ui_scheme(void) {
  ui_light = access(UI_LIGHT, F_OK) == 0;
  ui_pal = ui_light ? light : dark;
}

bool ui_init(const char *pattern) {
  ui_scheme();
  if (!FcInit())
    return false;
  FcPattern *wanted = FcNameParse((const FcChar8 *)pattern);
  FcConfigSubstitute(NULL, wanted, FcMatchPattern);
  FcDefaultSubstitute(wanted);
  FcResult result;
  FcPattern *match = FcFontMatch(NULL, wanted, &result);
  FcPatternDestroy(wanted);
  FcChar8 *file;
  if (match == NULL ||
      FcPatternGetString(match, FC_FILE, 0, &file) != FcResultMatch) {
    fprintf(stderr, "text: no font matches \"%s\"\n", pattern);
    return false;
  }
  font_file = strdup((const char *)file);
  FcPatternGetInteger(match, FC_INDEX, 0, &font_index);
  FcPatternDestroy(match);
  fprintf(stderr, "text: %s\n", font_file);
  return FT_Init_FreeType(&library) == 0 && skyline_init(&atlas, 512);
}

static const glyph *glyph_for(sized_font *f, unsigned gid);

static sized_font *font_for(int px) {
  for (size_t i = 0; i < size_count; i++)
    if (sizes[i].px == px)
      return &sizes[i];
  /* ponytail: a fixed handful of sizes; a UI that animates font size would
   * want eviction. */
  if (size_count == COUNTOF(sizes))
    return NULL;
  sized_font *f = &sizes[size_count];
  if (FT_New_Face(library, font_file, font_index, &f->face) != 0)
    return NULL;
  FT_Set_Pixel_Sizes(f->face, 0, px);
  f->hb = hb_ft_font_create_referenced(f->face);
  f->px = px;
  size_count++;
  /* Printable ASCII up front, so a frame uploads the atlas once instead of
   * after each string with new glyphs: the texture is in use by then, and
   * Mesa copies all of it for every such update. */
  for (FT_ULong c = ' '; c <= '~'; c++)
    glyph_for(f, FT_Get_Char_Index(f->face, c));
  return f;
}

bool ui_atlas_add(const uint8_t *coverage, int w, int h, int pitch, skyline_region *out) {
  while (!skyline_alloc(&atlas, w, h, out)) {
    if (atlas.size >= 4096 || !skyline_enlarge(&atlas, atlas.size * 2))
      return false;
  }
  /* Coverage, premultiplied white: the colour comes from the quad */
  uint8_t *rgba = malloc((size_t)w * h * 4);
  if (rgba == NULL)
    return false;
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      uint8_t *p = rgba + ((size_t)y * w + x) * 4;
      p[0] = p[1] = p[2] = p[3] = coverage[y * pitch + x];
    }
  skyline_blit(&atlas, *out, rgba, (size_t)w * 4);
  free(rgba);
  return true;
}

/* ponytail: linear search, fine for the few hundred glyphs of a Latin UI; a
 * hash when CJK text shows up. */
static const glyph *glyph_for(sized_font *f, unsigned gid) {
  for (size_t i = 0; i < f->count; i++)
    if (f->glyphs[i].gid == gid)
      return &f->glyphs[i];

  if (FT_Load_Glyph(f->face, gid, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT) != 0)
    return NULL;
  const FT_GlyphSlot slot = f->face->glyph;
  const FT_Bitmap *bitmap = &slot->bitmap;
  glyph g = {gid, {0}, slot->bitmap_left, slot->bitmap_top};
  if (bitmap->width > 0 && bitmap->rows > 0 &&
      bitmap->pixel_mode == FT_PIXEL_MODE_GRAY &&
      !ui_atlas_add(bitmap->buffer, bitmap->width, bitmap->rows, bitmap->pitch, &g.region))
    return NULL;

  if (f->count == f->capacity) {
    const size_t capacity = f->capacity ? f->capacity * 2 : 128;
    glyph *grown = realloc(f->glyphs, capacity * sizeof(*grown));
    if (grown == NULL)
      return NULL;
    f->glyphs = grown;
    f->capacity = capacity;
  }
  f->glyphs[f->count] = g;
  return &f->glyphs[f->count++];
}

/* The whole atlas, as a new image: the old one may be in a frame still in
 * flight. ponytail: copies all of it for a few new glyphs; dirty rows into a
 * persistent image if text changes every frame. */
static bool upload(void) {
  if (!atlas.dirty && texture.image != VK_NULL_HANDLE)
    return true;
  gfx_image fresh;
  if (!gfx_upload(&fresh, atlas.data, atlas.size, atlas.size))
    return false;
  gfx_release(&texture);
  texture = fresh;
  gfx_bind(GFX_TEXT, &texture);
  atlas.dirty = false;
  return true;
}

void ui_rounded(float x, float y, float w, float h, gfx_color c, float radius, float border) {
  gfx_push((gfx_quad){.x = x, .y = y, .w = w, .h = h, .u1 = 1, .v1 = 1,
                      .color = gfx_rgba(c), .tex = GFX_WHITE,
                      .radius = radius, .border = border});
}

void ui_fill(float x, float y, float w, float h, gfx_color c) {
  ui_rounded(x, y, w, h, c, 0, 0);
}

str ui_text_fit(int px, str utf8, float max, int *w) {
  static char out[80];
  int width = ui_text_width(px, utf8);
  size_t n = MIN(utf8.n, sizeof(out) - 3);
  if (width > max && n > 0) {
    memcpy(out, utf8.p, n);
    do {
      do
        n--;
      while (n > 0 && (out[n] & 0xc0) == 0x80);
      memcpy(out + n, "\u2026", 3);
      utf8 = (str){out, n + 3};
      width = ui_text_width(px, utf8);
    } while (n > 0 && width > max);
  }
  if (w != NULL)
    *w = width;
  return utf8;
}

int ui_text_height(int px) {
  const sized_font *f = font_for(px);
  if (f == NULL)
    return px;
  const FT_Size_Metrics *m = &f->face->size->metrics;
  return (int)((m->ascender - m->descender + 63) >> 6);
}

/* Measures, and draws too when `color` is given. */
static int run(int px, int x, int y, str utf8, const gfx_color *color) {
  sized_font *f = font_for(px);
  if (f == NULL)
    return 0;
  hb_buffer_t *buffer = hb_buffer_create();
  hb_buffer_add_utf8(buffer, utf8.p, (int)utf8.n, 0, (int)utf8.n);
  hb_buffer_guess_segment_properties(buffer);
  hb_shape(f->hb, buffer, NULL, 0);
  unsigned count;
  const hb_glyph_info_t *info = hb_buffer_get_glyph_infos(buffer, &count);
  const hb_glyph_position_t *pos = hb_buffer_get_glyph_positions(buffer, &count);

  /* Rasterize first, so the texture is complete before anything samples it. */
  if (color != NULL) {
    for (unsigned i = 0; i < count; i++)
      glyph_for(f, info[i].codepoint);
    if (!upload())
      color = NULL;
  }
  const int baseline = y + (int)((f->face->size->metrics.ascender + 32) >> 6);
  hb_position_t pen = 0;
  for (unsigned i = 0; i < count; i++) {
    const glyph *g = color != NULL ? glyph_for(f, info[i].codepoint) : NULL;
    if (g != NULL && g->region.w > 0) {
      const skyline_region r = g->region;
      gfx_push((gfx_quad){
          .x = x + (int)((pen + pos[i].x_offset + 32) >> 6) + g->left,
          .y = baseline - (int)((pos[i].y_offset + 32) >> 6) - g->top,
          .w = r.w, .h = r.h,
          .u0 = r.x, .v0 = r.y, .u1 = r.x + r.w, .v1 = r.y + r.h,
          .color = gfx_rgba(*color),
          .tex = GFX_TEXT | GFX_TEXEL,
      });
    }
    pen += pos[i].x_advance;
  }
  hb_buffer_destroy(buffer);
  return (int)((pen + 32) >> 6);
}

int ui_text_width(int px, str utf8) { return run(px, 0, 0, utf8, NULL); }

void ui_atlas_draw(skyline_region r, int x, int y, gfx_color color) {
  if (!upload())
    return;
  gfx_push((gfx_quad){.x = x, .y = y, .w = r.w, .h = r.h,
                      .u0 = r.x, .v0 = r.y, .u1 = r.x + r.w, .v1 = r.y + r.h,
                      .color = gfx_rgba(color), .tex = GFX_TEXT | GFX_TEXEL});
}

int ui_text(int px, int x, int y, str utf8, gfx_color color) {
  return run(px, x, y, utf8, &color);
}
