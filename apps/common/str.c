#include "str.h"

#include <stdarg.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

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
  /* Past stdio: its fseek/ftell and buffer cost an fstat and three lseeks */
  const int fd = fileno(f);
  struct stat st;
  char *p = fstat(fd, &st) == 0 ? malloc((size_t)st.st_size + 1) : NULL;
  size_t n = 0;
  for (ssize_t got = 1; p != NULL && n < (size_t)st.st_size && got > 0; n += got > 0 ? (size_t)got : 0)
    got = read(fd, p + n, (size_t)st.st_size - n);
  fclose(f);
  if (p != NULL)
    p[n] = '\0';
  return (str){p, n};
}
