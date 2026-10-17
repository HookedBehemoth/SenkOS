#include "wifi.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bus.h"
#include "util.h"
#include "wpa.h"
#include "wpa_bss.h"
#include "wpa_iface.h"

#define IFNAME "wlan0"
#define NETWORK "fi.w1.wpa_supplicant1.Network"
enum { SCAN_MS = 15000, FOUND_MAX = 64 };

static STRBUF(64) iface_path; /* "" without one */
static wpa_iface_props iface;
/* The link's signal (SignalPoll), dBm; 0: not known */
static int rssi;
/* Saved: slots that stay put, so a GetAll's answer finds its own (index and gen in its userdata);
 * shown in the order they came (seq) */
static struct {
  bool used;
  unsigned gen, seq;
  STRBUF(64) path;
  uint8_t ssid[32];
  uint8_t ssid_len;
} networks[WIFI_MAX];
static unsigned seq;
/* Found while adding, in the order they were */
static struct {
  STRBUF(64) path;
  wpa_bss_props p;
  bool gone;
} found[FOUND_MAX];
static size_t found_count;
static bool adding, added_new, dirty;
static uint64_t scanned_at;

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
  static const char *const states[] = {"authenticating", "associating", "associated", "4way_handshake",
                                       "group_handshake"};
  for (size_t i = 0; i < COUNTOF(states); i++)
    if (strcmp(iface.state, states[i]) == 0)
      return true;
  return false;
}
static bool completed(void) { return strcmp(iface.state, "completed") == 0; }

/* Saved networks */

static int network_find(const char *path) {
  for (int i = 0; i < WIFI_MAX; i++)
    if (networks[i].used && strcmp(networks[i].path.p, path) == 0)
      return i;
  return -1;
}

static int network_add(const char *path) {
  int i = network_find(path);
  for (int k = 0; i < 0 && k < WIFI_MAX; k++)
    if (!networks[k].used) {
      i = k;
      networks[i].used = true;
      networks[i].gen++;
      networks[i].seq = ++seq;
      networks[i].ssid_len = 0;
      sb_set(networks[i].path, str_c(path));
    }
  return i;
}

/* Its name in its config: "quoted", else hex */
static void config_ssid(int i, const char *v) {
  size_t n = strlen(v);
  networks[i].ssid_len = 0;
  if (n >= 2 && v[0] == '"' && v[n - 1] == '"') {
    n = MIN(n - 2, sizeof(networks[i].ssid));
    memcpy(networks[i].ssid, v + 1, n);
    networks[i].ssid_len = (uint8_t)n;
    return;
  }
  for (size_t k = 0; k + 1 < n && networks[i].ssid_len < sizeof(networks[i].ssid); k += 2) {
    const char two[3] = {v[k], v[k + 1], '\0'};
    networks[i].ssid[networks[i].ssid_len++] = (uint8_t)strtoul(two, NULL, 16);
  }
}

