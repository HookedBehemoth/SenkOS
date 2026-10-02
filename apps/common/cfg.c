#include "cfg.h"

cfg_event cfg_next(cfg_reader *reader) {
  while (reader->rest.n > 0) {
    const str line = str_trim(str_cut(&reader->rest, '\n'));
    if (line.n == 0 || line.p[0] == '#' || line.p[0] == ';')
      continue;
    if (line.p[0] == '[') {
      const size_t end = str_find(line, ']');
      if (end == line.n)
        continue;
      reader->section = str_trim(str_head(str_at(line, 1), end - 1));
      reader->key = reader->value = (str){0};
      return CFG_SECTION;
    }
    const size_t equals = str_find(line, '=');
    if (equals == line.n)
      continue;
    reader->key = str_trim(str_head(line, equals));
    reader->value = str_trim(str_at(line, equals + 1));
    return CFG_VALUE;
  }
  return CFG_END;
}

bool cfg_item(str *array, str *item) {
  while (array->n > 0)
    if ((*item = str_trim(str_cut(array, ';'))).n > 0)
      return true;
  return false;
}
