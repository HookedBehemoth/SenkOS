#include "media.h"

#include <signal.h>
#include <unistd.h>

#include "bus.h"
#include "launcher_internal.h"
#include "mpris.h"
#include "properties.h"
#include "util.h"

#define DBUS "org.freedesktop.DBus"
#define PLAYERS "org.mpris.MediaPlayer2"

static media_player players[MEDIA_MAX];
static uint64_t order;
static bool dirty;
static bus_call listing;

bool media_changed(void) {
  const bool was = dirty;
  dirty = false;
  return was;
}

/* By its name or its owner ("": not by that) */
static media_player *by(str name, str owner) {
  for (size_t i = 0; i < MEDIA_MAX; i++)
    if (players[i].used && ((name.n > 0 && str_eq(sb_str(players[i].name), name)) ||
                            (owner.n > 0 && str_eq(sb_str(players[i].owner), owner))))
      return &players[i];
  return NULL;
}

static void drop(media_player *p) {
  bus_call_cancel(&p->props);
  bus_call_cancel(&p->pid_call);
  p->used = false;
  dirty = true;
}

/* Its app: the one it runs in (an app's processes share its session) */
static running *app_of(const media_player *p) {
  const pid_t group = p->pid > 0 ? getsid(p->pid) : -1;
  for (size_t i = 0; group > 0 && i < RUNNING_MAX; i++)
    if (runs[i].pid == group)
      return &runs[i];
  return NULL;
}

bool media_playing(pid_t group) {
  for (size_t i = 0; i < MEDIA_MAX; i++)
    if (players[i].used && players[i].playing && players[i].pid > 0 && getsid(players[i].pid) == group)
      return true;
  return false;
}

const media_player *media_current(void) {
  const media_player *best = NULL;
  for (size_t i = 0; i < MEDIA_MAX; i++) {
    const media_player *p = &players[i];
    if (p->used && (best == NULL || p->playing > best->playing ||
                    (p->playing == best->playing && p->seen > best->seen)))
      best = p;
  }
  return best;
}

/* xesam:title, the first of xesam:artist, mpris:trackid */
static void metadata(media_player *p, bus_rd *r) {
  sb_set(p->title, S(""));
  sb_set(p->artist, S(""));
  sb_set(p->track, S(""));
  const uint8_t *end = bus_rd_array(r, 8);
  str key, sig;
  while (bus_rd_kv(r, end, &key, &sig)) {
    if (str_eq(key, S("xesam:title")) && str_eq(sig, S("s")))
      sb_set(p->title, bus_rd_str(r));
    else if (str_eq(key, S("mpris:trackid")) && (str_eq(sig, S("o")) || str_eq(sig, S("s"))))
      sb_set(p->track, bus_rd_str(r));
    else if (str_eq(key, S("xesam:artist")) && str_eq(sig, S("as"))) {
      const uint8_t *names = bus_rd_array(r, 4);
      while (bus_rd_more(r, names)) {
        const str name = bus_rd_str(r);
        if (p->artist.n == 0)
          sb_set(p->artist, name);
      }
    } else
      bus_skip(r, sig);
  }
}

/* GetAll's or PropertiesChanged's a{sv} */
static void apply(media_player *p, bus_rd *r) {
  const uint8_t *end = bus_rd_array(r, 8);
  str key, sig;
  while (bus_rd_kv(r, end, &key, &sig)) {
    if (str_eq(key, S("PlaybackStatus")) && str_eq(sig, S("s")))
      p->playing = str_eq(bus_rd_str(r), S("Playing"));
    else if (str_eq(key, S("Metadata")) && str_eq(sig, S("a{sv}")))
      metadata(p, r);
    else if (str_eq(key, S("CanGoNext")) && str_eq(sig, S("b")))
      p->can_next = bus_rd_bool(r);
    else if (str_eq(key, S("CanGoPrevious")) && str_eq(sig, S("b")))
      p->can_previous = bus_rd_bool(r);
    else
      bus_skip(r, sig);
  }
  dirty = true;
}

static void got_all(bus_call *call, const bus_error *error, bus_rd *props) {
  media_player *p = bus_call_owner(call, media_player, props);
  if (error == NULL)
    apply(p, props);
}

static void got_pid(bus_call *call, const bus_msg *m) {
  bus_error buf;
  bus_rd r = m->body;
  const uint32_t pid = bus_rd_u32(&r);
  if (bus_msg_error(m, &buf) == NULL && str_eq(m->sig, S("u")) && !r.bad)
    bus_call_owner(call, media_player, pid_call)->pid = (pid_t)pid;
}

/* Its properties and its process, from its unique name */
static void fetch(media_player *p) {
  bus_conn *bus = bus_session();
  if (bus == NULL)
    return;
  properties_call_get_all(bus, sb_str(p->owner), S(MPRIS_PATH), S(MPRIS_NAME), &p->props, got_all);
  bus_wr_str(bus_call_begin(bus, S(DBUS), S("/org/freedesktop/DBus"), S(DBUS), S("GetConnectionUnixProcessID"), "s", true),
             sb_str(p->owner));
  bus_call_end(bus, &p->pid_call, got_pid, NULL);
}

