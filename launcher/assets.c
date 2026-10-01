/*
 * Entries are the UI thread's; the loader only reads an entry's path, which
 * never changes, and hands back the uploaded image under the lock. It takes
 * the pending entry shown most recently first, so what is on screen now
 * loads before what was scrolled past.
 */
#include "assets.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <png.h>
#include <turbojpeg.h>
#include <webp/decode.h>

#include "gfx.h"

/* The whole process; past HIGH, evict down to LOW */
#define LIMIT (100u << 20)
#define HIGH (LIMIT / 10 * 9)
#define LOW (LIMIT / 10 * 8)
/* Longest side once loaded: a tile's icon at most */
#define MAX_PX 256
/* What a full decode may take on the way (JPEG decodes scaled down) */
#define MAX_DECODE_PX (2048 * 2048)
#define MAX_FILE (32u << 20)
#define MAX_ENTRIES 1024

enum { NONE, QUEUED, READY, FAILED };

typedef struct {
  char *path;
  int state;
  uint32_t slot;
  gfx_image image;
  /* gfx_frame() when last asked for; the loader reads it too */
  _Atomic uint64_t used;
} entry;

static entry entries[MAX_ENTRIES];
static size_t entry_count;
static int slot_owner[256]; /* entry index, -1 free */
static VkDeviceSize texture_bytes;
static Uint32 loaded_event;

static SDL_mutex *lock;
static SDL_cond *wake;
static int pending[MAX_ENTRIES], pending_count;
static struct {
  int index;
  bool ok;
  gfx_image image;
} done[MAX_ENTRIES];
static int done_count;

/* A file, or "pack:<offset>:<length>:<path>": part of one (romdb.h) */
static uint8_t *read_file(const char *key, size_t *size) {
  unsigned long at = 0, length = 0;
  int skip = 0;
  if (strncmp(key, "pack:", 5) == 0 &&
      sscanf(key, "pack:%lu:%lu:%n", &at, &length, &skip) != 2)
    return NULL;
  FILE *f = fopen(key + skip, "rb");
  if (f == NULL)
    return NULL;
  uint8_t *data = NULL;
  long n;
  if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) > 0 &&
      (skip == 0 || (at + length <= (unsigned long)n && (n = (long)length) > 0)) &&
      n <= MAX_FILE && fseek(f, (long)at, SEEK_SET) == 0 &&
      (data = malloc(n)) != NULL && fread(data, 1, n, f) != (size_t)n) {
    free(data);
    data = NULL;
  }
  fclose(f);
  *size = (size_t)n;
  return data;
}

static uint8_t *decode_png(const uint8_t *data, size_t size, int *w, int *h) {
  png_image img = {.version = PNG_IMAGE_VERSION};
  if (!png_image_begin_read_from_memory(&img, data, size))
    return NULL;
  uint8_t *rgba = NULL;
  img.format = PNG_FORMAT_RGBA;
  if ((size_t)img.width * img.height <= MAX_DECODE_PX)
    rgba = malloc(PNG_IMAGE_SIZE(img));
  if (rgba == NULL || !png_image_finish_read(&img, NULL, rgba, 0, NULL)) {
    png_image_free(&img);
    free(rgba);
    return NULL;
  }
  *w = (int)img.width;
  *h = (int)img.height;
  return rgba;
}

/* libjpeg-turbo scales by n/8 while decoding: the smallest that keeps the
 * longest side at MAX_PX or more */
static uint8_t *decode_jpeg(const uint8_t *data, size_t size, int *w, int *h) {
  tjhandle tj = tj3Init(TJINIT_DECOMPRESS);
  uint8_t *rgba = NULL;
  if (tj == NULL || tj3DecompressHeader(tj, data, size) != 0)
    goto out;
  const int jw = tj3Get(tj, TJPARAM_JPEGWIDTH), jh = tj3Get(tj, TJPARAM_JPEGHEIGHT);
  int n;
  const tjscalingfactor *factors = tj3GetScalingFactors(&n);
  tjscalingfactor best = {1, 1};
  for (int i = 0; i < n; i++) {
    const tjscalingfactor f = factors[i];
    if (SDL_max(TJSCALED(jw, f), TJSCALED(jh, f)) >= SDL_min(MAX_PX, SDL_max(jw, jh)) &&
        TJSCALED(jw, f) * TJSCALED(jh, f) < TJSCALED(jw, best) * TJSCALED(jh, best))
      best = f;
  }
  *w = TJSCALED(jw, best);
  *h = TJSCALED(jh, best);
  if ((size_t)*w * *h > MAX_DECODE_PX || tj3SetScalingFactor(tj, best) != 0 ||
      (rgba = malloc((size_t)*w * *h * 4)) == NULL)
    goto out;
  if (tj3Decompress8(tj, data, size, rgba, *w * 4, TJPF_RGBA) != 0) {
    free(rgba);
    rgba = NULL;
  }
out:
  tj3Destroy(tj);
  return rgba;
}

