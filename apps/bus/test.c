/* The wire format offline, then calls, answers and a signal over the session bus (skipped
 * without one): sysd's interface served on one connection, called from another. */
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "properties.h"
#include "system1.h"
#include "wpa_bss.h"

static void offline(void) {
  uint8_t buf[512] __attribute__((aligned(8)));
  /* Off by 4 from 8: alignment is the message's, not the buffer's */
  bus_wr w = {buf + 4, buf + sizeof(buf), buf + 4, false};
  const bus_wr_arr a = bus_wr_array(&w, 8);
  bus_wr_kv(&w, "SSID", "ay");
  bus_wr_bytes(&w, "home", 4);
  bus_wr_kv(&w, "Unknown", "a{sv}"); /* skipped: its length */
  const bus_wr_arr inner = bus_wr_array(&w, 8);
  bus_wr_kv(&w, "x", "t");
  bus_wr_u64(&w, 7);
  bus_wr_array_end(&w, inner);
  bus_wr_kv(&w, "Signal", "n");
  bus_wr_i16(&w, -61);
  bus_wr_kv(&w, "Privacy", "b");
  bus_wr_bool(&w, true);
  bus_wr_kv(&w, "Tuple", "(sv)"); /* skipped: each part */
  bus_wr_align(&w, 8);
  bus_wr_str(&w, "a");
  bus_wr_sig(&w, "d");
  bus_wr_f64(&w, 1.5);
  bus_wr_kv(&w, "Privacy", "s"); /* a known key of another type: skipped */
  bus_wr_str(&w, "no");
  bus_wr_array_end(&w, a);
  assert(!w.bad);

  wpa_bss_props p = {0};
  uint32_t changed;
  bus_rd r = {buf + 4, w.p, buf + 4, false};
  assert(wpa_bss_props_apply(&p, &r, &changed) == 0 && r.p == w.p);
  assert(p.ssid_len == 4 && memcmp(p.ssid, "home", 4) == 0 && p.signal == -61 && p.privacy);
  assert(changed == (WPA_BSS_PROP_SSID | WPA_BSS_PROP_SIGNAL | WPA_BSS_PROP_PRIVACY));
  r = (bus_rd){buf + 4, w.p, buf + 4, false};
  assert(wpa_bss_props_apply(&p, &r, &changed) == 0 && changed == 0);

  /* Cut anywhere: bad, never past the end */
  for (const uint8_t *end = buf + 4; end < w.p; end++) {
    r = (bus_rd){buf + 4, end, buf + 4, false};
    wpa_bss_props q = {0};
    assert(wpa_bss_props_apply(&q, &r, NULL) < 0 && r.p <= end);
  }
  /* A writer too small: bad, nothing past its end */
  w = (bus_wr){buf, buf + 6, buf, false};
  bus_wr_str(&w, "toolong");
  assert(w.bad && w.p <= w.end);
}

static bus_conn server, client;
static int suspended, cpu, answered, owned;
static bus_req held;

int system1_suspend(bus_req *req, void *userdata) {
  (void)userdata;
  /* The first answered later */
  if (suspended++ == 0) {
    held = *req;
    return 0;
  }
  return bus_reply(req);
}
int system1_set_cpu_max(bus_req *req, void *userdata, uint32_t khz) {
  (void)userdata;
  cpu = (int)khz;
  return bus_reply(req);
}
int system1_set_usb(bus_req *req, void *userdata, const char *state) {
  (void)userdata;
  return strcmp(state, "on") == 0 ? bus_reply(req) : bus_reply_error(req, "org.senkos.System1.Error.Failed", state);
}
#define NO(name, ...)                                                                                                  \
  int system1_##name(bus_req *req, void *userdata __VA_OPT__(, ) __VA_ARGS__) {                                         \
    (void)userdata;                                                                                                    \
    return -ENOSYS;                                                                                                    \
  }
NO(reboot)
NO(power_off)
NO(launcher_up)
NO(set_gpu_freq, uint32_t hz)
NO(set_wifi, bool on)
NO(set_time_zone, const char *zone)
NO(set_ntp, bool on)
NO(set_time, int64_t seconds)
NO(set_ssh, bool on)
NO(set_ssh_everywhere, bool on)
NO(set_light_sleep, bool on)
NO(set_light_theme, bool on)
NO(set_power_on_ms, uint32_t ms)

