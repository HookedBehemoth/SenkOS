/* Pad input, repeat, and grab policy. */
#include <errno.h>
#include <fcntl.h>
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
static int pad_fd = -1, stick_fd = -1;
static atomic_bool listening;

void pad_listen(bool on) { atomic_store(&listening, on); }

static void push_button(int button, bool down) {
  win_wake(UI_BUTTON, button << 1 | down);
}

/* A button or stick direction now down or up: its EV_KEY-like event, -1
 * for none. The sticks' directions come from their state at each report. */
static int next_event(struct pollfd p[2], bool *down) {
  static uint32_t dirs, changed;
  struct input_event e;
  if (changed == 0 && (p[1].revents & POLLIN)) {
    p[1].revents = 0;
    while (read(stick_fd, &e, sizeof(e)) == sizeof(e))
      ;
    int16_t axes[4];
    stick_read(stick_fd, axes);
    const uint32_t now = stick_held(axes, dirs);
    changed = now ^ dirs;
    dirs = now;
  }
  if (changed != 0) {
    const int b = __builtin_ctz(changed);
    changed &= changed - 1;
    *down = dirs >> b & 1;
    return b;
  }
  if (!(p[0].revents & POLLIN))
    return -1;
  p[0].revents = 0;
  if (read(pad_fd, &e, sizeof(e)) != sizeof(e) || e.type != EV_KEY || e.value == 2)
    return -1;
  *down = e.value;
  return pad_button(e.code);
}

static void *pad_thread(void *data) {
  (void)data;
  bool menu = false;
  int held = -1; /* the d-pad button repeating */
  uint64_t next = 0;
  struct pollfd p[2] = {{.fd = pad_fd, .events = POLLIN}, {.fd = stick_fd, .events = POLLIN}};
  for (;;) {
    bool down;
    int button = next_event(p, &down);
    if (button < 0) {
      const int wait = held < 0 ? -1 : (int)(next - MIN(next, ms_now()));
      const int ready = poll(p, 2, wait);
      if (ready == 0) {
        if (atomic_load(&listening))
          push_button(held, true);
        next += REPEAT_MS;
      } else if (ready < 0 && errno != EINTR) {
        break;
      }
      continue;
    }
    idle_poke();
    if (button == APPCTL_MENU)
      menu = down;
    else if (!menu && !atomic_load(&listening))
      continue;
    const bool dpad = button >= APPCTL_UP && button <= APPCTL_RIGHT;
    if (dpad || (button >= APPCTL_LS_UP && button <= APPCTL_LS_RIGHT)) {
      if (down) {
        held = button;
        next = ms_now() + REPEAT_DELAY;
      } else if (held == button) {
        held = -1;
      }
    }
    push_button(button, down);
  }
  return NULL;
}

void pad_start(void) {
  stick_fd = stick_open(O_NONBLOCK);
  if ((pad_fd = pad_open(0)) >= 0)
    spawn("pad", pad_thread, NULL);
}

void pad_grab(bool on) {
  /* Keep pause-menu presses out of the stopped app's queue. */
  if (pad_fd >= 0)
    ioctl(pad_fd, EVIOCGRAB, on);
  if (stick_fd >= 0)
    ioctl(stick_fd, EVIOCGRAB, on);
}
