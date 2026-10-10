/*
 * Key files, as XDG desktop entries are: "[section]" headers and "key=value"
 * lines; blank lines and '#' or ';' comments skipped, whitespace around keys
 * and values dropped. Everything is a slice of the text, nothing is copied
 * or cut. A value may be an array: items ';'-separated, a trailing one
 * optional ("Keywords=a;b;").
 */
#pragma once

#include "str.h"

typedef enum { CFG_END, CFG_SECTION, CFG_VALUE } cfg_event;

typedef struct {
  str rest;
  str section; /* the latest header, "" before any */
  str key, value;
} cfg_reader;

static inline cfg_reader cfg_init(str text) { return (cfg_reader){.rest = text}; }
cfg_event cfg_next(cfg_reader *reader);
/* The next item of an array, empty ones skipped: false after the last.
 * No "\;" escapes; add them when a value needs a ';'. */
bool cfg_item(str *array, str *item);
