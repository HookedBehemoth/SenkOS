/*
 * The apps the launcher started and that haven't exited: on screen, under
 * the pause screen, or minimized (Home) and paused in the background.
 *
 * Each gets a control channel (appctl.h). One that takes it (corehost) is
 * paused and resumed through it and has save states and options; any other
 * is stopped with SIGSTOP. They die with the launcher: SIGTERM by
 * PR_SET_PDEATHSIG, and corehost also quits when its socket closes. Nothing
 * is adopted when the launcher starts again.
 *
 * On the handheld weston.init starts the launcher as root, and it becomes shell at once
 * (run_drop_root): sysd answers shell (and root) alone, Wi-Fi's control socket and the apps'
 * cgroup are shell's, so the apps, the player's, can't reach them. It keeps CAP_SETUID and CAP_SETGID to start each app as
 * the player and CAP_KILL to pause and stop them; the app drops them all before it runs. The apps
 * get the environment the launcher was started with; the launcher its own runtime dir, settings
 * and caches (shell's home, /data/OS/launcher), and Weston's socket by its path.
 */
#pragma once

#include <stdbool.h>
#include <sys/types.h>

#include "appctl.h"
#include "str.h"

#define RUNNING_MAX 8

typedef struct {
  pid_t pid; /* 0: a free slot */
  STRBUF(64) label;
  STRBUF(320) icon;
  /* What it was launched with: the same again resumes it */
  STRBUF(512) exec, arg;
  STRBUF(256) dir;
  int ctl;
  appctl_shm *shm;
  /* SIGTERM sent, exit not seen yet */
  bool quitting;
  /* Where its pause screen was left, for the next pause: in Settings or
   * not, and the focus (a nav_id) in the menu and each category */
  bool in_settings;
  int settings_category; /* appctl's: which of the settings */
  uint32_t menu_focus, settings_focus[APPCTL_CATEGORIES + 1]; /* and the Power page (main.c) */
} running;

extern running runs[RUNNING_MAX];

/* win_wake(UI_EXITED, pid) when it is gone. NULL when there is no free slot
 * or the fork failed. */
running *run_launch(str label, str icon, str exec, str arg, str dir);
/* Started as root: shell from here on, as above; false if that failed. As anyone else: nothing
 * to do. Before any thread. */
bool run_drop_root(void);
/* A variable of the apps' environment (the launcher's own may differ), NULL if unset */
const char *run_app_env(const char *name);
running *run_find(pid_t pid);
running *run_same(str exec, str arg, str dir);
size_t run_count(void);
/* It speaks appctl: it filled in the shared header */
bool run_controlled(const running *r);
/* A request and its answer: the result, or -ETIMEDOUT / -EPIPE */
int run_request(running *r, uint32_t op, int32_t arg);
void run_pause(running *r);
/* `raise`: it was out of sight and has to come on top again */
void run_resume(running *r, bool raise);
void run_quit(running *r);
/* After its exit: reaped, channel closed, slot free. False when it ended
 * badly, not by our SIGTERM. `why` says how (also logged), until the next
 * call; "" when it exited fine. */
bool run_forget(running *r, str *why);
/* SIGTERM to every app: the launcher is going */
void run_quit_all(void);
