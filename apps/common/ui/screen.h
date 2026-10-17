/* Immediate-mode screen stack and focus restoration. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nav.h"
#include "str.h"

typedef struct ui_screen ui_screen;
typedef struct ui_screen_def ui_screen_def;

struct ui_screen {
  const ui_screen_def *def;
  uint32_t instance;
  nav_id focus;
  intptr_t arg;
};

struct ui_screen_def {
  const char *name;
  bool overlay;
  str title_text;
  str (*title)(const ui_screen *screen);
  str (*hint)(const ui_screen *screen, const nav *ui);
  void (*frame)(ui_screen *screen, nav *ui, ui_rect content);
  bool (*button)(ui_screen *screen, nav *ui, int button);
  void (*text)(ui_screen *screen, nav *ui, str value);
  /* On joining a stack, and on leaving it (what's pushed on it doesn't leave it) */
  void (*enter)(void);
  void (*leave)(void);
};

#define SCREEN_DEPTH 8
typedef struct {
  ui_screen items[SCREEN_DEPTH];
  size_t count;
  unsigned generation;
} screen_stack;

typedef struct {
  int battery;
  bool charging;
  int wifi;
  int bluetooth; /* -1 off, 0 on, 1 connected */
} ui_status;

ui_screen *screen_top(screen_stack *stack);
const ui_screen *screen_base(const screen_stack *stack);
const ui_screen *screen_find(const screen_stack *stack, const ui_screen_def *def);
const ui_screen_def *screen_current(const screen_stack *stack);
const ui_screen_def *screen_base_def(const screen_stack *stack);
bool screen_contains(const screen_stack *stack, const ui_screen_def *def);
intptr_t screen_arg(const screen_stack *stack, const ui_screen_def *def);
void screen_push(screen_stack *stack, nav *ui, const ui_screen_def *def,
                 intptr_t arg);
void screen_pop(screen_stack *stack, nav *ui);
void screen_replace(screen_stack *stack, nav *ui, const ui_screen_def *def,
                    intptr_t arg);
void screen_focus_save(screen_stack *stack, const nav *ui);
void screen_focus_restore(const screen_stack *stack, nav *ui);
bool screen_button(screen_stack *stack, nav *ui, int button);
void screen_draw(screen_stack *stack, nav *ui, ui_rect content);
str screen_title(const screen_stack *stack);
str screen_hint(const screen_stack *stack, const nav *ui);
ui_rect screen_chrome(const screen_stack *stack, const nav *ui, int w, int h,
                      const ui_status *status);
