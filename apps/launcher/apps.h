/*
 * What the launcher offers, by category, from folders under the data root
 * ($LAUNCHER_DATA, /data by default):
 *
 * - Applications, benchmarks: apps/{applications,benchmarks}.
 *   Each *.desktop (XDG desktop entry: Name, Exec, Path, Icon, Keywords,
 *   Categories) is an app; so is any other executable without a .desktop of
 *   the same name. Exec runs under sh -c, in Path or else the folder. Icon is
 *   a path, absolute or relative to the folder, extension optional.
 *   X-SenkOS-AskQuit=false: Quit in its pause menu doesn't ask first.
 * - Game systems: the .system files in apps/systems over the image's
 *   (SYSTEMS, apps.c), the way emulators are below. [System] Name, Folder
 *   (its page's, under the data root), Aliases (what other OSes call such a
 *   folder: "n64;n64dd;"), Patterns (its games: "*.n64;*.zip;"), MimeType
 *   (what its emulators take), Pack (the game pack's name in
 *   OS/assets/thumbnails, romdb.h), Order (of the pages; then by name).
 *   Every file in the folder matching Patterns is a game. Its title and box art come from the system's game
 *   pack by CRC, once that is known, or from the pack's game picked for it by hand (Select:
 *   kept in $XDG_CONFIG_HOME/launcher-matches); an image of the same name next to the ROM
 *   wins over the pack's.
 * - Emulators and players: the .desktop files in apps/emulators over the
 *   image's (EMULATORS): one of the same file name replaces the image's, or
 *   hides it (Hidden=true). One runs a system's games when its MimeType has
 *   the system's type; %f in Exec is the game ("$1"), Path its working
 *   directory (else the game's folder), X-SenkOS-AskQuit as an app's (a
 *   player that resumes where it was left). By name, the first runs a game
 *   unless Select on it picked another, which is kept in
 *   $XDG_CONFIG_HOME/launcher-cores.
 * - Sources: folders on the other card added to a system; their games join
 *   its page while the folder is there.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "str.h"

typedef struct {
  STRBUF(64) name;
  /* sh -c, with $1 = arg */
  STRBUF(512) exec, arg;
  STRBUF(256) dir;
  STRBUF(320) icon; /* a path, a key into a game pack, or "video:" and a video (assets.h) */
  STRBUF(128) tags; /* Keywords then Categories, each ';'-terminated */
  int core;         /* into the category's cores */
  uint32_t match;   /* the pack's game picked for it by hand (its CRC), 0: none */
  unsigned flags;   /* APP_*: its .desktop's, a game's its core's */
} app;
/* X-SenkOS-AskQuit=false */
enum { APP_QUIT_NOW = 1 };

/* An emulator or player, from its .desktop */
enum { CORES_MAX = 8 };
typedef struct {
  STRBUF(64) name;
  STRBUF(512) exec; /* sh -c, with $1 = the ROM */
  STRBUF(256) dir;
  unsigned flags; /* APP_* */
} core;

typedef struct {
  str name;
  str folder; /* under the data root */
  /* Game systems, from a .system: Aliases, Patterns, MimeType, Pack, Order */
  str names, roms, mimes, pack;
  int order;
  /* Those that run its games, found by apps_cores */
  core cores[CORES_MAX];
  size_t core_count;
  app *apps;
  size_t count, capacity;
} category;

extern category *categories;
extern size_t category_count;

/* Reads the systems: the categories are they, then the app folders; and apps_cores */
void apps_init(void);
/* Reads the emulators again: each system's cores */
void apps_cores(void);
/* A thread: UI_RELOAD (wake.h) when the card's emulators change */
void apps_watch(void);

/* Rereads the category's folder */
void apps_scan(category *c);
/* A category whose folder is missing isn't shown, nor a game system without
 * any of its sources */
bool apps_present(const category *c);
/* Runs `a` with core `index` from now on */
void apps_set_core(category *c, app *a, int index);
/* Its title and box art from the pack's game `crc` from now on; 0: by its own CRC again.
 * Shows once the page is read again. */
void apps_set_match(app *a, uint32_t crc);
/* The system's game pack, "" for none; until the next call */
str apps_pack(const category *c);
/* Kept in $XDG_CONFIG_HOME/launcher-folders, a line "category's folder\tpath"
 * each ("roms/gba\t/run/media/mmcblk1p2/gba": GBA games on the second card).
 * A folder may be a source of more than one system. */
enum { SOURCES_MAX = 64 };
typedef struct {
  STRBUF(256) path;
  size_t category;
} source;
extern source sources[SOURCES_MAX];
extern size_t source_count;
void sources_load(void);
/* The system a folder's games are for, by its name; -1 for none */
int apps_guess(str folder);
bool sources_has(str path, size_t category);
/* Adds it, or takes it out if it was there */
void sources_toggle(str path, size_t category);
/* A bit per source whose folder is there: it changes when a card comes or goes */
uint64_t sources_present(void);
/* $XDG_STATE_HOME/launcher`suffix` (what the launcher keeps for itself) and
 * $XDG_CONFIG_HOME/launcher`suffix` (what was set), their directories made
 * for writing */
FILE *state_file(const char *suffix, const char *mode);
FILE *config_file(const char *suffix, const char *mode);
