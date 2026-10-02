#include "skyline.h"

#include <stdlib.h>
#include <string.h>

struct skyline_node {
  uint16_t x, y, w;
};

#define INITIAL_NODES 512

static bool nodes_reserve(skyline *p, size_t extra) {
  if (p->node_count + extra <= p->node_capacity)
    return true;
  size_t capacity = p->node_capacity ? p->node_capacity * 2 : INITIAL_NODES;
  while (capacity < p->node_count + extra)
    capacity *= 2;
  struct skyline_node *grown = realloc(p->nodes, capacity * sizeof(*grown));
  if (grown == NULL)
    return false;
  p->nodes = grown;
  p->node_capacity = capacity;
  return true;
}

static void nodes_remove(skyline *p, size_t index) {
  memmove(&p->nodes[index], &p->nodes[index + 1],
          (p->node_count - index - 1) * sizeof(*p->nodes));
  p->node_count--;
}

static bool nodes_insert(skyline *p, size_t index, struct skyline_node node) {
  if (!nodes_reserve(p, 1))
    return false;
  memmove(&p->nodes[index + 1], &p->nodes[index],
          (p->node_count - index) * sizeof(*p->nodes));
  p->nodes[index] = node;
  p->node_count++;
  return true;
}

bool skyline_init(skyline *packer, uint16_t size) {
  memset(packer, 0, sizeof(*packer));
  if (!nodes_reserve(packer, INITIAL_NODES))
    return false;
  packer->nodes[0] = (struct skyline_node){1, 1, (uint16_t)(size - 2)};
  packer->node_count = 1;
  packer->size = size;
  /* Zeroed, like the reference's calloc: stale bytes between glyphs bleed into
   * neighbours under linear filtering. */
  packer->data = calloc((size_t)size * size, SKYLINE_BPP);
  if (packer->data == NULL) {
    free(packer->nodes);
    packer->nodes = NULL;
    return false;
  }
  skyline_touch(packer, 0, size);
  return true;
}

void skyline_touch(skyline *packer, uint16_t top, uint16_t bottom) {
  if (!packer->dirty || top < packer->dirty_top)
    packer->dirty_top = top;
  if (!packer->dirty || bottom > packer->dirty_bottom)
    packer->dirty_bottom = bottom;
  packer->dirty = true;
}

void skyline_destroy(skyline *packer) {
  free(packer->nodes);
  free(packer->data);
  memset(packer, 0, sizeof(*packer));
}

bool skyline_blit(skyline *packer, skyline_region region, const uint8_t *data,
                  size_t stride) {
  if (region.w == 0 || region.h == 0)
    return false;
  if (region.x + region.w > packer->size - 1 ||
      region.y + region.h > packer->size - 1)
    return false;
  for (uint16_t row = 0; row < region.h; row++)
    memcpy(packer->data + ((size_t)(region.y + row) * packer->size + region.x) *
                              SKYLINE_BPP,
           data + (size_t)row * stride, (size_t)region.w * SKYLINE_BPP);
  skyline_touch(packer, region.y, (uint16_t)(region.y + region.h));
  return true;
}

/* Where a rect of `w` x `h` would sit if placed at free-node `index`, or false
 * when it does not fit there. */
static bool fit(const skyline *p, size_t index, uint16_t w, uint16_t h,
                uint16_t *out_y) {
  const struct skyline_node *node = &p->nodes[index];
  const uint16_t x = node->x;
  uint16_t y = node->y;
  int32_t width_left = w;
  size_t i = index;

  if (x + w > p->size - 1)
    return false;
  while (width_left > 0) {
    if (i >= p->node_count)
      return false;
    node = &p->nodes[i];
    if (node->y > y)
      y = node->y;
    if (y + h > p->size - 1)
      return false;
    width_left -= node->w;
    i++;
  }
  *out_y = y;
  return true;
}

/* Merge adjacent free-nodes of the same height. */
static void merge(skyline *p) {
  for (size_t i = 0; i + 1 < p->node_count;) {
    if (p->nodes[i].y == p->nodes[i + 1].y) {
      p->nodes[i].w = (uint16_t)(p->nodes[i].w + p->nodes[i + 1].w);
      nodes_remove(p, i + 1);
    } else {
      i++;
    }
  }
}

bool skyline_alloc(skyline *packer, uint16_t w, uint16_t h,
                   skyline_region *out) {
  /* Min-height scoring, like the 2010 RectangleBinPack SkylineBinPack. The
   * padded size is what reserves the one-pixel spacing between regions. */
  const uint16_t padded_w = (uint16_t)(w + 1);
  const uint16_t padded_h = (uint16_t)(h + 1);
  size_t best_index = (size_t)-1;
  uint32_t best_width = UINT32_MAX;
  uint32_t best_height = UINT32_MAX;
  skyline_region region = {0, 0, w, h};

  for (size_t i = 0; i < packer->node_count; i++) {
    uint16_t y = 0;
    if (!fit(packer, i, padded_w, padded_h, &y))
      continue;
    const struct skyline_node *node = &packer->nodes[i];
    const uint32_t height = (uint32_t)y + padded_h;
    if (height < best_height ||
        (height == best_height && node->w > 0 && node->w < best_width)) {
      best_height = height;
      best_width = node->w;
      best_index = i;
      region.x = node->x;
      region.y = y;
    }
  }
  if (best_index == (size_t)-1)
    return false;

  /* Split the chosen free-node in two... */
  struct skyline_node split = {region.x, (uint16_t)(region.y + padded_h),
                               padded_w};
  if (!nodes_insert(packer, best_index, split))
    return false;

  /* ...then push the nodes it now overlaps out of the way. */
  const size_t i = best_index + 1;
  while (i < packer->node_count) {
    struct skyline_node *node = &packer->nodes[i];
    const struct skyline_node prev = packer->nodes[i - 1];
    if (node->x >= prev.x + prev.w)
      break;
    const int32_t shrink = (int32_t)prev.x + prev.w - node->x;
    node->x = (uint16_t)(node->x + shrink);
    const int32_t remaining = (int32_t)node->w - shrink;
    if (remaining > 0) {
      node->w = (uint16_t)remaining;
      break;
    }
    nodes_remove(packer, i);
  }

  merge(packer);
  packer->used += (size_t)padded_w * padded_h;
  *out = region;
  return true;
}

bool skyline_enlarge(skyline *packer, uint16_t size) {
  if (size == packer->size)
    return true;
  if (size < packer->size || (size & (uint16_t)(size - 1)) != 0)
    return false;

  uint8_t *grown = calloc((size_t)size * size, SKYLINE_BPP);
  if (grown == NULL)
    return false;
  const uint16_t old_size = packer->size;
  uint8_t *old_data = packer->data;

  packer->data = grown;
  packer->size = size;
  if (!nodes_reserve(packer, 1)) {
    packer->data = old_data;
    packer->size = old_size;
    free(grown);
    return false;
  }
  packer->nodes[packer->node_count++] = (struct skyline_node){
      (uint16_t)(old_size - 1), 1, (uint16_t)(size - old_size)};

  const skyline_region copy = {1, 1, (uint16_t)(old_size - 2),
                               (uint16_t)(old_size - 2)};
  skyline_blit(packer, copy, old_data + ((size_t)old_size + 1) * SKYLINE_BPP,
               (size_t)old_size * SKYLINE_BPP);
  free(old_data);
  skyline_touch(packer, 0, size);
  return true;
}
