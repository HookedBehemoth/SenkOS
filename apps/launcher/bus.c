#include "bus.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "util.h"
#include "wake.h"
#include "win.h"

static sd_bus *bus;
static uint64_t tried_at;
/* When it wants processing whatever its fd says (microseconds, CLOCK_MONOTONIC): a call timing
 * out, or messages it read already; UINT64_MAX: never */
static uint64_t due = UINT64_MAX;

static uint64_t now_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}

static struct {
  const char *name;
  void (*connected)(sd_bus *bus);
  void (*owned)(sd_bus *bus, bool owned);
} watches[4];
static size_t watch_count;

static STRBUF(160) refused;

void bus_done(const sd_bus_error *error, void *userdata) {
  if (error == NULL) {
    win_wake(UI_DONE, 0);
    return;
  }
  const str what = str_c(userdata), why = str_c(error->message != NULL ? error->message : error->name);
  str_fput(stderr, S("launcher: "), what, S(": "), why, S("\n"));
  sb_set(refused, what, S(": "), why);
  win_wake(UI_REFUSED, 0);
}

str bus_refused(void) { return sb_str(refused); }

void bus_watch(const char *name, void (*connected)(sd_bus *bus), void (*owned)(sd_bus *bus, bool owned)) {
  if (watch_count < COUNTOF(watches))
    watches[watch_count++] = (typeof(watches[0])){name, connected, owned};
}

static int owner_changed(sd_bus_message *m, void *userdata, sd_bus_error *error) {
  (void)error;
  const char *name, *before, *now;
  if (sd_bus_message_read(m, "sss", &name, &before, &now) >= 0)
    watches[(intptr_t)userdata].owned(bus, now[0] != '\0');
  return 0;
}

static int has_owner(sd_bus_message *m, void *userdata, sd_bus_error *error) {
  (void)error;
  int has = 0;
  if (sd_bus_message_read(m, "b", &has) >= 0)
    watches[(intptr_t)userdata].owned(bus, has);
  return 0;
}

static sd_bus *bus_open(void) {
  sd_bus *b = NULL;
  int r = sd_bus_open_system(&b);
  for (size_t i = 0; i < watch_count && r >= 0; i++) {
    char match[256];
    snprintf(match, sizeof(match),
             "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',"
             "member='NameOwnerChanged',arg0='%s'",
             watches[i].name);
    r = sd_bus_add_match(b, NULL, match, owner_changed, (void *)(intptr_t)i);
    if (r >= 0)
      r = sd_bus_call_method_async(b, NULL, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                                   "NameHasOwner", has_owner, (void *)(intptr_t)i, "s", watches[i].name);
    if (r >= 0 && watches[i].connected != NULL)
      watches[i].connected(b);
  }
  if (r < 0)
    return sd_bus_flush_close_unref(b);
  return b;
}

sd_bus *bus_get(void) {
  if (bus == NULL && ms_now() - tried_at >= 1000) {
    tried_at = ms_now();
    bus = bus_open();
  }
  return bus;
}

struct pollfd bus_pollfd(int *timeout_ms) {
  int ms = -1;
  due = UINT64_MAX;
  if (bus_get() == NULL)
    ms = (int)(1000 - MIN(1000, ms_now() - tried_at));
  else if (sd_bus_get_timeout(bus, &due) >= 0 && due != UINT64_MAX) {
    /* Now when it has messages read and waiting: it asks for no POLLIN then */
    const uint64_t now = now_us();
    ms = due > now ? (int)((due - now + 999) / 1000) : 0;
  } else
    due = UINT64_MAX;
  if (ms >= 0 && (*timeout_ms < 0 || ms < *timeout_ms))
    *timeout_ms = ms;
  return (struct pollfd){.fd = bus != NULL ? sd_bus_get_fd(bus) : -1, .events = bus != NULL ? sd_bus_get_events(bus) : 0};
}

void bus_dispatch(short revents) {
  if (bus == NULL || (revents == 0 && now_us() < due))
    return;
  int r;
  while ((r = sd_bus_process(bus, NULL)) > 0)
    ;
  if (r < 0) {
    fprintf(stderr, "launcher: system bus: %s\n", strerror(-r));
    for (size_t i = 0; i < watch_count; i++)
      watches[i].owned(bus, false);
    bus = sd_bus_flush_close_unref(bus);
  }
}

/* An a{oa{sa{sv}}} (GetManagedObjects) or, from InterfacesAdded, one oa{sa{sv}} */
static int object(sd_bus_message *m, bus_object_added *added) {
  const char *path;
  int r = sd_bus_message_read(m, "o", &path);
  if (r >= 0)
    r = sd_bus_message_enter_container(m, 'a', "{sa{sv}}");
  while (r >= 0 && (r = sd_bus_message_enter_container(m, 'e', "sa{sv}")) > 0) {
    const char *iface;
    r = sd_bus_message_read(m, "s", &iface);
    if (r >= 0 && !added(path, iface, m))
      r = sd_bus_message_skip(m, "a{sv}");
    if (r >= 0)
      r = sd_bus_message_exit_container(m);
  }
  return r < 0 ? r : sd_bus_message_exit_container(m);
}

static int objects(sd_bus_message *m, void *userdata, sd_bus_error *error) {
  (void)error;
  if (sd_bus_message_is_method_error(m, NULL))
    return 0;
  int r = sd_bus_message_enter_container(m, 'a', "{oa{sa{sv}}}");
  while (r >= 0 && (r = sd_bus_message_enter_container(m, 'e', "oa{sa{sv}}")) > 0) {
    r = object(m, (bus_object_added *)userdata);
    if (r >= 0)
      r = sd_bus_message_exit_container(m);
  }
  return 0;
}

int bus_get_objects(sd_bus *b, const char *service, bus_object_added *added) {
  return sd_bus_call_method_async(b, NULL, service, "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects",
                                  objects, (void *)added, NULL);
}

static int interfaces_added(sd_bus_message *m, void *userdata, sd_bus_error *error) {
  (void)error;
  object(m, (bus_object_added *)userdata);
  return 0;
}

static int interfaces_removed(sd_bus_message *m, void *userdata, sd_bus_error *error) {
  (void)error;
  const char *path, *iface;
  int r = sd_bus_message_read(m, "o", &path);
  if (r >= 0)
    r = sd_bus_message_enter_container(m, 'a', "s");
  while (r >= 0 && (r = sd_bus_message_read(m, "s", &iface)) > 0)
    ((bus_object_removed *)userdata)(path, iface);
  return 0;
}

int bus_watch_objects(sd_bus *b, const char *service, bus_object_added *added, bus_object_removed *removed) {
  char match[192];
  snprintf(match, sizeof(match),
           "type='signal',sender='%s',interface='org.freedesktop.DBus.ObjectManager',member='InterfacesAdded'",
           service);
  int r = sd_bus_add_match(b, NULL, match, interfaces_added, (void *)added);
  snprintf(match, sizeof(match),
           "type='signal',sender='%s',interface='org.freedesktop.DBus.ObjectManager',member='InterfacesRemoved'",
           service);
  if (r >= 0)
    r = sd_bus_add_match(b, NULL, match, interfaces_removed, (void *)removed);
  return r;
}
