#include "apps.h"
#include "cfg.h"
#include "romdb.h"
#include "util.h"

#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* Cores run in corehost: the launcher is their UI. $COREHOST
 * (/data/OS/profile.d) tries out another build. `env` before it: what the
 * core needs set (sh assignments). */
#define COREHOST_ENV(name, env, so) {S(name), S(env "exec \"${COREHOST:-corehost}\" ./" so "_libretro.so \"$1\"")}
#define COREHOST(name, so) COREHOST_ENV(name, "", so)
/* The cores' BIOS and asset files, in /data/bios (run from retroarch/cores) */
#define BIOS "COREHOST_SYSTEM=\"${PWD%/*/*}/bios\" "
/* Its memory stick beside the games: PPSSPP takes a folder named "psp" for
 * the stick's PSP folder, so not the ROM folder itself */
#define PSP_SAVE "COREHOST_SAVE=\"${1%/*}/memstick\" "
/* GLES unless a core that has both is told otherwise */
#define VULKAN "COREHOST_RENDER=vulkan "
#define SYSTEM(title, dir, exts, pack_name, ...)                                         \
  {.name = S(title), .folder = S("roms/" dir), .roms = S(exts), .system_dir = S("retroarch/cores"), \
   .pack = S(pack_name), .cores = (const core[]){__VA_ARGS__, {.name = {0}}}}

category categories[] = {
    SYSTEM("Game Boy Advance", "gba", ".gba;.zip;", "Nintendo - Game Boy Advance",
           COREHOST("mGBA", "mgba")),
    SYSTEM("Game Boy", "gb", ".gb;.zip;", "Nintendo - Game Boy",
           COREHOST("Gambatte", "gambatte"), COREHOST("mGBA", "mgba")),
    SYSTEM("Game Boy Color", "gbc", ".gbc;.gb;.zip;", "Nintendo - Game Boy Color",
           COREHOST("Gambatte", "gambatte"), COREHOST("mGBA", "mgba")),
    SYSTEM("NES", "nes", ".nes;.fds;.unf;.unif;.zip;", "Nintendo - Nintendo Entertainment System",
           COREHOST("FCEUmm", "fceumm")),
    SYSTEM("SNES", "snes", ".sfc;.smc;.zip;", "Nintendo - Super Nintendo Entertainment System",
           COREHOST("Snes9x", "snes9x")),
    SYSTEM("Nintendo 64", "n64", ".n64;.v64;.z64;.zip;", "Nintendo - Nintendo 64",
           COREHOST("Mupen64Plus-Next", "mupen64plus_next")),
    SYSTEM("Mega Drive", "md", ".md;.gen;.smd;.bin;.zip;", "Sega - Mega Drive - Genesis",
           COREHOST("Genesis Plus GX", "genesis_plus_gx")),
    SYSTEM("Master System", "ms", ".sms;.zip;", "Sega - Master System - Mark III",
           COREHOST("Genesis Plus GX", "genesis_plus_gx")),
    SYSTEM("Game Gear", "gg", ".gg;.zip;", "Sega - Game Gear",
           COREHOST("Genesis Plus GX", "genesis_plus_gx")),
    SYSTEM("Dreamcast", "dc", ".chd;.cdi;.gdi;.cue;", "Sega - Dreamcast",
           COREHOST_ENV("Flycast", BIOS, "flycast"), COREHOST_ENV("Flycast (Vulkan)", BIOS VULKAN, "flycast")),
    SYSTEM("PC Engine", "pce", ".pce;.cue;.chd;.zip;", "NEC - PC Engine - TurboGrafx 16",
           COREHOST("Beetle PCE Fast", "mednafen_pce_fast")),
    SYSTEM("PlayStation", "ps", ".cue;.chd;.pbp;.m3u;.bin;.img;", "Sony - PlayStation",
           COREHOST("PCSX ReARMed", "pcsx_rearmed")),
    SYSTEM("PSP", "psp", ".iso;.cso;.pbp;.chd;", "Sony - PlayStation Portable",
           COREHOST_ENV("PPSSPP", BIOS PSP_SAVE, "ppsspp"),
           COREHOST_ENV("PPSSPP (Vulkan)", BIOS PSP_SAVE VULKAN, "ppsspp")),
    SYSTEM("Arcade", "arcade", ".zip;.7z;", "",
           COREHOST("FinalBurn Neo", "fbneo")),
    SYSTEM("Neo Geo Pocket", "ngp", ".ngp;.ngc;.zip;", "SNK - Neo Geo Pocket Color",
           COREHOST("Beetle NGP", "mednafen_ngp")),
    SYSTEM("WonderSwan", "ws", ".ws;.wsc;.zip;", "Bandai - WonderSwan Color",
           COREHOST("Beetle WonderSwan", "mednafen_wswan")),
    SYSTEM("Atari 2600", "atari2600", ".a26;.bin;.zip;", "Atari - 2600",
           COREHOST("Stella 2014", "stella2014")),
    SYSTEM("Atari Lynx", "lynx", ".lnx;.o;.zip;", "Atari - Lynx",
           COREHOST("Handy", "handy")),
    /* Not games, but a folder of files run by one player all the same;
     * $MEDIAPLAYER tries out another build */
    {.name = S("Videos"), .folder = S("videos"), .roms = S(".mp4;.mkv;.webm;.mov;.m4v;.avi;.ts;"),
     .system_dir = S("videos"),
     .cores = (const core[]){{S("Media player"), S("exec \"${MEDIAPLAYER:-mediaplayer}\" \"$1\"")}, {.name = {0}}}},
    {.name = S("Ports"), .folder = S("OS/apps/ports")},
    {.name = S("Applications"), .folder = S("OS/apps/applications")},
    {.name = S("Benchmarks"), .folder = S("OS/apps/benchmarks")},
};
const size_t category_count = sizeof(categories) / sizeof(categories[0]);

