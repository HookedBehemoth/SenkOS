/* Wrapped text (ui.h) */
#include <stdlib.h>

#include "ui.h"

/* A line into `width`: whole words while they fit, at least one */
static size_t fit(str line, int px, float width) {
  size_t n = 0;
  float w = 0;
  while (n < line.n) {
    size_t end = n;
    while (end < line.n && line.p[end] == ' ')
      end++;
    while (end < line.n && line.p[end] != ' ')
      end++;
    w += ui_text_width(px, (str){line.p + n, end - n});
    if (w > width && n > 0)
      break;
    n = end;
  }
  return n;
}

bool ui_wrap(ui_lines *out, str line, int px, float width) {
  do {
    if (out->count == out->cap) {
      const size_t cap = out->cap ? out->cap * 2 : 256;
      str *grown = realloc(out->lines, cap * sizeof(*grown));
      if (grown == NULL)
        return false;
      out->lines = grown;
      out->cap = cap;
    }
    const size_t take = fit(line, px, width);
    out->lines[out->count++] = str_head(line, take);
    line = str_at(line, take);
    /* The spaces a line was broken at */
    while (line.n > 0 && line.p[0] == ' ')
      line = str_at(line, 1);
  } while (line.n > 0);
  return true;
}

bool ui_wrap_text(ui_lines *out, str text, int px, float width) {
  bool ok = true;
  while (ok && text.n > 0)
    ok = ui_wrap(out, str_cut(&text, '\n'), px, width);
  return ok;
}
