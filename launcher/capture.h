/*
 * The pause screen's backdrop: the frame on the output, blurred.
 */
#pragma once

#include <SDL.h>

/* NULL without Weston, or when the capture fails. */
SDL_Texture *capture_blurred(SDL_Renderer *renderer);
