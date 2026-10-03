/*
 * What the launcher offers, by category, from folders under the data root
 * ($LAUNCHER_DATA, /data by default):
 *
 * - Ports, applications, benchmarks: OS/apps/{ports,applications,benchmarks}.
 *   Each *.desktop (XDG desktop entry: Name, Exec, Path, Icon, Keywords,
 *   Categories) is an app; so is any other executable without a .desktop of
 *   the same name. Exec runs under sh -c, in Path or else the folder. Icon is
 *   a path, absolute or relative to the folder, extension optional.
 * - Game systems: a ROM folder each (roms/gba), every file with the system's
 *   extensions a game. Its title and box art come from the system's game
 *   pack by CRC, once that is known; an image of the same name next to the
 *   ROM wins over the pack's. A system has one or more emulators (cores):
 *   the first runs a game unless Select on it picked another, which is kept
 *   in $XDG_STATE_HOME/launcher-cores.
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
  STRBUF(320) icon; /* a path, or a key into a game pack */
  STRBUF(128) tags; /* Keywords then Categories, each ';'-terminated */
  int core;         /* into the category's cores */
} app;

typedef struct {
  str name;
  str exec; /* sh -c, with $1 = the ROM */
} core;

typedef struct {
  str name;
  str folder; /* under the data root */
  /* What a folder of its games is called elsewhere (another OS's card): "n64;n64dd;" */
  str names;
  /* Game systems: ROM extensions (".gba;.zip;"), the emulators' directory
   * under the data root, the game pack's name in OS/packs (romdb.h, "" for
   * none): titles and box art; and the emulators, ended by an empty one */
  str roms, system_dir, pack;
  const core *cores;
  app *apps;
  size_t count, capacity;
} category;

extern category categories[];
extern const size_t category_count;

/* Rereads the category's folder */
void apps_scan(category *c);
/* A game system whose folder is missing isn't shown */
bool apps_present(const category *c);
size_t apps_core_count(const category *c);
/* Runs `a` with core `index` from now on */
void apps_set_core(category *c, app *a, int index);
/* Kept in $XDG_STATE_HOME/launcher-folders, a line "system's folder\tpath"
 * each. A folder may be a source of more than one system. */
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
/* $XDG_STATE_HOME/launcher`suffix`, its directories made for writing */
FILE *state_file(const char *suffix, const char *mode);
