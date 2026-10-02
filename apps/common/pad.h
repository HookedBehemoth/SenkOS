/*
 * The built-in pad, straight from evdev, its buttons as appctl.h names them.
 * Not SDL's game controller: with a joystick open, SDL_WaitEvent polls every
 * millisecond, and SDL has no fd to poll the pad by.
 * ponytail: the built-in pad only, raw codes as it labels them.
 */
#pragma once

#include <stdint.h>

/* Its event node, opened with `flags` besides O_RDONLY | O_CLOEXEC; -1
 * without one */
int pad_open(int flags);
/* APPCTL_* for an EV_KEY code, -1 for one that isn't the pad's */
int pad_button(unsigned code);
/* What is held now, a bit per APPCTL_* button: the device's state, not its
 * queue */
uint16_t pad_held(int fd);
