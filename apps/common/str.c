#include "str.h"

#include <stdarg.h>
#include <stdlib.h>

bool str_put(char *p, size_t size, size_t *n, const str *parts, size_t count) {
  bool fits = true;
  for (size_t i = 0; i < count; i++) {
    const size_t room = size - 1 - *n, take = parts[i].n < room ? parts[i].n : room;
    if (take > 0)
      memcpy(p + *n, parts[i].p, take);
    *n += take;
    fits &= take == parts[i].n;
  }
  p[*n] = '\0';
  return fits;
}

bool str_fmt(char *p, size_t size, size_t *n, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  const int k = vsnprintf(p + *n, size - *n, fmt, ap);
  va_end(ap);
  if (k < 0) {
    p[*n] = '\0';
    return false;
  }
  const bool fits = (size_t)k < size - *n;
  *n = fits ? *n + k : size - 1;
  return fits;
}

size_t str_copy(char *dst, size_t size, str s) {
  size_t n = 0;
  str_put(dst, size, &n, &s, 1);
  return n;
}

void str_fputv(FILE *f, const str *parts, size_t count) {
  for (size_t i = 0; i < count; i++)
    fwrite(parts[i].p, 1, parts[i].n, f);
}

str str_dup(str s) {
  char *p = malloc(s.n + 1);
  if (p == NULL)
    return (str){0};
  if (s.n > 0)
    memcpy(p, s.p, s.n);
  p[s.n] = '\0';
  return (str){p, s.n};
}

str str_read(FILE *f) {
  if (f == NULL)
    return (str){0};
  char *p = NULL;
  long size = -1;
  if (fseek(f, 0, SEEK_END) == 0 && (size = ftell(f)) >= 0 && fseek(f, 0, SEEK_SET) == 0)
    p = malloc(size + 1);
  size_t n = p != NULL ? fread(p, 1, size, f) : 0;
  fclose(f);
  if (p != NULL)
    p[n] = '\0';
  return (str){p, n};
}
