/*
 * D-Bus' wire format, read and written in place: little-endian only (ours, BlueZ's,
 * wpa_supplicant's; a big-endian message is refused whole). gen.py turns each signature into a
 * straight run of these, each a bounds check and a load or a store; nothing here parses a
 * signature but bus_skip, for what a variant holds that nobody asked for.
 *
 * Readers and writers go bad instead of failing call by call: check `bad` once at the end. A bad
 * reader reads 0 and "", a bad writer writes nothing more. Alignment is from `base`, the
 * message's first byte, so buffers can be anywhere.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct {
  const uint8_t *p, *end, *base;
  bool bad;
} bus_rd;

typedef struct {
  uint8_t *p, *end, *base;
  bool bad;
} bus_wr;

/* Writes a value gen.py's code doesn't know the parts of (an a{sv}): the caller's */
typedef void bus_put(bus_wr *w, const void *ctx);

/* What a bad reader reads */
static const uint8_t bus_zero[8] __attribute__((aligned(8), unused));

static inline size_t bus_pad(const uint8_t *p, const uint8_t *base, size_t align) {
  return (size_t)(base - p) & (align - 1);
}

/* `n` bytes at the next multiple of `align`: the reader past them; bus_zero when they aren't there */
static inline const uint8_t *bus_rd_take(bus_rd *r, size_t align, size_t n) {
  const size_t pad = bus_pad(r->p, r->base, align);
  if (r->bad || pad > (size_t)(r->end - r->p) || n > (size_t)(r->end - r->p) - pad) {
    r->bad = true;
    r->p = r->end;
    return bus_zero;
  }
  const uint8_t *p = r->p + pad;
  r->p = p + n;
  return p;
}

static inline void bus_rd_align(bus_rd *r, size_t align) { bus_rd_take(r, align, 0); }

#define BUS_RD(type, name)                                                                          \
  static inline type bus_rd_##name(bus_rd *r) {                                                    \
    type v;                                                                                        \
    memcpy(&v, bus_rd_take(r, sizeof(type), sizeof(type)), sizeof(type));                          \
    return v;                                                                                      \
  }
BUS_RD(uint8_t, u8)
BUS_RD(int16_t, i16)
BUS_RD(uint16_t, u16)
BUS_RD(int32_t, i32)
BUS_RD(uint32_t, u32)
BUS_RD(int64_t, i64)
BUS_RD(uint64_t, u64)
BUS_RD(double, f64)
#undef BUS_RD

static inline bool bus_rd_bool(bus_rd *r) {
  const uint32_t v = bus_rd_u32(r);
  r->bad |= v > 1;
  return v != 0;
}

/* `len` bytes and their NUL */
static inline const char *bus_rd_text(bus_rd *r, size_t len, uint32_t *n) {
  const uint8_t *p = bus_rd_take(r, 1, len + 1);
  if (p == bus_zero)
    len = 0;
  else if (p[len] != 0) {
    r->bad = true;
    r->p = r->end;
    p = bus_zero;
    len = 0;
  }
  if (n != NULL)
    *n = (uint32_t)len;
  return (const char *)p;
}
/* s and o; their length in *n (or NULL). They point into the message. */
static inline const char *bus_rd_strn(bus_rd *r, uint32_t *n) { return bus_rd_text(r, bus_rd_u32(r), n); }
static inline const char *bus_rd_str(bus_rd *r) { return bus_rd_strn(r, NULL); }
/* g */
static inline const char *bus_rd_sig(bus_rd *r) { return bus_rd_text(r, bus_rd_u8(r), NULL); }

/* An array whose elements align to `align`: where it ends. Read them while bus_rd_more. */
static inline const uint8_t *bus_rd_array(bus_rd *r, size_t align) {
  const uint32_t n = bus_rd_u32(r);
  bus_rd_align(r, align);
  if (!r->bad && n > (size_t)(r->end - r->p)) {
    r->bad = true;
    r->p = r->end;
  }
  return r->bad ? r->p : r->p + n;
}
static inline bool bus_rd_more(const bus_rd *r, const uint8_t *end) { return !r->bad && r->p < end; }
/* Past the rest of an array */
static inline void bus_rd_skip_to(bus_rd *r, const uint8_t *end) {
  if (!r->bad)
    r->p = end;
}

/* ay: the bytes point into the message */
static inline const uint8_t *bus_rd_bytes(bus_rd *r, size_t *n) {
  const uint8_t *end = bus_rd_array(r, 1), *p = r->p;
  r->p = end;
  *n = (size_t)(end - p);
  return p;
}

/* A v holding exactly `sig`: the reader at its value */
static inline bool bus_rd_variant(bus_rd *r, const char *sig) { return strcmp(bus_rd_sig(r), sig) == 0 && !r->bad; }

/* An a{sv}'s next entry, the reader at its value (to read, or bus_skip(r, *sig)); false at `end` */
static inline bool bus_rd_kv(bus_rd *r, const uint8_t *end, const char **key, uint32_t *key_len, const char **sig) {
  if (!bus_rd_more(r, end))
    return false;
  bus_rd_align(r, 8);
  *key = bus_rd_strn(r, key_len);
  *sig = bus_rd_sig(r);
  return !r->bad;
}

