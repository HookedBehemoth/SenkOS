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
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

typedef struct {
  char name[64];
  /* sh -c, with $1 = arg */
  char exec[512];
  char arg[512];
  char dir[256];
  char icon[320]; /* a path, or a key into a game pack */
  char tags[128]; /* Keywords then Categories, ';'-separated */
  int core;       /* into the category's cores */
} app;

typedef struct {
  const char *name;
  const char *exec; /* sh -c, with $1 = the ROM */
} core;

typedef struct {
  const char *name;
  const char *folder; /* under the data root */
  /* Game systems: ROM extensions (".gba;.zip;"), the emulators' directory
   * under the data root, the game pack's name in OS/packs (romdb.h): titles
   * and box art; and the emulators, ended by an empty one */
  const char *roms, *system_dir, *pack;
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
/* $XDG_STATE_HOME/launcher`suffix`, its directories made for writing */
FILE *state_file(const char *suffix, const char *mode);
