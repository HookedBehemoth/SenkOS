#include "bluetooth.h"

#include <stdlib.h>
#include <string.h>

#include "adapter1.h"
#include "apps.h"
#include "bus.h"
#include "util.h"

static bt_device devices[BT_MAX];
static unsigned seq;
static STRBUF(64) adapter_path; /* "" without one */
static adapter1_props adapter;
/* The pairing screen is up; we asked for the search that's on */
static bool pairing, searching;
static bool paired_new, dirty;
/* GetManagedObjects; the adapter's own calls */
static bus_call loading;
static bus_call search;
static bus_said power_said, forget_said = {.what = "Couldn't forget"};

static void changed(void) { dirty = true; }

/* Powered as the player left it, off until turned on (BlueZ neither keeps it nor powers on:
 * AutoEnable=false) */
static bool wanted_on(void) {
  const str text = str_read(config_file("-bluetooth", "r"));
  str rest = text;
  const bool on = str_eq(str_cut(&rest, '\n'), S("on"));
  free((char *)text.p);
  return on;
}

bool bt_changed(void) {
  const bool was = dirty;
  dirty = false;
  return was;
}

static bt_device *find(const char *path) {
  for (size_t i = 0; i < BT_MAX; i++)
    if (devices[i].used && strcmp(devices[i].path.p, path) == 0)
      return &devices[i];
  return NULL;
}

static void drop(bt_device *d) {
  bus_call_cancel(&d->call);
  d->used = false;
}

static void searched(bus_call *call, const bus_error *error) {
  (void)call;
  if (error != NULL)
    searching = false;
}

/* What pairing wants of the adapter: searching, and pairable (BlueZ makes it so with our agent) */
static void sync_adapter(bus_conn *bus) {
  if (bus == NULL || adapter_path.n == 0)
    return;
  if (adapter.pairable != pairing)
    adapter1_set_pairable(bus, adapter_path.p, pairing, NULL, NULL);
  if (pairing && adapter.powered && !adapter.discovering && !searching)
    searching = adapter1_call_start_discovery(bus, adapter_path.p, &search, searched) >= 0;
  else if (!pairing && searching) {
    adapter1_call_stop_discovery(bus, adapter_path.p, NULL, NULL);
    searching = false;
  }
}

static void added(const char *path, const char *iface, bus_rd *props) {
  if (strcmp(iface, ADAPTER1_NAME) == 0) {
    if (adapter_path.n == 0)
      sb_set(adapter_path, str_c(path));
    if (strcmp(adapter_path.p, path) != 0)
      return;
    adapter1_props_apply(&adapter, props, NULL);
    if (!adapter.powered && wanted_on())
      bt_power(true);
    sync_adapter(bus_get());
  } else if (strcmp(iface, DEVICE1_NAME) == 0) {
    bt_device *d = find(path);
    for (size_t i = 0; d == NULL && i < BT_MAX; i++)
      if (!devices[i].used) {
        d = &devices[i];
        *d = (bt_device){.used = true, .seq = ++seq};
        sb_set(d->path, str_c(path));
      }
    if (d == NULL)
      return;
    d->gone = false;
    device1_props_apply(&d->p, props, NULL);
  } else
    return;
  changed();
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
      drop(d);
  }
  changed();
}

static void adapter_changed(const char *path, bus_rd *props) {
  uint32_t bits;
  if (strcmp(adapter_path.p, path) != 0 || adapter1_props_apply(&adapter, props, &bits) < 0 || bits == 0)
    return;
  /* Its search ended (or someone else's): ours to start again */
  if ((bits & ADAPTER1_PROP_DISCOVERING) && !adapter.discovering)
    searching = false;
  sync_adapter(bus_get());
  changed();
}

static void device_changed(const char *path, bus_rd *props) {
  uint32_t bits;
  bt_device *d = find(path);
  if (d != NULL && device1_props_apply(&d->p, props, &bits) >= 0 && bits != 0)
    changed();
}

static void connected(bus_conn *bus) {
  adapter1_watch_properties(bus, adapter_changed);
  device1_watch_properties(bus, device_changed);
  bus_watch_objects(bus, "org.bluez", added, removed);
  bt_agent_add(bus);
}

