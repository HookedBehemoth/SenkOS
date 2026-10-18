/*
 * Media players: MPRIS (org.mpris.MediaPlayer2.* on the player's session bus), a mirror of each
 * one's playback state and track kept by its PropertiesChanged. The quick menu shows the one
 * playing (else the last one that played or came) and controls it. A new track starting while its
 * app isn't in front: the OSD names it. An app whose player is playing isn't stopped behind the
 * launcher (run_pause).
 */
#pragma once

#include <stdbool.h>
#include <sys/types.h>

#include "conn.h"
#include "str.h"

enum { MEDIA_MAX = 8 };
typedef struct {
  bool used;
  uint64_t seen; /* when it came or started playing: the latest is shown */
  STRBUF(96) name;  /* org.mpris.MediaPlayer2.NAME */
  STRBUF(48) owner; /* its unique name: what its signals come from */
  pid_t pid;        /* 0 until known */
  bool playing, can_next, can_previous;
  STRBUF(128) title, artist, track;
  bus_call props, pid_call;
} media_player;

/* At start, before the session bus is used */
void media_init(void);
/* Once true after the mirror changed (watch.c: WATCH_MEDIA) */
bool media_changed(void);
/* What the quick menu shows, NULL without a player */
const media_player *media_current(void);
void media_play_pause(void);
void media_next(void);
void media_previous(void);
/* An app's (its process group, run_launch's setsid) player is playing */
bool media_playing(pid_t group);