static inline size_t bus_align_of(char code) {
  switch (code) {
  case 'n': case 'q': return 2;
  case 'b': case 'i': case 'u': case 'h': case 's': case 'o': case 'a': return 4;
  case 'x': case 't': case 'd': case '(': case '{': return 8;
  default: return 1;
  }
}

/* Past one complete type in a signature */
static inline const char *bus_sig_next(const char *s) {
  int depth = 0;
  do {
    switch (*s) {
    case '\0': return s;
    case '(': case '{': depth++; break;
    case ')': case '}': depth--; break;
    }
  } while (*s++ == 'a' || depth > 0);
  return s;
}

static void bus_skip(bus_rd *r, const char *sig);

/* Past one value of the type at *s, *s past the type */
static inline void bus_skip1(bus_rd *r, const char **s) {
  const char code = *(*s)++;
  switch (code) {
  case 'y': case 'n': case 'q': case 'b': case 'i': case 'u': case 'h': case 'x': case 't': case 'd':
    bus_rd_take(r, bus_align_of(code), bus_align_of(code));
    break;
  case 's': case 'o': bus_rd_str(r); break;
  case 'g': bus_rd_sig(r); break;
  case 'v': bus_skip(r, bus_rd_sig(r)); break;
  case 'a':
    bus_rd_skip_to(r, bus_rd_array(r, bus_align_of(**s)));
    *s = bus_sig_next(*s);
    break;
  case '(': case '{':
    bus_rd_align(r, 8);
    while (**s != ')' && **s != '}' && **s != '\0' && !r->bad)
      bus_skip1(r, s);
    if (**s == '\0')
      r->bad = true;
    else
      (*s)++;
    break;
  default: r->bad = true; r->p = r->end;
  }
}

/* Past values of the types in `sig` (a variant's, read off the wire) */
static void __attribute__((unused)) bus_skip(bus_rd *r, const char *sig) {
  while (*sig != '\0' && !r->bad)
    bus_skip1(r, &sig);
}

/* `n` bytes at the next multiple of `align`, the padding zeroed; NULL when they don't fit */
static inline uint8_t *bus_wr_take(bus_wr *w, size_t align, size_t n) {
  const size_t pad = bus_pad(w->p, w->base, align);
  if (w->bad || pad + n > (size_t)(w->end - w->p)) {
    w->bad = true;
    return NULL;
  }
  /* One store for any padding (< 8): what it zeroes past it is written next, or not sent */
  if (w->end - w->p >= 8)
    memcpy(w->p, &(uint64_t){0}, 8);
  else
    memset(w->p, 0, pad);
  uint8_t *p = w->p + pad;
  w->p = p + n;
  return p;
}

static inline void bus_wr_align(bus_wr *w, size_t align) { bus_wr_take(w, align, 0); }

#define BUS_WR(type, name)                                                                          \
  static inline void bus_wr_##name(bus_wr *w, type v) {                                            \
    uint8_t *p = bus_wr_take(w, sizeof(type), sizeof(type));                                       \
    if (p != NULL)                                                                                 \
      memcpy(p, &v, sizeof(type));                                                                 \
  }
BUS_WR(uint8_t, u8)
BUS_WR(int16_t, i16)
BUS_WR(uint16_t, u16)
BUS_WR(int32_t, i32)
BUS_WR(uint32_t, u32)
BUS_WR(int64_t, i64)
BUS_WR(uint64_t, u64)
BUS_WR(double, f64)
#undef BUS_WR

static inline void bus_wr_bool(bus_wr *w, bool v) { bus_wr_u32(w, v); }

static inline void bus_wr_text(bus_wr *w, const char *s, size_t n) {
  uint8_t *p = bus_wr_take(w, 1, n + 1);
  if (p != NULL) {
    memcpy(p, s, n);
    p[n] = 0;
  }
}
static inline void bus_wr_strn(bus_wr *w, const char *s, size_t n) {
  bus_wr_u32(w, (uint32_t)n);
  bus_wr_text(w, s, n);
}
static inline void bus_wr_str(bus_wr *w, const char *s) { bus_wr_strn(w, s, strlen(s)); }
static inline void bus_wr_sig(bus_wr *w, const char *s) {
  const size_t n = strlen(s);
  bus_wr_u8(w, (uint8_t)n);
  bus_wr_text(w, s, n);
}

/* An array: bus_wr_array, its elements, bus_wr_array_end */
typedef struct {
  uint8_t *len, *start;
} bus_wr_arr;
static inline bus_wr_arr bus_wr_array(bus_wr *w, size_t align) {
  uint8_t *len = bus_wr_take(w, 4, 4);
  return (bus_wr_arr){len, bus_wr_take(w, align, 0)};
}
static inline void bus_wr_array_end(bus_wr *w, bus_wr_arr a) {
  const uint32_t n = (uint32_t)(w->p - a.start);
  if (!w->bad)
    memcpy(a.len, &n, 4);
}

static inline void bus_wr_bytes(bus_wr *w, const void *p, size_t n) {
  bus_wr_u32(w, (uint32_t)n);
  uint8_t *to = bus_wr_take(w, 1, n);
  if (to != NULL)
    memcpy(to, p, n);
}

/* An a{sv}'s entry: its key, the variant's signature; its value next */
static inline void bus_wr_kv(bus_wr *w, const char *key, const char *sig) {
  bus_wr_align(w, 8);
  bus_wr_str(w, key);
  bus_wr_sig(w, sig);
}
