#include "apps.h"
#include "cfg.h"
#include "romdb.h"
#include "util.h"
#include "wake.h"

#include <dirent.h>
#include <fnmatch.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

/* The image's systems, emulators and players; apps/systems and
 * apps/emulators on the card over them */
#define SYSTEMS "/usr/share/senkos/systems"
#define EMULATORS "/usr/share/senkos/emulators"

/* The systems (apps_init), then the app folders */
enum { CATEGORIES_MAX = 64 };
static category all[CATEGORIES_MAX];
category *categories = all;
size_t category_count;
static const category app_folders[] = {
    {.name = S("Ports"), .folder = S("apps/ports")},
    {.name = S("Applications"), .folder = S("apps/applications")},
    {.name = S("Benchmarks"), .folder = S("apps/benchmarks")},
};

static const str image_exts[] = {S(".png"), S(".jpg"), S(".jpeg"), S(".webp"), S(".bmp")};

static str root(void) {
  static str r;
  if (r.p == NULL) {
    const char *env = getenv("LAUNCHER_DATA");
    r = env != NULL && env[0] != '\0' ? str_c(env) : S("/data");
  }
  return r;
}

/* $`var`/launcher`suffix`, else $HOME/`fallback`/launcher`suffix` */
static FILE *xdg_file(const char *var, const char *fallback, const char *suffix, const char *mode) {
  char path[512];
  const char *dir = getenv(var), *home = getenv("HOME");
  if (dir != NULL && dir[0] == '/')
    snprintf(path, sizeof(path), "%s/launcher%s", dir, suffix);
  else
    snprintf(path, sizeof(path), "%s/%s/launcher%s", home ? home : "/tmp", fallback, suffix);
  /* mkdir -p of what may be missing */
  for (char *slash = strchr(path + 1, '/'); mode[0] != 'r' && slash; slash = strchr(slash + 1, '/')) {
    *slash = '\0';
    mkdir(path, 0755);
    *slash = '/';
  }
  return fopen(path, mode);
}

FILE *state_file(const char *suffix, const char *mode) {
  return xdg_file("XDG_STATE_HOME", ".local/state", suffix, mode);
}

FILE *config_file(const char *suffix, const char *mode) {
  return xdg_file("XDG_CONFIG_HOME", ".config", suffix, mode);
}

/* Runs the game with core `index`: in its Path, else the game's folder. None:
 * nothing to run it with (main.c says so). */
static void use_core(const category *c, app *a, int index) {
  a->core = index;
  if (c->core_count == 0) {
    a->exec.n = a->exec.p[0] = 0;
    return;
  }
  sb_set(a->exec, sb_str(c->cores[index].exec));
  if (c->cores[index].dir.n > 0)
    sb_set(a->dir, sb_str(c->cores[index].dir));
  else
    sb_set(a->dir, str_head(sb_str(a->arg), str_rfind(sb_str(a->arg), '/')));
}

/* The core picked for `rom` in `picks` (launcher-cores: a line
 * "ROM\tcore name" each); else the first */
static int core_of(const category *c, str picks, str rom) {
  while (picks.n > 0) {
    str name = str_cut(&picks, '\n');
    if (!str_eq(str_cut(&name, '\t'), rom))
      continue;
    for (size_t i = 0; i < c->core_count; i++)
      if (str_eq(name, sb_str(c->cores[i].name)))
        return (int)i;
  }
  return 0;
}