static media_player *add(str name, str owner) {
  media_player *p = by(name, S(""));
  for (size_t i = 0; p == NULL && i < MEDIA_MAX; i++)
    if (!players[i].used)
      p = &players[i];
  if (p == NULL)
    return NULL;
  if (p->used)
    drop(p);
  *p = (media_player){.used = true, .seen = ++order};
  sb_set(p->name, name);
  sb_set(p->owner, owner);
  dirty = true;
  return p;
}

static void got_owner(bus_call *call, const bus_msg *m) {
  media_player *p = bus_call_owner(call, media_player, props);
  bus_error buf;
  bus_rd r = m->body;
  const str owner = bus_rd_str(&r);
  if (bus_msg_error(m, &buf) != NULL || !str_eq(m->sig, S("s")) || r.bad)
    drop(p);
  else if (sb_set(p->owner, owner))
    fetch(p);
}

static bool is_player(str name) {
  return name.n > sizeof(PLAYERS) && str_eq(str_head(name, sizeof(PLAYERS)), S(PLAYERS "."));
}

static void listed(bus_call *call, const bus_msg *m) {
  (void)call;
  bus_error buf;
  bus_conn *bus = bus_session();
  bus_rd r = m->body;
  if (bus == NULL || bus_msg_error(m, &buf) != NULL || !str_eq(m->sig, S("as")))
    return;
  const uint8_t *end = bus_rd_array(&r, 4);
  while (bus_rd_more(&r, end)) {
    const str name = bus_rd_str(&r);
    media_player *p = !r.bad && is_player(name) ? add(name, S("")) : NULL;
    if (p == NULL)
      continue;
    bus_wr_str(bus_call_begin(bus, S(DBUS), S("/org/freedesktop/DBus"), S(DBUS), S("GetNameOwner"), "s", true), name);
    bus_call_end(bus, &p->props, got_owner, NULL);
  }
}

static void owner_changed(bus_match *match, const bus_msg *m) {
  (void)match;
  bus_rd r = m->body;
  const str name = bus_rd_str(&r), before = bus_rd_str(&r), now = bus_rd_str(&r);
  (void)before;
  if (r.bad || !str_eq(m->sig, S("sss")) || !is_player(name))
    return;
  media_player *p = now.n > 0 ? add(name, now) : by(name, S(""));
  if (p != NULL && now.n > 0)
    fetch(p);
  else if (p != NULL)
    drop(p);
}

/* A new track playing, its app not in front: named on the OSD */
static void properties_changed(bus_match *match, const bus_msg *m) {
  (void)match;
  bus_rd r = m->body;
  media_player *p = by(S(""), m->sender);
  if (p == NULL || !str_eq(m->sig, S("sa{sv}as")) || !str_eq(m->path, S(MPRIS_PATH)) ||
      !str_eq(bus_rd_str(&r), S(MPRIS_NAME)))
    return;
  typeof(p->title) title = p->title, track = p->track;
  const bool was = p->playing;
  apply(p, &r);
  if (p->playing && !was)
    p->seen = ++order;
  const bool fresh = !str_eq(sb_str(track), sb_str(p->track)) || !str_eq(sb_str(title), sb_str(p->title));
  const running *app = app_of(p);
  if (p->playing && fresh && p->title.n > 0 && (app == NULL || app != launcher.front))
    osd_show_track(sb_str(p->title), sb_str(p->artist));
}

static bus_match owner_match = {.iface = S(DBUS), .member = S("NameOwnerChanged"), .fn = owner_changed},
                 changed_match = {.iface = S(PROPERTIES_NAME), .member = S("PropertiesChanged"), .fn = properties_changed};

static void connected(bus_conn *bus) {
  bus_add_match(bus, &owner_match,
                S("type='signal',sender='" DBUS "',interface='" DBUS "',member='NameOwnerChanged',arg0namespace='" PLAYERS "'"));
  bus_add_match(bus, &changed_match,
                S("type='signal',interface='" PROPERTIES_NAME "',member='PropertiesChanged',path='" MPRIS_PATH
                  "',arg0='" MPRIS_NAME "'"));
  bus_call_begin(bus, S(DBUS), S("/org/freedesktop/DBus"), S(DBUS), S("ListNames"), "", true);
  bus_call_end(bus, &listing, listed, NULL);
}

static void lost(void) {
  for (size_t i = 0; i < MEDIA_MAX; i++)
    if (players[i].used)
      drop(&players[i]);
}

void media_init(void) { bus_on_session(connected, lost); }

/* To the player shown; its app continued first, in case it was stopped behind the launcher */
static void control(int (*call)(bus_conn *bus, str service, bus_call *call, mpris_next_done *done)) {
  const media_player *p = media_current();
  bus_conn *bus = bus_session();
  if (p == NULL || bus == NULL)
    return;
  const running *app = app_of(p);
  if (app != NULL)
    kill(-app->pid, SIGCONT);
  call(bus, sb_str(p->owner), NULL, NULL);
}

void media_play_pause(void) { control(mpris_call_play_pause); }
void media_next(void) { control(mpris_call_next); }
void media_previous(void) { control(mpris_call_previous); }
