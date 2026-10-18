#include "wifi.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bus.h"
#include "properties.h"
#include "util.h"
#include "wpa.h"
#include "wpa_bss.h"
#include "wpa_iface.h"

#define IFNAME "wlan0"
#define NETWORK "fi.w1.wpa_supplicant1.Network"
enum { FOUND_MAX = 64 };

static STRBUF(64) iface_path; /* "" without one */
static wpa_iface_props iface;
/* The link's signal (SignalPoll), dBm; 0: not known */
static int rssi;
/* Saved: a slot of its own while it's there, its calls in it; shown in the order they came (seq) */
typedef struct {
  bool used;
  unsigned seq;
  STRBUF(64) path;
  uint8_t ssid[32];
  uint8_t ssid_len;
  bus_call get;     /* its properties */
  bus_call call;    /* selecting it */
} network;
static network networks[WIFI_MAX];
static unsigned seq;
/* Found while adding, in the order they were */
static struct {
  STRBUF(64) path;
  wpa_bss_props p;
  bool gone;
} found[FOUND_MAX];
static size_t found_count;
static bool adding, added_new, dirty;
/* What's being loaded or polled */
static bus_call getting_iface;
static bus_call loading_iface, loading_networks, polling;
/* Forgetting: remove, save, back on another if it was the one */
static bus_call forgetting;
static bool forgetting_current;
/* Adding, one at a time: AddNetwork (with WPA3, again without where it was built without), select,
 * save */
static struct {
  uint8_t ssid[32];
  uint8_t ssid_len;
  STRBUF(64) psk;
  bool sae;
  bus_call making;
  bus_call call;
} adding_net;

static void changed(void) { dirty = true; }

bool wifi_changed(void) {
  const bool was = dirty;
  dirty = false;
  return was;
}

bool wifi_on(void) {
  char line[8] = "";
  FILE *f = fopen("/run/wifi", "r");
  if (f != NULL) {
    if (fgets(line, sizeof(line), f) == NULL)
      line[0] = '\0';
    fclose(f);
  }
  return strncmp(line, "on", 2) == 0;
}

static int bars(int dbm) { return dbm >= -55 ? 4 : dbm >= -65 ? 3 : dbm >= -75 ? 2 : 1; }

/* A name's bytes to show: control characters as '?' */
static str name_of(const uint8_t *ssid, size_t n, char *buf, size_t size) {
  n = MIN(n, size - 1);
  for (size_t i = 0; i < n; i++)
    buf[i] = ssid[i] < 0x20 || ssid[i] == 0x7f ? '?' : (char)ssid[i];
  buf[n] = '\0';
  return (str){buf, n};
}

static bool connecting(void) {
  static const str states[] = {S("authenticating"), S("associating"), S("associated"), S("4way_handshake"),
                               S("group_handshake")};
  for (size_t i = 0; i < COUNTOF(states); i++)
    if (str_eq(sb_str(iface.state), states[i]))
      return true;
  return false;
}
static bool completed(void) { return str_eq(sb_str(iface.state), S("completed")); }

/* Saved networks */

static network *network_find(str path) {
  for (int i = 0; i < WIFI_MAX; i++)
    if (networks[i].used && str_eq(sb_str(networks[i].path), path))
      return &networks[i];
  return NULL;
}

static void drop(network *n) {
  bus_call_cancel(&n->get);
  bus_call_cancel(&n->call);
  n->used = false;
}

/* Its name in its config: "quoted", else hex */
static void config_ssid(network *n, str v) {
  n->ssid_len = 0;
  if (v.n >= 2 && v.p[0] == '"' && v.p[v.n - 1] == '"') {
    const size_t len = MIN(v.n - 2, sizeof(n->ssid));
    memcpy(n->ssid, v.p + 1, len);
    n->ssid_len = (uint8_t)len;
    return;
  }
  for (size_t k = 0; k + 1 < v.n && n->ssid_len < sizeof(n->ssid); k += 2) {
    const char two[3] = {v.p[k], v.p[k + 1], '\0'};
    n->ssid[n->ssid_len++] = (uint8_t)strtoul(two, NULL, 16);
  }
}