/* A Network's a{sv}: of its Properties (its config), the ssid */
static int network_props(int i, sd_bus_message *m) {
  int r = sd_bus_message_enter_container(m, 'a', "{sv}");
  while (r >= 0 && (r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
    const char *key, *type;
    r = sd_bus_message_read(m, "s", &key);
    if (r >= 0)
      r = sd_bus_message_peek_type(m, NULL, &type);
    if (r >= 0 && strcmp(key, "Properties") == 0 && strcmp(type, "a{sv}") == 0) {
      r = sd_bus_message_enter_container(m, 'v', "a{sv}");
      if (r >= 0)
        r = sd_bus_message_enter_container(m, 'a', "{sv}");
      while (r >= 0 && (r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
        const char *k, *v;
        r = sd_bus_message_read(m, "s", &k);
        if (r >= 0)
          r = sd_bus_message_peek_type(m, NULL, &type);
        if (r >= 0 && strcmp(k, "ssid") == 0 && strcmp(type, "s") == 0 && (r = sd_bus_message_read(m, "v", "s", &v)) >= 0)
          config_ssid(i, v);
        else if (r >= 0)
          r = sd_bus_message_skip(m, "v");
        if (r >= 0)
          r = sd_bus_message_exit_container(m);
      }
      if (r >= 0)
        r = sd_bus_message_exit_container(m);
      if (r >= 0)
        r = sd_bus_message_exit_container(m);
    } else if (r >= 0)
      r = sd_bus_message_skip(m, "v");
    if (r >= 0)
      r = sd_bus_message_exit_container(m);
  }
  return r < 0 ? r : sd_bus_message_exit_container(m);
}

static int network_got(sd_bus_message *m, void *userdata, sd_bus_error *error) {
  (void)error;
  const uintptr_t u = (uintptr_t)userdata;
  const size_t i = u & 0xff;
  if (!sd_bus_message_is_method_error(m, NULL) && networks[i].used && networks[i].gen == u >> 8) {
    network_props((int)i, m);
    changed();
  }
  return 0;
}

/* Its properties, for network_got */
static void get_network(sd_bus *bus, int i) {
  sd_bus_call_method_async(bus, NULL, WPA_SERVICE, networks[i].path.p, "org.freedesktop.DBus.Properties", "GetAll",
                           network_got, (void *)(uintptr_t)(networks[i].gen << 8 | (unsigned)i), "s", NETWORK);
}

static int networks_got(sd_bus_message *m, void *userdata, sd_bus_error *error) {
  (void)userdata, (void)error;
  const char *path;
  if (sd_bus_message_is_method_error(m, NULL))
    return 0;
  int r = sd_bus_message_enter_container(m, 'v', "ao");
  if (r >= 0)
    r = sd_bus_message_enter_container(m, 'a', "o");
  while (r >= 0 && (r = sd_bus_message_read(m, "o", &path)) > 0) {
    const int i = network_add(path);
    if (i >= 0)
      get_network(sd_bus_message_get_bus(m), i);
  }
  changed();
  return 0;
}

/* Its properties come later: AddNetwork sets them once it's made */
static void network_added(const char *object, const char *path, sd_bus_message *props) {
  (void)object, (void)props;
  const int i = network_add(path);
  if (i >= 0)
    get_network(bus_get(), i);
  changed();
}

static void network_removed(const char *object, const char *path) {
  (void)object;
  const int i = network_find(path);
  if (i >= 0)
    networks[i].used = false;
  changed();
}

/* Found while adding */

static size_t found_find(const char *path) {
  size_t i = 0;
  while (i < found_count && strcmp(found[i].path.p, path) != 0)
    i++;
  return i;
}

static void bss_added(const char *object, const char *path, sd_bus_message *props) {
  (void)object;
  size_t i = found_find(path);
  if (!adding || (i == found_count && found_count == FOUND_MAX))
    return;
  if (i == found_count) {
    found[found_count++] = (typeof(found[0])){0};
    sb_set(found[i].path, str_c(path));
  }
  found[i].gone = false;
  wpa_bss_props_apply(&found[i].p, props, NULL);
  changed();
}

static void bss_removed(const char *object, const char *path) {
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

static void bss_changed(const char *path, sd_bus_message *props) {
  uint32_t bits;
  const size_t i = found_find(path);
  if (i < found_count && wpa_bss_props_apply(&found[i].p, props, &bits) >= 0 && bits != 0)
    changed();
}

/* The interface */

static int signal_got(sd_bus_message *m, void *userdata, sd_bus_error *error) {
  (void)userdata, (void)error;
  if (sd_bus_message_is_method_error(m, NULL))
    return 0;
  /* An a{sv}, in a variant (2.11's) */
  int r = sd_bus_message_enter_container(m, 'v', "a{sv}");
  if (r >= 0)
    r = sd_bus_message_enter_container(m, 'a', "{sv}");
  while (r >= 0 && (r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
    const char *key, *type;
    int32_t v;
    r = sd_bus_message_read(m, "s", &key);
    if (r >= 0)
      r = sd_bus_message_peek_type(m, NULL, &type);
    if (r >= 0 && strcmp(key, "rssi") == 0 && strcmp(type, "i") == 0 && (r = sd_bus_message_read(m, "v", "i", &v)) >= 0)
      rssi = v;
    else if (r >= 0)
      r = sd_bus_message_skip(m, "v");
    if (r >= 0)
      r = sd_bus_message_exit_container(m);
  }
  changed();
  return 0;
}

void wifi_poll_signal(void) {
  if (bus_get() != NULL && iface_path.n > 0 && completed())
    sd_bus_call_method_async(bus_get(), NULL, WPA_IFACE_SERVICE, iface_path.p, WPA_IFACE_NAME, "SignalPoll", signal_got,
                             NULL, NULL);
}

static int iface_got(sd_bus_message *m, void *userdata, sd_bus_error *error) {
  (void)userdata, (void)error;
  if (!sd_bus_message_is_method_error(m, NULL) && wpa_iface_props_apply(&iface, m, NULL) >= 0) {
    wifi_poll_signal();
    changed();
  }
  return 0;
}

static void load(sd_bus *bus, const char *path) {
  sb_set(iface_path, str_c(path));
  sd_bus_call_method_async(bus, NULL, WPA_SERVICE, path, "org.freedesktop.DBus.Properties", "GetAll", iface_got, NULL,
                           "s", WPA_IFACE_NAME);
  sd_bus_call_method_async(bus, NULL, WPA_SERVICE, path, "org.freedesktop.DBus.Properties", "Get", networks_got, NULL,
                           "ss", WPA_IFACE_NAME, "Networks");
}

static void clear(void) {
  iface_path.n = 0;
  iface = (wpa_iface_props){0};
  rssi = 0;
  for (int i = 0; i < WIFI_MAX; i++)
    networks[i].used = false;
  found_count = 0;
  changed();
}

static void iface_changed(const char *path, sd_bus_message *props) {
  uint32_t bits;
  if (strcmp(path, iface_path.p) != 0 || wpa_iface_props_apply(&iface, props, &bits) < 0 || bits == 0)
    return;
  if (bits & (WPA_IFACE_PROP_STATE | WPA_IFACE_PROP_CURRENT_BSS)) {
    rssi = 0;
    wifi_poll_signal();
  }
  changed();
}

static void got_iface(const sd_bus_error *error, const char *path, void *userdata) {
  (void)userdata;
  if (error == NULL)
    load(bus_get(), path);
}

static void interface_added(const char *object, const char *path, sd_bus_message *props) {
  (void)object, (void)props;
  if (iface_path.n == 0)
    load(bus_get(), path);
}

static void interface_removed(const char *object, const char *path) {
  (void)object;
  if (strcmp(path, iface_path.p) == 0)
    clear();
}

static void connected(sd_bus *bus) {
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
static void owned(sd_bus *bus, bool owner) {
  static const wpa_get_interface_then then = {got_iface, NULL};
  clear();
  if (owner)
    wpa_call_get_interface(bus, IFNAME, &then);
}

void wifi_init(void) { bus_watch(WPA_SERVICE, connected, owned); }

int wifi_signal(void) {
  if (!wifi_on())
    return -1;
  return !completed() ? 0 : rssi != 0 ? bars(rssi) : 1;
}

static int current(void) { return iface.current_network[0] != '\0' ? network_find(iface.current_network) : -1; }

str wifi_status(char *buf, size_t size) {
  size_t n = 0;
  char name[64];
  const int i = current();
  const str net = i >= 0 ? name_of(networks[i].ssid, networks[i].ssid_len, name, sizeof(name)) : S("");
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
  int order[WIFI_MAX];
  size_t n = 0;
  for (int i = 0; i < WIFI_MAX; i++) {
    if (!networks[i].used)
      continue;
    size_t at = n++;
    for (; at > 0 && networks[order[at - 1]].seq > networks[i].seq; at--)
      order[at] = order[at - 1];
    order[at] = i;
  }
  const int on = current();
  for (size_t k = 0; k < n && k < max; k++) {
    const int i = order[k];
    char name[64];
    out[k] = (wifi_saved){.current = i == on, .connected = i == on && completed()};
    sb_set(out[k].path, sb_str(networks[i].path));
    sb_set(out[k].name, name_of(networks[i].ssid, networks[i].ssid_len, name, sizeof(name)));
  }
  return MIN(n, max);
}

void wifi_select(const char *path) {
  static const wpa_iface_select_network_then then = {bus_done, "Couldn't connect"};
  if (bus_get() != NULL && iface_path.n > 0)
    wpa_iface_call_select_network(bus_get(), iface_path.p, path, &then);
}

static void forgot(const sd_bus_error *error, void *userdata) {
  (void)userdata;
  if (error != NULL) {
    bus_done(error, "Couldn't forget");
    return;
  }
  /* Back on whichever saved one is there */
  wpa_iface_call_save_config(bus_get(), iface_path.p, NULL);
  wpa_iface_call_reassociate(bus_get(), iface_path.p, NULL);
}

void wifi_forget(const char *path) {
  static const wpa_iface_remove_network_then then = {forgot, NULL};
  if (bus_get() != NULL && iface_path.n > 0)
    wpa_iface_call_remove_network(bus_get(), iface_path.p, path, &then);
}

void wifi_adding(bool on) {
  if (on != adding) {
    adding = on;
    found_count = 0;
    scanned_at = 0;
    /* What it knew is shown as it's found again: no BSSAdded for what it has */
    if (on && bus_get() != NULL && iface_path.n > 0)
      wpa_iface_call_flush_bss(bus_get(), iface_path.p, 0, NULL);
    changed();
  }
  if (adding && bus_get() != NULL && iface_path.n > 0 && (scanned_at == 0 || ms_now() - scanned_at >= SCAN_MS)) {
    sd_bus_call_method_async(bus_get(), NULL, WPA_IFACE_SERVICE, iface_path.p, WPA_IFACE_NAME, "Scan", NULL, NULL,
                             "a{sv}", 1, "Type", "s", "active");
    scanned_at = ms_now();
  }
}

static bool saved(const uint8_t *ssid, size_t n) {
  for (int i = 0; i < WIFI_MAX; i++)
    if (networks[i].used && networks[i].ssid_len == n && memcmp(networks[i].ssid, ssid, n) == 0)
      return true;
  return false;
}

size_t wifi_found_list(wifi_found *out, size_t max) {
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
      n++;
    }
    out[k].bars = MAX(out[k].bars, bars(p->signal));
    out[k].secured |= p->privacy;
    out[k].gone &= found[i].gone;
  }
  return n;
}

/* One at a time: tried with WPA3 too, again without where it was built without */
static struct {
  uint8_t ssid[32];
  uint8_t ssid_len;
  STRBUF(64) psk;
  bool sae;
} adding_net;

static void add_network(void);

static int network_made(sd_bus_message *m, void *userdata, sd_bus_error *error) {
  (void)userdata, (void)error;
  const sd_bus_error *e = sd_bus_message_get_error(m);
  const char *path;
  if (e != NULL && adding_net.sae && sd_bus_error_has_name(e, "fi.w1.wpa_supplicant1.InvalidArgs")) {
    adding_net.sae = false;
    add_network();
  } else if (e != NULL)
    bus_done(e, "Couldn't add the network");
  else if (sd_bus_message_read(m, "o", &path) >= 0) {
    static const wpa_iface_select_network_then then = {bus_done, "Couldn't connect"};
    wpa_iface_call_select_network(bus_get(), iface_path.p, path, &then);
    wpa_iface_call_save_config(bus_get(), iface_path.p, NULL);
    added_new = true;
    changed();
  }
  return 0;
}

static void add_network(void) {
  sd_bus_message *m = NULL;
  const bool secured = adding_net.psk.n > 0;
  int r = sd_bus_message_new_method_call(bus_get(), &m, WPA_IFACE_SERVICE, iface_path.p, WPA_IFACE_NAME, "AddNetwork");
  if (r >= 0)
    r = sd_bus_message_open_container(m, 'a', "{sv}");
  if (r >= 0)
    r = sd_bus_message_open_container(m, 'e', "sv");
  if (r >= 0)
    r = sd_bus_message_append(m, "s", "ssid");
  if (r >= 0)
    r = sd_bus_message_open_container(m, 'v', "ay");
  if (r >= 0)
    r = sd_bus_message_append_array(m, 'y', adding_net.ssid, adding_net.ssid_len);
  if (r >= 0)
    r = sd_bus_message_close_container(m);
  if (r >= 0)
    r = sd_bus_message_close_container(m);
  if (r >= 0 && secured)
    r = sd_bus_message_append(m, "{sv}{sv}", "psk", "s", adding_net.psk.p, "key_mgmt", "s",
                              adding_net.sae ? "WPA-PSK SAE" : "WPA-PSK");
  if (r >= 0 && secured && adding_net.sae)
    r = sd_bus_message_append(m, "{sv}", "ieee80211w", "u", 1u);
  if (r >= 0 && !secured)
    r = sd_bus_message_append(m, "{sv}", "key_mgmt", "s", "NONE");
  if (r >= 0)
    r = sd_bus_message_close_container(m);
  if (r >= 0)
    r = sd_bus_call_async(bus_get(), NULL, m, network_made, NULL, 0);
  sd_bus_message_unref(m);
}

void wifi_add(const wifi_found *net, str psk) {
  if (bus_get() == NULL || iface_path.n == 0)
    return;
  memcpy(adding_net.ssid, net->ssid, net->ssid_len);
  adding_net.ssid_len = net->ssid_len;
  sb_set(adding_net.psk, psk);
  adding_net.sae = true;
  add_network();
}

bool wifi_added_new(void) {
  const bool was = added_new;
  added_new = false;
  return was;
}
