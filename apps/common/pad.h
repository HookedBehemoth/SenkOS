/*
 * The built-in pad, straight from evdev, its buttons as appctl.h names them.
 * Not SDL's game controller: with a joystick open, SDL_WaitEvent polls every
 * millisecond, and SDL has no fd to poll the pad by.
 * The built-in pad only, raw codes as it labels them.
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
uint16_t pad_held(int fd);
