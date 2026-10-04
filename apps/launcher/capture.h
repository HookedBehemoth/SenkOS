/*
 * The pause screen's backdrop: the frame on the output, blurred; and screenshots.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* RGBA, opaque, to free(). NULL without Weston, or when the capture fails. */
uint8_t *capture_blurred(int *w, int *h);

/* `launcher screenshot FILE`: the output's frame at full size, as a binary PPM */
bool capture_ppm(const char *path);