/* The game's line replaced: the others as they were, then its own */
void apps_set_core(category *c, app *a, int index) {
  const str text = str_read(config_file("-cores", "r"));
  FILE *f = config_file("-cores", "w");
  if (f != NULL) {
    for (str rest = text; rest.n > 0;) {
      const str line = str_cut(&rest, '\n');
      str name = line;
      if (!str_eq(str_cut(&name, '\t'), sb_str(a->arg)))
        str_fput(f, line, S("\n"));
    }
    str_fput(f, sb_str(a->arg), S("\t"), sb_str(c->cores[index].name), S("\n"));
    fclose(f);
  }
  free((char *)text.p);
  use_core(c, a, index);
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

/* Field codes (%f, %U, ...) are for file managers: the file's become "$1"
 * for an emulator (`file`), the rest go; %% is a percent sign */
static void field_codes(app *a, bool file) {
  STRBUF(sizeof(a->exec.p)) out = {0};
  for (size_t i = 0; i < a->exec.n; i++) {
    if (a->exec.p[i] != '%' || i + 1 == a->exec.n) {
      sb_add(out, (str){&a->exec.p[i], 1});
      continue;
    }
    const char code = a->exec.p[++i];
    if (code == '%')
      sb_add(out, S("%"));
    else if (file && strchr("fFuU", code) != NULL)
      sb_add(out, S("\"$1\""));
  }
  sb_set(a->exec, sb_str(out));
}

/* The [Desktop Entry] group; false for anything that isn't a shown app.
 * `mimes`: an emulator's MimeType, NULL for an app. */
static bool parse_desktop(const char *path, str folder, app *a, str *mimes) {
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
    else if (str_eq(r.key, S("MimeType")) && mimes != NULL)
      *mimes = str_dup(r.value);
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
  field_codes(a, mimes != NULL);
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

/* `name` matches one of `patterns` ("*.gba;*.zip;"), case aside */
static bool matches(str name, str patterns) {
  for (str pattern; cfg_item(&patterns, &pattern);) {
    char p[64];
    snprintf(p, sizeof(p), "%.*s", (int)pattern.n, pattern.p);
    if (fnmatch(p, name.p, FNM_CASEFOLD) == 0)
      return true;
  }
  return false;
}

/* A port: a folder with a port.ini ([Port] Name), run by portrun; its cover
 * or screenshot is the icon */
static bool parse_port(str dir, app *a) {
  STRBUF(600) ini;
  sb_set(ini, dir, S("/port.ini"));
  const str text = str_read(fopen(ini.p, "r"));
  if (text.p == NULL)
    return false;
  cfg_reader r = cfg_init(text);
  for (cfg_event e; (e = cfg_next(&r)) != CFG_END;)
    if (e == CFG_VALUE && str_eq(r.section, S("Port")) && str_eq(r.key, S("Name")))
      sb_set(a->name, r.value);
  free((char *)text.p);
  sb_set(a->exec, S("exec portrun \"$1\""));
  sb_set(a->arg, dir);
  sb_set(a->dir, dir);
  STRBUF(600) base;
  sb_set(base, dir, S("/cover"));
  find_image(a, sb_str(base));
  if (a->icon.n == 0 && sb_set(base, dir, S("/screenshot")))
    find_image(a, sb_str(base));
  return a->name.n > 0;
}

static void scan_entry(category *c, str picks, str folder, str name) {
  STRBUF(512) path;
  struct stat st;
  if (name.p[0] == '.' || !sb_set(path, folder, S("/"), name) || stat(path.p, &st) != 0)
    return;
  app *a;
  if (S_ISDIR(st.st_mode)) {
    if (c->roms.n == 0 && (a = add(c)) != NULL && parse_port(sb_str(path), a))
      c->count++;
    return;
  }
  if (!S_ISREG(st.st_mode))
    return;
  if (c->roms.n > 0) {
    if (!matches(name, c->roms) || (a = add(c)) == NULL)
      return;
    sb_set(a->name, stem(name));
    sb_set(a->arg, sb_str(path));
    use_core(c, a, core_of(c, picks, sb_str(path)));
    find_image(a, stem(sb_str(path)));
    uint32_t crc;
    romdb_game game;
    STRBUF(512) pack;
    if (c->pack.n > 0 && sb_set(pack, root(), S("/OS/assets/thumbnails/"), c->pack, S(".pack")) &&
        romdb_crc(sb_str(path), &st, &crc) && romdb_find(sb_str(pack), crc, &game)) {
      sb_set(a->name, game.title);
      if (a->icon.n == 0)
        sb_set(a->icon, sb_str(game.image));
    }
    c->count++;
    return;
  }
  if (str_iends(name, S(".desktop"))) {
    if ((a = add(c)) != NULL && parse_desktop(path.p, folder, a, NULL))
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

source sources[SOURCES_MAX];
size_t source_count;

static bool is_dir(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

bool apps_present(const category *c) {
  STRBUF(256) folder;
  if (sb_set(folder, root(), S("/"), c->folder) && is_dir(folder.p))
    return true;
  for (size_t i = 0; i < source_count; i++)
    if (&categories[sources[i].category] == c && is_dir(sources[i].path.p))
      return true;
  return false;
}

static void scan_folder(category *c, str picks, str folder) {
  DIR *dir = opendir(folder.p);
  if (dir == NULL)
    return;
  struct dirent *d;
  while ((d = readdir(dir)) != NULL)
    scan_entry(c, picks, folder, str_c(d->d_name));
  closedir(dir);
}

static bool shares_item(str a, str b) {
  for (str item; cfg_item(&a, &item);)
    for (str rest = b, other; cfg_item(&rest, &other);)
      if (str_eq(item, other))
        return true;
  return false;
}

static int by_core_name(const void *a, const void *b) {
  return strcasecmp(((const core *)a)->name.p, ((const core *)b)->name.p);
}

/* `each` on the files named *`ext` in the card's folder `mine` (under the
 * data root), then in the image's `image`, but for those the card has one of
 * the same name of: it replaces the image's, or hides it (Hidden=true) */
static void layered(const char *mine, const char *image, str ext, void (*each)(const char *path, void *ctx),
                    void *ctx) {
  STRBUF(256) card;
  sb_set(card, root(), S("/"), str_c(mine));
  STRBUF(4096) seen = {0};
  for (int pass = 0; pass < 2; pass++) {
    const char *folder = pass == 0 ? card.p : image;
    DIR *dir = opendir(folder);
    if (dir == NULL)
      continue;
    for (struct dirent *d; (d = readdir(dir)) != NULL;) {
      const str name = str_c(d->d_name);
      STRBUF(512) path;
      if (name.p[0] == '.' || !str_iends(name, ext) || !sb_set(path, str_c(folder), S("/"), name))
        continue;
      if (pass == 0)
        sb_add(seen, name, S(";"));
      else if (shares_item(name, sb_str(seen)))
        continue;
      each(path.p, ctx);
    }
    closedir(dir);
  }
}

/* An emulator, for each system its MimeType says */
static void add_core(const char *path, void *ctx) {
  (void)ctx;
  static app e;
  memset(&e, 0, sizeof(e));
  str mimes = {0};
  if (parse_desktop(path, S(""), &e, &mimes))
    for (size_t i = 0; i < category_count; i++) {
      category *c = &categories[i];
      if (c->roms.n == 0 || !shares_item(mimes, c->mimes) || c->core_count == CORES_MAX)
        continue;
      core *k = &c->cores[c->core_count++];
      sb_set(k->name, sb_str(e.name));
      sb_set(k->exec, sb_str(e.exec));
      sb_set(k->dir, sb_str(e.dir));
    }
  free((char *)mimes.p);
}

/* A system: its [System] group. Incomplete or hidden ones aren't. */
static void add_system(const char *path, void *ctx) {
  (void)ctx;
  const str text = str_read(fopen(path, "r"));
  if (text.p == NULL || category_count == CATEGORIES_MAX - COUNTOF(app_folders)) {
    free((char *)text.p);
    return;
  }
  category *c = &categories[category_count];
  memset(c, 0, sizeof(*c));
  bool hidden = false;
  cfg_reader r = cfg_init(text);
  for (cfg_event e; (e = cfg_next(&r)) != CFG_END;) {
    if (e != CFG_VALUE || !str_eq(r.section, S("System")))
      continue;
    str *field = str_eq(r.key, S("Name"))       ? &c->name
                 : str_eq(r.key, S("Folder"))   ? &c->folder
                 : str_eq(r.key, S("Aliases"))  ? &c->names
                 : str_eq(r.key, S("Patterns")) ? &c->roms
                 : str_eq(r.key, S("MimeType")) ? &c->mimes
                 : str_eq(r.key, S("Pack"))     ? &c->pack
                                                : NULL;
    if (field != NULL)
      *field = str_dup(r.value);
    else if (str_eq(r.key, S("Order")))
      c->order = (int)strtol(r.value.p, NULL, 10);
    else if (str_eq(r.key, S("Hidden")) && str_eq(r.value, S("true")))
      hidden = true;
  }
  free((char *)text.p);
  if (!hidden && c->name.n > 0 && c->folder.n > 0 && c->roms.n > 0 && c->mimes.n > 0)
    category_count++;
}

static int by_order(const void *a, const void *b) {
  const category *x = a, *y = b;
  return x->order != y->order ? (x->order > y->order) - (x->order < y->order) : strcasecmp(x->name.p, y->name.p);
}

void apps_init(void) {
  category_count = 0;
  layered("apps/systems", SYSTEMS, S(".system"), add_system, NULL);
  qsort(categories, category_count, sizeof(*categories), by_order);
  for (size_t i = 0; i < COUNTOF(app_folders); i++)
    categories[category_count++] = app_folders[i];
  apps_cores();
}

void apps_cores(void) {
  for (size_t i = 0; i < category_count; i++)
    categories[i].core_count = 0;
  layered("apps/emulators", EMULATORS, S(".desktop"), add_core, NULL);
  for (size_t i = 0; i < category_count; i++)
    qsort(categories[i].cores, categories[i].core_count, sizeof(core), by_core_name);
}

/* The category's folder, then its sources */
void apps_scan(category *c) {
  c->count = 0;
  STRBUF(256) folder;
  sb_set(folder, root(), S("/"), c->folder);
  const str picks = c->roms.n > 0 ? str_read(config_file("-cores", "r")) : (str){0};
  scan_folder(c, picks, sb_str(folder));
  for (size_t i = 0; i < source_count; i++)
    if (&categories[sources[i].category] == c)
      scan_folder(c, picks, sb_str(sources[i].path));
  free((char *)picks.p);
  qsort(c->apps, c->count, sizeof(*c->apps), by_name);
}

void sources_load(void) {
  const str text = str_read(config_file("-folders", "r"));
  source_count = 0;
  for (str rest = text; rest.n > 0 && source_count < SOURCES_MAX;) {
    str path = str_cut(&rest, '\n');
    const str system = str_cut(&path, '\t');
    for (size_t i = 0; i < category_count && path.n > 0 && path.p[0] == '/'; i++)
      if (str_eq(categories[i].folder, system) &&
          sb_set(sources[source_count].path, path)) {
        sources[source_count++].category = i;
        break;
      }
  }
  free((char *)text.p);
}

int apps_guess(str folder) {
  const str name = str_at(folder, str_rfind(folder, '/') + 1);
  for (size_t i = 0; i < category_count; i++)
    for (str names = categories[i].names, item; cfg_item(&names, &item);)
      if (str_ieq(item, name))
        return (int)i;
  return -1;
}

bool sources_has(str path, size_t category) {
  for (size_t i = 0; i < source_count; i++)
    if (sources[i].category == category && str_eq(sb_str(sources[i].path), path))
      return true;
  return false;
}

void sources_toggle(str path, size_t category) {
  const bool had = sources_has(path, category);
  FILE *f = config_file("-folders", "w");
  if (f == NULL)
    return;
  for (size_t i = 0; i < source_count; i++)
    if (sources[i].category != category || !str_eq(sb_str(sources[i].path), path))
      str_fput(f, categories[sources[i].category].folder, S("\t"), sb_str(sources[i].path), S("\n"));
  if (!had)
    str_fput(f, categories[category].folder, S("\t"), path, S("\n"));
  fclose(f);
  sources_load();
}

uint64_t sources_present(void) {
  uint64_t present = 0;
  for (size_t i = 0; i < source_count; i++)
    present |= (uint64_t)is_dir(sources[i].path.p) << i;
  return present;
}

/* The card's emulators, copied over MTP or SMB: UI_RELOAD once they settle. While the
 * folder isn't there, apps/ for it to come. */
static void *watch_thread(void *arg) {
  (void)arg;
  const int fd = inotify_init1(IN_CLOEXEC);
  STRBUF(256) apps, emulators;
  sb_set(apps, root(), S("/apps"));
  sb_set(emulators, sb_str(apps), S("/emulators"));
  enum { CHANGES = IN_CREATE | IN_DELETE | IN_CLOSE_WRITE | IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF };
  inotify_add_watch(fd, apps.p, IN_CREATE | IN_MOVED_TO);
  int wd = inotify_add_watch(fd, emulators.p, CHANGES);
  char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
  for (bool changed = false; fd >= 0;) {
    /* Quiet for 300 ms after a change: one reload for a copy of many */
    struct pollfd p = {.fd = fd, .events = POLLIN};
    if (poll(&p, 1, changed ? 300 : -1) == 0) {
      changed = false;
      win_wake(UI_RELOAD, 0);
      continue;
    }
    const ssize_t n = read(fd, buf, sizeof(buf));
    for (ssize_t at = 0; at < n;) {
      const struct inotify_event *e = (const struct inotify_event *)(buf + at);
      if (e->wd == wd)
        changed = true;
      else if (e->len > 0 && strcmp(e->name, "emulators") == 0)
        changed = (wd = inotify_add_watch(fd, emulators.p, CHANGES)) >= 0;
      at += (ssize_t)sizeof(*e) + e->len;
    }
  }
  return NULL;
}

void apps_watch(void) { spawn("watch", watch_thread, NULL); }
