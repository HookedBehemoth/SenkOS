/*
 * System updates, through downloadd (apps/downloadd): asks whether the server
 * has a newer build than /etc/build, then has it downloaded into the other
 * slot. A thread keeps the connection and wakes the loop (UI_UPDATES) on
 * every line; the screen reads the state under updates_lock.
 */
#pragma once

#include <pthread.h>
#include <stdbool.h>

#include "str.h"

enum { UPDATE_UNKNOWN, UPDATE_CHECKING, UPDATE_CURRENT, UPDATE_AVAILABLE, UPDATE_FAILED };
enum { UPDATE_IDLE, UPDATE_RUNNING, UPDATE_READY };
typedef struct {
  int check;               /* UPDATE_UNKNOWN .. UPDATE_FAILED */
  STRBUF(32) build;        /* the newer one */
  unsigned long long size; /* its download */
  STRBUF(512) notes;       /* or why the check failed */
  int job;                 /* UPDATE_IDLE .. UPDATE_READY */
  STRBUF(32) job_state;    /* downloadd's word */
  unsigned long long done, total;
  STRBUF(200) job_message;
  bool connected;
} update_state;

extern update_state updates;
extern pthread_mutex_t updates_lock;

/* The thread, once; each call asks again */
void updates_check(void);
void updates_install(void);
/* This build, /etc/build's */
str updates_build(void);
