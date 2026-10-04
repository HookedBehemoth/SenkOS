/*
 * The pad as keyboard and mouse, for games that only take those: [Keys] in
 * port.ini maps the pad's buttons, by gptokeyb's names (a, b, x, y, l1, l2,
 * r1, r2, start, back, up, down, left, right), to keys (a, enter, esc,
 * space, leftctrl, f1, ...), mouse buttons (mouse_left, mouse_right,
 * mouse_middle), mouse movement (mouse_movement_up, ...) and the wheel
 * (mouse_wheel_up, mouse_wheel_down); mouse_step is how far the pointer goes
 * a frame. A uinput device sends them: the compositor gives them to the game.
 * ponytail: buttons only, no analog sticks (the device has none) and none of
 * gptokeyb's hotkeys or states.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "appctl.h"
#include "str.h"

typedef struct {
  /* by APPCTL_* button: a key or mouse button, else a move or wheel step */
  uint16_t code[APPCTL_BUTTONS];
  int8_t dx[APPCTL_BUTTONS], dy[APPCTL_BUTTONS], wheel[APPCTL_BUTTONS];
  int step;
  bool any;
} keymap;

/* One [Keys] line; unknown names are told on stderr and skipped */
void keys_set(keymap *k, str name, str value);
/* Maps until `until` (a pidfd) is readable; at once without a mapping, a
 * pad or uinput */
void keys_run(const keymap *k, int until);
