/*
 * Strings as pointer and length, measured once where they come in (a literal
 * by the compiler, getenv and readdir by str_c) and never again.
 *
 * STRBUF is a fixed buffer that keeps its length, NUL-terminated for the C
 * APIs that want one (fopen, execle). Appending cuts short at its end.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

typedef struct {
  const char *p;
  size_t n;
} str;

#define S(lit) ((str){"" lit, sizeof(lit) - 1})

static inline str str_c(const char *s) { return (str){s, s != NULL ? strlen(s) : 0}; }

static inline str str_head(str s, size_t n) { return (str){s.p, n < s.n ? n : s.n}; }
static inline str str_at(str s, size_t i) { return i < s.n ? (str){s.p + i, s.n - i} : (str){s.p + s.n, 0}; }

static inline bool str_eq(str a, str b) { return a.n == b.n && (a.n == 0 || memcmp(a.p, b.p, a.n) == 0); }
static inline bool str_ieq(str a, str b) { return a.n == b.n && strncasecmp(a.p, b.p, a.n) == 0; }
static inline bool str_iends(str s, str end) { return s.n >= end.n && str_ieq(str_at(s, s.n - end.n), end); }

/* Index of the first / last `c`; s.n without one */
static inline size_t str_find(str s, char c) {
  const char *at = s.n ? (const char *)memchr(s.p, c, s.n) : NULL;
  return at != NULL ? (size_t)(at - s.p) : s.n;
}
static inline size_t str_rfind(str s, char c) {
  for (size_t i = s.n; i > 0; i--)
    if (s.p[i - 1] == c)
      return i - 1;
  return s.n;
}

/* What comes before the next `sep`; `*s` moves past it */
static inline str str_cut(str *s, char sep) {
  const size_t i = str_find(*s, sep);
  const str head = {s->p, i};
  *s = str_at(*s, i + 1);
  return head;
}

static inline str str_trim(str s) {
  while (s.n > 0 && (s.p[0] == ' ' || s.p[0] == '\t'))
    s.p++, s.n--;
  while (s.n > 0 && (s.p[s.n - 1] == ' ' || s.p[s.n - 1] == '\t' || s.p[s.n - 1] == '\r'))
    s.n--;
  return s;
}

#define STRBUF(size) \
  struct {           \
    size_t n;        \
    char p[size];    \
  }
#define sb_str(b) ((str){(b).p, (b).n})
#define STR_PARTS(...) (const str[]){__VA_ARGS__}, sizeof((const str[]){__VA_ARGS__}) / sizeof(str)
/* Appends the parts; false when they didn't all fit */
#define sb_add(b, ...) str_put((b).p, sizeof((b).p), &(b).n, STR_PARTS(__VA_ARGS__))
#define sb_set(b, ...) ((b).n = 0, sb_add(b, __VA_ARGS__))
/* Appends printf output (numbers: a string goes in as a str) */
#define sb_fmt(b, ...) str_fmt((b).p, sizeof((b).p), &(b).n, __VA_ARGS__)
#define str_fput(f, ...) str_fputv(f, STR_PARTS(__VA_ARGS__))

bool str_put(char *p, size_t size, size_t *n, const str *parts, size_t count);
__attribute__((format(printf, 4, 5))) bool str_fmt(char *p, size_t size, size_t *n, const char *fmt, ...);
void str_fputv(FILE *f, const str *parts, size_t count);
/* Into a plain array (a fixed struct field), cut short and terminated; the
 * length copied */
size_t str_copy(char *dst, size_t size, str s);
/* A copy on the heap, NUL-terminated; {NULL, 0} without memory */
str str_dup(str s);
/* All of `f`, which is closed, on the heap (free .p); {NULL, 0} for NULL */
str str_read(FILE *f);
