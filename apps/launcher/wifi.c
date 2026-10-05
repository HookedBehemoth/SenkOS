#include "wifi.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include "util.h"

#define CTRL "/run/wpa_supplicant/wlan0"
enum { SCAN_MS = 15000 };

wifi_network wifi_networks[WIFI_MAX];
size_t wifi_count;
static STRBUF(256) status;
static int fd = -1;
static uint64_t scanned_at;

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

str wifi_status(void) { return sb_str(status); }

/* Ours: a datagram socket bound in XDG_RUNTIME_DIR, for the answers */
static bool open_ctrl(void) {
  if (fd >= 0)
    return true;
  const char *run = getenv("XDG_RUNTIME_DIR");
  struct sockaddr_un local = {.sun_family = AF_UNIX}, remote = {.sun_family = AF_UNIX, .sun_path = CTRL};
  snprintf(local.sun_path, sizeof(local.sun_path), "%s/wpa-launcher", run != NULL ? run : "/tmp");
  unlink(local.sun_path);
  fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  const struct timeval two = {.tv_sec = 2};
  if (fd < 0 || bind(fd, (struct sockaddr *)&local, sizeof(local)) != 0 ||
      connect(fd, (struct sockaddr *)&remote, sizeof(remote)) != 0 ||
      setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &two, sizeof(two)) != 0) {
    if (fd >= 0)
      close(fd);
    fd = -1;
    return false;
  }
  return true;
}

/* A request, its answer (the buffer's, until the next); {NULL, 0} when there was none */
static str request(str cmd) {
  static char reply[8192];
  if (!open_ctrl())
    return (str){0};
  if (send(fd, cmd.p, cmd.n, 0) != (ssize_t)cmd.n) {
    close(fd);
    fd = -1;
    return (str){0};
  }
  const ssize_t n = recv(fd, reply, sizeof(reply) - 1, 0);
  if (n <= 0)
    return (str){0};
  reply[n] = '\0';
  return (str){reply, (size_t)n};
}

static bool ok(str cmd) { return str_eq(str_head(request(cmd), 2), S("OK")); }

/* The field `n` of a tab-separated line */
static str field(str line, int n) {
  str f = S("");
  for (int i = 0; i <= n; i++)
    f = str_cut(&line, '\t');
  return f;
}

static int bars(int dbm) { return dbm >= -55 ? 4 : dbm >= -65 ? 3 : dbm >= -75 ? 2 : 1; }

int wifi_signal(void) {
  if (!wifi_on())
    return -1;
  /* "RSSI=-56\nLINKSPEED=..."; FAIL when not on a network */
  str text = request(S("SIGNAL_POLL"));
  while (text.n > 0) {
    str value = str_cut(&text, '\n');
    if (str_eq(str_cut(&value, '='), S("RSSI")))
      return bars(atoi(value.p));
  }
  return 0;
}

static wifi_network *by_ssid(str ssid) {
  for (size_t i = 0; i < wifi_count; i++)
    if (str_eq(sb_str(wifi_networks[i].ssid), ssid))
      return &wifi_networks[i];
  return NULL;
}

void wifi_poll(void) {
  status.n = 0;
  if (!wifi_on()) {
    wifi_count = 0;
    sb_set(status, S("Off"));
    return;
  }
  /* STATUS: key=value lines */
  str text = request(S("STATUS")), ssid = S(""), ip = S(""), state = S("");
  if (text.p == NULL) {
    wifi_count = 0;
    sb_set(status, S("Starting…"));
    return;
  }
  while (text.n > 0) {
    str value = str_cut(&text, '\n');
    const str key = str_cut(&value, '=');
    if (str_eq(key, S("ssid")))
      ssid = value;
    else if (str_eq(key, S("ip_address")))
      ip = value;
    else if (str_eq(key, S("wpa_state")))
      state = value;
  }
  STRBUF(160) current;
  sb_set(current, ssid);
  if (str_eq(state, S("COMPLETED")))
    sb_set(status, S("Connected to "), ssid, ip.n ? S(" · ") : S(""), ip.n ? ip : S("getting an address…"));
  else if (str_eq(state, S("ASSOCIATING")) || str_eq(state, S("AUTHENTICATING")) ||
           str_eq(state, S("4WAY_HANDSHAKE")) || str_eq(state, S("GROUP_HANDSHAKE")) || str_eq(state, S("ASSOCIATED")))
    sb_set(status, S("Connecting…"));
  else
    sb_set(status, S("Not connected"));

  if (scanned_at == 0 || ms_now() - scanned_at >= SCAN_MS) {
    request(S("SCAN"));
    scanned_at = ms_now();
  }
  /* bssid / frequency / signal level / flags / ssid: the strongest of each name */
  wifi_count = 0;
  text = request(S("SCAN_RESULTS"));
  str_cut(&text, '\n');
  while (text.n > 0 && wifi_count < WIFI_MAX) {
    const str line = str_cut(&text, '\n'), name = field(line, 4);
    /* Hidden networks: no name, or NULs */
    if (name.n == 0 || str_eq(str_head(name, 4), S("\\x00")))
      continue;
    const int level = bars(atoi(field(line, 2).p));
    wifi_network *n = by_ssid(name);
    if (n == NULL) {
      n = &wifi_networks[wifi_count++];
      *n = (wifi_network){.bars = level};
      sb_set(n->ssid, name);
    }
    n->bars = MAX(n->bars, level);
    const str flags = field(line, 3);
    n->secured |= str_find(flags, 'W') < flags.n || str_find(flags, 'S') < flags.n;
    n->current = str_eq(name, sb_str(current)) && str_eq(state, S("COMPLETED"));
  }
  /* network id / ssid / bssid / flags: what's saved */
  text = request(S("LIST_NETWORKS"));
  str_cut(&text, '\n');
  while (text.n > 0) {
    wifi_network *n = by_ssid(field(str_cut(&text, '\n'), 1));
    if (n != NULL)
      n->saved = true;
  }
  /* The one it's on first, then by signal */
  for (size_t i = 1; i < wifi_count; i++)
    for (size_t k = i; k > 0; k--) {
      wifi_network *a = &wifi_networks[k - 1], *b = &wifi_networks[k];
      if ((b->current > a->current) || (b->current == a->current && b->bars > a->bars)) {
        const wifi_network t = *a;
        *a = *b;
        *b = t;
      }
    }
}

