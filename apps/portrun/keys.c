#include "keys.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/uinput.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "pad.h"
#include "util.h"

static const struct {
  const char *name;
  int button;
} buttons[] = {
    {"a", APPCTL_A}, {"b", APPCTL_B}, {"x", APPCTL_X}, {"y", APPCTL_Y},
    {"l1", APPCTL_L1}, {"l2", APPCTL_L2}, {"r1", APPCTL_R1}, {"r2", APPCTL_R2},
    {"start", APPCTL_START}, {"back", APPCTL_SELECT}, {"select", APPCTL_SELECT},
    {"up", APPCTL_UP}, {"down", APPCTL_DOWN}, {"left", APPCTL_LEFT}, {"right", APPCTL_RIGHT},
};

static const struct {
  const char *name;
  uint16_t code;
} names[] = {
    {"a", KEY_A}, {"b", KEY_B}, {"c", KEY_C}, {"d", KEY_D}, {"e", KEY_E}, {"f", KEY_F}, {"g", KEY_G},
    {"h", KEY_H}, {"i", KEY_I}, {"j", KEY_J}, {"k", KEY_K}, {"l", KEY_L}, {"m", KEY_M}, {"n", KEY_N},
    {"o", KEY_O}, {"p", KEY_P}, {"q", KEY_Q}, {"r", KEY_R}, {"s", KEY_S}, {"t", KEY_T}, {"u", KEY_U},
    {"v", KEY_V}, {"w", KEY_W}, {"x", KEY_X}, {"y", KEY_Y}, {"z", KEY_Z},
    {"1", KEY_1}, {"2", KEY_2}, {"3", KEY_3}, {"4", KEY_4}, {"5", KEY_5}, {"6", KEY_6}, {"7", KEY_7},
    {"8", KEY_8}, {"9", KEY_9}, {"0", KEY_0},
    {"f1", KEY_F1}, {"f2", KEY_F2}, {"f3", KEY_F3}, {"f4", KEY_F4}, {"f5", KEY_F5}, {"f6", KEY_F6},
    {"f7", KEY_F7}, {"f8", KEY_F8}, {"f9", KEY_F9}, {"f10", KEY_F10}, {"f11", KEY_F11}, {"f12", KEY_F12},
    {"esc", KEY_ESC}, {"escape", KEY_ESC}, {"enter", KEY_ENTER}, {"tab", KEY_TAB}, {"space", KEY_SPACE},
    {"backspace", KEY_BACKSPACE}, {"delete", KEY_DELETE}, {"insert", KEY_INSERT}, {"home", KEY_HOME},
    {"end", KEY_END}, {"pageup", KEY_PAGEUP}, {"pagedown", KEY_PAGEDOWN}, {"pause", KEY_PAUSE},
    {"up", KEY_UP}, {"down", KEY_DOWN}, {"left", KEY_LEFT}, {"right", KEY_RIGHT},
    {"shift", KEY_LEFTSHIFT}, {"leftshift", KEY_LEFTSHIFT}, {"rightshift", KEY_RIGHTSHIFT},
    {"ctrl", KEY_LEFTCTRL}, {"leftctrl", KEY_LEFTCTRL}, {"rightctrl", KEY_RIGHTCTRL},
    {"alt", KEY_LEFTALT}, {"leftalt", KEY_LEFTALT}, {"rightalt", KEY_RIGHTALT}, {"capslock", KEY_CAPSLOCK},
    {"-", KEY_MINUS}, {"minus", KEY_MINUS}, {"=", KEY_EQUAL}, {"equal", KEY_EQUAL}, {"+", KEY_KPPLUS},
    {",", KEY_COMMA}, {"comma", KEY_COMMA}, {".", KEY_DOT}, {"period", KEY_DOT}, {"/", KEY_SLASH},
    {"slash", KEY_SLASH}, {"\\", KEY_BACKSLASH}, {"backslash", KEY_BACKSLASH}, {";", KEY_SEMICOLON},
    {"semicolon", KEY_SEMICOLON}, {"'", KEY_APOSTROPHE}, {"apostrophe", KEY_APOSTROPHE},
    {"[", KEY_LEFTBRACE}, {"leftbrace", KEY_LEFTBRACE}, {"]", KEY_RIGHTBRACE}, {"rightbrace", KEY_RIGHTBRACE},
    {"`", KEY_GRAVE}, {"grave", KEY_GRAVE}, {"kpenter", KEY_KPENTER}, {"numpadenter", KEY_KPENTER},
    {"numpad0", KEY_KP0}, {"numpad1", KEY_KP1}, {"numpad2", KEY_KP2}, {"numpad3", KEY_KP3},
    {"numpad4", KEY_KP4}, {"numpad5", KEY_KP5}, {"numpad6", KEY_KP6}, {"numpad7", KEY_KP7},
    {"numpad8", KEY_KP8}, {"numpad9", KEY_KP9},
    {"mouse_left", BTN_LEFT}, {"mouse_right", BTN_RIGHT}, {"mouse_middle", BTN_MIDDLE},
};

