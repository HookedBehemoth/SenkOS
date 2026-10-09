/*
 * Skyline binary packing, after
 * http://clb.demon.fi/projects/even-more-rectangle-bin-packing
 *
 * A byte a texel: what is packed here is glyph and icon coverage.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SKYLINE_BPP 1

typedef struct {
  uint16_t x, y, w, h;
} skyline_region;

typedef struct {
  struct skyline_node *nodes;
  size_t node_count, node_capacity;
  uint16_t size;
  size_t used;
  uint8_t *data;
  /* The renderer clears this once it has uploaded the texture. Rows
   * [dirty_top, dirty_bottom) are what changed since. */
  bool dirty;
  uint16_t dirty_top, dirty_bottom;
} skyline;

bool skyline_init(skyline *packer, uint16_t size);
void skyline_destroy(skyline *packer);

/* False when nothing fits; the caller then enlarges and tries again. Regions
 * carry an implicit one-pixel spacing, so two allocations never touch. */
bool skyline_alloc(skyline *packer, uint16_t w, uint16_t h,
                   skyline_region *out);
/* `stride` in bytes. */
bool skyline_blit(skyline *packer, skyline_region region, const uint8_t *data,
                  size_t stride);
/* Marks rows [top, bottom) as changed. */
void skyline_touch(skyline *packer, uint16_t top, uint16_t bottom);
/* Power of two, larger than the current size. */
bool skyline_enlarge(skyline *packer, uint16_t size);
