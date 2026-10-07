#include "pad.h"

#include <fcntl.h>
#include <glob.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>

#include "appctl.h"
#include "str.h"

int input_open(str name, int flags) {
  glob_t g;
  if (glob("/sys/class/input/event*/device/name", 0, NULL, &g) != 0)
    return -1;
  int fd = -1;
  for (size_t i = 0; i < g.gl_pathc && fd < 0; i++) {
    char line[64];
    FILE *f = fopen(g.gl_pathv[i], "r");
    const size_t n = f != NULL ? fread(line, 1, sizeof(line), f) : 0;
    if (f != NULL)
      fclose(f);
    str dev = {line, n};
    if (!str_eq(str_cut(&dev, '\n'), name))
      continue;
    /* /sys/class/input/eventN/device/name -> /dev/input/eventN */
    str event = str_c(strstr(g.gl_pathv[i], "event"));
    STRBUF(64) path;
    if (sb_set(path, S("/dev/input/"), str_cut(&event, '/')))
      fd = open(path.p, O_RDONLY | O_CLOEXEC | flags);
  }
  globfree(&g);
  return fd;
}

int pad_open(int flags) { return input_open(S("gpio-keys-gamepad"), flags); }

int pad_button(unsigned code) {
  switch (code) {
  case BTN_DPAD_UP: return APPCTL_UP;
  case BTN_DPAD_DOWN: return APPCTL_DOWN;
  case BTN_DPAD_LEFT: return APPCTL_LEFT;
  case BTN_DPAD_RIGHT: return APPCTL_RIGHT;
  case BTN_TL: return APPCTL_L1;
  case BTN_TR: return APPCTL_R1;
  case BTN_TL2: return APPCTL_L2;
  case BTN_TR2: return APPCTL_R2;
  case BTN_SELECT: return APPCTL_SELECT;
  case BTN_START: return APPCTL_START;
  /* By label: A is the east one */
  case BTN_EAST: return APPCTL_A;
  case BTN_SOUTH: return APPCTL_B;
  case BTN_NORTH: return APPCTL_X;
  case BTN_WEST: return APPCTL_Y;
  case BTN_MODE: return APPCTL_MENU;
  default: return -1;
  }
}

uint16_t pad_held(int fd) {
  uint8_t bits[KEY_MAX / 8 + 1] = {0};
  uint16_t held = 0;
  if (fd < 0 || ioctl(fd, EVIOCGKEY(sizeof(bits)), bits) < 0)
    return 0;
  for (unsigned code = BTN_MISC; code <= BTN_DPAD_RIGHT; code++) {
    const int b = bits[code / 8] >> (code % 8) & 1 ? pad_button(code) : -1;
    if (b >= 0)
      held |= 1u << b;
  }
  return held;
}