/* bluetoothd came or went: what it has, from scratch */
static void owned(bus_conn *bus, bool owner) {
  for (size_t i = 0; i < BT_MAX; i++)
    drop(&devices[i]);
  adapter_path.n = 0;
  adapter = (adapter1_props){0};
  searching = false;
  bus_call_cancel(&search);
  bus_call_cancel(&loading);
  if (owner) {
    bus_get_objects(bus, "org.bluez", added, &loading);
    bt_agent_register(bus);
  }
  changed();
}

void bt_init(void) { bus_watch("org.bluez", connected, owned); }

size_t bt_list(const bt_device **out, size_t max) {
  const bt_device *all[BT_MAX];
  size_t n = 0;
  for (size_t i = 0; i < BT_MAX; i++) {
    if (!devices[i].used)
      continue;
    size_t at = n++;
    for (; at > 0 && all[at - 1]->seq > devices[i].seq; at--)
      all[at] = all[at - 1];
    all[at] = &devices[i];
  }
  n = MIN(n, max);
  memcpy(out, all, n * sizeof(*out));
  return n;
}

int bt_kind(const bt_device *d) {
  return strncmp(d->p.icon, "audio-", 6) == 0      ? BT_AUDIO
         : strcmp(d->p.icon, "input-gaming") == 0 ? BT_PAD
                                                  : BT_OTHER;
}

int bt_powered(void) { return adapter_path.n == 0 ? -1 : adapter.powered; }
bool bt_searching(void) { return adapter.discovering; }
bool bt_pairing_on(void) { return pairing; }

int bt_bar(void) {
  if (bt_powered() <= 0)
    return -1;
  for (size_t i = 0; i < BT_MAX; i++)
    if (devices[i].used && devices[i].p.connected)
      return 1;
  return 0;
}

void bt_power(bool on) {
  FILE *f = config_file("-bluetooth", "w");
  if (f != NULL) {
    fputs(on ? "on\n" : "off\n", f);
    fclose(f);
  }
  power_said.what = on ? "Couldn't turn Bluetooth on" : "Couldn't turn Bluetooth off";
  if (bus_get() != NULL && adapter_path.n > 0)
    adapter1_set_powered(bus_get(), adapter_path.p, on, &power_said.call, bus_said_done);
}

static void pairing_set(bool on) {
  pairing = on;
  /* What went while pairing goes now */
  for (size_t i = 0; i < BT_MAX; i++)
    if (devices[i].used && devices[i].gone)
      drop(&devices[i]);
  sync_adapter(bus_get());
  changed();
}
void bt_pairing_start(void) { pairing_set(true); }
void bt_pairing_stop(void) { pairing_set(false); }

/* The steps: what each says when it fails */
static void connected_done(bus_call *call, const bus_error *error) {
  const bt_device *d = bus_call_owner(call, bt_device, call);
  if (error != NULL)
    bus_tell(error, d->p.connected ? "Couldn't disconnect" : "Couldn't connect");
  changed();
}

static void trusted(bus_call *call, const bus_error *error) {
  bt_device *d = bus_call_owner(call, bt_device, call);
  if (error != NULL)
    bus_tell(error, "Couldn't trust it");
  /* Connected anyway: only it may not connect by itself */
  device1_call_connect(bus_get(), d->path.p, &d->call, connected_done);
}

static void paired(bus_call *call, const bus_error *error) {
  bt_device *d = bus_call_owner(call, bt_device, call);
  if (error != NULL) {
    bus_tell(error, "Couldn't pair");
    changed();
    return;
  }
  paired_new = true;
  /* Ours to connect again, and it may connect by itself */
  device1_set_trusted(bus_get(), d->path.p, true, &d->call, trusted);
}

void bt_connect(const char *path) {
  bt_device *d = find(path);
  if (d == NULL || bus_call_pending(&d->call) || bus_get() == NULL)
    return;
  if (d->p.connected)
    device1_call_disconnect(bus_get(), path, &d->call, connected_done);
  else
    device1_call_connect(bus_get(), path, &d->call, connected_done);
  changed();
}

void bt_pair(const char *path) {
  bt_device *d = find(path);
  if (d == NULL || bus_call_pending(&d->call) || bus_get() == NULL)
    return;
  device1_call_pair(bus_get(), path, &d->call, paired);
  changed();
}

bool bt_paired_new(void) {
  const bool was = paired_new;
  paired_new = false;
  return was;
}

/* Not the device's call: it goes with the device */
void bt_forget(const char *path) {
  if (bus_get() != NULL && adapter_path.n > 0)
    adapter1_call_remove_device(bus_get(), adapter_path.p, path, &forget_said.call, bus_said_done);
}
