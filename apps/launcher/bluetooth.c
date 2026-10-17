#include "bluetooth.h"

#include <string.h>

#include "adapter1.h"
#include "bus.h"
#include "util.h"

static bt_device devices[BT_MAX];
static size_t count;
static STRBUF(64) adapter_path; /* "" without one */
static adapter1_props adapter;
/* The pairing screen is up; we asked for the search that's on */
static bool pairing, searching;
static STRBUF(64) busy;
static bool paired_new;
/* For bt_changed */
static bool dirty;

static void changed(void) { dirty = true; }

static bt_device *find(const char *path) {
  for (size_t i = 0; i < count; i++)
    if (strcmp(devices[i].path.p, path) == 0)
      return &devices[i];
  return NULL;
}

static void search_failed(const sd_bus_error *error, void *userdata) {
  (void)userdata;
  if (error != NULL)
    searching = false;
}

/* What pairing wants of the adapter: searching, and pairable (BlueZ makes it so with our agent) */
static void sync_adapter(sd_bus *bus) {
  static const adapter1_start_discovery_then started = {search_failed, NULL};
  if (bus == NULL || adapter_path.n == 0)
    return;
  if (adapter.pairable != pairing)
    adapter1_set_pairable(bus, adapter_path.p, pairing, NULL);
  if (pairing && adapter.powered && !adapter.discovering && !searching)
    searching = adapter1_call_start_discovery(bus, adapter_path.p, &started) >= 0;
  else if (!pairing && searching) {
    adapter1_call_stop_discovery(bus, adapter_path.p, NULL);
    searching = false;
  }
}

static bool added(const char *path, const char *iface, sd_bus_message *props) {
  if (strcmp(iface, ADAPTER1_NAME) == 0) {
    if (adapter_path.n == 0)
      sb_set(adapter_path, str_c(path));
    if (strcmp(adapter_path.p, path) != 0)
      return false;
    adapter1_props_apply(&adapter, props, NULL);
    sync_adapter(bus_get());
  } else if (strcmp(iface, DEVICE1_NAME) == 0) {
    bt_device *d = find(path);
    if (d == NULL && count == BT_MAX)
      return false;
    if (d == NULL) {
      d = &devices[count++];
      *d = (bt_device){0};
      sb_set(d->path, str_c(path));
    }
    d->gone = false;
    device1_props_apply(&d->p, props, NULL);
  } else
    return false;
  changed();
  return true;
}

static void removed(const char *path, const char *iface) {
  if (strcmp(iface, ADAPTER1_NAME) == 0 && strcmp(adapter_path.p, path) == 0) {
    adapter_path.n = 0;
    adapter = (adapter1_props){0};
    searching = false;
  } else if (strcmp(iface, DEVICE1_NAME) == 0) {
    bt_device *d = find(path);
    if (d == NULL)
      return;
    if (pairing && !d->p.paired)
      d->gone = true;
    else
      memmove(d, d + 1, (size_t)(devices + --count - d) * sizeof(*d));
  }
  changed();
}

static void adapter_changed(const char *path, sd_bus_message *props) {
  uint32_t bits;
  if (strcmp(adapter_path.p, path) != 0 || adapter1_props_apply(&adapter, props, &bits) < 0 || bits == 0)
    return;
  if (!adapter.discovering)
    searching = false;
  sync_adapter(bus_get());
  changed();
}

static void device_changed(const char *path, sd_bus_message *props) {
  uint32_t bits;
  bt_device *d = find(path);
  if (d != NULL && device1_props_apply(&d->p, props, &bits) >= 0 && bits != 0)
    changed();
}

static void connected(sd_bus *bus) {
  adapter1_watch_properties(bus, adapter_changed);
  device1_watch_properties(bus, device_changed);
  bus_watch_objects(bus, "org.bluez", added, removed);
  bt_agent_add(bus);
}

