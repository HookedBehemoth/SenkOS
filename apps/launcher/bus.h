/*
 * The system bus, in the UI's loop: bus_pollfd before win_wait, bus_dispatch after. Replies and
 * signals run from bus_dispatch. Connected when first needed, again a second after it went.
 */
#pragma once

#include <basu/sd-bus.h>
#include <poll.h>
#include <stdbool.h>

#include "str.h"

/* NULL while not connected */
sd_bus *bus_get(void);
/* For win_wait: the bus' fd (-1 without one); *timeout_ms lowered to when it wants processing */
struct pollfd bus_pollfd(int *timeout_ms);
/* After it: when its fd had events, or that time came */
void bus_dispatch(short revents);

/* Before the first bus_get: `connected` on each new connection (for its matches and objects),
 * then `owned` once and whenever `name` gets an owner or loses it */
void bus_watch(const char *name, void (*connected)(sd_bus *bus), void (*owned)(sd_bus *bus, bool owned));

/* org.freedesktop.DBus.ObjectManager: each object's interface with its a{sv}, for `added` to read
 * (returning true), or skipped */
typedef bool bus_object_added(const char *path, const char *iface, sd_bus_message *props);
typedef void bus_object_removed(const char *path, const char *iface);
/* GetManagedObjects' */
int bus_get_objects(sd_bus *bus, const char *service, bus_object_added *added);
/* InterfacesAdded and InterfacesRemoved */
int bus_watch_objects(sd_bus *bus, const char *service, bus_object_added *added, bus_object_removed *removed);

/* A done that tells: UI_DONE, or UI_REFUSED with bus_refused saying "WHAT: WHY", `userdata` the
 * WHAT */
void bus_done(const sd_bus_error *error, void *userdata);
str bus_refused(void);
