/*
 * The system bus, in the UI's loop: bus_pollfd before win_wait, bus_dispatch after. Replies and
 * signals run from bus_dispatch. Connected when first needed, again a second after it went.
 */
#pragma once

#include <poll.h>
#include <stdbool.h>

#include "conn.h"
#include "str.h"

/* NULL while not connected */
bus_conn *bus_get(void);
/* For win_wait: the bus' fd (-1 without one); *timeout_ms lowered to when it's tried again */
struct pollfd bus_pollfd(int *timeout_ms);
/* After it: when its fd had events, or that time came */
void bus_dispatch(short revents);

/* Before the first bus_get: `connected` on each new connection (for its matches and objects),
 * then `owned` once and whenever `name` gets an owner or loses it */
void bus_watch(const char *name, void (*connected)(bus_conn *bus), void (*owned)(bus_conn *bus, bool owned));

/* org.freedesktop.DBus.ObjectManager: each object's interface with its a{sv} to read (e.g.
 * NAME_props_apply) */
typedef void bus_object_added(const char *path, const char *iface, bus_rd *props);
typedef void bus_object_removed(const char *path, const char *iface);
/* GetManagedObjects', pending in *call */
int bus_get_objects(bus_conn *bus, const char *service, bus_object_added *added, bus_call *call);
/* InterfacesAdded and InterfacesRemoved: one service's, the one program's */
int bus_watch_objects(bus_conn *bus, const char *service, bus_object_added *added, bus_object_removed *removed);

/* What came of it, told: UI_DONE, or UI_REFUSED with bus_refused saying "WHAT: WHY" */
void bus_tell(const bus_error *error, const char *what);
str bus_refused(void);
/* A call that tells (bus_said_done its done, for a method without OUT) */
typedef struct {
  bus_call call;
  const char *what;
} bus_said;
void bus_said_done(bus_call *call, const bus_error *error);
