/*
 * What a ROM is, by its CRC32: the system's game pack (tools/boxart-pack,
 * from libretro-database and libretro-thumbnails) has a title, metadata and
 * box art for every No-Intro ROM.
 *
 * Pack, little endian, every offset from the start of the file:
 *
 *   header   "GAMEPAK1", count, images, then the offsets of: crcs, records,
 *            image offsets, strings, WebP data, and the 7 lists
 *   crcs     count x u32, sorted: the binary search touches nothing else
 *   records  count x 32 bytes, in the same order (romdb_record)
 *   images   (images + 1) x u32 offsets into the WebP data
 *   lists    regions, languages, flags (one bit each), genres, developers,
 *            publishers, franchises (a record's u16 index, 0 = none):
 *            u32 n, then n x u32 string offsets
 *   strings  NUL-terminated; offset 0 is ""
 *
 * CRCs are computed in the background (a zip's from its directory) and
 * cached in $XDG_CACHE_HOME/launcher/crc32 by path, size and mtime.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <sys/stat.h>

#include "str.h"

typedef struct {
  uint32_t title, extra; /* string offsets */
  uint32_t regions, languages;
  uint16_t flags;
  uint16_t genre, developer, publisher, franchise;
  uint16_t image; /* 0xffff: none */
  uint16_t year;  /* 0: unknown */
  uint8_t players, revision;
} romdb_record;

typedef struct {
  str title;
  const romdb_record *record;
  /* For assets_get: the box art inside the pack, "" without one */
  STRBUF(300) image;
} romdb_game;

/* win_wake(UI_HASHED) when a CRC was computed, so the list can be rescanned */
void romdb_init(void);
/* The ROM's CRC if known; else false, and it is computed in the background */
bool romdb_crc(str path, const struct stat *st, uint32_t *crc);
bool romdb_find(str pack, uint32_t crc, romdb_game *out);
/* What else the ROM may be, for picking by hand: the pack's titles sharing the most words
 * with `query` (a file name, or what was typed; a word typed may be the start of one), best
 * first, a CRC each, one per title. How many, up to `max`. */
size_t romdb_search(str pack, str query, uint32_t *crcs, size_t max);