static const str image_exts[] = {S(".png"), S(".jpg"), S(".jpeg"), S(".webp"), S(".bmp")};

static str root(void) {
  static str r;
  if (r.p == NULL) {
    const char *env = getenv("LAUNCHER_DATA");
    r = env != NULL && env[0] != '\0' ? str_c(env) : S("/data");
  }
  return r;
}

FILE *state_file(const char *suffix, const char *mode) {
  char path[512];
  const char *state = getenv("XDG_STATE_HOME"), *home = getenv("HOME");
  if (state != NULL && state[0] == '/')
    snprintf(path, sizeof(path), "%s/launcher%s", state, suffix);
  else
    snprintf(path, sizeof(path), "%s/.local/state/launcher%s", home ? home : "/tmp", suffix);
  /* mkdir -p of the two levels under $HOME that may be missing */
  for (char *slash = strchr(path + 1, '/'); mode[0] != 'r' && slash; slash = strchr(slash + 1, '/')) {
    *slash = '\0';
    mkdir(path, 0755);
    *slash = '/';
  }
  return fopen(path, mode);
}

size_t apps_core_count(const category *c) {
  size_t n = 0;
  while (c->cores != NULL && c->cores[n].name.p != NULL)
    n++;
  return n;
}

/* The core picked for `rom` in `picks` (launcher-cores: a line
 * "ROM\tcore name" each, the last one standing); else the first.
 * ponytail: only ever appended to; compact it if it grows. */
static int core_of(const category *c, str picks, str rom) {
  int index = 0;
  const size_t cores = apps_core_count(c);
  while (picks.n > 0) {
    str name = str_cut(&picks, '\n');
    if (!str_eq(str_cut(&name, '\t'), rom))
      continue;
    for (size_t i = 0; i < cores; i++)
      if (str_eq(name, c->cores[i].name))
        index = (int)i;
  }
  return index;
}

void apps_set_core(category *c, app *a, int index) {
  FILE *f = state_file("-cores", "a");
  if (f != NULL) {
    str_fput(f, sb_str(a->arg), S("\t"), c->cores[index].name, S("\n"));
    fclose(f);
  }
  a->core = index;
  sb_set(a->exec, c->cores[index].exec);
}

