/*
 * The apps the launcher started and that haven't exited: on screen, under
 * the pause screen, or minimized (Home) and paused in the background.
 *
 * Each gets a control channel (appctl.h). One that takes it (corehost) is
 * paused and resumed through it and has save states and options; any other
 * is stopped with SIGSTOP. They die with the launcher: SIGTERM by
 * PR_SET_PDEATHSIG, and corehost also quits when its socket closes. Nothing
 * is adopted when the launcher starts again.
 */
#pragma once

#include <stdbool.h>
#include <sys/types.h>

#include <SDL.h>

#include "appctl.h"

#define RUNNING_MAX 8

typedef struct {
  pid_t pid; /* 0: a free slot */
  char label[64];
  char icon[320];
  /* What it was launched with: the same again resumes it */
  char exec[512], arg[512], dir[256];
  int ctl;
  appctl_shm *shm;
  /* SIGTERM sent, exit not seen yet */
  bool quitting;
  /* Where its pause screen was left, for the next pause: in Settings or
   * not, and the focus (a nav_id) in the menu and each category */
  bool in_settings;
  int settings_category; /* appctl's: which of the settings */
  uint32_t menu_focus, settings_focus[APPCTL_CATEGORIES];
} running;

extern running runs[RUNNING_MAX];

/* `exit_event` is posted with the pid as code when it is gone. NULL when
 * there is no free slot or the fork failed. */
running *run_launch(const char *label, const char *icon, const char *exec,
                    const char *arg, const char *dir, Uint32 exit_event);
running *run_find(pid_t pid);
running *run_same(const char *exec, const char *arg, const char *dir);
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
 * badly, not by our SIGTERM: `why` says how (also logged). */
bool run_forget(running *r, char *why, size_t size);
/* SIGTERM to every app: the launcher is going */
void run_quit_all(void);
