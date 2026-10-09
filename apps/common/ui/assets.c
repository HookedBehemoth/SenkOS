/*
 * Entries are the UI thread's; the loader only reads an entry's path, which
 * never changes, and hands back the uploaded image under the lock. It takes
 * the pending entry shown most recently first, so what is on screen now
 * loads before what was scrolled past.
 */
#include "assets.h"

#include <spawn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <png.h>
#include <turbojpeg.h>
#include <webp/decode.h>
#include <webp/encode.h>


#include "gfx.h"
#include "ui.h"

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
  str path;
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

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
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
    if (MAX(TJSCALED(jw, f), TJSCALED(jh, f)) >= MIN(MAX_PX, MAX(jw, jh)) &&
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

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* Uncompressed, 8 bits (a palette), 24 or 32; 32 with BI_BITFIELDS too, its
 * masks a byte each. ponytail: no RLE, no 1/4/16 bits; none of the art has
 * them. */
static uint8_t *decode_bmp(const uint8_t *d, size_t n, int *w, int *h) {
  if (n < 54)
    return NULL;
  const uint32_t offset = le32(d + 10), header = le32(d + 14), compression = le32(d + 30);
  const int32_t width = (int32_t)le32(d + 18), height = (int32_t)le32(d + 22);
  const int bpp = d[28] | d[29] << 8;
  /* r, g, b, a; a 32-bit BI_RGB pixel is BGRA, unless its alpha is all 0 */
  uint32_t masks[4] = {0xff0000, 0xff00, 0xff, bpp == 32 ? 0xff000000 : 0};
  if (compression == 3 && bpp == 32 && n >= 66) {
    for (int i = 0; i < 3; i++)
      masks[i] = le32(d + 54 + i * 4);
    masks[3] = header >= 56 && n >= 70 ? le32(d + 66) : 0;
  } else if (compression != 0 || (bpp != 8 && bpp != 24 && bpp != 32)) {
    return NULL;
  }
  const int64_t rows = height < 0 ? -(int64_t)height : height;
  if (width <= 0 || rows == 0 || (uint64_t)width * rows > MAX_DECODE_PX)
    return NULL;
  const size_t stride = ((size_t)width * bpp + 31) / 32 * 4;
  const uint8_t *palette = d + 14 + header;
  /* Only the colours used, maybe; past them is black */
  const size_t colors = MIN(le32(d + 46) ? le32(d + 46) : 256, 256u);
  if (offset > n || stride * rows > n - offset || (bpp == 8 && 14 + header + colors * 4 > offset))
    return NULL;
  uint8_t *rgba = malloc((size_t)width * rows * 4);
  if (rgba == NULL)
    return NULL;
  uint8_t alpha = 0;
  for (int64_t y = 0; y < rows; y++) {
    /* Bottom-up, unless the height is negative */
    const uint8_t *src = d + offset + stride * (size_t)(height < 0 ? y : rows - 1 - y);
    uint8_t *dst = rgba + (size_t)y * width * 4;
    for (int32_t x = 0; x < width; x++, dst += 4) {
      if (bpp == 8) {
        const uint8_t *c = src[x] < colors ? palette + src[x] * 4 : (const uint8_t *)"\0\0\0";
        dst[0] = c[2], dst[1] = c[1], dst[2] = c[0], dst[3] = 255;
      } else if (bpp == 24) {
        const uint8_t *c = src + x * 3;
        dst[0] = c[2], dst[1] = c[1], dst[2] = c[0], dst[3] = 255;
      } else {
        const uint32_t px = le32(src + x * 4);
        for (int i = 0; i < 4; i++)
          dst[i] = masks[i] ? (uint8_t)((px & masks[i]) >> __builtin_ctz(masks[i])) : 255;
        alpha |= dst[3];
      }
    }
  }
  if (bpp == 32 && masks[3] != 0 && alpha == 0)
    for (size_t i = 3; i < (size_t)width * rows * 4; i += 4)
      rgba[i] = 255;
  *w = width;
  *h = (int)rows;
  return rgba;
}

/* Thumbnails (assets.h): $XDG_CACHE_HOME/thumbnails/<hash of path, size and mtime>.webp.
 * ponytail: never cleaned; stale ones stay until the cache is cleared. */
static bool thumb_name(str path, char *out, size_t size) {
  char p[PATH_MAX];
  struct stat st;
  if (str_copy(p, sizeof(p), path) != path.n || stat(p, &st) != 0)
    return false;
  uint64_t h = 1469598103934665603u;
  const uint64_t parts[] = {(uint64_t)st.st_size, (uint64_t)st.st_mtim.tv_sec, (uint64_t)st.st_mtim.tv_nsec};
  for (size_t i = 0; i < path.n; i++)
    h = (h ^ (uint8_t)path.p[i]) * 1099511628211u;
  for (size_t i = 0; i < COUNTOF(parts); i++)
    h = (h ^ parts[i]) * 1099511628211u;
  const char *cache = getenv("XDG_CACHE_HOME"), *home = getenv("HOME");
  if (cache != NULL && cache[0] == '/')
    snprintf(out, size, "%s/thumbnails/%016llx.webp", cache, (unsigned long long)h);
  else
    snprintf(out, size, "%s/.cache/thumbnails/%016llx.webp", home != NULL ? home : "/tmp", (unsigned long long)h);
  return true;
}

/* The folders `name` goes in */
static void make_parents(const char *name) {
  char dir[PATH_MAX];
  snprintf(dir, sizeof(dir), "%s", name);
  for (char *slash = strchr(dir + 1, '/'); slash != NULL; slash = strchr(slash + 1, '/')) {
    *slash = '\0';
    mkdir(dir, 0755);
    *slash = '/';
  }
}

/* `rgba` (premultiplied) as WebP, written aside, then renamed: a reader sees all of it or nothing */
static void thumb_write(const char *name, const uint8_t *rgba, int w, int h) {
  const size_t n = (size_t)w * h * 4;
  uint8_t *straight = malloc(n), *webp = NULL;
  if (straight == NULL)
    return;
  for (size_t i = 0; i < n; i += 4)
    for (int c = 0; c < 4; c++)
      straight[i + c] = c < 3 && rgba[i + 3] != 0 ? (uint8_t)MIN(255, rgba[i + c] * 255 / rgba[i + 3]) : rgba[i + c];
  const size_t size = WebPEncodeRGBA(straight, w, h, w * 4, 85, &webp);
  free(straight);
  char tmp[PATH_MAX + 16];
  make_parents(name);
  snprintf(tmp, sizeof(tmp), "%s.%d", name, (int)getpid());
  FILE *f = size > 0 ? fopen(tmp, "wb") : NULL;
  const bool ok = f != NULL && fwrite(webp, 1, size, f) == size;
  if (f != NULL && fclose(f) == 0 && ok && rename(tmp, name) == 0) {
    WebPFree(webp);
    return;
  }
  fprintf(stderr, "assets: can't write %s\n", name);
  unlink(tmp);
  WebPFree(webp);
}

/* A video's: the player writes a frame of it as a thumbnail (ASSETS_THUMBNAILER) */
static bool thumb_video(str path, const char *name) {
  char video[PATH_MAX], size[16];
  str_copy(video, sizeof(video), path);
  snprintf(size, sizeof(size), "%d", MAX_PX);
  make_parents(name);
  char *argv[] = {ASSETS_THUMBNAILER, "--thumbnail", video, (char *)name, size, NULL};
  extern char **environ;
  pid_t pid;
  int status;
  return posix_spawnp(&pid, argv[0], NULL, NULL, argv, environ) == 0 && waitpid(pid, &status, 0) == pid &&
         WIFEXITED(status) && WEXITSTATUS(status) == 0;
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
  const int longest = MAX(*w, *h);
  if (longest <= MAX_PX)
    return;
  const int dw = MAX(1, *w * MAX_PX / longest);
  const int dh = MAX(1, *h * MAX_PX / longest);
  /* Each destination pixel only reads source pixels at or after its own
   * index, so writing over the source as it goes is safe */
  for (int y = 0; y < dh; y++) {
    const int y0 = y * *h / dh, y1 = MAX(y0 + 1, (y + 1) * *h / dh);
    for (int x = 0; x < dw; x++) {
      const int x0 = x * *w / dw, x1 = MAX(x0 + 1, (x + 1) * *w / dw);
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

static void *loader(void *data) {
  (void)data;
  for (;;) {
    pthread_mutex_lock(&lock);
    while (pending_count == 0)
      pthread_cond_wait(&wake, &lock);
    int best = 0;
    for (int i = 1; i < pending_count; i++)
      if (atomic_load(&entries[pending[i]].used) > atomic_load(&entries[pending[best]].used))
        best = i;
    const int index = pending[best];
    pending[best] = pending[--pending_count];
    pthread_mutex_unlock(&lock);

    size_t size = 0;
    int w = 0, h = 0;
    /* Its thumbnail if there is one (a video's made now), else the file */
    const str key = entries[index].path;
    const bool video = key.n > 6 && memcmp(key.p, "video:", 6) == 0;
    const str path = video ? str_at(key, 6) : key;
    char thumb[PATH_MAX];
    const bool cached = thumb_name(path, thumb, sizeof(thumb));
    uint8_t *file = cached ? read_file(thumb, &size) : NULL;
    if (file == NULL && video && cached && thumb_video(path, thumb))
      file = read_file(thumb, &size);
    const bool original = file == NULL && !video;
    if (original)
      file = read_file(key.p, &size);
    uint8_t *rgba = file != NULL ? decode(file, size, &w, &h) : NULL;
    free(file);
    gfx_image image = {0};
    bool ok = false;
    if (rgba != NULL) {
      const bool shrinks = MAX(w, h) > MAX_PX;
      prepare(rgba, &w, &h);
      if (original && shrinks && cached)
        thumb_write(thumb, rgba, w, h);
      ok = gfx_upload(&image, rgba, w, h);
      free(rgba);
    }
    if (!ok)
      fprintf(stderr, "assets: can't load %s\n", entries[index].path.p);

    pthread_mutex_lock(&lock);
    done[done_count].index = index;
    done[done_count].ok = ok;
    done[done_count].image = image;
    done_count++;
    pthread_mutex_unlock(&lock);
    win_wake(UI_LOADED, 0);
  }
  return NULL;
}

void assets_init(void) {
  for (size_t i = 0; i < COUNTOF(slot_owner); i++)
    slot_owner[i] = -1;
  spawn("assets", loader, NULL);
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
  pthread_mutex_lock(&lock);
  const int n = done_count;
  typeof(done[0]) arrived[MAX_ENTRIES];
  memcpy(arrived, done, n * sizeof(done[0]));
  done_count = 0;
  pthread_mutex_unlock(&lock);
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
  fprintf(stderr, "assets: %zu MB after eviction\n", used >> 20);
}

asset assets_get(str path) {
  const asset none = {GFX_PLACEHOLDER, 1, 1};
  if (path.n == 0)
    return none;
  /* ponytail: linear search, fine for a screenful a frame over a few
   * hundred paths; a hash when there are thousands */
  entry *e = NULL;
  for (size_t i = 0; i < entry_count && e == NULL; i++)
    if (str_eq(entries[i].path, path))
      e = &entries[i];
  if (e == NULL) {
    if (entry_count == MAX_ENTRIES || (e = &entries[entry_count])->path.p != NULL ||
        (e->path = str_dup(path)).p == NULL)
      return none;
    entry_count++;
  }
  atomic_store(&e->used, gfx_frame());
  if (e->state == READY)
    return (asset){e->slot, e->image.w, e->image.h};
  if (e->state == NONE) {
    e->state = QUEUED;
    pthread_mutex_lock(&lock);
    pending[pending_count++] = (int)(e - entries);
    pthread_cond_signal(&wake);
    pthread_mutex_unlock(&lock);
  }
  return none;
}