/* A Network's a{sv}: of its Properties (its config), the ssid */
static void network_props(network *n, bus_rd *r) {
  const uint8_t *end = bus_rd_array(r, 8);
  str key, sig;
  while (bus_rd_kv(r, end, &key, &sig)) {
    if (!str_eq(key, S("Properties")) || !str_eq(sig, S("a{sv}"))) {
      bus_skip(r, sig);
      continue;
    }
    const uint8_t *config = bus_rd_array(r, 8);
    while (bus_rd_kv(r, config, &key, &sig))
      if (str_eq(key, S("ssid")) && str_eq(sig, S("s")))
        config_ssid(n, bus_rd_str(r));
      else
        bus_skip(r, sig);
  }
}

static void network_got(bus_call *call, const bus_error *error, bus_rd *props) {
  if (error == NULL) {
    network_props(bus_call_owner(call, network, get), props);
    changed();
  }
}

/* One there, its properties asked for: a NetworkAdded's come before AddNetwork sets them */
static void network_add(bus_conn *bus, str path) {
  network *n = network_find(path);
  for (int i = 0; n == NULL && i < WIFI_MAX; i++)
    if (!networks[i].used) {
      n = &networks[i];
      *n = (network){.used = true, .seq = ++seq};
      sb_set(n->path, path);
    }
  if (n == NULL)
    return;
  properties_call_get_all(bus, S(WPA_SERVICE), path, S(NETWORK), &n->get, network_got);
  changed();
}

static void networks_got(bus_call *call, const bus_error *error, bus_rd *value) {
  (void)call;
  if (error != NULL || !bus_rd_variant(value, "ao"))
    return;
  const uint8_t *end = bus_rd_array(value, 4);
  while (bus_rd_more(value, end)) {
    const str path = bus_rd_str(value);
    if (!value->bad)
      network_add(bus_get(), path);
  }
}

static void network_added(str object, str path, bus_rd *props) {
  (void)object, (void)props;
  network_add(bus_get(), path);
}

static void network_removed(str object, str path) {
  (void)object;
  network *n = network_find(path);
  if (n != NULL)
    drop(n);
  changed();
}

/* Found while adding */

static size_t found_find(str path) {
  size_t i = 0;
  while (i < found_count && !str_eq(sb_str(found[i].path), path))
    i++;
  return i;
}

static void bss_added(str object, str path, bus_rd *props) {
  (void)object;
  size_t i = found_find(path);
  if (!adding || (i == found_count && found_count == FOUND_MAX))
    return;
  if (i == found_count) {
    found[found_count++] = (typeof(found[0])){0};
    sb_set(found[i].path, path);
  }
  found[i].gone = false;
  wpa_bss_props_apply(&found[i].p, props, NULL);
  changed();
}

static void bss_removed(str object, str path) {
  (void)object;
  const size_t i = found_find(path);
  if (i == found_count)
    return;
  if (adding)
    found[i].gone = true;
  else
    memmove(&found[i], &found[i + 1], (--found_count - i) * sizeof(found[0]));
  changed();
}

static void bss_changed(str path, bus_rd *props) {
  uint32_t bits;
  const size_t i = found_find(path);
  if (i < found_count && wpa_bss_props_apply(&found[i].p, props, &bits) >= 0 && bits != 0)
    changed();
}

/* The interface */

static void signal_got(bus_call *call, const bus_error *error, bus_rd *args) {
  (void)call;
  /* An a{sv}, in a variant (2.11's) */
  if (error != NULL || !bus_rd_variant(args, "a{sv}"))
    return;
  const uint8_t *end = bus_rd_array(args, 8);
  str key, sig;
  while (bus_rd_kv(args, end, &key, &sig))
    if (str_eq(key, S("rssi")) && str_eq(sig, S("i")))
      rssi = bus_rd_i32(args);
    else
      bus_skip(args, sig);
  changed();
}

void wifi_poll_signal(void) {
  if (bus_get() == NULL || iface_path.n == 0 || !completed())
    return;
  wpa_iface_call_signal_poll(bus_get(), sb_str(iface_path), &polling, signal_got);
}

