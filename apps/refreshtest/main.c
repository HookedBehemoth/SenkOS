/*
 * Does the panel show every frame? Patterns that change on each one, drawn on
 * the CPU into shm buffers, paced by frame callbacks; only what changed is
 * redrawn and damaged, so Weston's composition stays small at 120 Hz.
 *
 * - Cells: 60 in a grid, the next lit each frame. A photo exposed for 1/10 s
 *   shows 12 lit at 120 Hz, 6 at 60 Hz, all as bright; a gap or a brighter
 *   one is a frame the panel dropped or showed twice. 1/2 s: all 60 at 120 Hz.
 * - Flicker: the screen black and white on alternate frames. 30 Hz flicker at
 *   60 Hz, close to steady grey at 120 Hz.
 * - Line: a bar crossing at a speed that doesn't depend on the rate (at 1200
 *   px a second: 10 px a frame at 120 Hz, 20 at 60); at 120 Hz it reads
 *   sharper and smoother in motion. Up and down: faster and slower.
 *
 * Top left: the refresh rate, the frames a second, the frames late (a callback
 * more than 1.5 refreshes after the last: dropped before the panel, by us or
 * Weston); with the line, its speed. A: the next pattern, B: quit.
 */
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>

#include "appctl.h"
#include "pad.h"
#include "util.h"
#include "win.h"

enum { CELLS_X = 10, CELLS_Y = 6, SPEED_STEP = 300, SPEED_MAX = 3600, LINE_W = 8 };
enum { CELLS, FLICKER, LINE, PATTERNS };
enum { BLACK = 0xff000000, WHITE = 0xffffffff };
#define RECTS 8

typedef struct {
  int x, y, w, h;
} rect;

/* Two buffers, each remembering what it was last drawn with, to undo that */
static struct buf {
  struct wl_buffer *buffer;
  uint32_t *px;
  bool busy, clear;
  rect drawn[RECTS];
  int n;
} bufs[2];
static int width, height;

static void released(void *data, struct wl_buffer *b) {
  (void)b;
  ((struct buf *)data)->busy = false;
}
static const struct wl_buffer_listener buffer_listener = {released};

