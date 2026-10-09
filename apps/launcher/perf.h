/*
 * The performance overlay: a line a metric, top left over everything (the
 * kiosk shell keeps the launcher's window titled "perf" there), sampled once a
 * second. Frames are what the display shows (Weston's frames.so), CPU is
 * /proc/stat and cpu0's clock, GPU its time busy (kernel patch 0058's
 * busy_ns) and its devfreq clock, the video decoder its time busy (0059's,
 * Cedrus), memory MemTotal - MemAvailable, SD
 * the MMC disks' sectors, temperature the hottest thermal zone.
 *
 * Toggled in Settings > Debug and in every app's pause menu (Debug), the
 * same setting. Kept in $XDG_CONFIG_HOME/launcher-perf: "on|off" and the
 * metrics shown, by name.
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
/* The shown metrics' lines, since the last call (the first: since start) */
size_t perf_sample(str lines[PERF_METRICS]);
