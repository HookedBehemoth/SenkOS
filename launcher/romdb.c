#include "romdb.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <unistd.h>

#include <zlib.h>

typedef struct {
  char magic[8];
  uint32_t count, images;
  uint32_t crcs, records, image_offsets, strings, blob;
  uint32_t lists[7];
} header;

_Static_assert(sizeof(header) == 64, "pack header");
_Static_assert(sizeof(romdb_record) == 32, "pack record");

/* Mapped for good: a handful of systems, pages only read when looked at */
typedef struct {
  char *path;
  const uint8_t *map; /* NULL: missing or broken, not tried again */
  size_t size;
} pack;
static pack packs[16];
static size_t pack_count;

static const header *open_pack(const char *path, size_t *size) {
  pack *p = NULL;
  for (size_t i = 0; i < pack_count && p == NULL; i++)
    if (strcmp(packs[i].path, path) == 0)
      p = &packs[i];
  if (p == NULL) {
    if (pack_count == SDL_arraysize(packs))
      return NULL;
    p = &packs[pack_count++];
    p->path = strdup(path);
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    struct stat st;
    if (fd >= 0 && fstat(fd, &st) == 0 && st.st_size >= (off_t)sizeof(header)) {
      void *map = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
      const header *h = map;
      const uint64_t n = map != MAP_FAILED ? h->count : 0;
      /* Everything a lookup reads must lie inside the file, in order */
      if (map != MAP_FAILED && memcmp(h->magic, "GAMEPAK1", 8) == 0 &&
          h->crcs % 4 == 0 && h->records % 4 == 0 && h->image_offsets % 4 == 0 &&
          h->crcs >= sizeof(header) && h->crcs + n * 4 <= h->records &&
          h->records + n * sizeof(romdb_record) <= h->image_offsets &&
          h->image_offsets + ((uint64_t)h->images + 1) * 4 <= h->strings &&
          h->strings < h->blob && h->blob <= (uint64_t)st.st_size &&
          ((const char *)map)[h->blob - 1] == '\0') {
        p->map = map;
        p->size = st.st_size;
      } else {
        SDL_Log("romdb: %s isn't a game pack", path);
        if (map != MAP_FAILED)
          munmap(map, st.st_size);
      }
    }
    if (fd >= 0)
      close(fd);
  }
  *size = p->size;
  return (const header *)p->map;
}

static int by_crc(const void *key, const void *element) {
  const uint32_t a = *(const uint32_t *)key, b = *(const uint32_t *)element;
  return a < b ? -1 : a > b;
}

bool romdb_find(const char *path, uint32_t crc, romdb_game *out) {
  size_t size;
  const header *h = open_pack(path, &size);
  if (h == NULL)
    return false;
  const uint8_t *base = (const uint8_t *)h;
  const uint32_t *crcs = (const uint32_t *)(base + h->crcs);
  const uint32_t *found = bsearch(&crc, crcs, h->count, sizeof(*crcs), by_crc);
  if (found == NULL)
    return false;
  const romdb_record *r = (const romdb_record *)(base + h->records) + (found - crcs);
  out->record = r;
  out->title = r->title < h->blob - h->strings ? (const char *)base + h->strings + r->title : "";
  out->image[0] = '\0';
  const uint32_t *images = (const uint32_t *)(base + h->image_offsets);
  if (r->image < h->images && images[r->image] < images[r->image + 1] &&
      images[r->image + 1] <= size - h->blob)
    snprintf(out->image, sizeof(out->image), "pack:%u:%u:%s",
             h->blob + images[r->image], images[r->image + 1] - images[r->image], path);
  return true;
}

/* The CRC cache: what the thread works through and what it found */
enum { PENDING, HASHING, KNOWN, FAILED };
typedef struct {
  char *path;
  long long size, mtime;
  uint32_t crc;
  int state;
} rom;
static rom *roms;
static size_t rom_count, rom_capacity;
static SDL_mutex *lock;
static SDL_cond *wake;
static Uint32 hashed_event;
static char cache_path[512];

/* Under the lock */
static rom *remember(const char *path) {
  for (size_t i = 0; i < rom_count; i++)
    if (strcmp(roms[i].path, path) == 0)
      return &roms[i];
  if (rom_count == rom_capacity) {
    const size_t capacity = rom_capacity ? rom_capacity * 2 : 64;
    rom *grown = realloc(roms, capacity * sizeof(*grown));
    if (grown == NULL)
      return NULL;
    roms = grown;
    rom_capacity = capacity;
  }
  char *copy = strdup(path);
  if (copy == NULL)
    return NULL;
  roms[rom_count] = (rom){.path = copy, .state = FAILED};
  return &roms[rom_count++];
}

/* A zip's first file, from the central directory: nothing is inflated.
 * ponytail: the first entry only; multi-file zips would want the largest. */
