/*
 * The Wayland connection, xdg toplevels, and the loop around them: the
 * display, the caller's fds and the wake pipe in one poll. Threads, signal
 * handlers (SIGTERM, SIGINT) and a closed window wake the loop with a
 * (why, value) pair; win_woken hands them out in order.
 *
 * The keyboard is for testing on a desktop: evdev KEY_* codes, no keymap,
 * no repeat. The pad is pad.h's.
 */
#pragma once

#include <poll.h>
#include <stdbool.h>

struct wl_display;
struct wl_compositor;
struct wl_subcompositor;
struct wl_shm;
struct wl_surface;
struct xdg_surface;
struct xdg_toplevel;

extern struct wl_display *win_display;
extern struct wl_compositor *win_compositor;
extern struct wl_subcompositor *win_subcompositor;
extern struct wl_shm *win_shm;
/* The output's refresh rate in mHz, 0 when it didn't say */
extern int win_refresh_mhz;
/* The rates of its modes at the current size, ascending */
#define WIN_REFRESHES_MAX 8
extern int win_refreshes_mhz[WIN_REFRESHES_MAX], win_refresh_count;

/* What woke the loop: ours, then the app's own from WIN_WAKE_APP */
enum {
  WIN_QUIT,   /* SIGTERM, SIGINT, or a window's close */
  WIN_OUTPUT, /* The output's mode changed: win_refresh_mhz */
  WIN_WAKE_APP,
};

typedef struct win {
  const char *app_id, *title;
  bool fullscreen;
  /* The size it is drawn at: what's set before the first map, then the
   * compositor's, when it picks one */
  int w, h;
  /* Created by the first map, unless the app brought its own */
  struct wl_surface *surface;
  /* Acked: may be drawn to. False while unmapped. */
  bool configured;
  /* Has the focus (xdg's activated) */
  bool activated;
  /* Optional: after each configure is acked */
  void (*configure)(struct win *);
  /* Asked for by win_frame, until the compositor has shown the commit */
  bool frame_pending;
  struct xdg_surface *xsurface;
  struct xdg_toplevel *toplevel;
} win;

/* `key`, optional: a key pressed or released, while a window has focus */
bool win_connect(void (*key)(unsigned code, bool down));
/* Shown, back from the compositor's first configure. Mapped again, it's
 * unmapped first: a new toplevel, which Weston's shell puts on top. */
void win_map(win *w);
void win_unmap(win *w);
/* The next commit's frame callback: frame_pending until the compositor shows
 * it, at its repaint. Pacing by it, in win_wait, never blocks in a present;
 * a hidden window gets none, so wait for it with a timeout. */
void win_frame(win *w);

/* Any thread, signal handlers too. Dropped if the pipe is full. */
void win_wake(int why, int value);
/* The next wake, false when none is left */
bool win_woken(int *why, int *value);
/* Flushes, waits up to `timeout_ms` (-1: no limit) for the display, a wake
 * or one of `fds` (their revents set), and dispatches the display. False
 * once the connection is gone. */
bool win_wait(struct pollfd *fds, int n, int timeout_ms);
