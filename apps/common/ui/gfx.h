/*
 * Vulkan 1.3 renderer for immediate-mode screens: a frame is a list of quads,
 * drawn with one instanced draw of a 4-vertex strip. Each quad picks its
 * texture by descriptor slot (one sampler array, nonuniform indexing), clips
 * itself, and is a rounded rectangle, filled or as a border, by its SDF.
 *
 * Textures are uploaded on a transfer queue, from any thread; a slot can be
 * rebound at any time and the change reaches each frame's descriptor set
 * once that frame's previous use of it has finished.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <vulkan/vulkan.h>

#include "win.h"

/* Slots with a fixed meaning; the rest are the assets'. */
enum { GFX_WHITE, GFX_PLACEHOLDER, GFX_TEXT, GFX_BACKDROP, GFX_FIRST_FREE };
/* In gfx_quad.tex: uv in texels, not 0..1, so a growing atlas stays right */
#define GFX_TEXEL 0x80000000u

typedef struct {
  float x, y, w, h;
  float u0, v0, u1, v1;
  float clip[4]; /* x0 y0 x1 y1, filled in by gfx_push */
  uint32_t color; /* RGBA8, R in the low byte, straight alpha */
  uint32_t tex;
  float radius;
  float border; /* 0: filled */
} gfx_quad;

typedef struct {
  VkImage image;
  VkImageView view;
  VkDeviceMemory memory;
  VkDeviceSize bytes;
  int w, h;
} gfx_image;

typedef struct gfx_target gfx_target;

typedef struct {
  uint8_t r, g, b, a;
} gfx_color;

static inline uint32_t gfx_rgba(gfx_color c) {
  return c.r | c.g << 8 | c.b << 16 | (uint32_t)c.a << 24;
}

bool gfx_init(win *window);
/* A window to draw to. `translucent`: premultiplied alpha reaches the
 * compositor (the OSD); otherwise the window is opaque. */
gfx_target *gfx_target_create(win *window, bool translucent);
/* False when there is nothing to draw to (minimized, lost surface). */
bool gfx_begin(gfx_target *t, int *w, int *h);
void gfx_push(gfx_quad q);
void gfx_clip(float x, float y, float w, float h);
void gfx_noclip(void);
void gfx_end(gfx_target *t);

/* Any thread; blocks that thread until the copy is done. `rgba` is
 * premultiplied. */
bool gfx_upload(gfx_image *out, const void *rgba, int w, int h);
/* UI thread. NULL puts the placeholder back. */
void gfx_bind(uint32_t slot, const gfx_image *image);
/* UI thread: destroyed once no submitted frame can still use it. */
void gfx_release(gfx_image *image);
uint32_t gfx_slots(void);
/* Submitted frames so far; what a frame being built is numbered. */
uint64_t gfx_frame(void);
/* True once every frame up to `frame` has finished on the GPU. */
bool gfx_done(uint64_t frame);
