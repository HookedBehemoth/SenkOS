/*
 * The built-in pad, straight from evdev, its buttons as appctl.h names them.
 * Not SDL's game controller: with a joystick open, SDL_WaitEvent polls every
 * millisecond, and SDL has no fd to poll the pad by.
 * The built-in pad only (anbernic-gamepad: its buttons and sticks), raw codes
 * as it labels them.
 */
#pragma once

#include <stdint.h>

#include "str.h"

/* The event node of the input device called `name`, opened with `flags`
 * besides O_RDONLY | O_CLOEXEC; -1 without one */
int input_open(str name, int flags);
/* The pad's (input_open) */
int pad_open(int flags);
/* APPCTL_* for an EV_KEY code, -1 for one that isn't the pad's */
int pad_button(unsigned code);
/* What is held now, a bit per APPCTL_* button: the device's state, not its
 * queue */
uint32_t pad_held(int fd);
/* The APPCTL_* buttons the pad (fd) has, its sticks' directions too */
uint32_t pad_has(int fd);

/* The pad's sticks (fd: pad_open's) where they are now: left x, y, right x, y, -32767..32767 (down and right
 * positive), 0 inside the dead zone. Zeros for fd -1. */
void stick_read(int fd, int16_t axes[4]);
/* How far they push toward APPCTL_LS_UP + dir (dir < 8): 0..32767 */
int stick_tilt(const int16_t axes[4], int dir);
/* The directions pushed past half way, as APPCTL_LS_* bits. One in `was`
 * stays down to a third: no flutter at the edge. */
uint32_t stick_held(const int16_t axes[4], uint32_t was);
