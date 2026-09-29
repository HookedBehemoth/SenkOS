/*
 * One frame of the output through weston_capture_v1, then shrunk and
 * box-blurred on the CPU: once per pause, so the GPU never spends a frame on it.
 *
 * On a connection of its own: a wl_output bound on SDL's could not be released
 * (v1), and SDL's surface enter/leave handlers crash on the dead proxy.
 */
#include "capture.h"

#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>

#include "weston-output-capture-client-protocol.h"

typedef struct {
  struct wl_output *output;
  struct wl_shm *shm;
  struct weston_capture_v1 *capture;
  uint32_t shm_format;
  Uint32 sdl_format;
  int width, height;
  int done; /* 1 complete, -1 failed */
} capture;

static void global(void *data, struct wl_registry *registry, uint32_t name,
                   const char *interface, uint32_t version) {
  (void)version;
  capture *c = data;
  if (!c->output && SDL_strcmp(interface, wl_output_interface.name) == 0)
    c->output = wl_registry_bind(registry, name, &wl_output_interface, 1);
  else if (!c->shm && SDL_strcmp(interface, wl_shm_interface.name) == 0)
    c->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
  else if (SDL_strcmp(interface, weston_capture_v1_interface.name) == 0)
    c->capture = wl_registry_bind(registry, name, &weston_capture_v1_interface, 1);
}

static void global_remove(void *data, struct wl_registry *registry,
                          uint32_t name) {
  (void)data, (void)registry, (void)name;
}

static const struct wl_registry_listener registry_listener = {global,
                                                              global_remove};

/* The GL renderer reads back ARGB8888, or ABGR8888 without BGRA reads. wl_shm
 * codes are the DRM ones, but for 0 and 1. */
static void format(void *data, struct weston_capture_source_v1 *source,
                   uint32_t drm_format) {
  (void)source;
  capture *c = data;
  if (c->sdl_format != 0)
    return;
  switch (drm_format) {
  case 0x34325241: /* AR24 */
    c->shm_format = WL_SHM_FORMAT_ARGB8888;
    c->sdl_format = SDL_PIXELFORMAT_ARGB8888;
    break;
  case 0x34325258: /* XR24 */
    c->shm_format = WL_SHM_FORMAT_XRGB8888;
    c->sdl_format = SDL_PIXELFORMAT_ARGB8888;
    break;
  case WL_SHM_FORMAT_ABGR8888:
  case WL_SHM_FORMAT_XBGR8888:
    c->shm_format = drm_format;
    c->sdl_format = SDL_PIXELFORMAT_ABGR8888;
    break;
  }
}

static void size(void *data, struct weston_capture_source_v1 *source,
                 int32_t width, int32_t height) {
  (void)source;
  capture *c = data;
  c->width = width;
  c->height = height;
}

static void complete(void *data, struct weston_capture_source_v1 *source) {
  (void)source;
  ((capture *)data)->done = 1;
}

static void retry(void *data, struct weston_capture_source_v1 *source) {
  (void)source;
  ((capture *)data)->done = -1;
}

static void failed(void *data, struct weston_capture_source_v1 *source,
                   const char *msg) {
  (void)source;
  SDL_Log("capture: %s", msg ? msg : "failed");
  ((capture *)data)->done = -1;
}

static const struct weston_capture_source_v1_listener source_listener = {
    .format = format,
    .size = size,
    .complete = complete,
    .retry = retry,
    .failed = failed,
};

enum { SHRINK = 4, RADIUS = 2, PASSES = 3 };

/* One box pass along a line of `n` pixels `along` bytes apart, for `lines`
 * lines `across` bytes apart; edges clamp. A running sum: one add and one
 * subtract a pixel, whatever the radius. Alpha (byte 3) is left alone. */
static void box(const uint8_t *src, uint8_t *dst, int n, int lines,
                size_t along, size_t across) {
  for (int l = 0; l < lines; l++)
    for (int ch = 0; ch < 3; ch++) {
      const uint8_t *s = src + l * across + ch;
      uint8_t *d = dst + l * across + ch;
      int sum = 0;
      for (int k = -RADIUS; k <= RADIUS; k++)
        sum += s[SDL_clamp(k, 0, n - 1) * along];
      for (int i = 0; i < n; i++) {
        d[i * along] = sum / (RADIUS * 2 + 1);
        sum += s[SDL_min(i + RADIUS + 1, n - 1) * along] -
               s[SDL_max(i - RADIUS, 0) * along];
      }
    }
}

