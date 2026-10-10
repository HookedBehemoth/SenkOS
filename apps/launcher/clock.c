#include "clock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "util.h"

#define TIME "/data/OS/time"
#define ZONEINFO "/usr/share/zoneinfo/"

static STRBUF(256) target;

/* Where localtime links to, "" without one */
static str link_target(void) {
  char buf[256];
  const ssize_t n = readlink(TIME "/localtime", buf, sizeof(buf));
  return n > 0 && (size_t)n < sizeof(buf) ? (str){buf, (size_t)n} : S("");
}

str clock_zone(void) {
  const str t = sb_str(target), prefix = S(ZONEINFO);
  return t.n > prefix.n && memcmp(t.p, prefix.p, prefix.n) == 0 ? str_at(t, prefix.n) : S("");
}

bool clock_zone_chosen(void) { return access(TIME "/chosen", F_OK) == 0; }
bool clock_ntp(void) { return access(TIME "/manual", F_OK) != 0; }

bool clock_refresh(void) {
  static bool once;
  STRBUF(256) now;
  sb_set(now, link_target());
  if (once && str_eq(sb_str(now), sb_str(target)))
    return false;
  once = true;
  sb_set(target, sb_str(now));
  /* glibc reads /etc/localtime again only when TZ changes: the zone's file by name */
  if (target.n > 0) {
    STRBUF(260) tz;
    sb_set(tz, S(":"), sb_str(target));
    setenv("TZ", tz.p, 1);
  } else {
    unsetenv("TZ");
  }
  tzset();
  return true;
}

/* Zone.tab's ~420 names, kept for good once read (a few KB) */
enum { ZONES_MAX = 640, REGIONS_MAX = 16 };
static char *tab;
static str zones[ZONES_MAX], regions[REGIONS_MAX];
static size_t zone_count, region_count;

static int by_name(const void *a, const void *b) {
  const str *x = a, *y = b;
  const int c = memcmp(x->p, y->p, MIN(x->n, y->n));
  return c != 0 ? c : (x->n > y->n) - (x->n < y->n);
}

static str region_of(str zone) { return str_head(zone, str_find(zone, '/')); }

static void load(void) {
  if (tab != NULL)
    return;
  const str text = str_read(fopen(ZONEINFO "zone.tab", "r"));
  tab = text.p != NULL ? (char *)text.p : strdup("");
  /* "CC\tCOORDINATES\tZONE[\tCOMMENT]", # comments */
  for (str rest = text; rest.n > 0 && zone_count < ZONES_MAX;) {
    str line = str_cut(&rest, '\n');
    if (line.n == 0 || line.p[0] == '#')
      continue;
    str_cut(&line, '\t');
    str_cut(&line, '\t');
    const str zone = str_cut(&line, '\t');
    if (str_find(zone, '/') < zone.n)
      zones[zone_count++] = zone;
  }
  qsort(zones, zone_count, sizeof(str), by_name);
  for (size_t i = 0; i < zone_count && region_count < REGIONS_MAX; i++)
    if (region_count == 0 || !str_eq(regions[region_count - 1], region_of(zones[i])))
      regions[region_count++] = region_of(zones[i]);
}

size_t clock_regions(const str **out) {
  load();
  *out = regions;
  return region_count;
}

size_t clock_zones(str region, const str **out) {
  load();
  size_t first = 0;
  while (first < zone_count && !str_eq(region_of(zones[first]), region))
    first++;
  size_t n = 0;
  while (first + n < zone_count && str_eq(region_of(zones[first + n]), region))
    n++;
  *out = zones + first;
  return n;
}
