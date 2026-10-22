#include "pad.h"

#include <fcntl.h>
#include <glob.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>

#include "appctl.h"
#include "str.h"
#include "util.h"

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
  case BTN_THUMBL: return APPCTL_L3;
  case BTN_THUMBR: return APPCTL_R3;
  default: return -1;
  }
}

static uint32_t pad_bits(int fd, unsigned long request) {
  uint8_t bits[KEY_MAX / 8 + 1] = {0};
  uint32_t held = 0;
  if (fd < 0 || ioctl(fd, request, bits) < 0)
    return 0;
  for (unsigned code = BTN_MISC; code <= BTN_DPAD_RIGHT; code++) {
    const int b = bits[code / 8] >> (code % 8) & 1 ? pad_button(code) : -1;
    if (b >= 0)
      held |= 1u << b;
  }
  return held;
}

uint32_t pad_held(int fd) { return pad_bits(fd, EVIOCGKEY(KEY_MAX / 8 + 1)); }

uint32_t pad_has(int fd, int stick) {
  const uint32_t sticks = ((1u << (APPCTL_RS_RIGHT + 1)) - 1) & ~((1u << APPCTL_LS_UP) - 1);
  return pad_bits(fd, EVIOCGBIT(EV_KEY, KEY_MAX / 8 + 1)) | (stick >= 0 ? sticks : 0);
}

int stick_open(int flags) { return input_open(S("adc-joystick"), flags); }

/* Of full tilt (~9%): a stick at rest reads off center */
#define DEAD_ZONE 0x0c00

void stick_read(int fd, int16_t axes[4]) {
  static const unsigned codes[4] = {ABS_X, ABS_Y, ABS_RX, ABS_RY};
  for (int i = 0; i < 4; i++) {
    struct input_absinfo a;
    axes[i] = 0;
    if (fd < 0 || ioctl(fd, EVIOCGABS(codes[i]), &a) < 0 || a.maximum <= a.minimum)
      continue;
    const long half = (a.maximum - a.minimum) / 2;
    long v = (a.value - a.minimum - half) * 32767 / half;
    v = v > 0 ? MAX(v - DEAD_ZONE, 0) : MIN(v + DEAD_ZONE, 0);
    v = v * 32767 / (32767 - DEAD_ZONE);
    axes[i] = (int16_t)CLAMP(v, -32767, 32767);
  }
}

int stick_tilt(const int16_t axes[4], int dir) {
  /* up, down, left, right of the left stick, then the right one */
  const int v = axes[(dir / 4) * 2 + (dir % 4 < 2)];
  return dir % 2 ? MAX(v, 0) : MAX(-v, 0);
}

uint32_t stick_held(const int16_t axes[4], uint32_t was) {
  uint32_t held = 0;
  for (int dir = 0; dir < 8; dir++) {
    const uint32_t bit = 1u << (APPCTL_LS_UP + dir);
    if (stick_tilt(axes, dir) > (was & bit ? 32767 / 3 : 32767 / 2))
      held |= bit;
  }
  return held;
}