/* A quarter the size, then three box blurs each way, which is close to a
 * gaussian; linear filtering scales it back up. */
static SDL_Texture *blur(SDL_Renderer *renderer, const uint8_t *src,
                         int width, int height, Uint32 sdl_format) {
  const int w = width / SHRINK, h = height / SHRINK;
  const size_t stride = (size_t)w * 4;
  uint8_t *a = malloc(stride * h), *b = malloc(stride * h);
  SDL_Texture *texture = NULL;
  if (a == NULL || b == NULL || w == 0 || h == 0)
    goto out;
  /* Byte 3 is alpha in both formats: opaque, whatever the read back held */
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      for (int ch = 0; ch < 3; ch++) {
        unsigned sum = 0;
        for (int dy = 0; dy < SHRINK; dy++)
          for (int dx = 0; dx < SHRINK; dx++)
            sum += src[((size_t)(y * SHRINK + dy) * width + x * SHRINK + dx) *
                           4 + ch];
        a[y * stride + x * 4 + ch] = sum / (SHRINK * SHRINK);
      }
      a[y * stride + x * 4 + 3] = b[y * stride + x * 4 + 3] = 255;
    }
  for (int i = 0; i < PASSES; i++) {
    box(a, b, w, h, 4, stride);
    box(b, a, h, w, stride, 4);
  }
  texture = SDL_CreateTexture(renderer, sdl_format, SDL_TEXTUREACCESS_STATIC,
                              w, h);
  if (texture != NULL) {
    SDL_UpdateTexture(texture, NULL, a, (int)stride);
    SDL_SetTextureScaleMode(texture, SDL_ScaleModeLinear);
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_NONE);
  }
out:
  free(a);
  free(b);
  return texture;
}

SDL_Texture *capture_blurred(SDL_Renderer *renderer) {
  struct wl_display *display = wl_display_connect(NULL);
  if (display == NULL)
    return NULL;
  struct wl_registry *registry = wl_display_get_registry(display);
  capture c = {0};
  wl_registry_add_listener(registry, &registry_listener, &c);
  wl_display_roundtrip(display);

  struct weston_capture_source_v1 *source = NULL;
  struct wl_buffer *buffer = NULL;
  void *pixels = MAP_FAILED;
  size_t bytes = 0;
  SDL_Texture *texture = NULL;
  if (c.output == NULL || c.shm == NULL || c.capture == NULL)
    goto out;
  source = weston_capture_v1_create(c.capture, c.output,
                                    WESTON_CAPTURE_V1_SOURCE_FRAMEBUFFER);
  weston_capture_source_v1_add_listener(source, &source_listener, &c);
  wl_display_roundtrip(display);
  if (c.sdl_format == 0 || c.width <= 0 || c.height <= 0)
    goto out;

  const int stride = c.width * 4;
  bytes = (size_t)stride * c.height;
  const int fd = memfd_create("capture", MFD_CLOEXEC);
  if (fd < 0)
    goto out;
  if (ftruncate(fd, (off_t)bytes) == 0)
    pixels = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (pixels != MAP_FAILED) {
    struct wl_shm_pool *pool = wl_shm_create_pool(c.shm, fd, (int32_t)bytes);
    buffer = wl_shm_pool_create_buffer(pool, 0, c.width, c.height, stride,
                                       c.shm_format);
    wl_shm_pool_destroy(pool);
  }
  close(fd);
  if (buffer == NULL)
    goto out;
  /* Done at the next repaint, which the request schedules. */
  weston_capture_source_v1_capture(source, buffer);
  while (c.done == 0 && wl_display_dispatch(display) >= 0)
    ;
  if (c.done > 0)
    texture = blur(renderer, pixels, c.width, c.height, c.sdl_format);

out:
  if (buffer != NULL)
    wl_buffer_destroy(buffer);
  if (pixels != MAP_FAILED)
    munmap(pixels, bytes);
  if (source != NULL)
    weston_capture_source_v1_destroy(source);
  if (c.capture != NULL)
    weston_capture_v1_destroy(c.capture);
  if (c.shm != NULL)
    wl_shm_destroy(c.shm);
  if (c.output != NULL)
    wl_output_destroy(c.output);
  wl_registry_destroy(registry);
  wl_display_disconnect(display);
  return texture;
}
