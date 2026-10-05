/* Wrapped text and readmes (ui.h) */
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

static bool starts(str s, str with) { return s.n >= with.n && memcmp(s.p, with.p, with.n) == 0; }

/* One line's inline marks: **, __, `, <tags>, ![images](...) go; [text](url) is its text */
static void plain_inline(str s, char *out, size_t *n) {
  for (size_t i = 0; i < s.n;) {
    const str at = str_at(s, i);
    if (starts(at, S("**")) || starts(at, S("__"))) {
      i += 2;
    } else if (at.p[0] == '`') {
      i++;
    } else if (at.p[0] == '<' && str_find(at, '>') < at.n && str_find(at, ' ') > 0) {
      i += str_find(at, '>') + 1;
    } else if (at.p[0] == '[' || starts(at, S("!["))) {
      const bool image = at.p[0] == '!';
      const str rest = str_at(at, image ? 2 : 1);
      const size_t close = str_find(rest, ']');
      if (close < rest.n && close + 1 < rest.n && rest.p[close + 1] == '(' &&
          str_find(str_at(rest, close + 2), ')') < rest.n - close - 2) {
        if (!image)
          plain_inline(str_head(rest, close), out, n);
        i += (image ? 2 : 1) + close + 2 + str_find(str_at(rest, close + 2), ')') + 1;
      } else {
        out[(*n)++] = s.p[i++];
      }
    } else {
      out[(*n)++] = s.p[i++];
    }
  }
}

str ui_markdown(str md) {
  /* Never longer than the source, but for the bullets: "•" is 3 bytes for 2 */
  char *out = malloc(md.n + md.n / 2 + 1);
  size_t n = 0;
  if (out == NULL)
    return (str){0};
  while (md.n > 0) {
    str line = str_trim(str_cut(&md, '\n'));
    /* Rules and a table's |--|--| line */
    bool rule = line.n >= 3;
    for (size_t i = 0; i < line.n; i++)
      rule &= strchr("-=*_|: ", line.p[i]) != NULL;
    if (rule) {
      /* A table's: nothing, between its header and rows */
      if (str_find(line, '|') == line.n)
        out[n++] = '\n';
      continue;
    }
    while (line.n > 0 && line.p[0] == '#')
      line = str_at(line, 1);
    while (line.n > 0 && line.p[0] == '>')
      line = str_at(line, 1);
    line = str_trim(line);
    if (starts(line, S("- ")) || starts(line, S("* ")) || starts(line, S("+ "))) {
      memcpy(out + n, "• ", 4);
      n += 4;
      line = str_at(line, 2);
    }
    if (line.n > 0 && line.p[0] == '|') {
      /* A table row: its cells, apart */
      line = str_at(line, 1);
      if (line.n > 0 && line.p[line.n - 1] == '|')
        line.n--;
      for (bool first = true; line.n > 0; first = false) {
        if (!first)
          out[n++] = ' ', out[n++] = ' ';
        plain_inline(str_trim(str_cut(&line, '|')), out, &n);
      }
    } else {
      plain_inline(line, out, &n);
    }
    out[n++] = '\n';
  }
  /* Runs of blank lines as one */
  size_t w = 0;
  for (size_t r = 0; r < n; r++)
    if (!(out[r] == '\n' && w >= 2 && out[w - 1] == '\n' && out[w - 2] == '\n'))
      out[w++] = out[r];
  while (w > 0 && out[w - 1] == '\n')
    w--;
  out[w] = '\0';
  return (str){out, w};
}