/* bluetoothd came or went: what it has, from scratch */
static void owned(sd_bus *bus, bool owner) {
  count = 0;
  adapter_path.n = busy.n = 0;
  adapter = (adapter1_props){0};
  searching = false;
  if (owner) {
    bus_get_objects(bus, "org.bluez", added);
    bt_agent_register(bus);
  }
  changed();
}

void bt_init(void) { bus_watch("org.bluez", connected, owned); }

const bt_device *bt_devices(size_t *n) {
  *n = count;
  return devices;
}

int bt_kind(const bt_device *d) {
  return strncmp(d->p.icon, "audio-", 6) == 0      ? BT_AUDIO
         : strcmp(d->p.icon, "input-gaming") == 0 ? BT_PAD
                                                  : BT_OTHER;
}

int bt_powered(void) { return adapter_path.n == 0 ? -1 : adapter.powered; }
bool bt_searching(void) { return adapter.discovering; }
bool bt_busy(const char *path) { return busy.n > 0 && strcmp(busy.p, path) == 0; }
bool bt_pairing_on(void) { return pairing; }

int bt_bar(void) {
  if (bt_powered() <= 0)
    return -1;
  for (size_t i = 0; i < count; i++)
    if (devices[i].p.connected)
      return 1;
  return 0;
}

void bt_power(bool on) {
  static const adapter1_set_then on_then = {bus_done, "Couldn't turn Bluetooth on"};
  static const adapter1_set_then off_then = {bus_done, "Couldn't turn Bluetooth off"};
  if (bus_get() != NULL && adapter_path.n > 0)
    adapter1_set_powered(bus_get(), adapter_path.p, on, on ? &on_then : &off_then);
}

void bt_pairing(bool on) {
  if (on == pairing)
    return;
  pairing = on;
  /* What went while pairing goes now */
  size_t kept = 0;
  for (size_t i = 0; i < count; i++)
    if (!devices[i].gone)
      devices[kept++] = devices[i];
  count = kept;
  sync_adapter(bus_get());
  changed();
}

/* An action's end */
static void done(const sd_bus_error *error, void *what) {
  busy.n = 0;
  bus_done(error, what);
  changed();
}

void bt_connect(const char *path) {
  static const device1_connect_then connect_then = {done, "Couldn't connect"};
  static const device1_disconnect_then disconnect_then = {done, "Couldn't disconnect"};
  const bt_device *d = find(path);
  if (busy.n > 0 || d == NULL || bus_get() == NULL)
    return;
  if ((d->p.connected ? device1_call_disconnect(bus_get(), path, &disconnect_then)
                      : device1_call_connect(bus_get(), path, &connect_then)) >= 0)
    sb_set(busy, str_c(path));
  changed();
}

static void paired(const sd_bus_error *error, void *userdata) {
  static const device1_connect_then connect_then = {done, "Couldn't connect"};
  (void)userdata;
  if (error != NULL) {
    done(error, "Couldn't pair");
    return;
  }
  paired_new = true;
  /* Ours to connect again, and it may connect by itself */
  device1_set_trusted(bus_get(), busy.p, true, NULL);
  if (device1_call_connect(bus_get(), busy.p, &connect_then) < 0)
    done(NULL, NULL);
}

void bt_pair(const char *path) {
  static const device1_pair_then then = {paired, NULL};
  if (busy.n > 0 || bus_get() == NULL)
    return;
  if (device1_call_pair(bus_get(), path, &then) >= 0)
    sb_set(busy, str_c(path));
  changed();
}

bool bt_changed(void) {
  const bool was = dirty;
  dirty = false;
  return was;
}

bool bt_paired_new(void) {
  const bool was = paired_new;
  paired_new = false;
  return was;
}

void bt_forget(const char *path) {
  static const adapter1_remove_device_then then = {bus_done, "Couldn't forget"};
  if (bus_get() != NULL && adapter_path.n > 0)
    adapter1_call_remove_device(bus_get(), adapter_path.p, path, &then);
}
