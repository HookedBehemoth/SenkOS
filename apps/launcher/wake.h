/* The launcher's reasons to ui_wake (ui.h), and their values */
#pragma once

#include "ui.h"

enum {
  UI_BUTTON = UI_WAKE_APP, /* the pad: APPCTL_* << 1 | pressed */
  UI_VOLUME,               /* percent, 0 when muted */
  UI_EXITED,               /* an app's pid: it is gone (running.h) */
  UI_HASHED,               /* a ROM's CRC is known: titles and box art may be (romdb.h) */
};
