/*
 * What the status bar shows, told rather than polled: fds for win_wait, then what changed.
 * - The minute: a timer at the next full minute, cancelled early when the clock is set (NTP
 *   stepping it, `clock set`). Nothing tells the battery level or the signal without the
 *   AXP717 patch, so they're read again then too.
 * - /run/usb and /run/wifi (the usb and wifi scripts), the zone and NTP choice in
 *   /data/OS/time (`clock`): inotify.
 * - A card in or out: /proc/self/mountinfo.
 * - Power: the kernel's power_supply uevents (the charger; the AXP717's battery IRQs).
 * - Wi-Fi: wpa_supplicant's events (connected, disconnected), while it's on.
 */
#pragma once

#include <poll.h>

enum { WATCH_MINUTE = 1, WATCH_USB = 2, WATCH_WIFI = 4, WATCH_TIME = 8, WATCH_MOUNTS = 16, WATCH_POWER = 32 };
enum { WATCH_FDS = 5 };
extern struct pollfd watch_fds[WATCH_FDS];

void watch_init(void);
/* After each win_wait: what changed (WATCH_*), its fds drained */
unsigned watch_changed(void);
