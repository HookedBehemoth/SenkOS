#include "licenses.h"

#include <stdlib.h>

#include "ui.h"

license licenses[LICENSES_MAX];
size_t license_count;
/* The file, kept: the runs and strings point into it */
static const uint16_t *runs;
static str *strings;
static size_t string_count;
static STRBUF(256) labels[LICENSES_MAX];

static str string(size_t i) { return i < string_count ? strings[i] : (str){0}; }

void licenses_load(void) {
  static bool loaded;
  if (loaded)
    return;
  loaded = true;
  /* On the heap, so aligned for the u16s; little-endian like the device */
  str file = str_read(fopen(LICENSES, "rb"));
  const uint16_t *w = (const uint16_t *)file.p;
  if (file.n < 8)
    return;
  const size_t programs = w[0], names = w[1], run_count = w[2], count = w[3];
  const size_t head = 2 * (4 + programs * 3 + names + run_count * 2);
  if (head > file.n || (strings = malloc(count * sizeof(*strings))) == NULL)
    return;
  str rest = str_at(file, head);
  for (; string_count < count; string_count++)
    strings[string_count] = str_cut(&rest, '\0');
  const uint16_t *program = w + 4, *name = program + programs * 3;
  runs = name + names;
  size_t name_at = 0, run_at = 0;
  for (size_t i = 0; i < programs && license_count < LICENSES_MAX; i++) {
    const uint16_t *p = program + i * 3;
    if (name_at + p[1] > names || run_at + p[2] > run_count)
      break;
    license *l = &licenses[license_count];
    l->name = string(p[0]);
    /* "name · license & license" */
    sb_set(labels[license_count], l->name, S(" \u00b7 "));
    for (size_t j = 0; j < p[1]; j++)
      sb_add(labels[license_count], j > 0 ? S(" & ") : S(""), string(name[name_at + j]));
    l->label = sb_str(labels[license_count]);
    l->run = run_at, l->runs = p[2];
    name_at += p[1], run_at += p[2];
    license_count++;
  }
}

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

/* `line` wrapped onto the lines; false without memory */
static str *out;
static size_t count, cap;
static bool add(str line, int px, float width) {
  do {
    if (count == cap) {
      str *grown = realloc(out, (cap = cap ? cap * 2 : 1024) * sizeof(*out));
      if (grown == NULL)
        return false;
      out = grown;
    }
    const size_t take = fit(line, px, width);
    out[count++] = str_head(line, take);
    line = str_at(line, take);
    /* The spaces a line was broken at */
    while (line.n > 0 && line.p[0] == ' ')
      line = str_at(line, 1);
  } while (line.n > 0);
  return true;
}

size_t license_text(size_t i, int px, float width, const str **lines) {
  static size_t shown_i = (size_t)-1;
  static int shown_px;
  static float shown_width;
  if (i != shown_i || px != shown_px || width != shown_width) {
    shown_i = i, shown_px = px, shown_width = width;
    count = 0;
    /* Each paragraph (a file's heading is one), a blank line between */
    bool ok = true;
    for (uint32_t r = licenses[i].run; ok && r < licenses[i].run + licenses[i].runs; r++)
      for (uint32_t p = runs[r * 2]; ok && p < (uint32_t)runs[r * 2] + runs[r * 2 + 1]; p++) {
        str rest = string(p);
        ok = count == 0 || add(S(""), px, width);
        while (ok && rest.n > 0)
          ok = add(str_cut(&rest, '\n'), px, width);
      }
  }
  *lines = out;
  return count;
}
