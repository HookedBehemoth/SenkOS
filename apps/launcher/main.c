/* Launcher startup, event loop, and frame submission. */
#include <errno.h>
#include <glob.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <time.h>
#include <unistd.h>

#include "apps.h"
#include "assets.h"
#include "capture.h"
#include "clock.h"
#include "gfx.h"
#include "keys.h"
#include "launcher_internal.h"
#include "layer.h"
#include "pad.h"
#include "perf.h"
#include "power.h"
#include "romdb.h"
#include "sysd.h"
#include "util.h"
#include "wake.h"
#include "watch.h"
#include "bluetooth.h"
#include "wifi.h"

#define SLEEP_FAILED "/run/sleep-failed"

#define HOME_KEY S("home")

static void redraw(win *w);
win window = {.app_id = "launcher",
              .title = "Launcher",
              .w = 640,
              .h = 480,
              .configure = redraw};
static gfx_target *screen;
static ui_context ctx;

static int read_line(const char *path, char *out, size_t size) {
  FILE *f = fopen(path, "r");
  if (f == NULL)
    return -1;
  const bool ok = fgets(out, (int)size, f) != NULL;
  fclose(f);
  return ok ? 0 : -1;
}

static void read_status(ui_status *s) {
  char line[256];
  s->battery = -1;
  s->charging = false;
  glob_t g;
  if (glob("/sys/class/power_supply/*/capacity", 0, NULL, &g) == 0) {
    if (read_line(g.gl_pathv[0], line, sizeof(line)) == 0)
      s->battery = atoi(line);
    char path[512];
    snprintf(path, sizeof(path), "%.*s/status",
             (int)(strrchr(g.gl_pathv[0], '/') - g.gl_pathv[0]), g.gl_pathv[0]);
    s->charging = read_line(path, line, sizeof(line)) == 0 &&
                  strncmp(line, "Charging", 8) == 0;
    globfree(&g);
  }
  s->wifi = wifi_signal();
  s->bluetooth = bt_bar();
}

static void redraw(win *w) {
  (void)w;
  win_wake(UI_CONFIGURED, 0);
}

/* Weston's socket, which may not be there yet: Weston and the launcher start
 * together. Watched before it's looked for, so its creation can't slip in
 * between. */
static void wait_for_display(void) {
  const char *display = getenv("WAYLAND_DISPLAY"),
             *run = getenv("XDG_RUNTIME_DIR");
  char path[PATH_MAX];
  if (display == NULL)
    display = "wayland-0";
  if (display[0] == '/')
    snprintf(path, sizeof(path), "%s", display);
  else if (run != NULL)
    snprintf(path, sizeof(path), "%s/%s", run, display);
  else
    return;
  char dir[PATH_MAX];
  snprintf(dir, sizeof(dir), "%.*s", (int)(strrchr(path, '/') - path), path);
  const int fd = inotify_init1(IN_CLOEXEC);
  if (fd < 0 || inotify_add_watch(fd, dir, IN_CREATE) < 0) {
    if (fd >= 0)
      close(fd);
    return;
  }
  char events[1024];
  while (access(path, F_OK) != 0 && read(fd, events, sizeof(events)) > 0)
    ;
  close(fd);
}

