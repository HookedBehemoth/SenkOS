/*
 * The image's licenses, prepared by the image build (senkos_licenses in
 * senkos-image.bb) into one file, used as is (u16, little-endian):
 *
 *     program count, license count, run count, string count
 *     programs: name, license count, run count (each one's licenses and runs
 *               follow the ones before it's)
 *     licenses: string (a declared license, "MIT | Apache-2.0")
 *     runs: first string, count (a heading per file, then its paragraphs)
 *     the strings, UTF-8, each ended by NUL
 *
 * A row per recipe, by name, "ffmpeg · LGPL-2.1-or-later": the project's own
 * license files, then the full text of each license it declares that it
 * doesn't ship itself. Texts share paragraphs: each is stored once.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "str.h"

#ifndef LICENSES
#define LICENSES "/usr/share/senkos/licenses"
#endif
#define LICENSES_MAX 256

typedef struct {
  str label, name;
  uint32_t run, runs; /* first, count */
} license;

extern license licenses[LICENSES_MAX];
extern size_t license_count;

/* Once; nothing on a desktop */
void licenses_load(void);
/* licenses[i]'s texts, a blank line between, wrapped to `width` pixels at
 * `px`: lines into *lines, which live until the next call with another i, px
 * or width. Returns the count. */
size_t license_text(size_t i, int px, float width, const str **lines);
