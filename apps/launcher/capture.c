/*
 * One frame of the output through weston_capture_v1's "thumbnail" source (our
 * Weston patch: shrunk 4:1 each way on the GPU, so only 1/16 of it is read
 * back), then box-blurred on the CPU: once per pause.
 *
 * On a connection of its own: a wl_output v1 can't be released, so it goes
 * with its connection.
 */
#include "capture.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>

#include "util.h"
#include "weston-output-capture-client-protocol.h"

typedef struct {
  struct wl_output *output;
  struct wl_shm *shm;
  struct weston_capture_v1 *capture;
  uint32_t shm_format;
  bool bgra; /* else RGBA */
  int width, height;
  int done; /* 1 complete, -1 failed */
} capture;

static void global(void *data, struct wl_registry *registry, uint32_t name,
                   const char *interface, uint32_t version) {
  (void)version;
  capture *c = data;
  if (!c->output && strcmp(interface, wl_output_interface.name) == 0)
    c->output = wl_registry_bind(registry, name, &wl_output_interface, 1);
  else if (!c->shm && strcmp(interface, wl_shm_interface.name) == 0)
    c->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
  else if (strcmp(interface, weston_capture_v1_interface.name) == 0)
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
  if (c->shm_format != 0)
    return;
  switch (drm_format) {
  case 0x34325241: /* AR24 */
    c->shm_format = WL_SHM_FORMAT_ARGB8888;
    c->bgra = true;
    break;
  case 0x34325258: /* XR24 */
    c->shm_format = WL_SHM_FORMAT_XRGB8888;
    c->bgra = true;
    break;
  case WL_SHM_FORMAT_ABGR8888:
  case WL_SHM_FORMAT_XBGR8888:
    c->shm_format = drm_format;
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
  fprintf(stderr, "capture: %s\n", msg ? msg : "failed");
  ((capture *)data)->done = -1;
}

static const struct weston_capture_source_v1_listener source_listener = {
    .format = format,
    .size = size,
    .complete = complete,
    .retry = retry,
    .failed = failed,
};

enum { RADIUS = 2, PASSES = 3 };

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
        sum += s[CLAMP(k, 0, n - 1) * along];
      for (int i = 0; i < n; i++) {
        d[i * along] = sum / (RADIUS * 2 + 1);
        sum += s[MIN(i + RADIUS + 1, n - 1) * along] -
               s[MAX(i - RADIUS, 0) * along];
      }
    }
}

/* Three box blurs each way, in place, which is close to a gaussian; linear
 * filtering scales it back up. Returns a copy in RGBA. */
static uint8_t *blur(uint8_t *a, int w, int h, bool bgra) {
  const size_t stride = (size_t)w * 4;
  uint8_t *b = malloc(stride * h);
  if (b == NULL)
    return NULL;
  /* Byte 3 is alpha in both formats: opaque, whatever the read back held */
  for (size_t i = 3; i < stride * h; i += 4)
    a[i] = 255;
  for (int i = 0; i < PASSES; i++) {
    box(a, b, w, h, 4, stride);
    box(b, a, h, w, stride, 4);
  }
  memcpy(b, a, stride * h);
  if (bgra)
    for (size_t i = 0; i < stride * h; i += 4) {
      const uint8_t t = b[i];
      b[i] = b[i + 2];
      b[i + 2] = t;
    }
  return b;
}

uint8_t *capture_blurred(int *w, int *h) {
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
  uint8_t *rgba = NULL;
  if (c.output == NULL || c.shm == NULL || c.capture == NULL)
    goto out;
  source = weston_capture_v1_create(c.capture, c.output,
                                    WESTON_CAPTURE_V1_SOURCE_THUMBNAIL);
  weston_capture_source_v1_add_listener(source, &source_listener, &c);
  wl_display_roundtrip(display);
  if (c.shm_format == 0 || c.width <= 0 || c.height <= 0)
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
  if (c.done > 0 && (rgba = blur(pixels, c.width, c.height, c.bgra)) != NULL) {
    *w = c.width;
    *h = c.height;
  }

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
  return rgba;
}
