/*
 * Images off the SD card for immediate-mode screens: ask for one by path and
 * the size it is drawn at every frame it is shown, and draw whatever slot comes
 * back. Until the loader thread has decoded and uploaded it that is the
 * placeholder. Loaded no bigger than asked for (its longest side): a path at
 * two sizes is two images.
 *
 * The whole process stays under LIMIT: past the high mark, images not shown
 * for a while go, least recently shown first. assets_drop lets all of them go.
 *
 * What had to shrink to load is kept shrunk, a WebP in $XDG_CACHE_HOME/thumbnails, and read from
 * there from then on. "video:PATH" is a video's thumbnail: ASSETS_THUMBNAILER --thumbnail PATH OUT
 * SIZE writes it there (a frame as WebP, its longest side SIZE). "jar:PATH" is a J2ME jar's icon (jar.h).
 */
#pragma once

#include <stdint.h>

#include "str.h"

#define ASSETS_THUMBNAILER "mediaplayer"

typedef struct {
  uint32_t slot;
  int w, h; /* 1x1 for the placeholder */
} asset;

/* win_wake(UI_LOADED) whenever an image arrives, so an idle screen redraws */
void assets_init(void);
/* Before building a frame: takes in finished loads, evicts. */
void assets_frame(void);
/* "": the placeholder. `px`: the longest side it is drawn at. */
asset assets_get(str path, int px);
/* Every image out of memory (an app has the screen): asked for again, each loads again */
void assets_drop(void);
