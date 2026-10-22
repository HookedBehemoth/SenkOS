/*
 * The handheld's buttons from qemu's keyboard (senkos-qemu, tools/qemu): grabs
 * the keyboards and plays their keys out of uinput devices named like the
 * handheld's, which the launcher and corehost open by name (common/pad.c).
 * RetroArch's default keys: arrows, X A, Z B, S X, A Y, Q W L1 R1, E R L2 R2,
 * Enter start, right shift select, Esc menu; - = volume, P power.
 */
#include <fcntl.h>
#include <glob.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "util.h"

enum { PAD, VOLUME, POWER, DEVICES };
static const char *const names[DEVICES] = {"Anbernic Gamepad", "gpio-keys-volume", "axp20x-pek"};
static const struct {
  unsigned short key, dev, code;
} keys[] = {
    {KEY_UP, PAD, BTN_DPAD_UP},       {KEY_DOWN, PAD, BTN_DPAD_DOWN},
    {KEY_LEFT, PAD, BTN_DPAD_LEFT},   {KEY_RIGHT, PAD, BTN_DPAD_RIGHT},
    {KEY_X, PAD, BTN_EAST},           {KEY_Z, PAD, BTN_SOUTH},
    {KEY_S, PAD, BTN_NORTH},          {KEY_A, PAD, BTN_WEST},
    {KEY_Q, PAD, BTN_TL},             {KEY_W, PAD, BTN_TR},
    {KEY_E, PAD, BTN_TL2},            {KEY_R, PAD, BTN_TR2},
    {KEY_ENTER, PAD, BTN_START},      {KEY_RIGHTSHIFT, PAD, BTN_SELECT},
    {KEY_ESC, PAD, BTN_MODE},         {KEY_1, PAD, BTN_THUMBL},
    {KEY_2, PAD, BTN_THUMBR},         {KEY_MINUS, VOLUME, KEY_VOLUMEDOWN},
    {KEY_EQUAL, VOLUME, KEY_VOLUMEUP}, {KEY_P, POWER, KEY_POWER},
};

static int create(int dev) {
  const int fd = open("/dev/uinput", O_WRONLY | O_CLOEXEC);
  if (fd < 0 || ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0)
    return -1;
  for (size_t i = 0; i < COUNTOF(keys); i++)
    if (keys[i].dev == dev)
      ioctl(fd, UI_SET_KEYBIT, keys[i].code);
  struct uinput_setup setup = {.id = {.bustype = BUS_VIRTUAL}};
  /* The pad's ids, as SDL's mapping (wayland.sh) names it */
  if (dev == PAD)
    setup.id = (struct input_id){.bustype = BUS_HOST, .vendor = 1, .product = 1, .version = 0x100};
  snprintf(setup.name, sizeof(setup.name), "%s", names[dev]);
  if (ioctl(fd, UI_DEV_SETUP, &setup) < 0 || ioctl(fd, UI_DEV_CREATE) < 0)
    return -1;
  return fd;
}

static void emit(int fd, unsigned short type, unsigned short code, int value) {
  const struct input_event e = {.type = type, .code = code, .value = value};
  if (write(fd, &e, sizeof(e)) != sizeof(e))
    perror("qemupad: write");
}

int main(void) {
  /* The keyboards first: ours have no letter keys, but aren't there yet either */
  struct pollfd kbd[16];
  nfds_t n = 0;
  glob_t g;
  if (glob("/dev/input/event*", 0, NULL, &g) == 0) {
    for (size_t i = 0; i < g.gl_pathc && n < COUNTOF(kbd); i++) {
      uint8_t bits[KEY_MAX / 8 + 1] = {0};
      const int fd = open(g.gl_pathv[i], O_RDONLY | O_CLOEXEC);
      if (fd >= 0 && ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) >= 0 &&
          bits[KEY_A / 8] >> (KEY_A % 8) & 1 && ioctl(fd, EVIOCGRAB, 1) == 0)
        kbd[n++] = (struct pollfd){.fd = fd, .events = POLLIN};
      else if (fd >= 0)
        close(fd);
    }
    globfree(&g);
  }
  int out[DEVICES];
  for (int i = 0; i < DEVICES; i++)
    if ((out[i] = create(i)) < 0) {
      perror("qemupad: uinput");
      return 1;
    }
  fprintf(stderr, "qemupad: %u keyboards\n", (unsigned)n);
  /* In the background once the devices are there: the launcher looks for them next */
  if (daemon(0, 1) < 0)
    return 1;
  while (poll(kbd, n, -1) > 0) {
    for (nfds_t i = 0; i < n; i++) {
      struct input_event e;
      if (!(kbd[i].revents & POLLIN) || read(kbd[i].fd, &e, sizeof(e)) != sizeof(e))
        continue;
      /* Not the repeats: the launcher repeats the d-pad itself */
      if (e.type != EV_KEY || e.value == 2)
        continue;
      for (size_t k = 0; k < COUNTOF(keys); k++)
        if (keys[k].key == e.code) {
          emit(out[keys[k].dev], EV_KEY, keys[k].code, e.value);
          emit(out[keys[k].dev], EV_SYN, SYN_REPORT, 0);
        }
    }
  }
  return 1;
}