static bool make_buffers(int w, int h) {
  const size_t size = (size_t)w * h * 4;
  const int fd = memfd_create("refreshtest", MFD_CLOEXEC);
  if (fd < 0 || ftruncate(fd, (off_t)size * 2) < 0)
    return false;
  uint8_t *map = mmap(NULL, size * 2, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (map == MAP_FAILED)
    return false;
  struct wl_shm_pool *pool = wl_shm_create_pool(win_shm, fd, (int32_t)size * 2);
  for (int i = 0; i < 2; i++) {
    bufs[i] = (struct buf){.px = (uint32_t *)(map + size * i), .clear = true};
    bufs[i].buffer = wl_shm_pool_create_buffer(pool, (int32_t)size * i, w, h, w * 4, WL_SHM_FORMAT_XRGB8888);
    wl_buffer_add_listener(bufs[i].buffer, &buffer_listener, &bufs[i]);
  }
  wl_shm_pool_destroy(pool);
  close(fd);
  width = w;
  height = h;
  return true;
}

static void fill(struct buf *b, rect r, uint32_t color) {
  r.w = MIN(r.x + r.w, width) - MAX(r.x, 0);
  r.h = MIN(r.y + r.h, height) - MAX(r.y, 0);
  r.x = MAX(r.x, 0);
  r.y = MAX(r.y, 0);
  for (int y = r.y; y < r.y + r.h; y++)
    for (int x = r.x; x < r.x + r.w; x++)
      b->px[y * width + x] = color;
}

/* Drawn, damaged, and remembered to be undone next time */
static void draw(struct win *w, struct buf *b, rect r, uint32_t color) {
  fill(b, r, color);
  wl_surface_damage_buffer(w->surface, r.x, r.y, r.w, r.h);
  if (b->n < RECTS)
    b->drawn[b->n++] = r;
}

/* 3x5 glyphs, a bit a pixel from the top left, row by row */
static const struct {
  char c;
  uint16_t bits;
} glyphs[] = {
    {'0', 0x7b6f}, {'1', 0x2c97}, {'2', 0x73e7}, {'3', 0x73cf}, {'4', 0x5bc9}, {'5', 0x79cf},
    {'6', 0x79ef}, {'7', 0x7249}, {'8', 0x7bef}, {'9', 0x7bcf}, {'H', 0x5bed}, {'Z', 0x72a7},
    {'F', 0x79e4}, {'P', 0x7be4}, {'S', 0x79cf}, {'L', 0x4927}, {'A', 0x2bed}, {'T', 0x7492},
    {'E', 0x79e7}, {'X', 0x5aad},
};

static void text(struct win *w, struct buf *b, int x, int y, int scale, const char *s) {
  const int x0 = x;
  for (; *s; s++, x += 4 * scale)
    for (size_t g = 0; g < COUNTOF(glyphs); g++)
      if (glyphs[g].c == *s)
        for (int i = 0; i < 15; i++)
          if (glyphs[g].bits & (1 << (14 - i)))
            fill(b, (rect){x + i % 3 * scale, y + i / 3 * scale, scale, scale}, WHITE);
  const rect r = {x0, y, x - x0, 5 * scale};
  wl_surface_damage_buffer(w->surface, r.x, r.y, r.w, r.h);
  if (b->n < RECTS)
    b->drawn[b->n++] = r;
}

static int64_t ns_now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

int main(void) {
  if (!win_connect(NULL))
    return 1;
  struct win window = {.app_id = "refreshtest", .title = "Refresh test", .fullscreen = true, .w = 640, .h = 480};
  win_map(&window);
  if (!make_buffers(window.w, window.h))
    return 1;
  const int pad = pad_open(O_NONBLOCK);

  int pattern = CELLS, fps = 0, late = 0, frames = 0;
  double line_x = 0;
  int speed = 1200;
  unsigned frame = 0;
  int64_t second = ns_now(), last = 0;
  char status[48] = "";
  for (bool running = true; running;) {
    struct pollfd fds[1] = {{pad, POLLIN, 0}};
    if (!win_wait(fds, pad >= 0, window.frame_pending ? 100 : 0))
      break;
    for (int why, value; win_woken(&why, &value);)
      running &= why != WIN_QUIT;
    struct input_event e;
    while (pad >= 0 && read(pad, &e, sizeof(e)) == sizeof(e)) {
      const int button = e.type == EV_KEY && e.value == 1 ? pad_button(e.code) : -1;
      if (button == APPCTL_A) {
        pattern = (pattern + 1) % PATTERNS;
        bufs[0].clear = bufs[1].clear = true;
      }
      if (button == APPCTL_UP || button == APPCTL_DOWN)
        speed = CLAMP(speed + (button == APPCTL_UP ? SPEED_STEP : -SPEED_STEP), SPEED_STEP, SPEED_MAX);
      running &= button != APPCTL_B;
    }
    if (window.frame_pending)
      continue;
    struct buf *b = bufs[0].busy ? &bufs[1] : &bufs[0];
    if (b->busy)
      continue;

    /* Shown: the callback came. Late: over 1.5 refreshes since the last */
    const int64_t now = ns_now(), refresh_ns = win_refresh_mhz > 0 ? 1000000000000LL / win_refresh_mhz : 16666667;
    if (last != 0 && now - last > refresh_ns * 3 / 2)
      late++;
    last = now;
    frames++;
    if (now - second >= 1000000000) {
      fps = frames;
      frames = 0;
      second = now;
    }

    if (b->clear) {
      fill(b, (rect){0, 0, width, height}, BLACK);
      wl_surface_damage_buffer(window.surface, 0, 0, width, height);
      b->clear = false;
      b->n = 0;
    }
    /* Undone here; and what the other one showed, the frame on screen, is
     * damaged too: Weston updates its copy of the surface only there */
    const struct buf *other = b == &bufs[0] ? &bufs[1] : &bufs[0];
    for (int i = 0; i < other->n; i++)
      wl_surface_damage_buffer(window.surface, other->drawn[i].x, other->drawn[i].y, other->drawn[i].w,
                               other->drawn[i].h);
    for (int i = 0; i < b->n; i++) {
      fill(b, b->drawn[i], BLACK);
      wl_surface_damage_buffer(window.surface, b->drawn[i].x, b->drawn[i].y, b->drawn[i].w, b->drawn[i].h);
    }
    b->n = 0;

    const int top = 40, cw = width / CELLS_X, ch = (height - top) / CELLS_Y;
    switch (pattern) {
    case CELLS: {
      const int i = (int)(frame % (CELLS_X * CELLS_Y));
      draw(&window, b, (rect){i % CELLS_X * cw + 2, top + i / CELLS_X * ch + 2, cw - 4, ch - 4}, WHITE);
      break;
    }
    case FLICKER: draw(&window, b, (rect){0, top, width, height - top}, frame % 2 ? WHITE : BLACK); break;
    case LINE:
      line_x += speed * (double)refresh_ns / 1e9;
      if (line_x >= width)
        line_x -= width;
      draw(&window, b, (rect){(int)line_x, top, LINE_W, height - top}, WHITE);
      break;
    }
    snprintf(status, sizeof(status), "%dHZ %dFPS %dLATE", (win_refresh_mhz + 500) / 1000, fps, late);
    if (pattern == LINE)
      snprintf(status + strlen(status), sizeof(status) - strlen(status), " %dPX", speed);
    text(&window, b, 8, 8, 4, status);

    wl_surface_attach(window.surface, b->buffer, 0, 0);
    b->busy = true;
    win_frame(&window);
    wl_surface_commit(window.surface);
    frame++;
  }
  return 0;
}