static uint8_t *decode_webp(const uint8_t *data, size_t size, int *w, int *h) {
  if (!WebPGetInfo(data, size, w, h) || (size_t)*w * *h > MAX_DECODE_PX)
    return NULL;
  /* ponytail: full size, then shrunk; WebPDecoderConfig scales while
   * decoding if big WebPs show up */
  uint8_t *webp = WebPDecodeRGBA(data, size, w, h);
  if (webp == NULL)
    return NULL;
  uint8_t *rgba = malloc((size_t)*w * *h * 4);
  if (rgba != NULL)
    memcpy(rgba, webp, (size_t)*w * *h * 4);
  WebPFree(webp);
  return rgba;
}

static uint8_t *decode_bmp(const uint8_t *data, size_t size, int *w, int *h) {
  SDL_Surface *bmp = SDL_LoadBMP_RW(SDL_RWFromConstMem(data, (int)size), 1);
  SDL_Surface *s = bmp ? SDL_ConvertSurfaceFormat(bmp, SDL_PIXELFORMAT_RGBA32, 0) : NULL;
  SDL_FreeSurface(bmp);
  uint8_t *rgba = s ? malloc((size_t)s->w * s->h * 4) : NULL;
  if (rgba != NULL) {
    *w = s->w;
    *h = s->h;
    for (int y = 0; y < s->h; y++)
      memcpy(rgba + (size_t)y * s->w * 4, (uint8_t *)s->pixels + (size_t)y * s->pitch,
             (size_t)s->w * 4);
  }
  SDL_FreeSurface(s);
  return rgba;
}

/* By content, not by name */
static uint8_t *decode(const uint8_t *d, size_t n, int *w, int *h) {
  if (n >= 8 && memcmp(d, "\x89PNG", 4) == 0)
    return decode_png(d, n, w, h);
  if (n >= 3 && d[0] == 0xff && d[1] == 0xd8 && d[2] == 0xff)
    return decode_jpeg(d, n, w, h);
  if (n >= 12 && memcmp(d, "RIFF", 4) == 0 && memcmp(d + 8, "WEBP", 4) == 0)
    return decode_webp(d, n, w, h);
  if (n >= 2 && memcmp(d, "BM", 2) == 0)
    return decode_bmp(d, n, w, h);
  return NULL;
}

/* Premultiplied, then box-filtered down to MAX_PX, in place */
static void prepare(uint8_t *rgba, int *w, int *h) {
  for (size_t i = 0; i < (size_t)*w * *h * 4; i += 4)
    for (int c = 0; c < 3; c++)
      rgba[i + c] = (uint8_t)((rgba[i + c] * rgba[i + 3] + 127) / 255);
  const int longest = SDL_max(*w, *h);
  if (longest <= MAX_PX)
    return;
  const int dw = SDL_max(1, *w * MAX_PX / longest);
  const int dh = SDL_max(1, *h * MAX_PX / longest);
  /* Each destination pixel only reads source pixels at or after its own
   * index, so writing over the source as it goes is safe */
  for (int y = 0; y < dh; y++) {
    const int y0 = y * *h / dh, y1 = SDL_max(y0 + 1, (y + 1) * *h / dh);
    for (int x = 0; x < dw; x++) {
      const int x0 = x * *w / dw, x1 = SDL_max(x0 + 1, (x + 1) * *w / dw);
      uint32_t sum[4] = {0};
      for (int sy = y0; sy < y1; sy++)
        for (int sx = x0; sx < x1; sx++)
          for (int c = 0; c < 4; c++)
            sum[c] += rgba[((size_t)sy * *w + sx) * 4 + c];
      const uint32_t n = (uint32_t)((y1 - y0) * (x1 - x0));
      for (int c = 0; c < 4; c++)
        rgba[((size_t)y * dw + x) * 4 + c] = (uint8_t)(sum[c] / n);
    }
  }
  *w = dw;
  *h = dh;
}

static int loader(void *data) {
  (void)data;
  for (;;) {
    SDL_LockMutex(lock);
    while (pending_count == 0)
      SDL_CondWait(wake, lock);
    int best = 0;
    for (int i = 1; i < pending_count; i++)
      if (atomic_load(&entries[pending[i]].used) > atomic_load(&entries[pending[best]].used))
        best = i;
    const int index = pending[best];
    pending[best] = pending[--pending_count];
    SDL_UnlockMutex(lock);

    size_t size = 0;
    int w = 0, h = 0;
    uint8_t *file = read_file(entries[index].path, &size);
    uint8_t *rgba = file ? decode(file, size, &w, &h) : NULL;
    free(file);
    gfx_image image = {0};
    bool ok = false;
    if (rgba != NULL) {
      prepare(rgba, &w, &h);
      ok = gfx_upload(&image, rgba, w, h);
      free(rgba);
    }
    if (!ok)
      SDL_Log("assets: can't load %s", entries[index].path);

    SDL_LockMutex(lock);
    done[done_count].index = index;
    done[done_count].ok = ok;
    done[done_count].image = image;
    done_count++;
    SDL_UnlockMutex(lock);
    SDL_Event ev = {.type = loaded_event};
    SDL_PushEvent(&ev);
  }
  return 0;
}