/* Its id among the saved ones; empty when it isn't */
static str saved_id(str ssid) {
  static STRBUF(16) id;
  id.n = 0;
  str text = request(S("LIST_NETWORKS"));
  str_cut(&text, '\n');
  while (text.n > 0) {
    const str line = str_cut(&text, '\n');
    if (str_eq(field(line, 1), ssid))
      sb_set(id, field(line, 0));
  }
  return sb_str(id);
}

/* The name as hex, its bytes as wpa_supplicant escaped them (\xNN, \\, \") undone: no quoting */
static str hex_ssid(str ssid) {
  static STRBUF(80) out;
  out.n = 0;
  for (size_t i = 0; i < ssid.n; i++) {
    unsigned byte = (unsigned char)ssid.p[i];
    if (byte == '\\' && i + 1 < ssid.n && ssid.p[i + 1] == 'x' && i + 3 < ssid.n) {
      char two[3] = {ssid.p[i + 2], ssid.p[i + 3], 0};
      byte = (unsigned)strtoul(two, NULL, 16);
      i += 3;
    } else if (byte == '\\' && i + 1 < ssid.n) {
      byte = (unsigned char)ssid.p[++i];
    }
    sb_fmt(out, "%02x", byte);
  }
  return sb_str(out);
}

bool wifi_connect(str ssid, bool secured, str psk) {
  STRBUF(256) cmd;
  str id = saved_id(ssid);
  if (id.n > 0 && psk.n == 0) {
    sb_set(cmd, S("SELECT_NETWORK "), id);
    return ok(sb_str(cmd));
  }
  if (id.n > 0) {
    sb_set(cmd, S("REMOVE_NETWORK "), id);
    request(sb_str(cmd));
  }
  STRBUF(16) added;
  sb_set(added, request(S("ADD_NETWORK")));
  while (added.n > 0 && (added.p[added.n - 1] == '\n'))
    added.p[--added.n] = '\0';
  if (added.n == 0 || added.p[0] < '0' || added.p[0] > '9')
    return false;
  id = sb_str(added);
  bool done = sb_set(cmd, S("SET_NETWORK "), id, S(" ssid "), hex_ssid(ssid)) && ok(sb_str(cmd));
  if (done && secured) {
    /* WPA2 and WPA3; WPA2 alone where it was built without SAE */
    done = sb_set(cmd, S("SET_NETWORK "), id, S(" psk \""), psk, S("\"")) && ok(sb_str(cmd));
    sb_set(cmd, S("SET_NETWORK "), id, S(" key_mgmt WPA-PSK SAE"));
    if (done && ok(sb_str(cmd))) {
      sb_set(cmd, S("SET_NETWORK "), id, S(" ieee80211w 1"));
      ok(sb_str(cmd));
    } else if (done) {
      sb_set(cmd, S("SET_NETWORK "), id, S(" key_mgmt WPA-PSK"));
      done = ok(sb_str(cmd));
    }
  } else if (done) {
    done = sb_set(cmd, S("SET_NETWORK "), id, S(" key_mgmt NONE")) && ok(sb_str(cmd));
  }
  if (!done) {
    sb_set(cmd, S("REMOVE_NETWORK "), id);
    request(sb_str(cmd));
    return false;
  }
  sb_set(cmd, S("SELECT_NETWORK "), id);
  done = ok(sb_str(cmd));
  request(S("SAVE_CONFIG"));
  return done;
}

bool wifi_forget(str ssid) {
  const str id = saved_id(ssid);
  STRBUF(32) cmd;
  if (id.n == 0 || !sb_set(cmd, S("REMOVE_NETWORK "), id) || !ok(sb_str(cmd)))
    return false;
  request(S("SAVE_CONFIG"));
  /* Back on whichever saved one is there */
  request(S("REASSOCIATE"));
  return true;
}
