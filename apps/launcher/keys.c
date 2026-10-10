#include "keys.h"

#include <errno.h>
#include <linux/input.h>
#include <poll.h>
#include <pulse/pulseaudio.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>

#include "appctl.h"
#include "pad.h"
#include "util.h"
#include "wake.h"

enum { VOLUME_STEP = PA_VOLUME_NORM / 16 };
static struct {
  pa_mainloop_api *api;
  pa_context *ctx;
  pa_cvolume volume; /* channels 0: not known yet */
  bool muted;
  uint32_t sink;
  int sets; /* in flight: replies older than them are stale */
} vol = {.sink = PA_INVALID_INDEX};
/* The pad, for Menu held: its state, not its queue (pad_thread's) */
static int pad = -1;

static void volume_show(void) {
  win_wake(UI_VOLUME, vol.muted ? 0 : (int)((pa_cvolume_max(&vol.volume) * 100ull + PA_VOLUME_NORM / 2) / PA_VOLUME_NORM));
}

static void volume_info(pa_context *c, const pa_sink_info *i, int eol, void *u) {
  (void)c, (void)u, (void)eol;
  if (i == NULL || vol.sets > 0)
    return;
  const bool changed = vol.volume.channels != 0 &&
                       (vol.muted != (bool)i->mute || !pa_cvolume_equal(&vol.volume, &i->volume));
  vol.volume = i->volume;
  vol.muted = i->mute;
  vol.sink = i->index;
  if (changed)
    volume_show();
}

static void volume_refresh(pa_context *c) {
  pa_operation *o = pa_context_get_sink_info_by_name(c, "@DEFAULT_SINK@", volume_info, NULL);
  if (o != NULL)
    pa_operation_unref(o);
}

static void volume_event(pa_context *c, pa_subscription_event_type_t t, uint32_t idx, void *u) {
  (void)u;
  if ((t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) == PA_SUBSCRIPTION_EVENT_SERVER || idx == vol.sink)
    volume_refresh(c);
}

static void volume_set_done(pa_context *c, int ok, void *u) {
  (void)c, (void)ok, (void)u;
  vol.sets--;
}

static void volume_connect(void);
static void volume_retry(pa_mainloop_api *api, pa_time_event *e, const struct timeval *tv, void *u) {
  (void)tv, (void)u;
  api->time_free(e);
  volume_connect();
}

static void volume_state(pa_context *c, void *u) {
  (void)u;
  const pa_context_state_t s = pa_context_get_state(c);
  if (s == PA_CONTEXT_READY) {
    pa_context_set_subscribe_callback(c, volume_event, NULL);
    pa_operation *o = pa_context_subscribe(c, PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SERVER, NULL, NULL);
    if (o != NULL)
      pa_operation_unref(o);
    volume_refresh(c);
  } else if (!PA_CONTEXT_IS_GOOD(s)) { /* the server went away: again in a second */
    pa_context_unref(c);
    vol.ctx = NULL;
    vol.volume.channels = 0;
    vol.sink = PA_INVALID_INDEX;
    vol.sets = 0;
    struct timeval tv;
    vol.api->time_new(vol.api, pa_timeval_add(pa_gettimeofday(&tv), PA_USEC_PER_SEC), volume_retry, NULL);
  }
}

/* PULSE_SERVER (run_drop_root): libpulse ignores an XDG_RUNTIME_DIR that
 * isn't ours. NOFAIL's waiting needs D-Bus: volume_state retries instead (the
 * launcher starts first). */
static void volume_connect(void) {
  vol.ctx = pa_context_new(vol.api, "launcher");
  pa_context_set_state_callback(vol.ctx, volume_state, NULL);
  pa_context_connect(vol.ctx, NULL, PA_CONTEXT_NOAUTOSPAWN, NULL); /* failing: FAILED, volume_state */
}

/* A press or a held key's repeat: a step, up unmutes */
static void volume_key(pa_mainloop_api *api, pa_io_event *e, int fd, pa_io_event_flags_t f, void *u) {
  (void)api, (void)e, (void)f, (void)u;
  struct input_event ev;
  if (read(fd, &ev, sizeof(ev)) != sizeof(ev) || ev.type != EV_KEY || ev.value == 0 ||
      (ev.code != KEY_VOLUMEUP && ev.code != KEY_VOLUMEDOWN))
    return;
  const int by = ev.code == KEY_VOLUMEUP ? 1 : -1;
  if (pad >= 0 && pad_held(pad) & (1u << APPCTL_MENU)) {
    win_wake(UI_BRIGHTNESS, by);
    return;
  }
  if (vol.ctx == NULL || pa_context_get_state(vol.ctx) != PA_CONTEXT_READY || vol.volume.channels == 0)
    return;
  const int step = (int)((pa_cvolume_max(&vol.volume) + VOLUME_STEP / 2) / VOLUME_STEP) + by;
  pa_cvolume_scale(&vol.volume, (pa_volume_t)(CLAMP(step, 0, 16) * VOLUME_STEP));
  pa_operation *o = pa_context_set_sink_volume_by_index(vol.ctx, vol.sink, &vol.volume, volume_set_done, NULL);
  if (o != NULL) {
    vol.sets++;
    pa_operation_unref(o);
  }
  if (by > 0 && vol.muted) {
    vol.muted = false;
    if ((o = pa_context_set_sink_mute_by_index(vol.ctx, vol.sink, 0, volume_set_done, NULL)) != NULL) {
      vol.sets++;
      pa_operation_unref(o);
    }
  }
  volume_show();
}

void *volume_thread(void *keys) {
  pad = pad_open(0);
  pa_mainloop *ml = pa_mainloop_new();
  vol.api = pa_mainloop_get_api(ml);
  vol.api->io_new(vol.api, (int)(intptr_t)keys, PA_IO_EVENT_INPUT, volume_key, NULL);
  volume_connect();
  pa_mainloop_run(ml, NULL);
  return NULL;
}

enum { POWER_HOLD_MS = 1000 };
void *power_key_thread(void *key) {
  const int fd = (int)(intptr_t)key;
  struct input_event e;
  uint64_t until = 0; /* 0: up */
  for (;;) {
    const int wait = until == 0 ? -1 : (int)(until - MIN(until, ms_now()));
    const int ready = poll(&(struct pollfd){.fd = fd, .events = POLLIN}, 1, wait);
    if (ready == 0) {
      win_wake(UI_POWER_HOLD, 0);
      until = 0;
      continue;
    }
    if (ready < 0 && errno == EINTR)
      continue;
    if (ready < 0 || read(fd, &e, sizeof(e)) != sizeof(e))
      break;
    if (e.type == EV_KEY && e.code == KEY_POWER && e.value != 2)
      until = e.value ? ms_now() + POWER_HOLD_MS : 0;
  }
  close(fd);
  return NULL;
}
