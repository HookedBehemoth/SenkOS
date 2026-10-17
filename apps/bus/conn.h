/*
 * A D-Bus connection of our own (no sd-bus): a unix socket to the bus, in the caller's poll loop.
 * bus_open queues the authentication and Hello without waiting; poll bus_fd for bus_events, then
 * bus_process: it writes what's queued, reads what came and runs its callbacks. Nothing is
 * allocated per message: the buffers grow to the largest one.
 *
 * gen.py's code (NAME.h) is what calls, watches and serves: the functions below are its, and for
 * what it doesn't describe.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wire.h"

#define BUS_SYSTEM "/run/dbus/system_bus_socket"

enum { BUS_METHOD_CALL = 1, BUS_METHOD_RETURN, BUS_ERROR, BUS_SIGNAL };
enum { BUS_NO_REPLY_EXPECTED = 1 };

/* A message read, all of it pointing into the connection's buffer: valid while its callback runs */
typedef struct {
  uint8_t type, flags;
  uint32_t serial, reply_serial;
  /* "" when not there */
  const char *path, *iface, *member, *error, *dest, *sender, *sig;
  bus_rd body;
} bus_msg;

typedef struct {
  const char *name, *message;
} bus_error;

typedef struct bus_conn bus_conn;

/*
 * A pending call, kept in what it's about (a device, a network, a module's own): gen.py's
 * NAME_call_METHOD(..., call, done) fills it in, done(call, error, OUT...) runs once with the
 * answer, or with an error when the connection goes. bus_call_owner finds its owner. Calling again
 * with the same one cancels the first; bus_call_cancel when its owner goes, so done never runs on
 * what's gone. NULL in its place: no answer wanted.
 */
typedef struct bus_call bus_call;
struct bus_call {
  bus_call *next, **prev; /* prev: NULL while not pending */
  uint32_t serial;
  void (*replied)(bus_call *call, const bus_msg *reply); /* gen.py's: reads it, runs done */
  void (*done)(void);
};
static inline bool bus_call_pending(const bus_call *call) { return call->prev != NULL; }
void bus_call_cancel(bus_call *call);
#define bus_call_owner(call, type, member) ((type *)((char *)(call) - offsetof(type, member)))

/* Signals: each one whose interface and member are these (the bus sends those of its rules) */
typedef struct bus_match bus_match;
struct bus_match {
  bus_match *next;
  bool on;
  const char *iface, *member;
  void (*fn)(bus_match *match, const bus_msg *signal);
  void (*user)(void); /* gen.py's thunks: what to call */
};

/* A call to answer, copied to answer later */
typedef struct {
  bus_conn *conn;
  uint32_t serial;
  bool no_reply;
  char sender[48];
} bus_req;

/* An object's interface, served: dispatch reads a call and answers it (now or later). A negative
 * errno from it is answered as an error. */
typedef struct bus_object bus_object;
struct bus_object {
  bus_object *next;
  bool on;
  const char *path, *iface;
  int (*dispatch)(bus_req *req, const bus_msg *call, void *userdata);
  void *userdata;
};

struct bus_conn {
  int fd;
  bool authed;
  uint32_t serial;
  uint8_t *rx, *tx;
  size_t rx_len, rx_cap, tx_len, tx_cap;
  bus_wr w; /* the message being written */
  bus_call *calls;
  bus_match *matches;
  bus_object *objects;
};

/* Connects to the socket at `path` (BUS_SYSTEM); negative errno */
int bus_open(bus_conn *c, const char *path);
/* Pending calls get their error; matches and objects are let go (add them again on the next) */
void bus_close(bus_conn *c);
static inline int bus_fd(const bus_conn *c) { return c->fd; }
short bus_events(const bus_conn *c);
/* Negative errno: the connection is gone (bus_close it) */
int bus_process(bus_conn *c);

/* A method call to write: its body after it, then bus_call_end. `sig` is the body's. */
bus_wr *bus_call_begin(bus_conn *c, const char *dest, const char *path, const char *iface, const char *member,
                       const char *sig, bool reply);
int bus_call_end(bus_conn *c, bus_call *call, void (*replied)(bus_call *, const bus_msg *), void (*done)(void));
/* A reply's error, or NULL for a method return */
const bus_error *bus_msg_error(const bus_msg *reply, bus_error *buf);
extern const bus_error bus_malformed;

/* Its rule to the bus (AddMatch), and the match taking what matches it (once per connection) */
int bus_add_match(bus_conn *c, bus_match *match, const char *rule);
void bus_add_object(bus_conn *c, bus_object *object);
/* RequestName, not waiting for the answer */
int bus_request_name(bus_conn *c, const char *name);

/* Answers: a return with a body (bus_reply_begin, the body, bus_reply_end), an empty one, an error */
bus_wr *bus_reply_begin(bus_req *req, const char *sig);
int bus_reply_end(bus_req *req);
int bus_reply(bus_req *req);
int bus_reply_error(bus_req *req, const char *name, const char *message);

#define BUS_ERROR_INVALID_ARGS "org.freedesktop.DBus.Error.InvalidArgs"
