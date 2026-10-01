/*
 * Text: fontconfig picks the font, HarfBuzz shapes, FreeType rasterizes.
 * Glyphs are bitmaps, rendered once per pixel size and packed into one
 * texture (skyline, slot GFX_TEXT), so every size is crisp and nothing is
 * scaled. Drawing pushes gfx quads, clipped like any other.
 */
#pragma once

#include <stdbool.h>

#include <SDL.h>

/* `pattern` is a fontconfig pattern, e.g. "sans-serif:weight=medium". */
bool text_init(const char *pattern);
/* Height of a line at `px`: ascender to descender. */
int text_height(int px);
int text_width(int px, const char *utf8);
/* `y` is the top of the line box. Returns the advance. */
int text_draw(int px, int x, int y, const char *utf8, SDL_Color color);
