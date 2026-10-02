/*
 * Images off the SD card for immediate-mode screens: ask for one by path
 * every frame it is shown, and draw whatever slot comes back. Until the
 * loader thread has decoded and uploaded it that is the placeholder.
 *
 * The whole process stays under LIMIT: past the high mark, images not shown
 * for a while go, least recently shown first.
 */
#pragma once

#include <stdint.h>

#include "str.h"

typedef struct {
  uint32_t slot;
  int w, h; /* 1x1 for the placeholder */
} asset;

/* ui_wake(UI_LOADED) whenever an image arrives, so an idle screen redraws */
void assets_init(void);
/* Before building a frame: takes in finished loads, evicts. */
void assets_frame(void);
/* "": the placeholder */
asset assets_get(str path);
