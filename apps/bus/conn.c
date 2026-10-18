#include "conn.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define DBUS "org.freedesktop.DBus"
#define DBUS_PATH "/org/freedesktop/DBus"
/* The most one message we write may take; what we read may be up to the spec's 128 MiB */
enum { WRITE_MAX = 64 << 10, READ_MAX = 128 << 20 };

const bus_error bus_malformed = {S(BUS_ERROR_INVALID_ARGS), S("an answer of other types")};

static bool grow(uint8_t **buf, size_t *cap, size_t want) {
  if (*cap >= want)
    return true;
  size_t n = *cap > 0 ? *cap : 4096;
  while (n < want)
    n *= 2;
  uint8_t *p = realloc(*buf, n);
  if (p == NULL)
    return false;
  *buf = p;
  *cap = n;
  return true;
}

static int flush(bus_conn *c) {
  size_t at = 0;
  while (at < c->tx_len) {
    const ssize_t n = send(c->fd, c->tx + at, c->tx_len - at, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (n < 0 && errno == EINTR)
      continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      break;
    if (n < 0)
      return -errno;
    at += (size_t)n;
  }
  memmove(c->tx, c->tx + at, c->tx_len - at);
  c->tx_len -= at;
  return 0;
}

/* Left out when empty: no name, path or signature is */
static void field(bus_wr *w, uint8_t code, char sig, str text) {
  if (text.n == 0)
    return;
  bus_wr_align(w, 8);
  bus_wr_u8(w, code);
  bus_wr_sig(w, (char[]){sig, '\0'});
  if (sig == 'g')
    bus_wr_sig(w, text.p);
  else
    bus_wr_str(w, text);
}

/* A message's header written, the writer at its body */
static bus_wr *begin(bus_conn *c, uint8_t type, uint8_t flags, str dest, str path, str iface, str member, str error,
                     uint32_t reply_serial, const char *sig) {
  bus_wr *w = &c->w;
  if (c->fd < 0 || !grow(&c->tx, &c->tx_cap, c->tx_len + WRITE_MAX)) {
    *w = (bus_wr){.bad = true};
    return w;
  }
  *w = (bus_wr){c->tx + c->tx_len, c->tx + c->tx_len + WRITE_MAX, c->tx + c->tx_len, false};
  if (++c->serial == 0)
    c->serial = 1;
  bus_wr_u8(w, 'l');
  bus_wr_u8(w, type);
  bus_wr_u8(w, flags);
  bus_wr_u8(w, 1);
  bus_wr_u32(w, 0); /* the body's length, at the end */
  bus_wr_u32(w, c->serial);
  const bus_wr_arr fields = bus_wr_array(w, 8);
  field(w, 1, 'o', path);
  field(w, 2, 's', iface);
  field(w, 3, 's', member);
  field(w, 4, 's', error);
  if (reply_serial != 0) {
    bus_wr_align(w, 8);
    bus_wr_u8(w, 5);
    bus_wr_sig(w, "u");
    bus_wr_u32(w, reply_serial);
  }
  field(w, 6, 's', dest);
  field(w, 8, 'g', str_c(sig));
  bus_wr_array_end(w, fields);
  bus_wr_align(w, 8);
  return w;
}

/* Queued and sent as far as the socket takes it: its serial, or a negative errno */
static int64_t commit(bus_conn *c) {
  bus_wr *w = &c->w;
  if (w->bad)
    return c->fd < 0 ? -ENOTCONN : -EMSGSIZE;
  uint32_t fields, serial;
  memcpy(&fields, w->base + 12, 4);
  memcpy(&serial, w->base + 8, 4);
  const uint32_t body = (uint32_t)(w->p - (w->base + 16 + ((fields + 7) & ~7u)));
  memcpy(w->base + 4, &body, 4);
  c->tx_len += (size_t)(w->p - w->base);
  w->bad = true;
  const int r = flush(c);
  return r < 0 ? (int64_t)r : (int64_t)serial;
}

static void unlink_call(bus_call *call) {
  *call->prev = call->next;
  if (call->next != NULL)
    call->next->prev = call->prev;
  call->prev = NULL;
}

void bus_call_cancel(bus_call *call) {
  if (bus_call_pending(call))
    unlink_call(call);
}

bus_wr *bus_call_begin(bus_conn *c, str dest, str path, str iface, str member, const char *sig, bool reply) {
  return begin(c, BUS_METHOD_CALL, reply ? 0 : BUS_NO_REPLY_EXPECTED, dest, path, iface, member, S(""), 0, sig);
}

int bus_call_end(bus_conn *c, bus_call *call, void (*replied)(bus_call *, const bus_msg *), void (*done)(void)) {
  if (call != NULL)
    bus_call_cancel(call);
  const int64_t serial = commit(c);
  if (serial < 0)
    return (int)serial;
  if (call != NULL) {
    *call = (bus_call){.next = c->calls, .prev = &c->calls, .serial = (uint32_t)serial, .replied = replied, .done = done};
    if (c->calls != NULL)
      c->calls->prev = &call->next;
    c->calls = call;
  }
  return 0;
}

const bus_error *bus_msg_error(const bus_msg *m, bus_error *buf) {
  if (m->type != BUS_ERROR)
    return NULL;
  bus_rd r = m->body;
  *buf = (bus_error){m->error, m->sig.p[0] == 's' ? bus_rd_str(&r) : S("")};
  return buf;
}

int bus_add_match(bus_conn *c, bus_match *match, str rule) {
  if (!match->on) {
    match->on = true;
    match->next = c->matches;
    c->matches = match;
  }
  bus_wr *w = bus_call_begin(c, S(DBUS), S(DBUS_PATH), S(DBUS), S("AddMatch"), "s", false);
  bus_wr_str(w, rule);
  const int64_t r = commit(c);
  return r < 0 ? (int)r : 0;
}

void bus_add_object(bus_conn *c, bus_object *object) {
  if (!object->on) {
    object->on = true;
    object->next = c->objects;
    c->objects = object;
  }
}

int bus_request_name(bus_conn *c, str name) {
  bus_wr *w = bus_call_begin(c, S(DBUS), S(DBUS_PATH), S(DBUS), S("RequestName"), "su", false);
  bus_wr_str(w, name);
  bus_wr_u32(w, 0);
  const int64_t r = commit(c);
  return r < 0 ? (int)r : 0;
}

bus_wr *bus_reply_begin(bus_req *req, const char *sig) {
  return begin(req->conn, BUS_METHOD_RETURN, BUS_NO_REPLY_EXPECTED, sb_str(req->sender), S(""), S(""), S(""), S(""),
               req->serial, sig);
}

int bus_reply_end(bus_req *req) {
  if (req->no_reply) {
    req->conn->w.bad = true;
    return 0;
  }
  const int64_t r = commit(req->conn);
  return r < 0 ? (int)r : 0;
}

bus_wr *bus_signal_begin(bus_conn *c, str path, str iface, str member, const char *sig) {
  return begin(c, BUS_SIGNAL, BUS_NO_REPLY_EXPECTED, S(""), path, iface, member, S(""), 0, sig);
}

int bus_signal_end(bus_conn *c) {
  const int64_t r = commit(c);
  return r < 0 ? (int)r : 0;
}

int bus_reply(bus_req *req) {
  bus_reply_begin(req, "");
  return bus_reply_end(req);
}

int bus_reply_error(bus_req *req, str name, str message) {
  bus_wr *w = begin(req->conn, BUS_ERROR, BUS_NO_REPLY_EXPECTED, sb_str(req->sender), S(""), S(""), S(""), name,
                    req->serial, "s");
  bus_wr_str(w, message);
  return bus_reply_end(req);
}

int bus_open(bus_conn *c, const char *path) {
  struct sockaddr_un addr = {.sun_family = AF_UNIX};
  if (strlen(path) >= sizeof(addr.sun_path))
    return -ENAMETOOLONG;
  strcpy(addr.sun_path, path);
  *c = (bus_conn){.fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0), .rx = c->rx, .tx = c->tx,
                  .rx_cap = c->rx_cap, .tx_cap = c->tx_cap};
  if (c->fd < 0)
    return -errno;
  if (connect(c->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
      fcntl(c->fd, F_SETFL, fcntl(c->fd, F_GETFL) | O_NONBLOCK) < 0) {
    const int e = -errno;
    close(c->fd);
    c->fd = -1;
    return e;
  }
  /* Credentials, BEGIN and Hello at once: the bus answers OK first */
  char uid[16], auth[64];
  const int n = snprintf(uid, sizeof(uid), "%u", (unsigned)getuid());
  size_t len = (size_t)snprintf(auth, sizeof(auth), "%cAUTH EXTERNAL ", '\0');
  for (int i = 0; i < n; i++)
    len += (size_t)snprintf(auth + len, sizeof(auth) - len, "%02x", (unsigned char)uid[i]);
  len += (size_t)snprintf(auth + len, sizeof(auth) - len, "\r\nBEGIN\r\n");
  if (!grow(&c->tx, &c->tx_cap, len)) {
    bus_close(c);
    return -ENOMEM;
  }
  memcpy(c->tx, auth, len);
  c->tx_len = len;
  bus_call_begin(c, S(DBUS), S(DBUS_PATH), S(DBUS), S("Hello"), "", true);
  const int64_t r = commit(c);
  if (r < 0) {
    bus_close(c);
    return (int)r;
  }
  return 0;
}

void bus_close(bus_conn *c) {
  if (c->fd >= 0)
    close(c->fd);
  c->fd = -1;
  c->rx_len = c->tx_len = 0;
  for (bus_match *m = c->matches; m != NULL; m = m->next)
    m->on = false;
  for (bus_object *o = c->objects; o != NULL; o = o->next)
    o->on = false;
  c->matches = NULL;
  c->objects = NULL;
  /* What's pending gets its error: the bus would have said NoReply */
  uint8_t body[64] __attribute__((aligned(8)));
  bus_wr w = {body, body + sizeof(body), body, false};
  bus_wr_str(&w, S("the connection closed"));
  const bus_msg gone = {.type = BUS_ERROR, .path = S(""), .iface = S(""), .member = S(""), .dest = S(""),
                        .sender = S(""), .error = S("org.freedesktop.DBus.Error.NoReply"), .sig = S("s"),
                        .body = {body, w.p, body, false}};
  while (c->calls != NULL) {
    bus_call *call = c->calls;
    unlink_call(call);
    call->replied(call, &gone);
  }
}

short bus_events(const bus_conn *c) { return POLLIN | (c->tx_len > 0 ? POLLOUT : 0); }

static void dispatch(bus_conn *c, const uint8_t *h, size_t len) {
  bus_msg m = {.type = h[1], .flags = h[2], .path = S(""), .iface = S(""), .member = S(""), .error = S(""),
               .dest = S(""), .sender = S(""), .sig = S("")};
  str *text[8] = {[1] = &m.path, [2] = &m.iface, [3] = &m.member, [4] = &m.error, [6] = &m.dest,
                          [7] = &m.sender};
  bus_rd r = {h + 4, h + len, h, false};
  const uint32_t body = bus_rd_u32(&r);
  m.serial = bus_rd_u32(&r);
  const uint8_t *end = bus_rd_array(&r, 8);
  while (bus_rd_more(&r, end)) {
    bus_rd_align(&r, 8);
    const uint8_t code = bus_rd_u8(&r);
    const str sig = bus_rd_sig(&r);
    if (code < 8 && text[code] != NULL && (str_eq(sig, S("s")) || str_eq(sig, S("o"))))
      *text[code] = bus_rd_str(&r);
    else if (code == 5 && str_eq(sig, S("u")))
      m.reply_serial = bus_rd_u32(&r);
    else if (code == 8 && str_eq(sig, S("g")))
      m.sig = bus_rd_sig(&r);
    else
      bus_skip(&r, sig);
  }
  bus_rd_align(&r, 8);
  if (r.bad || (size_t)(r.end - r.p) != body)
    return;
  m.body = (bus_rd){r.p, r.end, h, false};

  if (m.type == BUS_METHOD_RETURN || m.type == BUS_ERROR) {
    for (bus_call *call = c->calls; call != NULL; call = call->next)
      if (call->serial == m.reply_serial) {
        unlink_call(call);
        call->replied(call, &m);
        return;
      }
  } else if (m.type == BUS_SIGNAL) {
    for (bus_match *match = c->matches, *next; match != NULL; match = next) {
      next = match->next;
      if (str_eq(match->member, m.member) && str_eq(match->iface, m.iface))
        match->fn(match, &m);
    }
  } else if (m.type == BUS_METHOD_CALL) {
    bus_req req = {.conn = c, .serial = m.serial, .no_reply = m.flags & BUS_NO_REPLY_EXPECTED};
    if (!sb_set(req.sender, m.sender))
      return;
    for (bus_object *o = c->objects; o != NULL; o = o->next)
      if (str_eq(o->path, m.path) && (m.iface.n == 0 || str_eq(o->iface, m.iface))) {
        const int e = o->dispatch(&req, &m, o->userdata);
        if (e < 0)
          bus_reply_error(&req, S("org.freedesktop.DBus.Error.Failed"), str_c(strerror(-e)));
        return;
      }
    bus_reply_error(&req, S("org.freedesktop.DBus.Error.UnknownMethod"), S("no such object or interface"));
  }
}

/* Each whole message read; the rest kept for the next read */
static int dispatch_all(bus_conn *c) {
  size_t at = 0;
  if (!c->authed) {
    const uint8_t *nl = memmem(c->rx, c->rx_len, "\r\n", 2);
    if (nl == NULL)
      return c->rx_len > 512 ? -EPROTO : 0;
    if (c->rx_len < 3 || memcmp(c->rx, "OK ", 3) != 0)
      return -EACCES;
    at = (size_t)(nl - c->rx) + 2;
    c->authed = true;
  }
  while (c->rx_len - at >= 16) {
    const uint8_t *h = c->rx + at;
    uint32_t body, fields;
    memcpy(&body, h + 4, 4);
    memcpy(&fields, h + 12, 4);
    const size_t len = 16 + (((size_t)fields + 7) & ~(size_t)7) + body;
    if (h[0] != 'l' || h[3] != 1)
      return -EPROTO;
    if (len > READ_MAX)
      return -EMSGSIZE;
    if (c->rx_len - at < len)
      break;
    dispatch(c, h, len);
    at += len;
    if (c->fd < 0)
      return -ECONNRESET;
  }
  memmove(c->rx, c->rx + at, c->rx_len - at);
  c->rx_len -= at;
  return 0;
}

int bus_process(bus_conn *c) {
  if (c->fd < 0)
    return -ENOTCONN;
  int r = flush(c);
  while (r >= 0) {
    if (!grow(&c->rx, &c->rx_cap, c->rx_len + 4096))
      return -ENOMEM;
    const ssize_t n = recv(c->fd, c->rx + c->rx_len, c->rx_cap - c->rx_len, MSG_DONTWAIT);
    if (n == 0)
      return -ECONNRESET;
    if (n < 0 && errno == EINTR)
      continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      return flush(c);
    if (n < 0)
      return -errno;
    c->rx_len += (size_t)n;
    r = dispatch_all(c);
  }
  return r;
}
