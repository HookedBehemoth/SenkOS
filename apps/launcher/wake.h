/* The launcher's reasons to win_wake (win.h, ui.h), and their values */
#pragma once

#include "ui.h"

enum {
  UI_BUTTON = UI_WAKE_APP, /* the pad: APPCTL_* << 1 | pressed */
  UI_KEY,                  /* a keyboard, or the desktop: the same */
  UI_VOLUME,               /* percent, 0 when muted */
  UI_BRIGHTNESS,           /* a volume key with Menu held: the step, 1 or -1 */
  UI_EXITED,               /* an app's pid: it is gone (running.h) */
  UI_HASHED,               /* a ROM's CRC is known: titles and box art may be (romdb.h) */
  UI_LAUNCH,               /* `launcher launch PATH` asked for an app (ctl_path) */
  UI_RELOAD,               /* downloadd added a game pack (its Job), "reload" on the FIFO, or the card's emulators changed (apps_watch) */
  UI_UPDATES,              /* downloadd said something about system updates (updates.h) */
  UI_REFUSED,              /* sysd didn't do what was asked (sysd.h) */
  UI_DONE,                 /* sysd did it: what it changed may show */
  UI_POWER_HOLD,           /* the power key held POWER_HOLD_MS: the power options */
};