static bool zip_crc(FILE *f, uint32_t *crc) {
  uint8_t tail[65536 + 22];
  if (fseek(f, 0, SEEK_END) != 0)
    return false;
  const long size = ftell(f);
  const long n = size < (long)sizeof(tail) ? size : (long)sizeof(tail);
  if (n < 22 || fseek(f, size - n, SEEK_SET) != 0 || fread(tail, 1, n, f) != (size_t)n)
    return false;
  for (long i = n - 22; i >= 0; i--) {
    uint32_t sig, dir;
    memcpy(&sig, tail + i, 4);
    if (sig != 0x06054b50)
      continue;
    memcpy(&dir, tail + i + 16, 4);
    uint8_t entry[46];
    if (fseek(f, dir, SEEK_SET) != 0 || fread(entry, 1, sizeof(entry), f) != sizeof(entry))
      return false;
    memcpy(&sig, entry, 4);
    memcpy(crc, entry + 16, 4);
    return sig == 0x02014b50;
  }
  return false;
}

static bool file_crc(const char *path, uint32_t *crc) {
  FILE *f = fopen(path, "rb");
  if (f == NULL)
    return false;
  const size_t len = strlen(path);
  bool ok;
  if (len > 4 && strcasecmp(path + len - 4, ".zip") == 0) {
    ok = zip_crc(f, crc);
  } else {
    static uint8_t buf[1 << 16];
    uLong c = crc32(0, NULL, 0);
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
      c = crc32(c, buf, (uInt)n);
    ok = !ferror(f);
    *crc = (uint32_t)c;
  }
  fclose(f);
  return ok;
}

static int hasher(void *data) {
  (void)data;
  SDL_LockMutex(lock);
  for (;;) {
    size_t i = 0;
    while (i < rom_count && roms[i].state != PENDING)
      i++;
    if (i == rom_count) {
      SDL_CondWait(wake, lock);
      continue;
    }
    roms[i].state = HASHING;
    char *path = strdup(roms[i].path);
    const long long size = roms[i].size, mtime = roms[i].mtime;
    SDL_UnlockMutex(lock);

    uint32_t crc = 0;
    const bool ok = path != NULL && file_crc(path, &crc);
    if (ok) {
      FILE *f = fopen(cache_path, "a");
      if (f != NULL) {
        fprintf(f, "%08x %lld %lld %s\n", crc, size, mtime, path);
        fclose(f);
      }
    }
    free(path);

    SDL_LockMutex(lock);
    /* Unless the file changed meanwhile: then it is pending again */
    if (roms[i].state == HASHING && roms[i].size == size && roms[i].mtime == mtime) {
      roms[i].crc = crc;
      roms[i].state = ok ? KNOWN : FAILED;
    }
    SDL_Event ev = {.type = hashed_event};
    SDL_PushEvent(&ev);
  }
  return 0;
}

void romdb_init(Uint32 hashed) {
  hashed_event = hashed;
  lock = SDL_CreateMutex();
  wake = SDL_CreateCond();
  const char *cache = getenv("XDG_CACHE_HOME"), *home = getenv("HOME");
  if (cache != NULL && cache[0] == '/')
    snprintf(cache_path, sizeof(cache_path), "%s/launcher/crc32", cache);
  else
    snprintf(cache_path, sizeof(cache_path), "%s/.cache/launcher/crc32", home ? home : "/tmp");
  for (char *slash = strchr(cache_path + 1, '/'); slash; slash = strchr(slash + 1, '/')) {
    *slash = '\0';
    mkdir(cache_path, 0755);
    *slash = '/';
  }
  /* "crc size mtime path"; a later line for a path wins */
  FILE *f = fopen(cache_path, "r");
  char line[1024];
  while (f != NULL && fgets(line, sizeof(line), f)) {
    unsigned crc;
    long long size, mtime;
    int at;
    line[strcspn(line, "\n")] = '\0';
    if (sscanf(line, "%x %lld %lld %n", &crc, &size, &mtime, &at) != 3)
      continue;
    rom *r = remember(line + at);
    if (r != NULL)
      *r = (rom){r->path, size, mtime, crc, KNOWN};
  }
  if (f != NULL)
    fclose(f);
  SDL_DetachThread(SDL_CreateThread(hasher, "crc32", NULL));
}

bool romdb_crc(const char *path, const struct stat *st, uint32_t *crc) {
  SDL_LockMutex(lock);
  rom *r = remember(path);
  bool known = false;
  if (r != NULL) {
    if (r->size != st->st_size || r->mtime != st->st_mtime) {
      r->size = st->st_size;
      r->mtime = st->st_mtime;
      r->state = PENDING;
      SDL_CondSignal(wake);
    }
    known = r->state == KNOWN;
    *crc = r->crc;
  }
  SDL_UnlockMutex(lock);
  return known;
}
