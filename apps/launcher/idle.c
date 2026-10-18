#include "idle.h"

#include <errno.h>
#include <linux/input.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "apps.h"
#include "layer.h"
#include "media.h"
#include "pad.h"
#include "power.h"
#include "sysd.h"
#include "util.h"
#include "wake.h"

/* The choices, in seconds, 0: never */
static const int dim_steps[] = {0, 15, 30, 60, 120, 300};
static const int off_steps[] = {0, 30, 60, 120, 300, 600, 1800};
static const int sleep_steps[] = {0, 60, 120, 300, 600, 1800, 3600};
/* The power key that turned the screen on is let go after it: sysd mustn't sleep on that */
enum { POWER_GRACE_MS = 2000 };

static struct {
  int dim, off, sleep;
  bool music;
} set = {60, 120, 600, true};

enum { AWAKE, DIMMED, OFF };
static int state;
/* The lid closed while music plays: the screen off till it opens */
static bool lid_closed, lid_off;
/* What the screen is: what was last set */
static bool screen_on = true, dimmed;
/* A Suspend asked for, not answered yet */
static bool sleeping;
/* Since: a key; the music stopping; the screen woken */
static atomic_uint_fast64_t last_input;
static atomic_bool resting; /* state != AWAKE: a key wakes the UI */
static uint64_t quiet_from, woke_at;
static bool was_playing;
/* sysd's SetSleepHold, as sent */
static struct {
  bool lid, power, sent;
} held;
static bus_call hold_call, sleep_call;

static bool music_playing(void) {
  const media_player *p = media_current();
  return p != NULL && p->playing;
}

static void order(int changed);

static void load(void) {
  FILE *f = config_file("-idle", "r");
  if (f == NULL)
    return;
  int dim, off, sleep, music;
  if (fscanf(f, "%d %d %d %d", &dim, &off, &sleep, &music) == 4)
    set = (typeof(set)){dim, off, sleep, music != 0};
  order(IDLE_DIM);
  fclose(f);
}

static void save(void) {
  FILE *f = config_file("-idle", "w");
  if (f == NULL)
    return;
  fprintf(f, "%d %d %d %d\n", set.dim, set.off, set.sleep, set.music);
  fclose(f);
}

/* The screen as the state wants it: undimmed before it comes on, so it doesn't flash dim */
static void apply(void) {
  const bool want_on = state != OFF && !lid_off, want_dim = state != AWAKE;
  if (!want_dim && dimmed)
    power_dim(dimmed = false);
  if (want_on != screen_on) {
    layer_screen(screen_on = want_on);
    if (want_on)
      woke_at = ms_now();
  }
  if (want_dim && !dimmed && screen_on)
    power_dim(dimmed = true);
  atomic_store(&resting, state != AWAKE);
}

static void wake(uint64_t now) {
  atomic_store(&last_input, now);
  state = AWAKE;
  apply();
}

static void *lid_thread(void *arg) {
  const int fd = (int)(intptr_t)arg;
  struct input_event e;
  for (;;) {
    const ssize_t n = read(fd, &e, sizeof(e));
    if (n < 0 && errno == EINTR)
      continue;
    if (n != sizeof(e))
      break;
    if (e.type == EV_SW && e.code == SW_LID)
      win_wake(UI_LID, e.value);
  }
  close(fd);
  return NULL;
}

void idle_init(void) {
  load();
  atomic_store(&last_input, ms_now());
  /* A launcher before this one may have left it off */
  layer_screen(true);
  const int lid = input_open(S("gpio-keys-lid"), 0);
  if (lid < 0)
    return;
  unsigned char sw[SW_MAX / 8 + 1] = {0};
  if (ioctl(lid, EVIOCGSW(sizeof(sw)), sw) >= 0)
    lid_closed = sw[SW_LID / 8] & (1 << (SW_LID % 8));
  spawn("lid", lid_thread, (void *)(intptr_t)lid);
}

void idle_poke(void) {
  atomic_store(&last_input, ms_now());
  if (atomic_load(&resting))
    win_wake(UI_ACTIVE, 0);
}

void idle_event(int why, int value) {
  const uint64_t now = ms_now();
  if (why == UI_LID) {
    lid_closed = value != 0;
    /* Closed: sysd sleeps, unless music holds it (then the screen goes off) */
    lid_off = lid_closed && held.lid;
    if (lid_closed && !lid_off && dimmed)
      power_dim(dimmed = false); /* save-settings keeps the brightness at sleep */
  }
  if (!lid_closed)
    wake(now);
  else
    apply();
}

bool idle_screen_off(void) { return !screen_on; }

static void hold_done(bus_call *call, const bus_error *error) {
  (void)call;
  if (error != NULL)
    held.sent = false;
}

static void slept(bus_call *call, const bus_error *error) {
  (void)call, (void)error;
  sleeping = false;
  wake(ms_now());
}

/* sysd's hold as it should be: the lid while music plays, the power key while the screen is off
 * (and just after it came on) */
