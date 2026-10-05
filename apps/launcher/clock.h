/*
 * Time settings, as `clock` (anbernic-base, root's: sleepd runs it on "clock ..." in /run/power)
 * keeps them in /data/OS/time: the zone (localtime, a link into /usr/share/zoneinfo, which
 * /etc/localtime links to), `chosen` when it was picked rather than guessed, `manual` when the
 * time is set by hand rather than by NTP. The zones to pick from: tzdata's zone.tab, a region
 * (Europe) then a place (Europe/Berlin).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "str.h"

/* "Europe/Berlin"; "" for none yet (UTC) */
str clock_zone(void);
bool clock_zone_chosen(void);
bool clock_ntp(void);
/* The zone changed since the last call (or this is the first): TZ is set to it, for this
 * process's clock and what it starts */
bool clock_refresh(void);

/* zone.tab's, sorted, read once: the regions, and a region's zones (whole names) */
size_t clock_regions(const str **out);
size_t clock_zones(str region, const str **out);
