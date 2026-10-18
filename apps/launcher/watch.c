#include "watch.h"

#include <fcntl.h>
#include <linux/netlink.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include "bluetooth.h"
#include "bus.h"
#include "media.h"
#include "wifi.h"

#define TIME_DIR "/data/OS/time"

enum { MINUTE, FILES, MOUNTS, UEVENTS, BUS, SESSION };
struct pollfd watch_fds[WATCH_FDS];
static int run_wd = -1, time_wd = -1, os_wd = -1;

static void arm_minute(void) {
  /* Not time(): it's the coarse clock, a tick behind, and would arm the minute just past */
  struct timespec now;
  clock_gettime(CLOCK_REALTIME, &now);
  const struct itimerspec at = {.it_value = {.tv_sec = (now.tv_sec / 60 + 1) * 60}};
  timerfd_settime(watch_fds[MINUTE].fd, TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET, &at, NULL);
}

/* `clock` makes it on first use: until then /data/OS, for it to come */
static void watch_time(void) {
  enum { CHANGES = IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE | IN_ATTRIB };
  time_wd = inotify_add_watch(watch_fds[FILES].fd, TIME_DIR, CHANGES);
  if (time_wd < 0 && os_wd < 0)
    os_wd = inotify_add_watch(watch_fds[FILES].fd, "/data/OS", IN_CREATE | IN_MOVED_TO);
}

void watch_init(void) {
  for (int i = 0; i < WATCH_FDS; i++)
    watch_fds[i] = (struct pollfd){.fd = -1, .events = POLLIN};
  watch_fds[MINUTE].fd = timerfd_create(CLOCK_REALTIME, TFD_CLOEXEC | TFD_NONBLOCK);
  arm_minute();
  watch_fds[FILES].fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
  run_wd = inotify_add_watch(watch_fds[FILES].fd, "/run", IN_CLOSE_WRITE | IN_MOVED_TO);
  watch_time();
  /* Changed: POLLPRI | POLLERR, cleared by the poll itself */
  watch_fds[MOUNTS] = (struct pollfd){.fd = open("/proc/self/mountinfo", O_RDONLY | O_CLOEXEC), .events = POLLPRI};
  const int ue = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_KOBJECT_UEVENT);
  const struct sockaddr_nl kernel = {.nl_family = AF_NETLINK, .nl_groups = 1};
  if (ue >= 0 && bind(ue, (const struct sockaddr *)&kernel, sizeof(kernel)) != 0)
    close(ue);
  else
    watch_fds[UEVENTS].fd = ue;
}

void watch_prepare(int *timeout_ms) {
  watch_fds[BUS] = bus_pollfd(timeout_ms);
  watch_fds[SESSION] = bus_session_pollfd(timeout_ms);
}

static unsigned files(int fd) {
  char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
  unsigned changed = 0;
  for (ssize_t n; (n = read(fd, buf, sizeof(buf))) > 0;)
    for (ssize_t at = 0; at < n;) {
      const struct inotify_event *e = (const struct inotify_event *)(buf + at);
      const char *name = e->len > 0 ? e->name : "";
      if (e->wd == run_wd && strcmp(name, "usb") == 0)
        changed |= WATCH_USB;
      else if (e->wd == run_wd && strcmp(name, "wifi") == 0)
        changed |= WATCH_WIFI;
      else if (e->wd == time_wd)
        changed |= WATCH_TIME;
      else if (e->wd == os_wd && strcmp(name, "time") == 0 && time_wd < 0) {
        watch_time();
        changed |= WATCH_TIME;
      }
      at += (ssize_t)sizeof(*e) + e->len;
    }
  return changed;
}

/* "change@/devices/.../power_supply/battery", then KEY=value lines */
static unsigned uevents(int fd) {
  char buf[2048];
  unsigned changed = 0;
  for (ssize_t n; (n = recv(fd, buf, sizeof(buf) - 1, 0)) > 0;) {
    buf[n] = '\0';
    if (strstr(buf, "/power_supply/") != NULL)
      changed |= WATCH_POWER;
  }
  return changed;
}

unsigned watch_changed(void) {
  unsigned changed = 0;
  if (watch_fds[MINUTE].revents) {
    uint64_t expired;
    /* Read or ECANCELED (the clock was set): either way the next minute */
    const ssize_t n = read(watch_fds[MINUTE].fd, &expired, sizeof(expired));
    (void)n;
    arm_minute();
    /* The signal tells nothing by itself */
    wifi_poll_signal();
    changed |= WATCH_MINUTE;
  }
  if (watch_fds[FILES].revents)
    changed |= files(watch_fds[FILES].fd);
  if (watch_fds[MOUNTS].revents)
    changed |= WATCH_MOUNTS;
  if (watch_fds[UEVENTS].revents)
    changed |= uevents(watch_fds[UEVENTS].fd);
  bus_dispatch(watch_fds[BUS].revents);
  bus_session_dispatch(watch_fds[SESSION].revents);
  if (bt_changed())
    changed |= WATCH_BLUETOOTH;
  if (wifi_changed())
    changed |= WATCH_WIFI;
  if (media_changed())
    changed |= WATCH_MEDIA;
  for (int i = 0; i < WATCH_FDS; i++)
    watch_fds[i].revents = 0;
  return changed;
}