static void hold(uint64_t now) {
  bus_conn *bus = sysd_bus();
  const bool lid = set.music && music_playing(),
             power = (state == OFF && !lid_closed) || now < woke_at + POWER_GRACE_MS;
  if (bus == NULL) {
    held.sent = false;
    return;
  }
  if (held.sent && held.lid == lid && held.power == power)
    return;
  if (held.lid && !lid && lid_closed && dimmed)
    power_dim(dimmed = false); /* about to sleep: save-settings keeps the brightness */
  held.sent = system1_call_set_sleep_hold(bus, lid, power, &hold_call, hold_done) >= 0;
  held.lid = lid;
  held.power = power;
}

static void earliest(uint64_t *next, uint64_t at, uint64_t now) {
  if (at > now && (*next == 0 || at < *next))
    *next = at;
}

int idle_tick(uint64_t now) {
  const bool playing = set.music && music_playing();
  if (was_playing && !playing)
    quiet_from = now;
  was_playing = playing;
  if (layer_inhibited)
    atomic_store(&last_input, now);
  if (sleeping)
    return -1;
  const uint64_t last = atomic_load(&last_input);
  uint64_t next = 0;

  int want = AWAKE;
  if (set.dim > 0 && now >= last + set.dim * 1000ull)
    want = DIMMED;
  if (set.off > 0 && now >= last + set.off * 1000ull)
    want = OFF;
  if (want != state) {
    state = want;
    apply();
  }
  if (set.dim > 0)
    earliest(&next, last + set.dim * 1000ull, now);
  if (set.off > 0)
    earliest(&next, last + set.off * 1000ull, now);
  earliest(&next, woke_at + POWER_GRACE_MS, now);
  hold(now);

  /* Asleep, unless music plays or the lid is closed (sysd's then) */
  if (set.sleep > 0 && !playing && !lid_closed) {
    const uint64_t at = MAX(last, quiet_from) + set.sleep * 1000ull;
    bus_conn *bus = sysd_bus();
    if (now >= at && bus != NULL) {
      if (dimmed)
        power_dim(dimmed = false); /* save-settings keeps the brightness at sleep */
      sleeping = system1_call_suspend(bus, &sleep_call, slept) >= 0;
      if (!sleeping)
        wake(now);
      return -1;
    }
    earliest(&next, at, now);
  }
  return next != 0 ? (int)(next - now) : -1;
}

/* --- settings ------------------------------------------------------------ */

/* A row's: each its own text, a page shows them together */
static str duration(int row, int s) {
  static STRBUF(16) out[IDLE_ROWS];
  out[row].n = 0;
  if (s == 0)
    return S("Never");
  if (s < 60)
    sb_fmt(out[row], "%d s", s);
  else if (s < 3600)
    sb_fmt(out[row], "%d min", s / 60);
  else
    sb_fmt(out[row], "%d h", s / 3600);
  return sb_str(out[row]);
}

str idle_value(int row) {
  switch (row) {
  case IDLE_DIM: return duration(row, set.dim);
  case IDLE_OFF: return duration(row, set.off);
  case IDLE_SLEEP: return duration(row, set.sleep);
  }
  return S("");
}

bool idle_music(void) { return set.music; }

static const struct {
  const int *steps;
  int count;
} choices[] = {
    [IDLE_DIM] = {dim_steps, COUNTOF(dim_steps)},
    [IDLE_OFF] = {off_steps, COUNTOF(off_steps)},
    [IDLE_SLEEP] = {sleep_steps, COUNTOF(sleep_steps)},
};

static int *time_of(int row) { return row == IDLE_DIM ? &set.dim : row == IDLE_OFF ? &set.off : &set.sleep; }

/* Dim, off, sleep in order, those not Never: the others moved to the nearest choice that keeps it,
 * away from the one just changed. The same time is fine: the latter's then (off, not dim; asleep) */
static void order(int changed) {
  for (int i = changed + 1; i <= IDLE_SLEEP; i++) {
    int *t = time_of(i);
    for (int before = i - 1; before >= IDLE_DIM && *t != 0; before--)
      if (*time_of(before) != 0) {
        for (int k = 0; *t < *time_of(before) && k < choices[i].count; k++)
          *t = choices[i].steps[k];
        break;
      }
  }
  for (int i = changed - 1; i >= IDLE_DIM; i--) {
    int *t = time_of(i);
    for (int after = i + 1; after <= IDLE_SLEEP && *t != 0; after++)
      if (*time_of(after) != 0) {
        for (int k = choices[i].count - 1; *t > *time_of(after) && k > 0; k--)
          *t = choices[i].steps[k];
        break;
      }
  }
}

void idle_step(int row, int by) {
  if (row == IDLE_MUSIC)
    set.music = !set.music;
  else {
    int *t = time_of(row), at = 0;
    const int count = choices[row].count;
    for (int i = 0; i < count; i++)
      if (choices[row].steps[i] == *t)
        at = i;
    *t = choices[row].steps[(at + by + count) % count];
    order(row);
  }
  save();
  atomic_store(&last_input, ms_now());
}
