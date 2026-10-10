/*
 * The performance overlay: a line a metric, in a corner over everything (the
 * kiosk shell keeps the launcher's window titled "perf CORNER" there), sampled once a
 * second. Frames are what the display shows (Weston's frames.so), CPU is
 * /proc/stat and cpu0's clock, GPU its time busy (kernel patch 0058's
 * busy_ns) and its devfreq clock, the video decoder its time busy (0059's,
 * Cedrus), memory MemTotal - MemAvailable, SD
 * the MMC disks' sectors, temperature the hottest thermal zone.
 *
 * Toggled in Settings > Debug and in every app's pause menu (Debug), the
 * same setting. Kept in $XDG_CONFIG_HOME/launcher-perf: "on|off", the
 * metrics shown, by name, and the corner's name.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "str.h"

enum { PERF_FPS, PERF_CPU, PERF_GPU, PERF_VPU, PERF_MEM, PERF_SD, PERF_TEMP, PERF_METRICS };
extern const str perf_labels[PERF_METRICS];

void perf_init(void);
bool perf_on(void);
bool perf_shows(int metric);
/* A metric's line on or off, -1 the overlay; saved */
void perf_toggle(int metric);
enum { PERF_TOP_LEFT, PERF_TOP_RIGHT, PERF_BOTTOM_LEFT, PERF_BOTTOM_RIGHT, PERF_CORNERS };
/* Its window's title in each, and the corners as shown */
extern const str perf_titles[PERF_CORNERS], perf_corner_labels[PERF_CORNERS];
int perf_corner(void);
/* To the next corner; saved */
void perf_next_corner(void);
/* The shown metrics' lines, since the last call (the first: since start) */
size_t perf_sample(str lines[PERF_METRICS]);
