#include "updates.h"

#include <stdio.h>
#include <string.h>

#include "bus.h"
#include "updates1.h"
#include "wake.h"
#include "win.h"

update_state updates;
static bus_call checking, installing, listing;

str updates_reason(str why) {
  const size_t scheme = str_find(why, ':');
  if (scheme + 2 < why.n && why.p[scheme + 1] == '/' && why.p[scheme + 2] == '/')
    for (size_t i = scheme + 3; i + 1 < why.n; i++)
      if (why.p[i] == ':' && why.p[i + 1] == ' ')
        return str_at(why, i + 2);
  return why;
}

str updates_build(void) {
  static STRBUF(32) build;
  if (build.n == 0) {
    char line[32] = "";
    FILE *f = fopen("/etc/build", "r");
    if (f != NULL && fgets(line, sizeof(line), f) == NULL)
      line[0] = '\0';
    if (f != NULL)
      fclose(f);
    line[strcspn(line, "\n")] = '\0';
    sb_set(build, line[0] ? str_c(line) : S("unknown"));
  }
  return sb_str(build);
}

/* A job's news (its Job signal, or Jobs' at connecting) */
static void job(str object, str id, str state, uint64_t done, uint64_t total, str message) {
  (void)object;
  if (id.n > 5 && memcmp(id.p, "pack:", 5) == 0) {
    sb_set(updates.pack_news, state);
    sb_set(updates.pack_why, message);
    if (str_eq(state, S("done")))
      win_wake(UI_RELOAD, 0);
  } else if (str_eq(id, S("os"))) {
    sb_set(updates.job_state, state);
    updates.done = done;
    updates.total = total;
    sb_set(updates.job_message, message);
    updates.job = str_eq(state, S("done"))                                     ? UPDATE_READY
                  : str_eq(state, S("failed")) || str_eq(state, S("canceled")) ? UPDATE_IDLE
                                                                               : UPDATE_RUNNING;
  }
  win_wake(UI_UPDATES, 0);
}

/* a(sstts): each as Job says it */
static void listed(bus_call *call, const bus_error *error, bus_rd *jobs) {
  (void)call;
  if (error != NULL)
    return;
  const uint8_t *end = bus_rd_array(jobs, 8);
  while (bus_rd_more(jobs, end)) {
    bus_rd_align(jobs, 8);
    const str id = bus_rd_str(jobs), state = bus_rd_str(jobs);
    const uint64_t done = bus_rd_u64(jobs), total = bus_rd_u64(jobs);
    const str message = bus_rd_str(jobs);
    if (!jobs->bad)
      job(S(""), id, state, done, total, message);
  }
}

static void checked(bus_call *call, const bus_error *error, bool newer, str build, uint64_t size, str notes) {
  (void)call;
  if (error != NULL) {
    updates.check = UPDATE_FAILED;
    sb_set(updates.notes, error->message);
  } else if (newer) {
    updates.check = UPDATE_AVAILABLE;
    sb_set(updates.build, build);
    updates.size = size;
    sb_set(updates.notes, notes);
  } else
    updates.check = UPDATE_CURRENT;
  win_wake(UI_UPDATES, 0);
}

static void installed(bus_call *call, const bus_error *error) {
  (void)call;
  if (error == NULL)
    return;
  updates.job = UPDATE_IDLE;
  sb_set(updates.job_state, S("failed"));
  sb_set(updates.job_message, error->message);
  win_wake(UI_UPDATES, 0);
}

static void ask_check(bus_conn *bus) {
  if (updates1_call_check(bus, &checking, checked) < 0) {
    updates.check = UPDATE_FAILED;
    sb_set(updates.notes, S("can't ask downloadd"));
  }
}

static void ask_pack(bus_conn *bus) {
  updates1_call_pack(bus, sb_str(updates.pack), NULL, NULL);
  updates.pack.n = 0;
}

static void connected(bus_conn *bus) { updates1_watch_job(bus, job); }

static void owned(bus_conn *bus, bool owner) {
  updates.connected = owner;
  if (owner) {
    updates1_call_jobs(bus, &listing, listed);
    /* Asked before it was there */
    if (updates.check == UPDATE_CHECKING)
      ask_check(bus);
    if (updates.pack.n > 0)
      ask_pack(bus);
  }
  win_wake(UI_UPDATES, 0);
}

void updates_init(void) { bus_watch(S(UPDATES1_SERVICE), connected, owned); }

/* The bus, NULL while downloadd isn't on it */
static bus_conn *downloadd(void) { return updates.connected ? bus_get() : NULL; }

void updates_check(void) {
  updates.check = UPDATE_CHECKING;
  bus_conn *bus = downloadd();
  if (bus != NULL)
    ask_check(bus);
}

void updates_install(void) {
  bus_conn *bus = downloadd();
  updates.job = UPDATE_RUNNING;
  sb_set(updates.job_state, S("queued"));
  if (bus == NULL || updates1_call_update(bus, &installing, installed) < 0)
    installed(NULL, &(bus_error){S(""), S("downloadd isn't running")});
}

void updates_pack(str name) {
  sb_set(updates.pack, name);
  bus_conn *bus = downloadd();
  if (bus != NULL)
    ask_pack(bus);
}