void keys_set(keymap *k, str name, str value) {
  if (value.n >= 2 && value.p[0] == '"' && value.p[value.n - 1] == '"')
    value = str_head(str_at(value, 1), value.n - 2);
  if (str_eq(name, S("mouse_step"))) {
    k->step = atoi(value.p);
    return;
  }
  int b = -1;
  for (size_t i = 0; i < COUNTOF(buttons) && b < 0; i++)
    if (str_eq(name, str_c(buttons[i].name)))
      b = buttons[i].button;
  if (b < 0 || value.n == 0) {
    fprintf(stderr, "portrun: [Keys] %.*s: not a button\n", (int)name.n, name.p);
    return;
  }
  static const struct {
    const char *name;
    int8_t dx, dy, wheel;
  } moves[] = {
      {"mouse_movement_up", 0, -1, 0}, {"mouse_movement_down", 0, 1, 0},
      {"mouse_movement_left", -1, 0, 0}, {"mouse_movement_right", 1, 0, 0},
      {"mouse_wheel_up", 0, 0, 1}, {"mouse_wheel_down", 0, 0, -1},
  };
  for (size_t i = 0; i < COUNTOF(moves); i++)
    if (str_ieq(value, str_c(moves[i].name))) {
      k->dx[b] = moves[i].dx, k->dy[b] = moves[i].dy, k->wheel[b] = moves[i].wheel;
      k->any = true;
      return;
    }
  for (size_t i = 0; i < COUNTOF(names); i++)
    if (str_ieq(value, str_c(names[i].name))) {
      k->code[b] = names[i].code;
      k->any = true;
      return;
    }
  fprintf(stderr, "portrun: [Keys] %.*s=%.*s: no such key\n", (int)name.n, name.p, (int)value.n, value.p);
}

static void emit(int fd, uint16_t type, uint16_t code, int32_t value) {
  struct input_event e = {.type = type, .code = code, .value = value};
  (void)!write(fd, &e, sizeof(e));
}

static int create(const keymap *k) {
  const int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) {
    fprintf(stderr, "portrun: /dev/uinput: %s, the pad stays a pad\n", strerror(errno));
    return -1;
  }
  ioctl(fd, UI_SET_EVBIT, EV_KEY);
  ioctl(fd, UI_SET_EVBIT, EV_REL);
  ioctl(fd, UI_SET_EVBIT, EV_SYN);
  for (int b = 0; b < APPCTL_BUTTONS; b++)
    if (k->code[b] != 0)
      ioctl(fd, UI_SET_KEYBIT, k->code[b]);
  /* A pointer needs a button for libinput to take it as one */
  ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);
  ioctl(fd, UI_SET_RELBIT, REL_X);
  ioctl(fd, UI_SET_RELBIT, REL_Y);
  ioctl(fd, UI_SET_RELBIT, REL_WHEEL);
  struct uinput_setup setup = {.id = {.bustype = BUS_VIRTUAL, .vendor = 0x1209, .product = 0x7072}};
  snprintf(setup.name, sizeof(setup.name), "portrun keys");
  if (ioctl(fd, UI_DEV_SETUP, &setup) != 0 || ioctl(fd, UI_DEV_CREATE) != 0) {
    fprintf(stderr, "portrun: uinput: %s\n", strerror(errno));
    close(fd);
    return -1;
  }
  return fd;
}

void keys_run(const keymap *k, int until) {
  int pad = -1, out = -1;
  if (!k->any || (pad = pad_open(O_NONBLOCK)) < 0 || (out = create(k)) < 0) {
    if (pad >= 0)
      close(pad);
    return;
  }
  const int step = k->step > 0 ? k->step : 4;
  uint16_t held = 0;
  for (;;) {
    int dx = 0, dy = 0;
    for (int b = 0; b < APPCTL_BUTTONS; b++)
      if (held >> b & 1)
        dx += k->dx[b], dy += k->dy[b];
    /* A frame's movement while one is held */
    struct pollfd p[2] = {{.fd = pad, .events = POLLIN}, {.fd = until, .events = POLLIN}};
    const int n = poll(p, 2, dx != 0 || dy != 0 ? 16 : -1);
    if (n < 0 && errno != EINTR)
      break;
    if (p[1].revents != 0 || p[0].revents & (POLLERR | POLLHUP))
      break;
    if (n == 0) {
      if (dx != 0)
        emit(out, EV_REL, REL_X, dx * step);
      if (dy != 0)
        emit(out, EV_REL, REL_Y, dy * step);
      emit(out, EV_SYN, SYN_REPORT, 0);
      continue;
    }
    struct input_event e;
    while (read(pad, &e, sizeof(e)) == sizeof(e)) {
      const int b = e.type == EV_KEY && e.value != 2 ? pad_button(e.code) : -1;
      if (b < 0)
        continue;
      held = e.value ? held | 1u << b : held & ~(1u << b);
      if (k->code[b] != 0)
        emit(out, EV_KEY, k->code[b], e.value);
      else if (k->wheel[b] != 0 && e.value)
        emit(out, EV_REL, REL_WHEEL, k->wheel[b]);
      emit(out, EV_SYN, SYN_REPORT, 0);
    }
  }
  ioctl(out, UI_DEV_DESTROY);
  close(out);
  close(pad);
}