int main(int argc, char **argv) {
  if (argc == 3 && strcmp(argv[1], "launch") == 0)
    return control_request_launch(argv[2]);
  if (argc == 3 && strcmp(argv[1], "screenshot") == 0)
    return capture_ppm(argv[2]) ? 0 : 1;
  if (argc != 1) {
    fprintf(stderr, "usage: launcher [launch PATH]\n");
    return 2;
  }
  if (!run_drop_root()) {
    fprintf(stderr, "launcher: can't become shell: %s\n", strerror(errno));
    return 1;
  }
  if (!gfx_init(window.title) || !ui_init("sans-serif")) {
    fprintf(stderr, "launcher: can't draw\n");
    return 1;
  }
  assets_init();
  wait_for_display();
  if (!win_connect(launcher_key)) {
    fprintf(stderr, "launcher: no Wayland display\n");
    return 1;
  }
  layer_connect();
  win_map(&window);
  if ((screen = gfx_target_create(&window, false)) == NULL) {
    fprintf(stderr, "launcher: can't draw\n");
    return 1;
  }
  romdb_init(); /* its wakes need the connection */
  for (int b = 0; b < APPCTL_BUTTONS; b++)
    button_names[b] = str_c(appctl_button_names[b]);
  for (int c = 0; c < APPCTL_CATEGORIES; c++)
    category_names[c] = str_c(appctl_category_names[c]);
  category_names[POWER_PAGE] = S("Power");
  category_names[DEBUG_PAGE] = S("Debug");
  pad_listen(true);
  pad_start();
  /* The bus' names watched, before it's used */
  sysd_init();
  bt_init();
  wifi_init();
  power_init();
  perf_init();
  power_use(HOME_KEY);
  clock_refresh();
  const int volume_keys = input_open(S("gpio-keys-volume"), 0);
  if (volume_keys >= 0)
    spawn("volume", volume_thread, (void *)(intptr_t)volume_keys);
  const int power_key = input_open(S("axp20x-pek"), 0);
  if (power_key >= 0)
    spawn("power key", power_key_thread, (void *)(intptr_t)power_key);
  if (!control_open())
    fprintf(stderr, "launcher: no launch FIFO\n");
  static nav ui = {.animate = true, .duration = 150};
  launcher.visible = session_home_screens();
  screen_push(launcher.visible, &ui, &home_screen, 0);
  apps_init();
  apps_watch();
  sources_load();
  uint64_t present = sources_present();
  size_t page = home_load_page();
  home_rescan(page);
  ui_status st;
  read_status(&st);
  uint64_t status_at = ms_now();
  storage_refresh_usb();
  clock_refresh();
  watch_init();
  unsigned changed = 0;
  bool told_up = false;
  bool animating = true;

  for (;;) {
    session_check_start();
    /* Searching for networks again every so often */
    const bool seconds =
        !told_up || screen_contains(launcher.visible, &wifi_adding_screen);
    int wait = launcher.front != NULL && !showing_pause() && !session_starting()
                   ? -1
               : animating ? 16
               : seconds   ? 1000
                           : -1;
    str said = S("");
    const int step = power_tick(ms_now(), &said);
    if (said.n > 0)
      osd_show_text(said, power_calibrating() ? INT_MAX / 2 : RESULT_MS);
    if (!power_calibrating())
      osd_limit_notice();
    if (step >= 0 && (wait < 0 || step < wait))
      wait = step;
    static uint64_t perf_due;
    if (perf_on()) {
      const int left = (int)(perf_due - MIN(perf_due, ms_now()));
      if (wait < 0 || left < wait)
        wait = left;
    }
    if (osd_due() != 0) {
      const int left = (int)(osd_due() - MIN(osd_due(), ms_now()));
      if (wait < 0 || left < wait)
        wait = left;
    }
    if (widgets_due() != 0 && (launcher.front == NULL || showing_pause())) {
      const int left = (int)(widgets_due() - MIN(widgets_due(), ms_now()));
      if (wait < 0 || left < wait)
        wait = left;
    }
    watch_prepare(&wait);
    if (!win_wait(watch_fds, WATCH_FDS, wait))
      return 1;
    changed |= watch_changed();
    for (int why, value; win_woken(&why, &value); animating = true)
      if (!launcher_input(&ui, why, value, &page))
        return 0;
    /* Searching and pairing while their screens are up */
    bt_pairing(screen_contains(launcher.visible, &bt_pairing_screen));
    wifi_adding(screen_contains(launcher.visible, &wifi_adding_screen));
    const uint64_t now = ms_now();
    osd_tick(now);
    if (perf_on() && now >= perf_due) {
      show_perf();
      perf_due = now + 1000;
    }
    static bool dropped;
    if (launcher.front != NULL && !showing_pause() && !session_starting()) {
      if (!dropped)
        assets_drop();
      dropped = true;
      animating = false;
      continue;
    }
    dropped = false;
    if (input_capture_expire(now))
      animating = true;
    bool dirty = animating || (widgets_due() != 0 && now >= widgets_due());
    const bool second = seconds && now - status_at >= 1000;
    if (second || changed != 0) {
      if (second && !told_up && (told_up = SYSD("Up", launcher_up))) {
        power_apply();
        const str why = str_read(fopen(SLEEP_FAILED, "r"));
        if (why.n > 0) {
          launcher_show_failure(&ui, FAIL_SLEEP, S("Couldn't wake up"),
                                S("The handheld had to restart, so anything "
                                  "unsaved was lost. Light sleep avoids this "
                                  "but uses more battery."),
                                S(""));
          FILE *f = fopen(SLEEP_FAILED, "w");
          if (f != NULL)
            fclose(f);
          dirty = true;
        }
        free((char *)why.p);
      }
      if (second)
        status_at = now;
      if (changed & (WATCH_MINUTE | WATCH_POWER | WATCH_WIFI | WATCH_BLUETOOTH)) {
        const ui_status was = st;
        read_status(&st);
        /* Wi-Fi and Bluetooth: their screens show more than the bar */
        dirty |= (changed & (WATCH_MINUTE | WATCH_WIFI | WATCH_BLUETOOTH)) || was.battery != st.battery ||
                 was.charging != st.charging || was.wifi != st.wifi;
      }
      if (changed & WATCH_USB)
        dirty |= storage_refresh_usb();
      if (changed & WATCH_TIME) {
        clock_refresh();
        dirty = true;
      }
      const uint64_t now_present =
          changed & WATCH_MOUNTS ? sources_present() : present;
      if (now_present != present) {
        present = now_present;
        if (page >= 1 && !apps_present(&categories[page - 1]))
          home_turn_page(&ui, &page, 1);
        home_rescan(page);
        dirty = true;
      }
    }
    changed = 0;
    if (!dirty)
      continue;

    assets_frame();
    int w, h;
    if (!gfx_begin(screen, &w, &h))
      continue;
    gfx_clear(BG);
    session_draw_backdrop(w, h);
    nav_begin(&ui, &ctx, now);
    widgets_frame_begin(now);
    launcher.draw = (typeof(launcher.draw)){.w = w, .h = h, .page = page};
    launcher.changed = &changed;
    screen_stack *drawn = launcher.visible;
    const unsigned generation = drawn != NULL ? drawn->generation : 0;
    if (launcher.front != NULL && !showing_pause()) {
      session_draw_starting(w, h);
    } else {
      const ui_rect content = screen_chrome(launcher.visible, &ui, w, h, &st);
      screen_draw(launcher.visible, &ui, content);
    }
    const nav_id requested_focus = ui.focus;
    animating = nav_end(&ui, RING, 2);
    const bool calibrate = launcher.draw.calibrate;
    widgets_frame_end();
    if (launcher.visible == drawn && drawn != NULL &&
        drawn->generation == generation)
      screen_focus_save(drawn, &ui);
    else if (launcher.visible != NULL) {
      nav_focus(&ui, requested_focus);
      screen_focus_save(launcher.visible, &ui);
    }
    gfx_end(screen);
    nav_run_deferred(&ui);
    osd_flush();
    static bool presented;
    if (!presented) {
      struct timespec ts;
      clock_gettime(CLOCK_BOOTTIME, &ts);
      fprintf(stderr, "first frame at %.3f s\n", ts.tv_sec + ts.tv_nsec / 1e9);
      presented = true;
    }
    if (calibrate) {
      resume_front(&ui);
      power_calibrate();
      osd_show_text(power_calibrating() ? power_progress()
                                        : S("No frame counter"),
                    power_calibrating() ? INT_MAX / 2 : RESULT_MS);
    }
  }
}