void assets_init(Uint32 loaded) {
  loaded_event = loaded;
  for (size_t i = 0; i < SDL_arraysize(slot_owner); i++)
    slot_owner[i] = -1;
  lock = SDL_CreateMutex();
  wake = SDL_CreateCond();
  SDL_DetachThread(SDL_CreateThread(loader, "assets", NULL));
}

/* The anonymous part of the resident set (heap, decoders, the driver's own)
 * plus the textures, which live in device memory on any GPU */
static size_t memory_used(void) {
  size_t anon_kb = 0;
  char line[128];
  FILE *f = fopen("/proc/self/status", "r");
  while (f != NULL && fgets(line, sizeof(line), f))
    if (sscanf(line, "RssAnon: %zu kB", &anon_kb) == 1)
      break;
  if (f != NULL)
    fclose(f);
  return anon_kb * 1024 + texture_bytes;
}

static void evict(entry *e) {
  gfx_bind(e->slot, NULL);
  slot_owner[e->slot] = -1;
  texture_bytes -= e->image.bytes;
  gfx_release(&e->image);
  e->state = NONE;
}

/* The least recently shown that the last frame didn't show; NULL if none */
static entry *coldest(void) {
  entry *best = NULL;
  for (size_t i = 0; i < entry_count; i++) {
    entry *e = &entries[i];
    if (e->state == READY && e->used + 1 < gfx_frame() &&
        (best == NULL || e->used < best->used))
      best = e;
  }
  return best;
}

static uint32_t free_slot(void) {
  for (uint32_t s = GFX_FIRST_FREE; s < gfx_slots(); s++)
    if (slot_owner[s] < 0)
      return s;
  entry *e = coldest();
  if (e == NULL)
    return 0;
  const uint32_t s = e->slot;
  evict(e);
  return s;
}

void assets_frame(void) {
  SDL_LockMutex(lock);
  const int n = done_count;
  typeof(done[0]) arrived[MAX_ENTRIES];
  memcpy(arrived, done, n * sizeof(done[0]));
  done_count = 0;
  SDL_UnlockMutex(lock);
  if (n == 0)
    return;

  for (int i = 0; i < n; i++) {
    entry *e = &entries[arrived[i].index];
    if (!arrived[i].ok) {
      e->state = FAILED;
      continue;
    }
    const uint32_t s = free_slot();
    if (s == 0) {
      /* ponytail: more images on screen than slots; asked for again, it
       * loads again. A texture atlas when a screen shows that many. */
      gfx_release(&arrived[i].image);
      e->state = NONE;
      continue;
    }
    e->image = arrived[i].image;
    e->slot = s;
    e->state = READY;
    slot_owner[s] = (int)arrived[i].index;
    texture_bytes += e->image.bytes;
    gfx_bind(s, &e->image);
  }
  size_t used = memory_used();
  if (used <= HIGH)
    return;
  entry *e;
  while (used > LOW && (e = coldest()) != NULL) {
    used -= e->image.bytes;
    evict(e);
  }
  SDL_Log("assets: %zu MB after eviction", used >> 20);
}

asset assets_get(const char *path) {
  const asset none = {GFX_PLACEHOLDER, 1, 1};
  if (path == NULL || path[0] == '\0')
    return none;
  /* ponytail: linear search, fine for a screenful a frame over a few
   * hundred paths; a hash when there are thousands */
  entry *e = NULL;
  for (size_t i = 0; i < entry_count && e == NULL; i++)
    if (strcmp(entries[i].path, path) == 0)
      e = &entries[i];
  if (e == NULL) {
    if (entry_count == MAX_ENTRIES || (e = &entries[entry_count])->path != NULL ||
        (e->path = strdup(path)) == NULL)
      return none;
    entry_count++;
  }
  atomic_store(&e->used, gfx_frame());
  if (e->state == READY)
    return (asset){e->slot, e->image.w, e->image.h};
  if (e->state == NONE) {
    e->state = QUEUED;
    SDL_LockMutex(lock);
    pending[pending_count++] = (int)(e - entries);
    SDL_CondSignal(wake);
    SDL_UnlockMutex(lock);
  }
  return none;
}
