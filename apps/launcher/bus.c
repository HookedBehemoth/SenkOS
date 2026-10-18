#include "bus.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "util.h"
#include "wake.h"
#include "win.h"

static bus_conn conn;
static bool up;
static uint64_t tried_at;

static struct {
  str name;
  void (*connected)(bus_conn *bus);
  void (*owned)(bus_conn *bus, bool owned);
  bus_call has; /* NameHasOwner */
} watches[4];
static size_t watch_count;

static STRBUF(160) refused;

void bus_tell(const bus_error *error, str what) {
  if (error == NULL) {
    win_wake(UI_DONE, 0);
    return;
  }
  const str why = error->message.n > 0 ? error->message : error->name;
  str_fput(stderr, S("launcher: "), what, S(": "), why, S("\n"));
  sb_set(refused, what, S(": "), why);
  win_wake(UI_REFUSED, 0);
}

str bus_refused(void) { return sb_str(refused); }

void bus_said_done(bus_call *call, const bus_error *error) {
  bus_tell(error, bus_call_owner(call, bus_said, call)->what);
}

void bus_watch(str name, void (*connected)(bus_conn *bus), void (*owned)(bus_conn *bus, bool owned)) {
  if (watch_count < COUNTOF(watches))
    watches[watch_count++] = (typeof(watches[0])){.name = name, .connected = connected, .owned = owned};
}

#define DBUS "org.freedesktop.DBus"

static void owner_changed(bus_match *match, const bus_msg *m) {
  (void)match;
  bus_rd r = m->body;
  const str name = bus_rd_str(&r), before = bus_rd_str(&r), now = bus_rd_str(&r);
  (void)before;
  for (size_t i = 0; i < watch_count && !r.bad && str_eq(m->sig, S("sss")); i++)
    if (str_eq(watches[i].name, name))
      watches[i].owned(&conn, now.n > 0);
}
static bus_match owner_match = {.iface = S(DBUS), .member = S("NameOwnerChanged"), .fn = owner_changed};

static void has_owner(bus_call *call, const bus_msg *m) {
  bus_error buf;
  bus_rd r = m->body;
  const bool has = bus_rd_bool(&r);
  if (bus_msg_error(m, &buf) == NULL && str_eq(m->sig, S("b")) && !r.bad)
    bus_call_owner(call, typeof(watches[0]), has)->owned(&conn, has);
}

static bool bus_open_system(void) {
  int r = bus_open(&conn, BUS_SYSTEM);
  for (size_t i = 0; i < watch_count && r >= 0; i++) {
    STRBUF(256) match;
    sb_set(match, S("type='signal',sender='" DBUS "',interface='" DBUS "',member='NameOwnerChanged',arg0='"),
           watches[i].name, S("'"));
    r = bus_add_match(&conn, &owner_match, sb_str(match));
    if (r >= 0) {
      bus_wr_str(bus_call_begin(&conn, S(DBUS), S("/org/freedesktop/DBus"), S(DBUS), S("NameHasOwner"), "s", true),
                 watches[i].name);
      r = bus_call_end(&conn, &watches[i].has, has_owner, NULL);
    }
    if (r >= 0 && watches[i].connected != NULL)
      watches[i].connected(&conn);
  }
  if (r < 0)
    bus_close(&conn);
  return r >= 0;
}

bus_conn *bus_get(void) {
  if (!up && ms_now() - tried_at >= 1000) {
    tried_at = ms_now();
    up = bus_open_system();
  }
  return up ? &conn : NULL;
}

struct pollfd bus_pollfd(int *timeout_ms) {
  if (bus_get() == NULL) {
    const int ms = (int)(1000 - MIN(1000, ms_now() - tried_at));
    if (*timeout_ms < 0 || ms < *timeout_ms)
      *timeout_ms = ms;
    return (struct pollfd){.fd = -1};
  }
  return (struct pollfd){.fd = bus_fd(&conn), .events = bus_events(&conn)};
}

void bus_dispatch(short revents) {
  if (!up || revents == 0)
    return;
  const int r = bus_process(&conn);
  if (r < 0) {
    fprintf(stderr, "launcher: system bus: %s\n", strerror(-r));
    /* Calls from what fails meanwhile fail too: it's still the bus, closed */
    tried_at = ms_now();
    bus_close(&conn);
    up = false;
    for (size_t i = 0; i < watch_count; i++)
      watches[i].owned(&conn, false);
  }
}

/* One object of an a{oa{sa{sv}}} (GetManagedObjects), or InterfacesAdded's oa{sa{sv}}: each
 * interface's a{sv} to `added`, then past it */
static void object(bus_rd *r, bus_object_added *added) {
  const str path = bus_rd_str(r);
  const uint8_t *end = bus_rd_array(r, 8);
  while (bus_rd_more(r, end)) {
    bus_rd_align(r, 8);
    const str iface = bus_rd_str(r);
    bus_rd props = *r;
    bus_rd_skip_to(r, bus_rd_array(r, 8));
    if (!r->bad)
      added(path, iface, &props);
  }
}

static void objects(bus_call *call, const bus_msg *m) {
  bus_error buf;
  bus_rd r = m->body;
  if (bus_msg_error(m, &buf) != NULL || !str_eq(m->sig, S("a{oa{sa{sv}}}")))
    return;
  const uint8_t *end = bus_rd_array(&r, 8);
  while (bus_rd_more(&r, end)) {
    bus_rd_align(&r, 8);
    object(&r, (bus_object_added *)call->done);
  }
}

int bus_get_objects(bus_conn *b, str service, bus_object_added *added, bus_call *call) {
  bus_call_begin(b, service, S("/"), S("org.freedesktop.DBus.ObjectManager"), S("GetManagedObjects"), "", true);
  return bus_call_end(b, call, objects, (void (*)(void))added);
}

static void interfaces_added(bus_match *match, const bus_msg *m) {
  bus_rd r = m->body;
  if (str_eq(m->sig, S("oa{sa{sv}}")))
    object(&r, (bus_object_added *)match->user);
}

static void interfaces_removed(bus_match *match, const bus_msg *m) {
  bus_rd r = m->body;
  if (!str_eq(m->sig, S("oas")))
    return;
  const str path = bus_rd_str(&r);
  const uint8_t *end = bus_rd_array(&r, 4);
  while (bus_rd_more(&r, end)) {
    const str iface = bus_rd_str(&r);
    if (!r.bad)
      ((bus_object_removed *)match->user)(path, iface);
  }
}

#define OBJECTS "org.freedesktop.DBus.ObjectManager"
static bus_match added_match = {.iface = S(OBJECTS), .member = S("InterfacesAdded"), .fn = interfaces_added},
                 removed_match = {.iface = S(OBJECTS), .member = S("InterfacesRemoved"), .fn = interfaces_removed};

int bus_watch_objects(bus_conn *b, str service, bus_object_added *added, bus_object_removed *removed) {
  STRBUF(192) match;
  added_match.user = (void (*)(void))added;
  removed_match.user = (void (*)(void))removed;
  sb_set(match, S("type='signal',sender='"), service, S("',interface='" OBJECTS "',member='InterfacesAdded'"));
  int r = bus_add_match(b, &added_match, sb_str(match));
  sb_set(match, S("type='signal',sender='"), service, S("',interface='" OBJECTS "',member='InterfacesRemoved'"));
  if (r >= 0)
    r = bus_add_match(b, &removed_match, sb_str(match));
  return r;
}
