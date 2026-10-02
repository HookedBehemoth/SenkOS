/*
 * Icons: strokes from SVGs (launcher/icons, tools/icon-paths -> icons.inc),
 * stroked by FreeType at the size they're drawn and packed into the glyph
 * atlas, so they're as crisp as the text beside them and tinted like it.
 */
#pragma once

#include <SDL.h>

#include "icons.inc"

/* `icon` (ICON_*) as a `px` square, its top left at x, y */
void icon_draw(int icon, int px, int x, int y, SDL_Color color);