static void iface_got(bus_call *call, const bus_error *error, bus_rd *props) {
  (void)call;
  if (error == NULL && wpa_iface_props_apply(&iface, props, NULL) >= 0) {
    wifi_poll_signal();
    changed();
  }
}

static void load(bus_conn *bus, str path) {
  sb_set(iface_path, path);
  properties_call_get_all(bus, S(WPA_SERVICE), path, S(WPA_IFACE_NAME), &loading_iface, iface_got);
  properties_call_get(bus, S(WPA_SERVICE), path, S(WPA_IFACE_NAME), S("Networks"), &loading_networks, networks_got);
}

static void clear(void) {
  iface_path.n = 0;
  iface = (wpa_iface_props){0};
  rssi = 0;
  for (int i = 0; i < WIFI_MAX; i++)
    drop(&networks[i]);
  found_count = 0;
  bus_call_cancel(&getting_iface);
  bus_call_cancel(&loading_iface);
  bus_call_cancel(&loading_networks);
  bus_call_cancel(&polling);
  bus_call_cancel(&forgetting);
  bus_call_cancel(&adding_net.making);
  bus_call_cancel(&adding_net.call);
  changed();
}

static void iface_changed(str path, bus_rd *props) {
  uint32_t bits;
  if (!str_eq(path, sb_str(iface_path)) || wpa_iface_props_apply(&iface, props, &bits) < 0 || bits == 0)
    return;
  if (bits & (WPA_IFACE_PROP_STATE | WPA_IFACE_PROP_CURRENT_BSS)) {
    rssi = 0;
    wifi_poll_signal();
  }
  changed();
}

static void got_iface(bus_call *call, const bus_error *error, str path) {
  (void)call;
  if (error == NULL)
    load(bus_get(), path);
}

static void interface_added(str object, str path, bus_rd *props) {
  (void)object, (void)props;
  if (iface_path.n == 0)
    load(bus_get(), path);
}

static void interface_removed(str object, str path) {
  (void)object;
  if (str_eq(path, sb_str(iface_path)))
    clear();
}

static void connected(bus_conn *bus) {
  wpa_watch_interface_added(bus, interface_added);
  wpa_watch_interface_removed(bus, interface_removed);
  wpa_iface_watch_properties(bus, iface_changed);
  wpa_iface_watch_network_added(bus, network_added);
  wpa_iface_watch_network_removed(bus, network_removed);
  wpa_iface_watch_bss_added(bus, bss_added);
  wpa_iface_watch_bss_removed(bus, bss_removed);
  wpa_bss_watch_properties(bus, bss_changed);
}

/* wpa_supplicant came (Wi-Fi on) or went */
static void owned(bus_conn *bus, bool owner) {
  clear();
  if (owner)
    wpa_call_get_interface(bus, S(IFNAME), &getting_iface, got_iface);
}

void wifi_init(void) { bus_watch(S(WPA_SERVICE), connected, owned); }

int wifi_signal(void) {
  if (!wifi_on())
    return -1;
  return !completed() ? 0 : rssi != 0 ? bars(rssi) : 1;
}

static const network *current(void) {
  return iface.current_network.n > 0 ? network_find(sb_str(iface.current_network)) : NULL;
}

str wifi_status(char *buf, size_t size) {
  size_t n = 0;
  char name[64];
  const network *on = current();
  const str net = on != NULL ? name_of(on->ssid, on->ssid_len, name, sizeof(name)) : S("");
  if (!wifi_on())
    str_put(buf, size, &n, STR_PARTS(S("Off")));
  else if (iface_path.n == 0)
    str_put(buf, size, &n, STR_PARTS(S("Starting…")));
  else if (completed()) {
    char ip[INET_ADDRSTRLEN] = "";
    struct ifaddrs *all;
    if (getifaddrs(&all) == 0) {
      for (const struct ifaddrs *a = all; a != NULL && ip[0] == '\0'; a = a->ifa_next)
        if (a->ifa_addr != NULL && a->ifa_addr->sa_family == AF_INET && strcmp(a->ifa_name, IFNAME) == 0)
          inet_ntop(AF_INET, &((const struct sockaddr_in *)a->ifa_addr)->sin_addr, ip, sizeof(ip));
      freeifaddrs(all);
    }
    str_put(buf, size, &n,
            STR_PARTS(S("Connected to "), net, S(" · "), ip[0] != '\0' ? str_c(ip) : S("getting an address…")));
  } else if (connecting())
    str_put(buf, size, &n, STR_PARTS(S("Connecting to "), net, S("…")));
  else
    str_put(buf, size, &n, STR_PARTS(S("Not connected")));
  return (str){buf, n};
}

