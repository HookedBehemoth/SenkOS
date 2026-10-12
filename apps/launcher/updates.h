/* Update service state and commands. */
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
  STRBUF(96) pack;         /* the pack asked for, until it's sent */
  STRBUF(32) pack_news;    /* a pack's job's latest state, for a toast; taken by the loop */
  STRBUF(200) pack_why;    /* and why it failed */
} update_state;

extern update_state updates;
extern pthread_mutex_t updates_lock;

/* The thread, once; each call asks again */
void updates_check(void);
void updates_install(void);
/* A .system file's Pack from the server into OS/assets/thumbnails ("reload" when it's there) */
void updates_pack(str name);
/* This build, /etc/build's */
str updates_build(void);
str updates_reason(str why);
