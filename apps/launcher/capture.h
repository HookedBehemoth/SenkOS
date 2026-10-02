/*
 * The pause screen's backdrop: the frame on the output, blurred.
 */
#pragma once

#include <stdint.h>

/* RGBA, opaque, to free(). NULL without Weston, or when the capture fails. */
uint8_t *capture_blurred(int *w, int *h);