static bool is_file(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static bool is_image(str name) {
  for (size_t i = 0; i < COUNTOF(image_exts); i++)
    if (str_iends(name, image_exts[i]))
      return true;
  return false;
}

/* The icon: `base` if it is an image, else `base` with an image extension */
static void find_image(app *a, str base) {
  if (is_image(base) && sb_set(a->icon, base) && is_file(a->icon.p))
    return;
  for (size_t i = 0; i < COUNTOF(image_exts); i++)
    if (sb_set(a->icon, base, image_exts[i]) && is_file(a->icon.p))
      return;
  sb_set(a->icon, S(""));
}

static app *add(category *c) {
  if (c->count == c->capacity) {
    const size_t capacity = c->capacity ? c->capacity * 2 : 32;
    app *grown = realloc(c->apps, capacity * sizeof(*grown));
    if (grown == NULL)
      return NULL;
    c->apps = grown;
    c->capacity = capacity;
  }
  app *a = &c->apps[c->count];
  memset(a, 0, sizeof(*a));
  return a;
}

/* Field codes (%f, %U, ...) are for file managers; %% is a percent sign.
 * Returns the new length. */
static size_t strip_field_codes(char *s, size_t n) {
  size_t out = 0;
  for (size_t in = 0; in < n; in++) {
    if (s[in] == '%' && in + 1 < n) {
      if (s[++in] == '%')
        s[out++] = '%';
      continue;
    }
    s[out++] = s[in];
  }
  s[out] = '\0';
  return out;
}

/* The [Desktop Entry] group; false for anything that isn't a shown app */
static bool parse_desktop(const char *path, str folder, app *a) {
  const str text = str_read(fopen(path, "r"));
  if (text.p == NULL)
    return false;
  STRBUF(256) icon = {0};
  bool shown = true;
  cfg_reader r = cfg_init(text);
  for (cfg_event e; (e = cfg_next(&r)) != CFG_END;) {
    /* Localized keys (Name[de]) aren't ours to pick */
    if (e != CFG_VALUE || !str_eq(r.section, S("Desktop Entry")))
      continue;
    if (str_eq(r.key, S("Name")))
      sb_set(a->name, r.value);
    else if (str_eq(r.key, S("Exec")))
      sb_set(a->exec, r.value);
    else if (str_eq(r.key, S("Path")))
      sb_set(a->dir, r.value);
    else if (str_eq(r.key, S("Icon")))
      sb_set(icon, r.value);
    else if (str_eq(r.key, S("Keywords")) || str_eq(r.key, S("Categories")))
      for (str item; cfg_item(&r.value, &item);)
        sb_add(a->tags, item, S(";"));
    else if ((str_eq(r.key, S("Hidden")) || str_eq(r.key, S("NoDisplay"))) &&
             str_eq(r.value, S("true")))
      shown = false;
    else if (str_eq(r.key, S("Type")) && !str_eq(r.value, S("Application")))
      shown = false;
  }
  free((char *)text.p);
  a->exec.n = strip_field_codes(a->exec.p, a->exec.n);
  if (a->dir.n == 0)
    sb_set(a->dir, folder);
  if (icon.n > 0) {
    STRBUF(512) base;
    if (icon.p[0] == '/')
      sb_set(base, sb_str(icon));
    else
      sb_set(base, folder, S("/"), sb_str(icon));
    find_image(a, sb_str(base));
  }
  return shown && a->name.n > 0 && a->exec.n > 0;
}

/* `name` without its extension */
static str stem(str name) {
  const size_t dot = str_rfind(name, '.');
  return dot != 0 ? str_head(name, dot) : name;
}

static bool has_ext(str name, str exts) {
  const size_t dot = str_rfind(name, '.');
  if (dot == 0 || dot == name.n)
    return false;
  for (str ext; cfg_item(&exts, &ext);)
    if (str_ieq(ext, str_at(name, dot)))
      return true;
  return false;
}

static void scan_entry(category *c, str picks, str folder, str name) {
  STRBUF(512) path;
  struct stat st;
  if (name.p[0] == '.' || !sb_set(path, folder, S("/"), name) ||
      stat(path.p, &st) != 0 || !S_ISREG(st.st_mode))
    return;
  app *a;
  if (c->roms.n > 0) {
    if (!has_ext(name, c->roms) || (a = add(c)) == NULL)
      return;
    sb_set(a->name, stem(name));
    sb_set(a->arg, sb_str(path));
    a->core = core_of(c, picks, sb_str(path));
    sb_set(a->exec, c->cores[a->core].exec);
    sb_set(a->dir, root(), S("/"), c->system_dir);
    find_image(a, stem(sb_str(path)));
    uint32_t crc;
    romdb_game game;
    STRBUF(512) pack;
    if (c->pack.n > 0 && sb_set(pack, root(), S("/OS/packs/"), c->pack, S(".pack")) &&
        romdb_crc(sb_str(path), &st, &crc) && romdb_find(sb_str(pack), crc, &game)) {
      sb_set(a->name, game.title);
      if (a->icon.n == 0)
        sb_set(a->icon, sb_str(game.image));
    }
    c->count++;
    return;
  }
  if (str_iends(name, S(".desktop"))) {
    if ((a = add(c)) != NULL && parse_desktop(path.p, folder, a))
      c->count++;
    return;
  }
  /* A bare executable, unless a .desktop speaks for it */
  STRBUF(600) desktop;
  sb_set(desktop, sb_str(path), S(".desktop"));
  if (is_image(name) || access(path.p, X_OK) != 0 || is_file(desktop.p) || (a = add(c)) == NULL)
    return;
  sb_set(a->name, name);
  sb_set(a->exec, S("exec \"$1\""));
  sb_set(a->arg, sb_str(path));
  sb_set(a->dir, folder);
  find_image(a, sb_str(path));
  c->count++;
}

static int by_name(const void *a, const void *b) {
  return strcasecmp(((const app *)a)->name.p, ((const app *)b)->name.p);
}

bool apps_present(const category *c) {
  STRBUF(256) folder;
  struct stat st;
  return c->roms.n == 0 || (sb_set(folder, root(), S("/"), c->folder) &&
                            stat(folder.p, &st) == 0 && S_ISDIR(st.st_mode));
}

void apps_scan(category *c) {
  c->count = 0;
  STRBUF(256) folder;
  sb_set(folder, root(), S("/"), c->folder);
  DIR *dir = opendir(folder.p);
  if (dir == NULL)
    return;
  const str picks = c->roms.n > 0 ? str_read(state_file("-cores", "r")) : (str){0};
  struct dirent *d;
  while ((d = readdir(dir)) != NULL)
    scan_entry(c, picks, sb_str(folder), str_c(d->d_name));
  closedir(dir);
  free((char *)picks.p);
  qsort(c->apps, c->count, sizeof(*c->apps), by_name);
}