static void ok(bus_call *call, const bus_error *error) {
  (void)call;
  assert(error == NULL);
  answered++;
}
static void refused(bus_call *call, const bus_error *error) {
  (void)call;
  assert(error != NULL && strcmp(error->name, "org.senkos.System1.Error.Failed") == 0 &&
         strcmp(error->message, "sideways") == 0);
  answered++;
}
static void failed(bus_call *call, const bus_error *error) {
  (void)call;
  assert(error != NULL && strcmp(error->name, "org.freedesktop.DBus.Error.Failed") == 0);
  answered++;
}
static void gone(bus_call *call, const bus_error *error) {
  (void)call;
  assert(error != NULL && strcmp(error->name, "org.freedesktop.DBus.Error.NoReply") == 0);
  answered++;
}
static void got_all(bus_call *call, const bus_error *error, bus_rd *props) {
  (void)call;
  assert(error == NULL);
  const uint8_t *end = bus_rd_array(props, 8);
  const char *key, *sig;
  uint32_t n;
  bool features = false;
  while (bus_rd_kv(props, end, &key, &n, &sig)) {
    features |= strcmp(key, "Features") == 0 && strcmp(sig, "as") == 0;
    bus_skip(props, sig);
  }
  assert(!props->bad && features);
  answered++;
}
static void got(bus_call *call, const bus_error *error, bus_rd *value) {
  (void)call;
  assert(error == NULL && bus_rd_variant(value, "as"));
  const uint8_t *end = bus_rd_array(value, 4);
  int n = 0;
  while (bus_rd_more(value, end))
    n += strncmp(bus_rd_str(value), "org.freedesktop.DBus.", 21) == 0;
  assert(!value->bad && n > 0);
  answered++;
}

static void owner_changed(bus_match *match, const bus_msg *m) {
  (void)match;
  bus_rd r = m->body;
  const char *name = bus_rd_str(&r), *before = bus_rd_str(&r), *now = bus_rd_str(&r);
  (void)before;
  if (!r.bad && strcmp(name, SYSTEM1_NAME) == 0 && now[0] != '\0')
    owned++;
}
static bus_match owner_match = {.iface = "org.freedesktop.DBus", .member = "NameOwnerChanged", .fn = owner_changed};

static void run_until(const int *n, int want) {
  const time_t until = time(NULL) + 3;
  while (*n < want) {
    assert(time(NULL) <= until);
    struct pollfd fds[] = {{bus_fd(&server), bus_events(&server), 0}, {bus_fd(&client), bus_events(&client), 0}};
    poll(fds, 2, 100);
    assert(bus_process(&server) >= 0 && bus_process(&client) >= 0);
  }
}

int main(void) {
  offline();
  const char *addr = getenv("DBUS_SESSION_BUS_ADDRESS");
  if (addr == NULL || strncmp(addr, "unix:path=", 10) != 0) {
    puts("no session bus at a path: the bus part skipped");
    return 77;
  }
  char path[108];
  snprintf(path, sizeof(path), "%.*s", (int)strcspn(addr + 10, ","), addr + 10);
  assert(bus_open(&server, path) == 0 && bus_open(&client, path) == 0);

  bus_add_match(&client, &owner_match, "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',"
                                       "member='NameOwnerChanged',arg0='" SYSTEM1_NAME "'");
  /* Its rule in before the name changes hands */
  bus_call sync = {0};
  assert(properties_call_get(&client, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                             "Interfaces", &sync, got) == 0);
  run_until(&answered, 1);
  system1_serve(&server, NULL);
  assert(bus_request_name(&server, SYSTEM1_NAME) == 0);
  run_until(&owned, 1);

  static bus_call calls[5];
  assert(system1_call_set_cpu_max(&client, 816000, &calls[0], ok) == 0);
  assert(system1_call_set_usb(&client, "on", &calls[1], ok) == 0);
  assert(system1_call_set_usb(&client, "sideways", &calls[2], refused) == 0);
  assert(system1_call_reboot(&client, &calls[3], failed) == 0);
  assert(properties_call_get_all(&client, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                                 &calls[4], got_all) == 0);
  run_until(&answered, 6);
  assert(cpu == 816000);

  /* Answered later; one cancelled never runs */
  static bus_call later, cancelled;
  assert(system1_call_suspend(&client, &later, ok) == 0 && system1_call_suspend(&client, &cancelled, ok) == 0);
  bus_call_cancel(&cancelled);
  run_until(&suspended, 2);
  assert(bus_reply(&held) == 0);
  run_until(&answered, 7);

  /* Pending when the connection goes: its error */
  static bus_call orphan;
  assert(system1_call_suspend(&client, &orphan, gone) == 0);
  bus_close(&client);
  assert(answered == 8 && !bus_call_pending(&orphan));
  bus_close(&server);
  puts("ok");
  return 0;
}
