/* App launch, pause, resume, and exit. */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#include "assets.h"
#include "capture.h"
#include "cfg.h"
#include "launcher_internal.h"
#include "layer.h"
#include "power.h"
#include "util.h"
#include "wake.h"

static screen_stack home_screens;
static gfx_image backdrop;
#define HOME_KEY S("home")

bool showing_pause(void) {
  return launcher.front != NULL && launcher.visible == &launcher.front->screens;
}

static void drop_backdrop(void) {
  gfx_bind(GFX_BACKDROP, NULL);
  gfx_release(&backdrop);
}

static str starting;
static STRBUF(600) starting_image;

void go_home(nav *ui) {
  if (showing_pause())
    screen_focus_save(launcher.visible, ui);
  launcher.front = NULL;
  launcher.visible = &home_screens;
  power_use(HOME_KEY);
  starting = S("");
  launcher.capture.option = -1;
  pad_listen(true);
  drop_backdrop();
  layer_show(&window, true);
  screen_focus_restore(launcher.visible, ui);
}

static str power_key(const running *r) {
  return r->arg.n > 0   ? sb_str(r->arg)
         : r->dir.n > 0 ? sb_str(r->dir)
                        : sb_str(r->exec);
}

static void hand_over(nav *ui, running *r) {
  if (launcher.front == NULL)
    screen_focus_save(&home_screens, ui);
  launcher.front = r;
  launcher.visible = NULL;
  power_use(power_key(r));
  pad_listen(false);
  pad_grab(false);
}

/* The same app again comes back instead of starting twice (two of a game
 * would share its save) */
void session_start(nav *ui, str label, str icon, str exec, str arg, str dir,
                   unsigned flags) {
  running *r = run_same(exec, arg, dir);
  if (r != NULL) {
    hand_over(ui, r);
    layer_show_app(r->pid);
    run_resume(r);
    layer_show(&window, false);
    return;
  }
  r = run_launch(label, icon, exec, arg, dir);
  if (r == NULL)
    return;
  r->flags = flags;
  hand_over(ui, r);
  starting = S("Loading\u2026");
  sb_set(starting_image, icon);
}

/* Paused first, so the frame blurred is the one it stays on. Menu has been
 * released (input): the app saw all of it, and nothing more until it resumes.
 */
void session_pause(nav *ui) {
  run_pause(launcher.front);
  power_cancel();
  drop_backdrop();
  int w, h;
  uint8_t *rgba = capture_blurred(&w, &h);
  if (rgba != NULL && gfx_upload(&backdrop, rgba, w, h))
    gfx_bind(GFX_BACKDROP, &backdrop);
  free(rgba);
  launcher.visible = &launcher.front->screens;
  if (!launcher.visible->count)
    screen_push(launcher.visible, ui, &pause_screen, 0);
  launcher.capture.option = -1;
  launcher.notice.n = 0;
  pad_listen(true);
  pad_grab(true);
  screen_focus_restore(launcher.visible, ui);
  layer_show(&window, true);
}

void resume_front(nav *ui) {
  if (showing_pause()) {
    while (screen_top(launcher.visible)->def->overlay)
      screen_pop(launcher.visible, ui);
    screen_focus_save(launcher.visible, ui);
  }
  launcher.visible = NULL;
  pad_listen(false);
  pad_grab(false);
  layer_show(&window, false);
  drop_backdrop();
  run_resume(launcher.front);
}

void quit_front(nav *ui) {
  if (showing_pause())
    screen_focus_save(launcher.visible, ui);
  launcher.visible = NULL;
  pad_listen(false);
  pad_grab(false);
  run_quit(launcher.front);
}

void launcher_set_binding(running *r, int i, uint16_t combo) {
  atomic_store(&r->shm->options[i].buttons, combo);
  run_request(r, APPCTL_OPTIONS, 0);
}

void launcher_step_option(running *r, int i, int by) {
  appctl_option *o = &r->shm->options[i];
  const int n = MIN(o->count, APPCTL_VALUES);
  if (n == 0 || o->kind != APPCTL_CHOICE)
    return;
  atomic_store(&o->value, (uint8_t)((atomic_load(&o->value) % n + n + by) % n));
  run_request(r, APPCTL_OPTIONS, 0);
}

bool launcher_ignored(str key) {
  str lines = str_read(state_file("-ignored", "r"));
  const char *text = lines.p;
  bool found = false;
  while (lines.n > 0 && !found)
    found = str_eq(str_cut(&lines, '\n'), key);
  free((char *)text);
  return found;
}

void launcher_ignore(str key) {
  FILE *f = state_file("-ignored", "a");
  if (f != NULL) {
    str_fput(f, key, S("\n"));
    fclose(f);
  }
}

void session_app_exited(nav *ui, running *r, size_t *page) {
  const bool had_screen = r == launcher.front;
  if (had_screen && showing_pause())
    pad_grab(false);
  STRBUF(sizeof(r->label.p)) label;
  STRBUF(1300) key;
  sb_set(label, sb_str(r->label));
  /* Tabs: no field has one. One line, for the ignore list. */
  sb_set(key, sb_str(r->dir), S("\t"), sb_str(r->exec), S("\t"),
         sb_str(r->arg));
  key.p[key.n = str_find(sb_str(key), '\n')] = '\0';
  str why;
  const bool ok = run_forget(r, &why);
  if (had_screen)
    go_home(ui);
  STRBUF(96) title;
  if (!ok && !launcher_ignored(sb_str(key))) {
    sb_set(title, sb_str(label), S(" stopped"));
    launcher_show_failure(ui, FAIL_LAUNCH, sb_str(title), why, sb_str(key));
  }
  if (*page == 0 && run_count() == 0)
    home_turn_page(ui, page, 1);
  home_rescan(*page);
}

screen_stack *session_home_screens(void) {
  return &home_screens;
}

bool session_starting(void) { return starting.n > 0; }

void session_check_start(void) {
  if (session_starting() && window.configured && !window.activated &&
      launcher.front != NULL)
    starting = S("");
}

void session_draw_backdrop(int w, int h) {
  if (!showing_pause() || backdrop.image == VK_NULL_HANDLE)
    return;
  gfx_push((gfx_quad){.w = w,
                      .h = h,
                      .u1 = 1,
                      .v1 = 1,
                      .color = 0xffffffff,
                      .tex = GFX_BACKDROP});
  ui_fill(0, 0, w, h, SCRIM);
}

void session_draw_starting(int w, int h) {
  const asset splash = starting_image.n > 0
                           ? assets_get(sb_str(starting_image), SPLASH_PX)
                           : (asset){.slot = GFX_PLACEHOLDER};
  if (splash.slot != GFX_PLACEHOLDER) {
    const float scale = MIN((float)w / splash.w, (float)h / splash.h);
    gfx_push((gfx_quad){.x = (w - splash.w * scale) / 2,
                        .y = (h - splash.h * scale) / 2,
                        .w = splash.w * scale,
                        .h = splash.h * scale,
                        .u1 = 1,
                        .v1 = 1,
                        .color = 0xffffffff,
                        .tex = splash.slot});
  }
  ui_fill(0, h - ROW, w, ROW, SCRIM);
  ui_text(ROW_TEXT, (w - ui_text_width(ROW_TEXT, starting)) / 2,
          h - (ROW + ui_text_height(ROW_TEXT)) / 2, starting, FG);
}
