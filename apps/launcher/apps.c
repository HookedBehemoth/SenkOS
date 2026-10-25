#include "apps.h"
#include "cfg.h"
#include "romdb.h"
#include "running.h"
#include "util.h"
#include "wake.h"
#include "xml.h"

#include <dirent.h>
#include <elf.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <ftw.h>
#include <grp.h>
#include <linux/openat2.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <squashfuse.h>

/* The image's systems, emulators and players; apps/systems and
 * apps/emulators on the card over them */
#define SYSTEMS "/usr/share/senkos/systems"
#define EMULATORS "/usr/share/senkos/emulators"
#define APPLICATIONS "/usr/share/senkos/applications"

/* The systems (apps_init), then the app folders */
enum { CATEGORIES_MAX = 64 };
static category all[CATEGORIES_MAX];
category *categories = all;
size_t category_count;
static const category app_folders[] = {
    {.name = S("Applications"), .folder = S("apps/applications"), .image = S(APPLICATIONS)},
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

/* $`var`/launcher`suffix`, else $HOME/`fallback`/launcher`suffix`; for writing, its folders made */
static void xdg_path(char path[512], const char *var, const char *fallback, const char *suffix, bool write) {
  const char *dir = getenv(var), *home = getenv("HOME");
  if (dir != NULL && dir[0] == '/')
    snprintf(path, 512, "%s/launcher%s", dir, suffix);
  else
    snprintf(path, 512, "%s/%s/launcher%s", home ? home : "/tmp", fallback, suffix);
  /* mkdir -p of what may be missing */
  for (char *slash = strchr(path + 1, '/'); write && slash; slash = strchr(slash + 1, '/')) {
    *slash = '\0';
    mkdir(path, 0755);
    *slash = '/';
  }
}

static FILE *xdg_file(const char *var, const char *fallback, const char *suffix, const char *mode) {
  char path[512];
  xdg_path(path, var, fallback, suffix, mode[0] != 'r');
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
  a->flags = c->cores[index].flags;
  if (c->cores[index].dir.n > 0)
    sb_set(a->dir, sb_str(c->cores[index].dir));
  else
    sb_set(a->dir, str_head(sb_str(a->arg), str_rfind(sb_str(a->arg), '/')));
}

/* The value of `key`'s line ("key\tvalue") in `lines`, if it has one */
static bool line_of(str lines, str key, str *value) {
  while (lines.n > 0) {
    *value = str_cut(&lines, '\n');
    if (str_eq(str_cut(value, '\t'), key))
      return true;
  }
  return false;
}

/* The config file `suffix`'s line for `key` replaced: the others as they were, then its own
 * (none for an empty value) */
static void set_line(const char *suffix, str key, str value) {
  const str text = str_read(config_file(suffix, "r"));
  FILE *f = config_file(suffix, "w");
  if (f != NULL) {
    for (str rest = text; rest.n > 0;) {
      const str line = str_cut(&rest, '\n');
      str name = line;
      if (!str_eq(str_cut(&name, '\t'), key))
        str_fput(f, line, S("\n"));
    }
    if (value.n > 0)
      str_fput(f, key, S("\t"), value, S("\n"));
    fclose(f);
  }
  free((char *)text.p);
}

str apps_pack(const category *c) {
  static STRBUF(512) path;
  if (c->pack.n == 0 || !sb_set(path, root(), S("/OS/assets/thumbnails/"), c->pack, S(".pack")))
    return S("");
  return sb_str(path);
}

/* The core picked for `rom` in `picks` (launcher-cores: a line
 * "ROM\tcore name" each); else the first */
static int core_of(const category *c, str picks, str rom) {
  str name;
  if (line_of(picks, rom, &name))
    for (size_t i = 0; i < c->core_count; i++)
      if (str_eq(name, sb_str(c->cores[i].name)))
        return (int)i;
  return 0;
}

void apps_set_core(category *c, app *a, int index) {
  set_line("-cores", sb_str(a->arg), sb_str(c->cores[index].name));
  use_core(c, a, index);
}

/* launcher-matches: a line "ROM\tCRC" (hex) each */
void apps_set_match(app *a, uint32_t crc) {
  STRBUF(16) hex = {0};
  if (crc != 0)
    sb_fmt(hex, "%08x", crc);
  set_line("-matches", sb_str(a->arg), sb_str(hex));
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
    else if (str_eq(r.key, S("X-SenkOS-AskQuit")) && str_eq(r.value, S("false")))
      a->flags |= APP_QUIT_NOW;
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

/* An aarch64 AppImage: the runtime, an ELF marked AI 2 (type 2), then its file system at
 * `*offset` */
static bool appimage_header(int fd, size_t *offset) {
  Elf64_Ehdr h;
  if (pread(fd, &h, sizeof(h), 0) != sizeof(h) || memcmp(h.e_ident, ELFMAG, SELFMAG) != 0 ||
      h.e_ident[EI_CLASS] != ELFCLASS64 || memcmp(&h.e_ident[EI_ABIVERSION], "AI\2", 3) != 0 ||
      h.e_machine != EM_AARCH64)
    return false;
  *offset = h.e_shoff + (size_t)h.e_shentsize * h.e_shnum;
  return true;
}

/* An AppImage's files: its SquashFS, else what dwarfsextract took out of its DwarFS (`dir`) */
typedef struct {
  sqfs *fs;
  int dir;
} appfs;

/* The file `name` in its root, through symlinks (which stay inside); NULL if there's none (or
 * it's over 4 MB) */
static str appfs_file(appfs *f, const char *name) {
  if (f->fs == NULL) {
    struct open_how how = {.flags = O_RDONLY | O_CLOEXEC, .resolve = RESOLVE_IN_ROOT};
    const int fd = (int)syscall(SYS_openat2, f->dir, name, &how, sizeof(how));
    struct stat st;
    if (fd >= 0 && (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > 4 << 20)) {
      close(fd);
      return (str){0};
    }
    return fd >= 0 ? str_read(fdopen(fd, "r")) : (str){0};
  }
  char path[256];
  snprintf(path, sizeof(path), "%s", name);
  for (int links = 0; links < 8; links++) {
    sqfs_inode inode;
    bool found;
    if (sqfs_inode_get(f->fs, &inode, sqfs_inode_root(f->fs)) != SQFS_OK ||
        sqfs_lookup_path(f->fs, &inode, path, &found) != SQFS_OK || !found)
      break;
    size_t size = sizeof(path);
    if (S_ISLNK(inode.base.mode) && sqfs_readlink(f->fs, &inode, path, &size) == SQFS_OK)
      continue;
    if (!S_ISREG(inode.base.mode) || inode.xtra.reg.file_size > 4 << 20)
      break;
    sqfs_off_t n = (sqfs_off_t)inode.xtra.reg.file_size;
    char *data = malloc((size_t)n + 1);
    if (data == NULL || sqfs_read_range(f->fs, &inode, 0, &n, data) != SQFS_OK) {
      free(data);
      break;
    }
    data[n] = '\0';
    return (str){data, (size_t)n};
  }
  return (str){0};
}

/* The .desktop in its root */
static str appfs_desktop(appfs *f) {
  if (f->fs == NULL) {
    DIR *dir = fdopendir(openat(f->dir, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    str text = {0};
    for (struct dirent *d; dir != NULL && text.p == NULL && (d = readdir(dir)) != NULL;)
      if (str_iends(str_c(d->d_name), S(".desktop")))
        text = appfs_file(f, d->d_name);
    if (dir != NULL)
      closedir(dir);
    return text;
  }
  sqfs_inode root;
  sqfs_dir dir;
  sqfs_dir_entry entry;
  sqfs_name name;
  sqfs_err err;
  if (sqfs_inode_get(f->fs, &root, sqfs_inode_root(f->fs)) != SQFS_OK ||
      sqfs_dir_open(f->fs, &root, &dir, 0) != SQFS_OK)
    return (str){0};
  sqfs_dentry_init(&entry, name);
  while (sqfs_dir_next(f->fs, &dir, &entry, &err))
    if (str_iends(str_c(sqfs_dentry_name(&entry)), S(".desktop")))
      return appfs_file(f, sqfs_dentry_name(&entry));
  return (str){0};
}

static bool is_png(str data) { return data.n > 4 && memcmp(data.p, "\x89PNG", 4) == 0; }

/* A PNG for the icon: .DirIcon, else Icon= in the hicolor theme (.DirIcon may be an SVG) */
static str appfs_icon(appfs *f, str desktop) {
  str png = appfs_file(f, ".DirIcon");
  if (is_png(png))
    return png;
  free((char *)png.p);
  STRBUF(256) name = {0};
  cfg_reader r = cfg_init(desktop);
  for (cfg_event e; (e = cfg_next(&r)) != CFG_END;)
    if (e == CFG_VALUE && str_eq(r.section, S("Desktop Entry")) && str_eq(r.key, S("Icon")))
      sb_set(name, r.value);
  static const char *const sizes[] = {"256x256", "512x512", "128x128"};
  for (size_t i = 0; i < COUNTOF(sizes) && name.n > 0; i++) {
    char path[320];
    snprintf(path, sizeof(path), "usr/share/icons/hicolor/%s/apps/%s.png", sizes[i], name.p);
    if (is_png(png = appfs_file(f, path)))
      return png;
    free((char *)png.p);
  }
  return (str){0};
}

static void write_file(const char *path, str data) {
  FILE *f = fopen(path, "w");
  if (f != NULL) {
    fwrite(data.p, 1, data.n, f);
    fclose(f);
  }
}

/* `base`.png and `base`.desktop in the cache, from `f` (NULL: none to be had). The .desktop
 * last, and whole: it says the cache is there. */
static void cache_appimage(appfs *f, const char *base) {
  char path[540], tmp[560];
  const str text = f != NULL ? appfs_desktop(f) : (str){0};
  const str png = f != NULL ? appfs_icon(f, text) : (str){0};
  snprintf(path, sizeof(path), "%s.png", base);
  if (png.n > 0)
    write_file(path, png);
  else
    unlink(path);
  snprintf(path, sizeof(path), "%s.desktop", base);
  snprintf(tmp, sizeof(tmp), "%s.new", path);
  write_file(tmp, text);
  rename(tmp, path);
  free((char *)text.p);
  free((char *)png.p);
}

static int remove_entry(const char *path, const struct stat *st, int type, struct FTW *ftw) {
  (void)st, (void)type, (void)ftw;
  remove(path);
  return 0;
}

/* The runtime's own dwarfsextract, run as an app is (made executable as it would be), takes the
 * .desktop and icons out into `$2` */
static const char dwarfs_extract[] =
    "chmod +x \"$1\" && umask 002 && exec timeout 60 \"$1\" --appimage-dwarfsextract -i \"$1\" -o \"$2\" "
    "--log-level error --pattern '*.desktop' --pattern .DirIcon --pattern '*.png' "
    "--pattern 'usr/share/applications/*.desktop' --pattern 'usr/share/icons/hicolor/*/apps/*.png'";

/* One at a time: each wakes a rescan, which starts the next */
static atomic_bool extracting;
typedef struct {
  char path[512], base[512];
} extract_job;

/* DwarFS takes seconds to decompress its way to a .desktop: not on the UI's thread */
static void *extract_thread(void *arg) {
  extract_job *job = arg;
  /* The apps' user writes into it, we read and remove what it wrote (umask 002): its group */
  char dir[] = "/tmp/appimage-XXXXXX";
  const struct group *player = getgrnam("player");
  appfs f = {.dir = -1};
  const bool made = mkdtemp(dir) != NULL;
  if (made && chmod(dir, 0770) == 0 && (player == NULL || chown(dir, (uid_t)-1, player->gr_gid) == 0) &&
      run_helper(dwarfs_extract, job->path, dir))
    f.dir = open(dir, O_PATH | O_DIRECTORY | O_CLOEXEC);
  cache_appimage(f.dir >= 0 ? &f : NULL, job->base);
  if (f.dir >= 0)
    close(f.dir);
  if (made)
    nftw(dir, remove_entry, 8, FTW_DEPTH | FTW_PHYS);
  free(job);
  atomic_store(&extracting, false);
  win_wake(UI_RELOAD, 0);
  return NULL;
}

static void extract_later(const char *path, const char *base) {
  if (atomic_exchange(&extracting, true))
    return;
  extract_job *job = malloc(sizeof(*job));
  if (job != NULL) {
    snprintf(job->path, sizeof(job->path), "%s", path);
    snprintf(job->base, sizeof(job->base), "%s", base);
    if (spawn("appimage", extract_thread, job))
      return;
  }
  free(job);
  atomic_store(&extracting, false);
}

/* `path` as an app if it's an AppImage: made executable as it starts. Its .desktop and icon are
 * cached for as long as they're newer than it; a DwarFS one's come later (extract_thread). */
static bool appimage(const char *path, const struct stat *st, str folder, str name, app *a) {
  size_t offset;
  const int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0 || !appimage_header(fd, &offset)) {
    if (fd >= 0)
      close(fd);
    return false;
  }
  char base[512], suffix[320], file[540];
  snprintf(suffix, sizeof(suffix), "/appimage/%.*s", (int)name.n, name.p);
  xdg_path(base, "XDG_CACHE_HOME", ".cache", suffix, true);
  snprintf(file, sizeof(file), "%s.desktop", base);
  struct stat cached;
  if (stat(file, &cached) != 0 || cached.st_mtime < st->st_mtime) {
    sqfs fs;
    if (sqfs_init(&fs, fd, offset) == SQFS_OK) {
      cache_appimage(&(appfs){.fs = &fs}, base);
      sqfs_destroy(&fs);
    } else {
      extract_later(path, base);
    }
  }
  close(fd);
  parse_desktop(file, folder, a, NULL);
  if (a->name.n == 0)
    sb_set(a->name, stem(name));
  sb_set(a->exec, S("chmod +x \"$1\" 2>/dev/null; exec \"$1\""));
  sb_set(a->arg, str_c(path));
  sb_set(a->dir, folder);
  snprintf(file, sizeof(file), "%s.png", base);
  sb_set(a->icon, is_file(file) ? str_c(file) : S(""));
  return true;
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

/* Art named by ES-style game lists (<game><path>./X</path><image>./Y</image></game>): the
 * folder's gamelist.xml (a scraper's) and its folders' gameinfo.xml (a PortMaster port's own),
 * paths in either the folder's, entities left as written. By path, sorted. */
typedef struct {
  str path, image;
} listed;
typedef struct {
  char *text;
  listed *games;
  size_t count;
} game_list;

static void list_add_file(char **text, size_t *n, const char *path) {
  const str s = str_read(fopen(path, "r"));
  char *grown = s.p != NULL ? realloc(*text, *n + s.n + 1) : NULL;
  if (grown != NULL) {
    memcpy(grown + *n, s.p, s.n);
    *n += s.n;
    grown[*n] = '\0';
    *text = grown;
  }
  free((char *)s.p);
}

/* Without a leading "./" */
static str without_dot(str s) { return s.n >= 2 && s.p[0] == '.' && s.p[1] == '/' ? str_at(s, 2) : s; }

static int by_path(const void *a, const void *b) {
  const str x = ((const listed *)a)->path, y = ((const listed *)b)->path;
  const int c = memcmp(x.p, y.p, x.n < y.n ? x.n : y.n);
  return c != 0 ? c : (x.n > y.n) - (x.n < y.n);
}

static game_list game_list_read(str folder) {
  game_list l = {0};
  size_t n = 0;
  STRBUF(512) path;
  if (sb_set(path, folder, S("/gamelist.xml")))
    list_add_file(&l.text, &n, path.p);
  DIR *dir = opendir(folder.p);
  for (struct dirent *d; dir != NULL && (d = readdir(dir)) != NULL;)
    if (d->d_name[0] != '.' && (d->d_type == DT_DIR || d->d_type == DT_UNKNOWN) &&
        sb_set(path, folder, S("/"), str_c(d->d_name), S("/gameinfo.xml")))
      list_add_file(&l.text, &n, path.p);
  if (dir != NULL)
    closedir(dir);
  size_t capacity = 0;
  listed g = {0};
  str *field = NULL;
  xml_reader r = xml_init((str){l.text, n});
  for (xml_event e; (e = xml_next(&r)) != XML_END;) {
    if (e == XML_START)
      field = str_eq(r.name, S("path")) ? &g.path : str_eq(r.name, S("image")) ? &g.image : NULL;
    else if (e == XML_TEXT && field != NULL)
      *field = without_dot(str_trim(r.content));
    else if (e == XML_CLOSE && str_eq(r.name, S("game"))) {
      if (g.path.n > 0 && g.image.n > 0) {
        if (l.count == capacity) {
          listed *grown = realloc(l.games, (capacity = capacity ? capacity * 2 : 16) * sizeof(*grown));
          if (grown == NULL)
            break;
          l.games = grown;
        }
        l.games[l.count++] = g;
      }
      g = (listed){0};
    } else
      field = NULL;
  }
  if (l.count > 0)
    qsort(l.games, l.count, sizeof(*l.games), by_path);
  return l;
}

static void game_list_free(game_list *l) {
  free(l->games);
  free(l->text);
}

static void listed_art(const game_list *l, str folder, str name, app *a) {
  const listed key = {name, {0}}, *g = l->count ? bsearch(&key, l->games, l->count, sizeof(key), by_path) : NULL;
  if (g == NULL)
    return;
  if (g->image.p[0] == '/' ? sb_set(a->icon, g->image) : sb_set(a->icon, folder, S("/"), g->image))
    if (is_file(a->icon.p))
      return;
  sb_set(a->icon, S(""));
}

static void scan_entry(category *c, str picks, str matched, const game_list *art, str folder, str name) {
  STRBUF(512) path;
  struct stat st;
  if (name.p[0] == '.' || !sb_set(path, folder, S("/"), name) || stat(path.p, &st) != 0)
    return;
  app *a;
  if (!S_ISREG(st.st_mode))
    return;
  if (c->roms.n > 0) {
    if (!matches(name, c->roms) || (a = add(c)) == NULL)
      return;
    sb_set(a->name, stem(name));
    sb_set(a->arg, sb_str(path));
    use_core(c, a, core_of(c, picks, sb_str(path)));
    find_image(a, stem(sb_str(path)));
    if (a->icon.n == 0)
      listed_art(art, folder, name, a);
    /* Picked by hand, else by its CRC */
    str match;
    STRBUF(16) hex;
    if (line_of(matched, sb_str(path), &match) && sb_set(hex, match))
      a->match = (uint32_t)strtoul(hex.p, NULL, 16);
    /* A jar names itself, and its icon is its art (jar.h) */
    const bool jar = str_iends(name, S(".jar"));
    if (jar && a->icon.n == 0 && !sb_set(a->icon, S("jar:"), sb_str(path)))
      a->icon.n = 0;
    uint32_t crc = a->match;
    char own[sizeof(a->name.p)];
    romdb_game game;
    const str pack = apps_pack(c);
    const bool hashed = (jar || (pack.n > 0 && crc == 0)) && romdb_crc(sb_str(path), &st, &crc, own, sizeof(own));
    if (hashed && own[0] != '\0')
      sb_set(a->name, str_c(own));
    if (a->match != 0)
      crc = a->match;
    if (pack.n > 0 && (a->match != 0 || hashed) && romdb_find(pack, crc, &game)) {
      sb_set(a->name, game.title);
      if (a->icon.n == 0)
        sb_set(a->icon, sb_str(game.image));
    }
    /* A video without an image: a frame of it (assets.h) */
    if (a->icon.n == 0 && str_eq(c->folder, S("videos")))
      sb_set(a->icon, S("video:"), sb_str(path));
    c->count++;
    return;
  }
  if (str_iends(name, S(".desktop"))) {
    if ((a = add(c)) != NULL && parse_desktop(path.p, folder, a, NULL))
      c->count++;
    return;
  }
  /* An AppImage or a bare executable, unless a .desktop speaks for it */
  STRBUF(600) desktop;
  sb_set(desktop, sb_str(path), S(".desktop"));
  if (is_image(name) || is_file(desktop.p) || (a = add(c)) == NULL)
    return;
  if (appimage(path.p, &st, folder, name, a)) {
    c->count++;
    return;
  }
  if (access(path.p, X_OK) != 0)
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

typedef STRBUF(256) folder_path;

/* The category's folder under the data root, each part matched ignoring case (gba, GBA) */
static void category_folder(const category *c, folder_path *out) {
  sb_set(*out, root());
  for (str rest = c->folder; rest.n > 0;) {
    const str part = str_cut(&rest, '/');
    const size_t at = out->n;
    if (!sb_add(*out, S("/"), part) || is_dir(out->p))
      continue;
    out->p[at] = '\0';
    DIR *dir = opendir(out->p);
    out->p[at] = '/';
    for (struct dirent *d; dir != NULL && (d = readdir(dir)) != NULL;)
      if (str_ieq(str_c(d->d_name), part)) {
        out->n = at;
        sb_add(*out, S("/"), str_c(d->d_name));
        break;
      }
    if (dir != NULL)
      closedir(dir);
  }
}

bool apps_present(const category *c) {
  folder_path folder;
  category_folder(c, &folder);
  if (is_dir(folder.p) || (c->image.n > 0 && is_dir(c->image.p)))
    return true;
  for (size_t i = 0; i < source_count; i++)
    if (&categories[sources[i].category] == c && is_dir(sources[i].path.p))
      return true;
  return false;
}

static void scan_folder(category *c, str picks, str matched, str folder) {
  DIR *dir = opendir(folder.p);
  if (dir == NULL)
    return;
  game_list art = c->roms.n > 0 ? game_list_read(folder) : (game_list){0};
  struct dirent *d;
  while ((d = readdir(dir)) != NULL)
    scan_entry(c, picks, matched, &art, folder, str_c(d->d_name));
  closedir(dir);
  game_list_free(&art);
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
      sb_set(k->file, str_c(strrchr(path, '/') + 1));
      sb_set(k->exec, sb_str(e.exec));
      sb_set(k->dir, sb_str(e.dir));
      k->flags = e.flags;
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
                 : str_eq(r.key, S("Emulator")) ? &c->emulator
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
  for (size_t i = 0; i < category_count; i++) {
    category *c = &categories[i];
    qsort(c->cores, c->core_count, sizeof(core), by_core_name);
    /* The system's Emulator first */
    for (size_t j = 1; j < c->core_count; j++)
      if (str_eq(sb_str(c->cores[j].file), c->emulator)) {
        const core k = c->cores[j];
        memmove(&c->cores[1], &c->cores[0], j * sizeof(core));
        c->cores[0] = k;
      }
  }
}

/* The category's folder, then its sources */
void apps_scan(category *c) {
  c->count = 0;
  folder_path folder;
  category_folder(c, &folder);
  const str picks = c->roms.n > 0 ? str_read(config_file("-cores", "r")) : (str){0};
  const str matched = c->roms.n > 0 ? str_read(config_file("-matches", "r")) : (str){0};
  scan_folder(c, picks, matched, sb_str(folder));
  DIR *dir = c->image.n > 0 ? opendir(c->image.p) : NULL;
  for (struct dirent *d; dir != NULL && (d = readdir(dir)) != NULL;) {
    STRBUF(512) mine;
    const str name = str_c(d->d_name);
    if (str_iends(name, S(".desktop")) && sb_set(mine, sb_str(folder), S("/"), name) && !is_file(mine.p))
      scan_entry(c, picks, matched, &(game_list){0}, c->image, name);
  }
  if (dir != NULL)
    closedir(dir);
  for (size_t i = 0; i < source_count; i++)
    if (&categories[sources[i].category] == c)
      scan_folder(c, picks, matched, sb_str(sources[i].path));
  free((char *)picks.p);
  free((char *)matched.p);
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
