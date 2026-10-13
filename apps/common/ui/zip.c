#include "zip.h"

#include <stdlib.h>
#include <string.h>

#include <zlib.h>

/* Bigger isn't a manifest or an icon */
#define MAX_ENTRY (4u << 20)
#define MAX_DIR (4u << 20)

/* Zip records: signature, fixed size, field offsets (APPNOTE.TXT 4.3) */
#define LOCAL_SIG 0x04034b50
#define LOCAL_SIZE 30
#define LOCAL_NAME_LEN 26
#define LOCAL_EXTRA_LEN 28

#define ENTRY_SIG 0x02014b50
#define ENTRY_SIZE 46
#define ENTRY_METHOD 10
#define ENTRY_PACKED 20
#define ENTRY_FULL 24
#define ENTRY_NAME_LEN 28
#define ENTRY_EXTRA_LEN 30
#define ENTRY_COMMENT_LEN 32
#define ENTRY_LOCAL 42

#define END_SIG 0x06054b50
#define END_SIZE 22
#define END_DIR_SIZE 12
#define END_DIR_AT 16
#define MAX_COMMENT 65535

#define METHOD_STORED 0
#define METHOD_DEFLATE 8

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* Entry `at` (local header offset) as `full` bytes: stored, or raw deflate */
static uint8_t *zip_entry(FILE *f, uint32_t at, uint16_t method, uint32_t packed, uint32_t full) {
  uint8_t local[LOCAL_SIZE];
  if (packed > MAX_ENTRY || full > MAX_ENTRY || (method != METHOD_STORED && method != METHOD_DEFLATE) ||
      (method == METHOD_STORED && packed != full) ||
      fseek(f, at, SEEK_SET) != 0 || fread(local, 1, sizeof(local), f) != sizeof(local) ||
      u32(local) != LOCAL_SIG ||
      fseek(f, (long)at + LOCAL_SIZE + u16(local + LOCAL_NAME_LEN) + u16(local + LOCAL_EXTRA_LEN), SEEK_SET) != 0)
    return NULL;
  uint8_t *in = malloc(packed + 1), *out = malloc(full + 1);
  bool ok = in != NULL && out != NULL && fread(in, 1, packed, f) == packed;
  if (ok && method == METHOD_STORED) {
    memcpy(out, in, full);
  } else if (ok) {
    z_stream z = {.next_in = in, .avail_in = packed, .next_out = out, .avail_out = full};
    ok = inflateInit2(&z, -MAX_WBITS) == Z_OK;
    if (ok) {
      ok = inflate(&z, Z_FINISH) == Z_STREAM_END && z.total_out == full;
      inflateEnd(&z);
    }
  }
  free(in);
  if (!ok) {
    free(out);
    return NULL;
  }
  out[full] = '\0';
  return out;
}

static uint8_t *zip_find(FILE *f, str name, size_t *size) {
  uint8_t tail[MAX_COMMENT + END_SIZE];
  if (fseek(f, 0, SEEK_END) != 0)
    return NULL;
  const long end = ftell(f);
  const long n = end < (long)sizeof(tail) ? end : (long)sizeof(tail);
  if (n < END_SIZE || fseek(f, end - n, SEEK_SET) != 0 || fread(tail, 1, n, f) != (size_t)n)
    return NULL;
  long i = n - END_SIZE;
  while (i >= 0 && u32(tail + i) != END_SIG)
    i--;
  const uint32_t dir_size = i >= 0 ? u32(tail + i + END_DIR_SIZE) : 0;
  uint8_t *dir = dir_size > 0 && dir_size <= MAX_DIR ? malloc(dir_size) : NULL;
  uint8_t *out = NULL;
  if (dir == NULL || fseek(f, u32(tail + i + END_DIR_AT), SEEK_SET) != 0 || fread(dir, 1, dir_size, f) != dir_size) {
    free(dir);
    return NULL;
  }
  for (uint32_t at = 0; out == NULL && at + ENTRY_SIZE <= dir_size && u32(dir + at) == ENTRY_SIG;) {
    const uint8_t *e = dir + at;
    const uint32_t name_n = u16(e + ENTRY_NAME_LEN);
    if (at + ENTRY_SIZE + name_n > dir_size)
      break;
    if (str_ieq((str){(const char *)e + ENTRY_SIZE, name_n}, name)) {
      out = zip_entry(f, u32(e + ENTRY_LOCAL), u16(e + ENTRY_METHOD), u32(e + ENTRY_PACKED), u32(e + ENTRY_FULL));
      *size = u32(e + ENTRY_FULL);
      break;
    }
    at += ENTRY_SIZE + name_n + u16(e + ENTRY_EXTRA_LEN) + u16(e + ENTRY_COMMENT_LEN);
  }
  free(dir);
  return out;
}

uint8_t *zip_read(const char *path, str name, size_t *size) {
  FILE *f = fopen(path, "rb");
  if (f == NULL)
    return NULL;
  uint8_t *data = zip_find(f, name, size);
  fclose(f);
  return data;
}
