#include "text.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb-ft.h>
#include <hb.h>

#include "skyline.h"

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

static SDL_Renderer *renderer;
static FT_Library library;
static char *font_file;
static int font_index;
static skyline atlas;
static SDL_Texture *texture;
static sized_font sizes[16];
static size_t size_count;

bool text_init(SDL_Renderer *r, const char *pattern) {
  renderer = r;
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
    SDL_Log("text: no font matches \"%s\"", pattern);
    return false;
  }
  font_file = strdup((const char *)file);
  FcPatternGetInteger(match, FC_INDEX, 0, &font_index);
  FcPatternDestroy(match);
  SDL_Log("text: %s", font_file);
  return FT_Init_FreeType(&library) == 0 && skyline_init(&atlas, 512);
}

static const glyph *glyph_for(sized_font *f, unsigned gid);

static sized_font *font_for(int px) {
  for (size_t i = 0; i < size_count; i++)
    if (sizes[i].px == px)
      return &sizes[i];
  /* ponytail: a fixed handful of sizes; a UI that animates font size would
   * want eviction. */
  if (size_count == SDL_arraysize(sizes))
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
      bitmap->pixel_mode == FT_PIXEL_MODE_GRAY) {
    while (!skyline_alloc(&atlas, bitmap->width, bitmap->rows, &g.region)) {
      if (atlas.size >= 4096 || !skyline_enlarge(&atlas, atlas.size * 2))
        return NULL;
      SDL_DestroyTexture(texture);
      texture = NULL;
    }
    /* White, with the coverage as alpha: the colour comes from the colour mod. */
    uint8_t *rgba = malloc((size_t)bitmap->width * bitmap->rows * 4);
    if (rgba == NULL)
      return NULL;
    for (unsigned y = 0; y < bitmap->rows; y++)
      for (unsigned x = 0; x < bitmap->width; x++) {
        uint8_t *p = rgba + ((size_t)y * bitmap->width + x) * 4;
        p[0] = p[1] = p[2] = 255;
        p[3] = bitmap->buffer[y * bitmap->pitch + x];
      }
    skyline_blit(&atlas, g.region, rgba, (size_t)bitmap->width * 4);
    free(rgba);
  }

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

static bool upload(void) {
  if (texture == NULL) {
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                SDL_TEXTUREACCESS_STATIC, atlas.size,
                                atlas.size);
    if (texture == NULL)
      return false;
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    skyline_touch(&atlas, 0, atlas.size);
  }
  if (atlas.dirty) {
    const SDL_Rect rows = {0, atlas.dirty_top, atlas.size,
                           atlas.dirty_bottom - atlas.dirty_top};
    SDL_UpdateTexture(texture, &rows,
                      atlas.data + (size_t)atlas.dirty_top * atlas.size *
                                       SKYLINE_BPP,
                      atlas.size * SKYLINE_BPP);
    atlas.dirty = false;
  }
  return true;
}

int text_height(int px) {
  const sized_font *f = font_for(px);
  if (f == NULL)
    return px;
  const FT_Size_Metrics *m = &f->face->size->metrics;
  return (int)((m->ascender - m->descender + 63) >> 6);
}

/* Measures, and draws too when `color` is given. */
static int run(int px, int x, int y, const char *utf8, const SDL_Color *color) {
  sized_font *f = font_for(px);
  if (f == NULL)
    return 0;
  hb_buffer_t *buffer = hb_buffer_create();
  hb_buffer_add_utf8(buffer, utf8, -1, 0, -1);
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
    else {
      SDL_SetTextureColorMod(texture, color->r, color->g, color->b);
      SDL_SetTextureAlphaMod(texture, color->a);
    }
  }
  const int baseline = y + (int)((f->face->size->metrics.ascender + 32) >> 6);
  hb_position_t pen = 0;
  for (unsigned i = 0; i < count; i++) {
    const glyph *g = color != NULL ? glyph_for(f, info[i].codepoint) : NULL;
    if (g != NULL && g->region.w > 0) {
      const SDL_Rect src = {g->region.x, g->region.y, g->region.w, g->region.h};
      const SDL_Rect dst = {
          x + (int)((pen + pos[i].x_offset + 32) >> 6) + g->left,
          baseline - (int)((pos[i].y_offset + 32) >> 6) - g->top, src.w, src.h};
      SDL_RenderCopy(renderer, texture, &src, &dst);
    }
    pen += pos[i].x_advance;
  }
  hb_buffer_destroy(buffer);
  return (int)((pen + 32) >> 6);
}

int text_width(int px, const char *utf8) { return run(px, 0, 0, utf8, NULL); }

int text_draw(int px, int x, int y, const char *utf8, SDL_Color color) {
  return run(px, x, y, utf8, &color);
}
