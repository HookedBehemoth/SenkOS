/*
 * The pad as keyboard and mouse, for games that only take those: [Keys] in
 * port.ini maps the pad's buttons, by gptokeyb's names (a, b, x, y, l1, l2,
 * r1, r2, start, back, up, down, left, right), to keys (a, enter, esc,
 * space, leftctrl, f1, ...), mouse buttons (mouse_left, mouse_right,
 * mouse_middle), mouse movement (mouse_movement_up, ...) and the wheel
 * (mouse_wheel_up, mouse_wheel_down); mouse_step is how far the pointer goes
 * a frame. A uinput device sends them: the compositor gives them to the game.
 *
 * Each line is a binding: what it presses, and the pad buttons that do (a
 * combo, appctl.h's bits), the line's button until the launcher changes it.
 * When two held combos overlap, the bigger one wins.
 * Buttons only, no analog sticks (the device has none) and none of
 * gptokeyb's hotkeys or states.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "appctl.h"
#include "str.h"

enum { KEYS_MAX = 32 };

typedef struct {
  /* [Keys]'s button name: the binding's key; its value: what it presses */
  char button[8], target[APPCTL_TEXT];
  /* A key or mouse button, else a move or a wheel step */
  uint16_t code;
  int8_t dx, dy, wheel;
  uint16_t combo;
  bool on;
} key_binding;

typedef struct {
  key_binding b[KEYS_MAX];
  int n, step;
  /* Mapping, from the pad to the uinput device */
  bool open;
  int pad, out;
  /* Held on the pad; held when the launcher let go of it, ignored until released */
  uint16_t held, ignored;
} keymap;

/* One [Keys] line; unknown names are told on stderr and skipped */
void keys_set(keymap *k, str name, str value);
/* The pad and the device; false (nothing to map) without a binding, a pad or uinput */
bool keys_open(keymap *k);
void keys_close(keymap *k);
/* The pad's events, mapped */
void keys_input(keymap *k);
/* A pointer move is held: keys_move each frame */
bool keys_moving(const keymap *k);
void keys_move(keymap *k);
/* After combos changed: presses and releases by what is held now */
void keys_apply(keymap *k);
/* Paused: everything let go. Resumed: what is held now was for the launcher. */
void keys_release(keymap *k);
void keys_resume(keymap *k);
