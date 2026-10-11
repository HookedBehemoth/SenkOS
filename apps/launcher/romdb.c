#include "romdb.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <unistd.h>

#include <zlib.h>

#include "wake.h"

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
  str path;
  const uint8_t *map; /* NULL: missing or broken, not tried again */
  size_t size;
} pack;
static pack packs[16];
static size_t pack_count;

static const header *open_pack(str path, size_t *size) {
  pack *p = NULL;
  for (size_t i = 0; i < pack_count && p == NULL; i++)
    if (str_eq(packs[i].path, path))
      p = &packs[i];
  if (p == NULL) {
    if (pack_count == COUNTOF(packs) || (path = str_dup(path)).p == NULL)
      return NULL;
    p = &packs[pack_count++];
    p->path = path;
    const int fd = open(path.p, O_RDONLY | O_CLOEXEC);
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
        fprintf(stderr, "romdb: %s isn't a game pack\n", path.p);
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

void romdb_retry(void) {
  size_t n = 0;
  for (size_t i = 0; i < pack_count; i++)
    if (packs[i].map != NULL)
      packs[n++] = packs[i];
    else
      free((char *)packs[i].path.p);
  pack_count = n;
}

static int by_crc(const void *key, const void *element) {
  const uint32_t a = *(const uint32_t *)key, b = *(const uint32_t *)element;
  return a < b ? -1 : a > b;
}

bool romdb_find(str path, uint32_t crc, romdb_game *out) {
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
  /* The pack's strings end with a NUL: open_pack checked */
  const char *title = (const char *)base + h->strings + r->title;
  out->title = r->title < h->blob - h->strings ? (str){title, strlen(title)} : S("");
  sb_set(out->image, S(""));
  const uint32_t *images = (const uint32_t *)(base + h->image_offsets);
  if (r->image < h->images && images[r->image] < images[r->image + 1] &&
      images[r->image + 1] <= size - h->blob) {
    sb_fmt(out->image, "pack:%u:%u:", h->blob + images[r->image],
           images[r->image + 1] - images[r->image]);
    sb_add(out->image, path);
  }
  return true;
}

/* Up to 16 words of `s`, lowercase ASCII, up to its first tag: "Zelda, The (USA) [!]" is
 * "zelda", "the". Anything else ASCII parts them, and digits from letters: "mother3". */
enum { WORDS = 16 };
static size_t words(str s, str *out, char (*buf)[32]) {
  size_t n = 0;
  for (size_t i = 0; i < s.n && s.p[i] != '(' && s.p[i] != '[' && n < WORDS;) {
    size_t len = 0;
    for (; i < s.n && ((unsigned char)s.p[i] >= 0x80 || isalnum((unsigned char)s.p[i])); i++) {
      if (len > 0 && !isdigit((unsigned char)buf[n][len - 1]) != !isdigit((unsigned char)s.p[i]))
        break;
      if (len < sizeof(buf[n]))
        buf[n][len++] = (char)tolower((unsigned char)s.p[i]);
    }
    if (len > 0) {
      out[n] = (str){buf[n], len};
      n++;
    }
    while (i < s.n && (unsigned char)s.p[i] < 0x80 && !isalnum((unsigned char)s.p[i]) && s.p[i] != '(' &&
           s.p[i] != '[')
      i++;
  }
  return n;
}

typedef struct {
  uint32_t crc;
  const char *title; /* interned: one title, one pointer */
  int score;
} hit;

/* Best first, then by title */
static int by_score(const hit *x, const hit *y) {
  return x->score != y->score ? y->score - x->score : strcasecmp(x->title, y->title);
}

/* Every record scored, a few thousand words; an index if packs grow past ~100k */
size_t romdb_search(str path, str query, uint32_t *crcs, size_t max) {
  size_t size;
  const header *h = open_pack(path, &size);
  str q[WORDS];
  char qbuf[WORDS][32];
  const size_t qn = words(query, q, qbuf);
  if (h == NULL || qn == 0 || max == 0)
    return 0;
  const uint8_t *base = (const uint8_t *)h;
  const romdb_record *records = (const romdb_record *)(base + h->records);
  const uint32_t *all = (const uint32_t *)(base + h->crcs);
  hit *hits = malloc((max + 1) * sizeof(*hits));
  size_t n = 0;
  for (uint32_t i = 0; hits != NULL && i < h->count; i++) {
    const romdb_record *r = &records[i];
    if (r->title == 0 || r->title >= h->blob - h->strings)
      continue;
    const char *title = (const char *)base + h->strings + r->title;
    str t[WORDS];
    char tbuf[WORDS][32];
    const size_t tn = words(str_c(title), t, tbuf);
    /* In halves: a word is one of the title's, or its initials ("aos": Aria of Sorrow); half of
     * one when it's only the start of one */
    char initials[WORDS];
    for (size_t b = 0; b < tn; b++)
      initials[b] = t[b].p[0];
    size_t halves = 0;
    for (size_t a = 0; a < qn; a++) {
      size_t best = q[a].n >= 2 && q[a].n <= tn && memmem(initials, tn, q[a].p, q[a].n) != NULL ? 2 : 0;
      for (size_t b = 0; b < tn && best < 2; b++)
        if (t[b].n >= q[a].n && memcmp(t[b].p, q[a].p, q[a].n) == 0)
          best = t[b].n == q[a].n ? 2 : 1;
      halves += best;
    }
    if (halves == 0)
      continue;
    /* Dice: words shared, over all of both; a title in many regions once (one with art) */
    const hit e = {all[i], title, (int)(halves * 1000 / (qn + tn))};
    size_t k = 0;
    while (k < n && hits[k].title != e.title)
      k++;
    if (k < n) {
      if (records[i].image != 0xffff && hits[k].crc != e.crc) {
        const uint32_t *at = bsearch(&hits[k].crc, all, h->count, sizeof(*all), by_crc);
        if (at != NULL && records[at - all].image == 0xffff)
          hits[k].crc = e.crc;
      }
      continue;
    }
    /* The best `max`, kept sorted: the new one in, the worst out */
    if (n == max && by_score(&e, &hits[n - 1]) >= 0)
      continue;
    k = n < max ? n++ : n - 1;
    while (k > 0 && by_score(&e, &hits[k - 1]) < 0) {
      hits[k] = hits[k - 1];
      k--;
    }
    hits[k] = e;
  }
  for (size_t i = 0; i < n; i++)
    crcs[i] = hits[i].crc;
  free(hits);
  return n;
}

/* The CRC cache: what the thread works through and what it found */
enum { PENDING, HASHING, KNOWN, FAILED };
typedef struct {
  str path; /* never freed: the hasher reads it without the lock */
  long long size, mtime;
  uint32_t crc;
  int state;
} rom;
static rom *roms;
static size_t rom_count, rom_capacity;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static char cache_path[512];

/* Under the lock */
static rom *remember(str path) {
  for (size_t i = 0; i < rom_count; i++)
    if (str_eq(roms[i].path, path))
      return &roms[i];
  if (rom_count == rom_capacity) {
    const size_t capacity = rom_capacity ? rom_capacity * 2 : 64;
    rom *grown = realloc(roms, capacity * sizeof(*grown));
    if (grown == NULL)
      return NULL;
    roms = grown;
    rom_capacity = capacity;
  }
  const str copy = str_dup(path);
  if (copy.p == NULL)
    return NULL;
  roms[rom_count] = (rom){.path = copy, .state = FAILED};
  return &roms[rom_count++];
}

/* A zip's first file, from the central directory: nothing is inflated.
 * The first entry only; multi-file zips would want the largest. */
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

/* No-Intro's CRCs leave out the dumpers' headers: iNES, fwNES, Lynx's and a
 * SNES or PC Engine copier's 512 bytes. In a zip they count; the DATs carry
 * NES and Lynx headered CRCs too. */
static size_t header_size(str path, const uint8_t *buf, size_t n, off_t size) {
  if (n >= 16 && (memcmp(buf, "NES\x1a", 4) == 0 || memcmp(buf, "FDS\x1a", 4) == 0))
    return 16;
  if (n >= 64 && memcmp(buf, "LYNX", 5) == 0)
    return 64;
  if (n >= 512 && size % 1024 == 512 &&
      (str_iends(path, S(".smc")) || str_iends(path, S(".sfc")) || str_iends(path, S(".pce"))))
    return 512;
  return 0;
}

static bool file_crc(str path, uint32_t *crc) {
  FILE *f = fopen(path.p, "rb");
  if (f == NULL)
    return false;
  bool ok;
  if (str_iends(path, S(".zip"))) {
    ok = zip_crc(f, crc);
  } else {
    static uint8_t buf[1 << 16];
    uLong c = crc32(0, NULL, 0);
    size_t n = fread(buf, 1, sizeof(buf), f);
    struct stat st;
    const size_t skip = header_size(path, buf, n, fstat(fileno(f), &st) == 0 ? st.st_size : 0);
    c = crc32(c, buf + skip, (uInt)(n - skip));
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
      c = crc32(c, buf, (uInt)n);
    ok = !ferror(f);
    *crc = (uint32_t)c;
  }
  fclose(f);
  return ok;
}

static void *hasher(void *data) {
  (void)data;
  pthread_mutex_lock(&lock);
  for (;;) {
    size_t i = 0;
    while (i < rom_count && roms[i].state != PENDING)
      i++;
    if (i == rom_count) {
      pthread_cond_wait(&wake, &lock);
      continue;
    }
    roms[i].state = HASHING;
    const str path = roms[i].path;
    const long long size = roms[i].size, mtime = roms[i].mtime;
    pthread_mutex_unlock(&lock);

    uint32_t crc = 0;
    const bool ok = file_crc(path, &crc);
    if (ok) {
      FILE *f = fopen(cache_path, "a");
      if (f != NULL) {
        fprintf(f, "%08x %lld %lld ", crc, size, mtime);
        str_fput(f, path, S("\n"));
        fclose(f);
      }
    }

    pthread_mutex_lock(&lock);
    /* Unless the file changed meanwhile: then it is pending again */
    if (roms[i].state == HASHING && roms[i].size == size && roms[i].mtime == mtime) {
      roms[i].crc = crc;
      roms[i].state = ok ? KNOWN : FAILED;
    }
    win_wake(UI_HASHED, 0);
  }
  return 0;
}

void romdb_init(void) {
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
  char *line = NULL;
  size_t capacity = 0;
  ssize_t n;
  while (f != NULL && (n = getline(&line, &capacity, f)) >= 0) {
    unsigned crc;
    long long size, mtime;
    int at;
    str rest = {line, n};
    const str whole = str_cut(&rest, '\n');
    line[whole.n] = '\0';
    if (sscanf(line, "%x %lld %lld %n", &crc, &size, &mtime, &at) != 3)
      continue;
    rom *r = remember(str_at(whole, at));
    if (r != NULL)
      *r = (rom){r->path, size, mtime, crc, KNOWN};
  }
  free(line);
  if (f != NULL)
    fclose(f);
  spawn("crc32", hasher, NULL);
}

bool romdb_crc(str path, const struct stat *st, uint32_t *crc) {
  pthread_mutex_lock(&lock);
  rom *r = remember(path);
  bool known = false;
  if (r != NULL) {
    if (r->size != st->st_size || r->mtime != st->st_mtime) {
      r->size = st->st_size;
      r->mtime = st->st_mtime;
      r->state = PENDING;
      pthread_cond_signal(&wake);
    }
    known = r->state == KNOWN;
    *crc = r->crc;
  }
  pthread_mutex_unlock(&lock);
  return known;
}
