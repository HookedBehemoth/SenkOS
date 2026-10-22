/* Pad input, repeat, and grab policy. */
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <pthread.h>
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
/* The grab changes once nothing is held: what is held when it starts or ends would be
 * released where the app or we don't see it, stuck down for whoever missed it */
static pthread_mutex_t grab_lock = PTHREAD_MUTEX_INITIALIZER;
static bool grab_want, grab_now;

static void grab_settle(void) {
  pthread_mutex_lock(&grab_lock);
  if (grab_want != grab_now && pad_held(pad_fd) == 0 && ioctl(pad_fd, EVIOCGRAB, grab_want) == 0)
    grab_now = grab_want;
  pthread_mutex_unlock(&grab_lock);
}

void pad_listen(bool on) { atomic_store(&listening, on); }

static void push_button(int button, bool down) {
  win_wake(UI_BUTTON, button << 1 | down);
}

/* A button or stick direction now down or up, -1 for none (yet): what the
 * next event says, the sticks' directions from their state at each report */
static int next_event(bool *down) {
  static uint32_t dirs, changed;
  static bool moved;
  struct input_event e;
  while (changed == 0) {
    if (read(pad_fd, &e, sizeof(e)) != sizeof(e))
      return -1;
    if (e.type == EV_KEY && e.value != 2) {
      *down = e.value;
      return pad_button(e.code);
    }
    moved |= e.type == EV_ABS;
    if (e.type == EV_SYN && moved) {
      int16_t axes[4];
      stick_read(pad_fd, axes);
      const uint32_t now = stick_held(axes, dirs);
      changed = now ^ dirs;
      dirs = now;
      moved = false;
    }
  }
  const int b = __builtin_ctz(changed);
  changed &= changed - 1;
  *down = dirs >> b & 1;
  return b;
}

static void *pad_thread(void *data) {
  (void)data;
  bool menu = false;
  int held = -1; /* the d-pad button repeating */
  uint64_t next = 0;
  for (;;) {
    bool down;
    int button = next_event(&down);
    if (button < 0) {
      const int wait = held < 0 ? -1 : (int)(next - MIN(next, ms_now()));
      struct pollfd p = {.fd = pad_fd, .events = POLLIN};
      const int ready = poll(&p, 1, wait);
      if (ready == 0) {
        if (atomic_load(&listening))
          push_button(held, true);
        next += REPEAT_MS;
      } else if ((ready < 0 && errno != EINTR) || (ready > 0 && (p.revents & ~POLLIN))) {
        break;
      }
      continue;
    }
    idle_poke();
    grab_settle();
    const bool repeats = (button >= APPCTL_UP && button <= APPCTL_RIGHT) ||
                         (button >= APPCTL_LS_UP && button <= APPCTL_LS_RIGHT);
    /* Seen even when not ours: let go in the app, it doesn't repeat at the next pause */
    if (repeats && !down && held == button)
      held = -1;
    if (button == APPCTL_MENU)
      menu = down;
    else if (!menu && !atomic_load(&listening))
      continue;
    if (repeats && down) {
      held = button;
      next = ms_now() + REPEAT_DELAY;
    }
    push_button(button, down);
  }
  return NULL;
}

void pad_start(void) {
  if ((pad_fd = pad_open(O_NONBLOCK)) >= 0)
    spawn("pad", pad_thread, NULL);
}

void pad_grab(bool on) {
  /* Keep pause-menu presses out of the stopped app's queue. */
  if (pad_fd < 0)
    return;
  pthread_mutex_lock(&grab_lock);
  grab_want = on;
  pthread_mutex_unlock(&grab_lock);
  grab_settle();
}