size_t wifi_saved_list(wifi_saved *out, size_t max) {
  const network *order[WIFI_MAX];
  size_t n = 0;
  for (int i = 0; i < WIFI_MAX; i++) {
    if (!networks[i].used)
      continue;
    size_t at = n++;
    for (; at > 0 && order[at - 1]->seq > networks[i].seq; at--)
      order[at] = order[at - 1];
    order[at] = &networks[i];
  }
  const network *on = current();
  for (size_t k = 0; k < n && k < max; k++) {
    const network *w = order[k];
    char name[64];
    out[k] = (wifi_saved){.current = w == on, .connected = w == on && completed(), .busy = bus_call_pending(&w->call)};
    sb_set(out[k].path, sb_str(w->path));
    sb_set(out[k].name, name_of(w->ssid, w->ssid_len, name, sizeof(name)));
  }
  return MIN(n, max);
}

static void selected(bus_call *call, const bus_error *error) {
  (void)call;
  if (error != NULL)
    bus_tell(error, S("Couldn't connect"));
  changed();
}

void wifi_select(str path) {
  network *n = network_find(path);
  if (n != NULL && bus_get() != NULL && iface_path.n > 0 &&
      wpa_iface_call_select_network(bus_get(), sb_str(iface_path), path, &n->call, selected) >= 0)
    changed();
}

static void reassociated(bus_call *call, const bus_error *error) {
  (void)call;
  if (error != NULL)
    bus_tell(error, S("Couldn't connect again"));
}

static void forget_saved(bus_call *call, const bus_error *error) {
  if (error != NULL)
    bus_tell(error, S("Couldn't save the networks"));
  /* Back on whichever saved one is there */
  else if (forgetting_current)
    wpa_iface_call_reassociate(bus_get(), sb_str(iface_path), call, reassociated);
}

static void forgot(bus_call *call, const bus_error *error) {
  if (error != NULL)
    bus_tell(error, S("Couldn't forget"));
  else
    wpa_iface_call_save_config(bus_get(), sb_str(iface_path), call, forget_saved);
}

/* Not the network's call: it goes with the network */
void wifi_forget(str path) {
  if (bus_get() == NULL || iface_path.n == 0)
    return;
  forgetting_current = str_eq(path, sb_str(iface.current_network));
  wpa_iface_call_remove_network(bus_get(), sb_str(iface_path), path, &forgetting, forgot);
}

static void scan_args(bus_wr *w, const void *ctx) {
  (void)ctx;
  const bus_wr_arr a = bus_wr_array(w, 8);
  bus_wr_kv(w, S("Type"), "s");
  bus_wr_str(w, S("active"));
  bus_wr_array_end(w, a);
}

/* One search: opening the screen again searches again */
static void adding_set(bool on) {
  adding = on;
  found_count = 0;
  /* What it knew is shown as it's found again: no BSSAdded for what it has */
  if (on && bus_get() != NULL && iface_path.n > 0) {
    wpa_iface_call_flush_bss(bus_get(), sb_str(iface_path), 0, NULL, NULL);
    wpa_iface_call_scan(bus_get(), sb_str(iface_path), scan_args, NULL, NULL, NULL);
  }
  changed();
}
void wifi_adding_start(void) { adding_set(true); }
void wifi_adding_stop(void) { adding_set(false); }

static bool saved(const uint8_t *ssid, size_t n) {
  for (int i = 0; i < WIFI_MAX; i++)
    if (networks[i].used && networks[i].ssid_len == n && memcmp(networks[i].ssid, ssid, n) == 0)
      return true;
  return false;
}

