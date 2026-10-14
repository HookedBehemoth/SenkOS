/*
 * Where the launcher's windows go, through Weston's senkos shell (apps/shell,
 * senkos_shell_v1): the launcher's own above the apps, shown and hidden;
 * overlays above everything. Without it (a desktop compositor, testing): the
 * window mapped and unmapped, overlays wherever the compositor puts them.
 */
#pragma once

#include <stdbool.h>

#include "win.h"

/* After win_connect */
void layer_connect(void);
/* Over the app, focused, from its next frame; or hidden, the app focused.
 * Mapped first if it isn't */
void layer_show(win *w, bool shown);
/* At the edges (LAYER_*) of the screen, margin in, centred on the others,
 * never focused. Before win_map; again to move it. */
enum { LAYER_TOP = 1, LAYER_BOTTOM = 2, LAYER_LEFT = 4, LAYER_RIGHT = 8 };
void layer_overlay(win *w, unsigned anchor, int margin);
/* The windows of the app with that process group, under the launcher's: it
 * comes back as it was, without mapping them again. Not Xwayland's. */
void layer_show_app(int pgid);
/* The display's refresh rate in mHz, one of its modes'; the shell keeps it */
void layer_refresh(int mhz);
