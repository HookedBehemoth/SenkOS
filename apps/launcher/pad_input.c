/* Pad input, repeat, and grab policy. */
#include <errno.h>
#include <linux/input.h>
#include <poll.h>
#include <stdatomic.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "idle.h"
#include "launcher_internal.h"
#include "pad.h"
#include "util.h"
#include "wake.h"

void pad_listen(bool on);

enum { REPEAT_DELAY = 350, REPEAT_MS = 80 };
static int pad_fd = -1;
static atomic_bool listening;

void pad_listen(bool on) { atomic_store(&listening, on); }

static void push_button(int button, bool down) {
  win_wake(UI_BUTTON, button << 1 | down);
}

static void *pad_thread(void *data) {
  (void)data;
  struct input_event e;
  bool menu = false;
  int held = -1; /* the d-pad button repeating */
  uint64_t next = 0;
  for (;;) {
    struct pollfd p = {.fd = pad_fd, .events = POLLIN};
    const int wait = held < 0 ? -1 : (int)(next - MIN(next, ms_now()));
    const int ready = poll(&p, 1, wait);
    if (ready == 0) {
      if (atomic_load(&listening))
        push_button(held, true);
      next += REPEAT_MS;
      continue;
    }
    if (ready < 0 && errno == EINTR)
      continue;
    if (ready < 0 || read(pad_fd, &e, sizeof(e)) != sizeof(e))
      break;
    if (e.type != EV_KEY || e.value == 2)
      continue;
    idle_poke();
    if (e.code == BTN_MODE)
      menu = e.value;
    else if (!menu && !atomic_load(&listening))
      continue;
    const int button = pad_button(e.code);
    if (button < 0)
      continue;
    if (button <= APPCTL_RIGHT && button >= APPCTL_UP) {
      if (e.value) {
        held = button;
        next = ms_now() + REPEAT_DELAY;
      } else if (held == button) {
        held = -1;
      }
    }
    push_button(button, e.value);
  }
  return NULL;
}

void pad_start(void) {
  if ((pad_fd = pad_open(0)) >= 0)
    spawn("pad", pad_thread, NULL);
}

void pad_grab(bool on) {
  /* Keep pause-menu presses out of the stopped app's queue. */
  if (pad_fd >= 0)
    ioctl(pad_fd, EVIOCGRAB, on);
}
