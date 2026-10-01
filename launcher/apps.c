#include "apps.h"
#include "romdb.h"

#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

category categories[] = {
    /* mGBA reads portable.ini in the cwd and keeps its config next to it */
    {.name = "Game Boy Advance", .folder = "roms/gba", .roms = ".gba;.zip;",
     .system_dir = "mgba", .system_exec = "exec ./mgba \"$1\"",
     .pack = "Nintendo - Game Boy Advance"},
    {.name = "Ports", .folder = "OS/apps/ports"},
    {.name = "Applications", .folder = "OS/apps/applications"},
    {.name = "Benchmarks", .folder = "OS/apps/benchmarks"},
};
const size_t category_count = sizeof(categories) / sizeof(categories[0]);

static const char *const image_exts[] = {".png", ".jpg", ".jpeg", ".webp", ".bmp"};

static const char *root(void) {
  const char *r = getenv("LAUNCHER_DATA");
  return r != NULL && r[0] != '\0' ? r : "/data";
}

static bool is_file(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static bool ends_with(const char *s, const char *suffix) {
  const size_t n = strlen(s), m = strlen(suffix);
  return n >= m && strcasecmp(s + n - m, suffix) == 0;
}

static bool is_image(const char *name) {
  for (size_t i = 0; i < sizeof(image_exts) / sizeof(*image_exts); i++)
    if (ends_with(name, image_exts[i]))
      return true;
  return false;
}

/* `base` if it is an image, else `base` with an image extension */
static void find_image(char *out, size_t size, const char *base) {
  if (is_image(base) && is_file(base)) {
    snprintf(out, size, "%s", base);
    return;
  }
  for (size_t i = 0; i < sizeof(image_exts) / sizeof(*image_exts); i++)
    if (snprintf(out, size, "%s%s", base, image_exts[i]) < (int)size && is_file(out))
      return;
  out[0] = '\0';
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

/* Field codes (%f, %U, ...) are for file managers; %% is a percent sign */
static void strip_field_codes(char *s) {
  char *out = s;
  for (const char *in = s; *in; in++) {
    if (in[0] == '%' && in[1] != '\0') {
      if (in[1] == '%')
        *out++ = '%';
      in++;
      continue;
    }
    *out++ = *in;
  }
  *out = '\0';
}

static void append_tags(char *tags, size_t size, const char *value) {
  const size_t n = strlen(tags);
  snprintf(tags + n, size - n, "%s%s", n && tags[n - 1] != ';' ? ";" : "", value);
}

/* The [Desktop Entry] group; false for anything that isn't a shown app */
static bool parse_desktop(const char *path, const char *folder, app *a) {
  FILE *f = fopen(path, "r");
  if (f == NULL)
    return false;
  char line[1024], icon[256] = "";
  bool in_entry = false, shown = true;
  while (fgets(line, sizeof(line), f)) {
    line[strcspn(line, "\r\n")] = '\0';
    if (line[0] == '[') {
      in_entry = strcmp(line, "[Desktop Entry]") == 0;
      continue;
    }
    char *eq = strchr(line, '=');
    if (!in_entry || line[0] == '#' || eq == NULL)
      continue;
    /* Localized keys (Name[de]) aren't ours to pick */
    char *key = line, *value = eq + 1;
    for (*eq-- = '\0'; eq >= key && *eq == ' '; eq--)
      *eq = '\0';
    value += strspn(value, " ");
    if (strcmp(key, "Name") == 0)
      snprintf(a->name, sizeof(a->name), "%s", value);
    else if (strcmp(key, "Exec") == 0)
      snprintf(a->exec, sizeof(a->exec), "%s", value);
    else if (strcmp(key, "Path") == 0)
      snprintf(a->dir, sizeof(a->dir), "%s", value);
    else if (strcmp(key, "Icon") == 0)
      snprintf(icon, sizeof(icon), "%s", value);
    else if (strcmp(key, "Keywords") == 0 || strcmp(key, "Categories") == 0)
      append_tags(a->tags, sizeof(a->tags), value);
    else if ((strcmp(key, "Hidden") == 0 || strcmp(key, "NoDisplay") == 0) &&
             strcmp(value, "true") == 0)
      shown = false;
    else if (strcmp(key, "Type") == 0 && strcmp(value, "Application") != 0)
      shown = false;
  }
  fclose(f);
  strip_field_codes(a->exec);
  if (a->dir[0] == '\0')
    snprintf(a->dir, sizeof(a->dir), "%s", folder);
  if (icon[0] != '\0') {
    char base[512];
    snprintf(base, sizeof(base), "%s%s%s", icon[0] == '/' ? "" : folder,
             icon[0] == '/' ? "" : "/", icon);
    find_image(a->icon, sizeof(a->icon), base);
  }
  return shown && a->name[0] != '\0' && a->exec[0] != '\0';
}

static bool has_ext(const char *name, const char *exts) {
  const char *dot = strrchr(name, '.');
  if (dot == NULL || dot == name)
    return false;
  const size_t n = strlen(dot);
  for (const char *e = exts; *e; e += strcspn(e, ";") + (e[strcspn(e, ";")] != '\0'))
    if (strcspn(e, ";") == n && strncasecmp(e, dot, n) == 0)
      return true;
  return false;
}

/* `name` without its extension, in `out` */
static void stem(char *out, size_t size, const char *name) {
  const char *dot = strrchr(name, '.');
  snprintf(out, size, "%.*s", dot && dot != name ? (int)(dot - name) : (int)strlen(name), name);
}

static void scan_entry(category *c, const char *folder, const char *name) {
  char path[512], base[512];
  struct stat st;
  if (name[0] == '.' || snprintf(path, sizeof(path), "%s/%s", folder, name) >= (int)sizeof(path) ||
      stat(path, &st) != 0 || !S_ISREG(st.st_mode))
    return;
  app *a;
  if (c->roms != NULL) {
    if (!has_ext(name, c->roms) || (a = add(c)) == NULL)
      return;
    stem(a->name, sizeof(a->name), name);
    snprintf(a->exec, sizeof(a->exec), "%s", c->system_exec);
    snprintf(a->arg, sizeof(a->arg), "%s", path);
    snprintf(a->dir, sizeof(a->dir), "%s/%s", root(), c->system_dir);
    stem(base, sizeof(base), path);
    find_image(a->icon, sizeof(a->icon), base);
    uint32_t crc;
    romdb_game game;
    char pack[512];
    if (c->pack != NULL &&
        snprintf(pack, sizeof(pack), "%s/OS/packs/%s.pack", root(), c->pack) < (int)sizeof(pack) &&
        romdb_crc(path, &st, &crc) && romdb_find(pack, crc, &game)) {
      snprintf(a->name, sizeof(a->name), "%s", game.title);
      if (a->icon[0] == '\0')
        snprintf(a->icon, sizeof(a->icon), "%s", game.image);
    }
    c->count++;
    return;
  }
  if (ends_with(name, ".desktop")) {
    if ((a = add(c)) != NULL && parse_desktop(path, folder, a))
      c->count++;
    return;
  }
  /* A bare executable, unless a .desktop speaks for it */
  char desktop[600];
  snprintf(desktop, sizeof(desktop), "%s.desktop", path);
  if (is_image(name) || access(path, X_OK) != 0 || is_file(desktop) || (a = add(c)) == NULL)
    return;
  snprintf(a->name, sizeof(a->name), "%s", name);
  snprintf(a->exec, sizeof(a->exec), "exec \"$1\"");
  snprintf(a->arg, sizeof(a->arg), "%s", path);
  snprintf(a->dir, sizeof(a->dir), "%s", folder);
  find_image(a->icon, sizeof(a->icon), path);
  c->count++;
}

static int by_name(const void *a, const void *b) {
  return strcasecmp(((const app *)a)->name, ((const app *)b)->name);
}

void apps_scan(category *c) {
  c->count = 0;
  char folder[256];
  snprintf(folder, sizeof(folder), "%s/%s", root(), c->folder);
  DIR *dir = opendir(folder);
  if (dir == NULL)
    return;
  struct dirent *d;
  while ((d = readdir(dir)) != NULL)
    scan_entry(c, folder, d->d_name);
  closedir(dir);
  qsort(c->apps, c->count, sizeof(*c->apps), by_name);
}
