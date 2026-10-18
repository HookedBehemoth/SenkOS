/* downloadd (org.senkos.Updates1 on the system bus): system updates and game packs. Its news wakes
 * the UI (UI_UPDATES; UI_RELOAD once a pack is there). */
#pragma once

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
  bool connected;          /* downloadd is on the bus */
  STRBUF(96) pack;         /* the pack asked for, until it's sent */
  STRBUF(32) pack_news;    /* a pack's job's latest state, for a toast; taken by the loop */
  STRBUF(200) pack_why;    /* and why it failed */
} update_state;

extern update_state updates;

/* At start, before the bus is used */
void updates_init(void);
/* Each asks again */
void updates_check(void);
void updates_install(void);
/* A .system file's Pack from the server into OS/assets/thumbnails ("reload" when it's there) */
void updates_pack(str name);
/* This build, /etc/build's */
str updates_build(void);
str updates_reason(str why);
