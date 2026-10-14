/* Tracks launched apps and their control channels. */
#pragma once

#include <stdbool.h>
#include <sys/types.h>

#include "appctl.h"
#include "screen.h"
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
  unsigned flags; /* APP_* (apps.h) */
  /* SIGTERM sent, exit not seen yet */
  bool quitting;
  screen_stack screens;
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
void run_resume(running *r);
void run_quit(running *r);
/* After its exit: reaped, channel closed, slot free. False when it ended
 * badly, not by our SIGTERM. `why` says how (also logged), until the next
 * call; "" when it exited fine. */
bool run_forget(running *r, str *why);
/* SIGTERM to every app: the launcher is going */
void run_quit_all(void);
