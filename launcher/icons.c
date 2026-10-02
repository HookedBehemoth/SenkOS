#define ICON_PATHS
#include "icons.h"

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include FT_STROKER_H

#include "text.h"

/* Lucide's: 2 in the 24 box */
#define STROKE 2.0f

typedef struct {
  int icon, px;
  bool ok;
  skyline_region region;
} cached;

/* ponytail: linear search over every icon at every size drawn; a handful */
static cached cache[64];
static size_t cache_count;
static FT_Library library;

/* The 24 box, y down, to the bitmap's 26.6, y up */
static FT_Vector at(const float *p, float scale, int px) {
  return (FT_Vector){(FT_Pos)lroundf(p[0] * scale * 64), (FT_Pos)lroundf((px - p[1] * scale) * 64)};
}

static int points(int op) { return op == ICON_CUBIC ? 6 : op == ICON_CLOSE ? 0 : 2; }

/* Whether the subpath from here ends in ICON_CLOSE */
static bool closes(const float *p) {
  for (int op; (op = (int)*p) != ICON_END && op != ICON_MOVE; p += 1 + points(op))
    if (op == ICON_CLOSE)
      return true;
  return false;
}

static bool rasterize(int icon, int px, skyline_region *out) {
  FT_Stroker stroker;
  if ((library == NULL && FT_Init_FreeType(&library) != 0) ||
      FT_Stroker_New(library, &stroker) != 0)
    return false;
  const float scale = px / 24.0f;
  FT_Stroker_Set(stroker, (FT_Fixed)lroundf(STROKE / 2 * scale * 64), FT_STROKER_LINECAP_ROUND,
                 FT_STROKER_LINEJOIN_ROUND, 0);
  bool open = false;
  for (const float *p = icon_paths[icon]; (int)*p != ICON_END;) {
    const int op = (int)*p++;
    FT_Vector v[3];
    for (int i = 0; i < points(op) / 2; i++)
      v[i] = at(p + i * 2, scale, px);
    switch (op) {
    case ICON_MOVE:
      if (open)
        FT_Stroker_EndSubPath(stroker);
      FT_Stroker_BeginSubPath(stroker, &v[0], !closes(p + 2));
      open = true;
      break;
    case ICON_LINE: FT_Stroker_LineTo(stroker, &v[0]); break;
    case ICON_CUBIC: FT_Stroker_CubicTo(stroker, &v[0], &v[1], &v[2]); break;
    /* EndSubPath joins a closed one back to its start */
    case ICON_CLOSE:
      FT_Stroker_EndSubPath(stroker);
      open = false;
      break;
    }
    p += points(op);
  }
  if (open)
    FT_Stroker_EndSubPath(stroker);

  FT_UInt n_points, n_contours;
  FT_Outline outline;
  bool ok = false;
  FT_Stroker_GetCounts(stroker, &n_points, &n_contours);
  if (FT_Outline_New(library, n_points, n_contours, &outline) == 0) {
    outline.n_points = outline.n_contours = 0;
    FT_Stroker_Export(stroker, &outline);
    FT_Bitmap bitmap = {.rows = px, .width = px, .pitch = px, .num_grays = 256,
                        .pixel_mode = FT_PIXEL_MODE_GRAY, .buffer = calloc(px, px)};
    ok = bitmap.buffer != NULL && FT_Outline_Get_Bitmap(library, &outline, &bitmap) == 0 &&
         text_atlas_add(bitmap.buffer, px, px, px, out);
    free(bitmap.buffer);
    FT_Outline_Done(library, &outline);
  }
  FT_Stroker_Done(stroker);
  return ok;
}

void icon_draw(int icon, int px, int x, int y, SDL_Color color) {
  if (icon < 0 || icon >= ICON_COUNT || px <= 0)
    return;
  cached *c = NULL;
  for (size_t i = 0; i < cache_count && c == NULL; i++)
    if (cache[i].icon == icon && cache[i].px == px)
      c = &cache[i];
  if (c == NULL) {
    if (cache_count == SDL_arraysize(cache))
      return;
    c = &cache[cache_count++];
    *c = (cached){icon, px, false, {0}};
    c->ok = rasterize(icon, px, &c->region);
  }
  if (c->ok)
    text_atlas_draw(c->region, x, y, color);
}