size_t wifi_found_list(wifi_found *out, size_t max) {
  const bool busy = bus_call_pending(&adding_net.making) || bus_call_pending(&adding_net.call);
  size_t n = 0;
  for (size_t i = 0; i < found_count; i++) {
    const wpa_bss_props *p = &found[i].p;
    /* Hidden: no name, or NULs */
    if (p->ssid_len == 0 || p->ssid[0] == '\0' || saved(p->ssid, p->ssid_len))
      continue;
    size_t k = 0;
    while (k < n && !(out[k].ssid_len == p->ssid_len && memcmp(out[k].ssid, p->ssid, p->ssid_len) == 0))
      k++;
    if (k == n) {
      if (n == max)
        continue;
      char name[64];
      out[n] = (wifi_found){.ssid_len = MIN(p->ssid_len, sizeof(out[n].ssid)), .gone = true};
      memcpy(out[n].ssid, p->ssid, out[n].ssid_len);
      sb_set(out[n].name, name_of(p->ssid, p->ssid_len, name, sizeof(name)));
      out[n].busy = busy && adding_net.ssid_len == out[n].ssid_len &&
                    memcmp(adding_net.ssid, out[n].ssid, out[n].ssid_len) == 0;
      n++;
    }
    out[k].bars = MAX(out[k].bars, bars(p->signal));
    out[k].secured |= p->privacy;
    out[k].gone &= found[i].gone;
  }
  return n;
}

static void add_saved(bus_call *call, const bus_error *error) {
  (void)call;
  if (error != NULL)
    bus_tell(error, S("Couldn't save the network"));
  added_new = true;
  changed();
}

static void add_selected(bus_call *call, const bus_error *error) {
  if (error != NULL)
    bus_tell(error, S("Couldn't connect"));
  wpa_iface_call_save_config(bus_get(), sb_str(iface_path), call, add_saved);
}

static void add_network(void);

static void network_made(bus_call *call, const bus_error *error, str path) {
  (void)call;
  if (error != NULL && adding_net.sae && str_eq(error->name, S("fi.w1.wpa_supplicant1.InvalidArgs"))) {
    adding_net.sae = false;
    add_network();
  } else if (error != NULL)
    bus_tell(error, S("Couldn't add the network"));
  else
    wpa_iface_call_select_network(bus_get(), sb_str(iface_path), path, &adding_net.call, add_selected);
  changed();
}

/* AddNetwork's a{sv}: the name, and the key (with WPA3 where it may) or none */
static void network_args(bus_wr *w, const void *ctx) {
  (void)ctx;
  const bool secured = adding_net.psk.n > 0;
  const bus_wr_arr a = bus_wr_array(w, 8);
  bus_wr_kv(w, S("ssid"), "ay");
  bus_wr_bytes(w, adding_net.ssid, adding_net.ssid_len);
  bus_wr_kv(w, S("key_mgmt"), "s");
  bus_wr_str(w, !secured ? S("NONE") : adding_net.sae ? S("WPA-PSK SAE") : S("WPA-PSK"));
  if (secured) {
    bus_wr_kv(w, S("psk"), "s");
    bus_wr_str(w, sb_str(adding_net.psk));
  }
  if (secured && adding_net.sae) {
    bus_wr_kv(w, S("ieee80211w"), "u");
    bus_wr_u32(w, 1);
  }
  bus_wr_array_end(w, a);
}

static void add_network(void) {
  wpa_iface_call_add_network(bus_get(), sb_str(iface_path), network_args, NULL, &adding_net.making, network_made);
}

void wifi_add(const wifi_found *net, str psk) {
  if (bus_get() == NULL || iface_path.n == 0 || bus_call_pending(&adding_net.making) || bus_call_pending(&adding_net.call))
    return;
  memcpy(adding_net.ssid, net->ssid, net->ssid_len);
  adding_net.ssid_len = net->ssid_len;
  sb_set(adding_net.psk, psk);
  adding_net.sae = true;
  add_network();
  changed();
}

bool wifi_added_new(void) {
  const bool was = added_new;
  added_new = false;
  return was;
}
