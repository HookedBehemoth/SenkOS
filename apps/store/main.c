/*
 * Store: the server's ports, to install, update and remove. downloadd does
 * the work (its socket, its job lines); this shows the catalog, a port's page
 * (screenshot, description, what it needs, its readme) and how a job goes:
 * queued, downloading with its share done, installed. The launcher hears from
 * downloadd when a port came or went.
 *
 * Pages: All, Installed, then a page per genre (L1/R1, or left/right past
 * the edge). A opens a port, B goes back and quits from the grid. `store ID`
 * opens that port's page, which the launcher does for an installed port.
 *
 * The catalog is /v1/ports of the server /etc/store names ([Store] Server,
 * apps/downloadd); thumbnails, screenshots and readmes are that server's
 * (/media/NAME), cached in $XDG_CACHE_HOME/store by name (their content's).
 */
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "appctl.h"
#include "assets.h"
#include "cfg.h"
#include "gfx.h"
#include "nav.h"
#include "pad.h"
#include "ui.h"
#include "util.h"

#define CONFIG "/etc/store"
#define SOCKET "/run/downloadd.sock"
#ifndef PORTS
#define PORTS "/data/apps/ports"
#endif

enum { BAR = 32, BAR_TEXT = 18, ROW = 40, ROW_TEXT = 20, SMALL = 16, ICON_PX = 20, MARGIN = 12 };
enum { TILE_W = 144, TILE_H = 150, THUMB_W = 128, THUMB_H = 96, TILE_TEXT = 16, RADIUS = 12, GAP = 8 };
enum { SHOT_W = 288, SHOT_H = 216, README_TEXT = 16 };

/* The system's palette (ui.h) */
#define BG ui_pal.bg
#define FG ui_pal.fg
#define DIM ui_pal.dim
#define FAINT ui_pal.faint
#define FOCUS ui_pal.focus
#define TILE ui_pal.tile
#define EDGE ui_pal.edge
#define ACCENT ui_pal.accent
#define OK ui_pal.ok
#define BAD ui_pal.bad
#define RING ui_pal.accent

/* The store's reasons to win_wake */
enum {
  STORE_BUTTON = UI_WAKE_APP, /* APPCTL_* << 1 | pressed */
  STORE_CATALOG,              /* the catalog's fetch ended */
  STORE_FILE,                 /* a cached file arrived */
};

/* downloadd's job states, as it says them */
enum { JOB_NONE, JOB_QUEUED, JOB_DOWNLOADING, JOB_INSTALLING, JOB_DONE, JOB_REMOVED, JOB_FAILED, JOB_CANCELED };
static const str job_words[] = {S(""), S("queued"), S("downloading"), S("installing"), S("done"), S("removed"),
                                S("failed"), S("canceled")};

/* Slices of the catalog's text */
typedef struct {
  str id, name, porter, description, genres, layers, needs, instructions;
  str url, readme, thumbnail, screenshots;
  unsigned long long version, size, installed_size;
  bool installed, outdated;
  int job;
  unsigned long long done, total;
  STRBUF(200) message;
} port;

static port *ports;
static size_t port_count;
static str catalog; /* the text, kept */
static STRBUF(256) catalog_error;
static bool catalog_loading;
static STRBUF(256) server;

/* All, Installed, then the genres */
enum { PAGES_MAX = 64 };
static str genres[PAGES_MAX];
static size_t page_count = 2, page;

static void read_config(void) {
  sb_set(server, S("https://store.invalid"));
  /* A desktop's test against a server of its own */
  if (getenv("STORE_SERVER") != NULL) {
    sb_set(server, str_c(getenv("STORE_SERVER")));
    return;
  }
  const str text = str_read(fopen(CONFIG, "r"));
  cfg_reader r = cfg_init(text);
  for (cfg_event e; (e = cfg_next(&r)) != CFG_END;)
    if (e == CFG_VALUE && str_eq(r.section, S("Store")) && str_eq(r.key, S("Server")))
      sb_set(server, r.value);
  free((char *)text.p);
  while (server.n > 0 && server.p[server.n - 1] == '/')
    server.p[--server.n] = '\0';
}

static size_t to_file(char *p, size_t size, size_t n, void *f) { return fwrite(p, size, n, f); }

/* `url` into `path` (by a .part first); false and why */
static bool download(const char *url, const char *path, char *why, size_t why_size) {
  char part[600];
  snprintf(part, sizeof(part), "%s.part", path);
  FILE *f = fopen(part, "w");
  if (f == NULL) {
    snprintf(why, why_size, "%s: %s", part, strerror(errno));
    return false;
  }
  CURL *c = curl_easy_init();
  curl_easy_setopt(c, CURLOPT_URL, url);
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 256L);
  curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 20L);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, to_file);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, f);
  const CURLcode rc = curl_easy_perform(c);
  curl_easy_cleanup(c);
  const bool ok = fclose(f) == 0 && rc == CURLE_OK && rename(part, path) == 0;
  if (!ok) {
    snprintf(why, why_size, "%s", rc != CURLE_OK ? curl_easy_strerror(rc) : strerror(errno));
    unlink(part);
  }
  return ok;
}

/* ---- files off the CDN, in the cache ---- */

enum { FILES_MAX = 4096 };
enum { FILE_QUEUED = 1, FILE_READY, FILE_FAILED };
static struct {
  STRBUF(400) url;
  STRBUF(400) path;
  _Atomic int state;
} files[FILES_MAX];
static size_t file_count;
static pthread_mutex_t file_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t file_wake = PTHREAD_COND_INITIALIZER;
static STRBUF(300) cache_dir;

static void *file_thread(void *arg) {
  (void)arg;
  for (;;) {
    pthread_mutex_lock(&file_lock);
    size_t i;
    for (;;) {
      /* The newest asked for first: what's on screen now */
      for (i = file_count; i > 0 && atomic_load(&files[i - 1].state) != FILE_QUEUED; i--)
        ;
      if (i > 0)
        break;
      pthread_cond_wait(&file_wake, &file_lock);
    }
    pthread_mutex_unlock(&file_lock);
    char why[256];
    const bool ok = download(files[i - 1].url.p, files[i - 1].path.p, why, sizeof(why));
    if (!ok)
      fprintf(stderr, "store: %s: %s\n", files[i - 1].url.p, why);
    atomic_store(&files[i - 1].state, ok ? FILE_READY : FILE_FAILED);
    win_wake(STORE_FILE, 0);
  }
  return NULL;
}

/* The cached copy's path once it's there, else "" (and it's on its way).
 * "/media/NAME" is the server's. */
static str cached(str url) {
  if (url.n == 0)
    return S("");
  static STRBUF(400) full;
  if (url.p[0] == '/') {
    sb_set(full, sb_str(server), url);
    url = sb_str(full);
  }
  for (size_t i = 0; i < file_count; i++)
    if (str_eq(sb_str(files[i].url), url))
      return atomic_load(&files[i].state) == FILE_READY ? sb_str(files[i].path) : S("");
  /* Named as on the server, where the name is its hash's */
  const str name = str_at(url, str_rfind(url, '/') + 1);
  if (file_count == FILES_MAX || name.n == 0 || name.p[0] == '.')
    return S("");
  pthread_mutex_lock(&file_lock);
  const size_t i = file_count;
  sb_set(files[i].url, url);
  sb_set(files[i].path, sb_str(cache_dir), S("/"), name);
  const bool there = access(files[i].path.p, F_OK) == 0;
  atomic_store(&files[i].state, there ? FILE_READY : FILE_QUEUED);
  file_count++;
  pthread_cond_signal(&file_wake);
  pthread_mutex_unlock(&file_lock);
  return there ? sb_str(files[i].path) : S("");
}

static void make_cache_dir(void) {
  const char *xdg = getenv("XDG_CACHE_HOME"), *home = getenv("HOME");
  if (xdg != NULL && xdg[0] != '\0')
    sb_set(cache_dir, str_c(xdg), S("/store"));
  else
    sb_set(cache_dir, str_c(home != NULL ? home : "/tmp"), S("/.cache/store"));
  char parent[300];
  snprintf(parent, sizeof(parent), "%.*s", (int)(str_rfind(sb_str(cache_dir), '/')), cache_dir.p);
  mkdir(parent, 0755);
  mkdir(cache_dir.p, 0755);
}

/* ---- the catalog ---- */

static STRBUF(256) catalog_why;
static void *catalog_thread(void *arg) {
  (void)arg;
  char path[400], why[256] = "";
  snprintf(path, sizeof(path), "%s/catalog", cache_dir.p);
  STRBUF(400) url;
  sb_set(url, sb_str(server), S("/v1/ports"));
  if (!download(url.p, path, why, sizeof(why)))
    sb_set(catalog_why, str_c(why));
  win_wake(STORE_CATALOG, 0);
  return NULL;
}

/* Installed here, and whether the catalog has a newer one than downloadd put here */
static void check_installed(port *p) {
  char path[300];
  snprintf(path, sizeof(path), PORTS "/%.*s", (int)p->id.n, p->id.p);
  p->installed = access(path, F_OK) == 0;
  p->outdated = false;
  if (!p->installed)
    return;
  strncat(path, "/.store", sizeof(path) - strlen(path) - 1);
  const str text = str_read(fopen(path, "r"));
  cfg_reader r = cfg_init(text);
  for (cfg_event e; (e = cfg_next(&r)) != CFG_END;)
    if (e == CFG_VALUE && str_eq(r.key, S("Version")))
      p->outdated = strtoull(r.value.p, NULL, 10) < p->version;
  free((char *)text.p);
}

static bool has_genre(const port *p, str genre) {
  str list = p->genres, item;
  while (cfg_item(&list, &item))
    if (str_ieq(item, genre))
      return true;
  return false;
}

/* downloadd's connection (below) */
static int daemon_fd;
static uint64_t daemon_tried;

static bool before(str a, str b) {
  const int c = strncasecmp(a.p, b.p, MIN(a.n, b.n));
  return c < 0 || (c == 0 && a.n < b.n);
}

/* The fetched catalog: a port a section */
static void load_catalog(void) {
  catalog_loading = false;
  if (catalog_why.n > 0) {
    sb_set(catalog_error, sb_str(catalog_why));
    return;
  }
  char path[400];
  snprintf(path, sizeof(path), "%s/catalog", cache_dir.p);
  const str text = str_read(fopen(path, "r"));
  if (text.p == NULL)
    return;
  size_t n = 0;
  for (size_t i = 0; i < text.n; i++)
    n += text.p[i] == '[';
  port *list = calloc(n + 1, sizeof(*list));
  if (list == NULL)
    return;
  size_t count = 0;
  cfg_reader r = cfg_init(text);
  for (cfg_event e; (e = cfg_next(&r)) != CFG_END;) {
    if (e == CFG_SECTION && count < n + 1) {
      list[count++] = (port){.id = r.section, .name = r.section};
      continue;
    }
    if (e != CFG_VALUE || count == 0)
      continue;
    port *p = &list[count - 1];
#define KEY(k, field) else if (str_eq(r.key, S(k))) p->field = r.value
    if (0) {
    }
    KEY("Name", name);
    KEY("Porter", porter);
    KEY("Description", description);
    KEY("Genres", genres);
    KEY("Layers", layers);
    KEY("Needs", needs);
    KEY("Instructions", instructions);
    KEY("Url", url);
    KEY("Readme", readme);
    KEY("Thumbnail", thumbnail);
    KEY("Screenshots", screenshots);
#undef KEY
    else if (str_eq(r.key, S("Version"))) p->version = strtoull(r.value.p, NULL, 10);
    else if (str_eq(r.key, S("Size"))) p->size = strtoull(r.value.p, NULL, 10);
    else if (str_eq(r.key, S("InstalledSize"))) p->installed_size = strtoull(r.value.p, NULL, 10);
  }
  /* What downloadd said before the catalog was here stays */
  for (size_t i = 0; i < count; i++)
    for (size_t k = 0; k < port_count; k++)
      if (str_eq(ports[k].id, list[i].id)) {
        list[i].job = ports[k].job, list[i].done = ports[k].done, list[i].total = ports[k].total;
        list[i].message = ports[k].message;
      }
  free(ports);
  free((char *)catalog.p);
  ports = list;
  port_count = count;
  catalog = text;
  catalog_error.n = 0;
  page_count = 2;
  /* downloadd says how every job stands when one connects: again, now that there are ports */
  if (daemon_fd >= 0) {
    close(daemon_fd);
    daemon_fd = -1;
    daemon_tried = 0;
  }
  for (size_t i = 0; i < count; i++) {
    check_installed(&ports[i]);
    str list_ = ports[i].genres, g;
    while (cfg_item(&list_, &g)) {
      bool known = false;
      for (size_t k = 2; k < page_count && !known; k++)
        known = str_ieq(genres[k], g);
      if (!known && page_count < PAGES_MAX)
        genres[page_count++] = g;
    }
  }
  /* By name, after All and Installed */
  for (size_t i = 3; i < page_count; i++)
    for (size_t k = i; k > 2 && before(genres[k], genres[k - 1]); k--) {
      const str t = genres[k];
      genres[k] = genres[k - 1];
      genres[k - 1] = t;
    }
}

static void fetch_catalog(void) {
  if (catalog_loading)
    return;
  catalog_loading = true;
  catalog_why.n = 0;
  spawn("catalog", catalog_thread, NULL);
}

static str page_name(size_t i) { return i == 0 ? S("All") : i == 1 ? S("Installed") : genres[i]; }

static bool on_page(const port *p, size_t i) {
  return i == 0 || (i == 1 ? p->installed || (p->job >= JOB_QUEUED && p->job <= JOB_INSTALLING)
                           : has_genre(p, genres[i]));
}

static port *find(str id) {
  for (size_t i = 0; i < port_count; i++)
    if (str_eq(ports[i].id, id))
      return &ports[i];
  return NULL;
}

/* ---- downloadd ---- */

static int daemon_fd = -1;
static char daemon_in[2048];
static size_t daemon_fill;

static void connect_daemon(void) {
  if (daemon_fd >= 0 || ms_now() - daemon_tried < 3000)
    return;
  daemon_tried = ms_now();
  const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  struct sockaddr_un addr = {.sun_family = AF_UNIX, .sun_path = SOCKET};
  if (fd >= 0 && connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
    daemon_fd = fd;
    daemon_fill = 0;
  } else if (fd >= 0) {
    close(fd);
  }
}

static bool ask(str verb, str id) {
  connect_daemon();
  STRBUF(128) line;
  sb_set(line, verb, id.n ? S(" ") : S(""), id, S("\n"));
  return daemon_fd >= 0 && write(daemon_fd, line.p, line.n) == (ssize_t)line.n;
}

/* "job ID STATE DONE TOTAL [MESSAGE]" */
static void daemon_line(str line) {
  if (!str_eq(str_cut(&line, ' '), S("job")))
    return;
  port *p = find(str_cut(&line, ' '));
  const str word = str_cut(&line, ' ');
  if (p == NULL)
    return;
  for (int s = 1; s < (int)COUNTOF(job_words); s++)
    if (str_eq(word, job_words[s]))
      p->job = s;
  p->done = strtoull(str_cut(&line, ' ').p, NULL, 10);
  p->total = strtoull(line.p, NULL, 10);
  str_cut(&line, ' ');
  sb_set(p->message, line);
  if (p->job >= JOB_DONE)
    check_installed(p);
}

static void read_daemon(void) {
  const ssize_t got = read(daemon_fd, daemon_in + daemon_fill, sizeof(daemon_in) - daemon_fill);
  if (got <= 0) {
    close(daemon_fd);
    daemon_fd = -1;
    return;
  }
  daemon_fill += (size_t)got;
  for (char *end; (end = memchr(daemon_in, '\n', daemon_fill)) != NULL;) {
    daemon_line((str){daemon_in, (size_t)(end - daemon_in)});
    daemon_fill -= (size_t)(end + 1 - daemon_in);
    memmove(daemon_in, end + 1, daemon_fill);
  }
  if (daemon_fill == sizeof(daemon_in))
    daemon_fill = 0;
}

/* ---- the pad ---- */

enum { REPEAT_DELAY = 350, REPEAT_MS = 80 };
static int pad_fd = -1;

static void *pad_thread(void *data) {
  (void)data;
  struct input_event e;
  int held = -1;
  uint64_t next = 0;
  for (;;) {
    struct pollfd p = {.fd = pad_fd, .events = POLLIN};
    const int ready = poll(&p, 1, held < 0 ? -1 : (int)(next - MIN(next, ms_now())));
    if (ready == 0) {
      win_wake(STORE_BUTTON, held << 1 | 1);
      next += REPEAT_MS;
      continue;
    }
    if (ready < 0 && errno == EINTR)
      continue;
    if (ready < 0 || read(pad_fd, &e, sizeof(e)) != sizeof(e))
      break;
    if (e.type != EV_KEY || e.value == 2)
      continue;
    const int button = pad_button(e.code);
    if (button < 0)
      continue;
    if (button >= APPCTL_UP && button <= APPCTL_RIGHT) {
      if (e.value)
        held = button, next = ms_now() + REPEAT_DELAY;
      else if (held == button)
        held = -1;
    }
    win_wake(STORE_BUTTON, button << 1 | e.value);
  }
  return NULL;
}

static void key(unsigned code, bool down) {
  int b = -1;
  switch (code) {
  case KEY_UP: b = APPCTL_UP; break;
  case KEY_DOWN: b = APPCTL_DOWN; break;
  case KEY_LEFT: b = APPCTL_LEFT; break;
  case KEY_RIGHT: b = APPCTL_RIGHT; break;
  case KEY_ENTER: b = APPCTL_A; break;
  case KEY_BACKSPACE: case KEY_ESC: b = APPCTL_B; break;
  case KEY_PAGEUP: b = APPCTL_L1; break;
  case KEY_PAGEDOWN: b = APPCTL_R1; break;
  case KEY_X: b = APPCTL_X; break;
  }
  if (b >= 0)
    win_wake(STORE_BUTTON, b << 1 | down);
}

/* ---- screens ---- */

enum { GRID, DETAIL, README };
static int screen = GRID;
static port *shown;
static bool confirm_remove;
static ui_lines readme;
static str readme_text;
static nav_id grid_focus;
static STRBUF(256) open_id; /* `store ID`: once the catalog has it */

static void redraw(win *w) {
  (void)w;
  win_wake(UI_CONFIGURED, 0);
}
static win window = {.app_id = "store", .title = "Store", .w = 640, .h = 480, .configure = redraw};

static str megabytes(unsigned long long bytes) {
  static STRBUF(32) out;
  out.n = 0;
  if (bytes >= 100 << 20)
    sb_fmt(out, "%llu MB", bytes >> 20);
  else
    sb_fmt(out, "%.1f MB", bytes / 1048576.0);
  return sb_str(out);
}

static void top_bar(int w, str title, str right) {
  const int y = (BAR - ui_text_height(BAR_TEXT)) / 2;
  int tw;
  const str t = ui_text_fit(BAR_TEXT, title, w - MARGIN * 2 - 120, &tw);
  ui_text(BAR_TEXT, (w - tw) / 2, y, t, FG);
  if (right.n > 0)
    ui_text(SMALL, w - MARGIN - ui_text_width(SMALL, right), (BAR - ui_text_height(SMALL)) / 2, right, DIM);
  ui_fill(0, BAR, w, 1, FAINT);
}

static void hint(int w, int h, str text) {
  (void)w;
  ui_text(SMALL, MARGIN * 2, h - MARGIN - ui_text_height(SMALL), text, DIM);
}

static bool job_active(const port *p) { return p->job >= JOB_QUEUED && p->job <= JOB_INSTALLING; }

static void progress_bar(float x, float y, float w, const port *p) {
  ui_rounded(x, y, w, 6, EDGE, 3, 0);
  const float share = p->total > 0 ? (float)p->done / p->total : 0;
  if (p->job == JOB_INSTALLING || p->job == JOB_DOWNLOADING)
    ui_rounded(x, y, MAX(6, w * share), 6, ACCENT, 3, 0);
}

static void thumbnail(str url, float x, float y, float w, float h) {
  const asset a = assets_get(cached(url), (int)MAX(w, h));
  if (a.slot == GFX_PLACEHOLDER) {
    ui_rounded(x, y, w, h, EDGE, 8, 0);
    return;
  }
  const float scale = MIN(w / a.w, h / a.h);
  gfx_push((gfx_quad){.x = x + (w - a.w * scale) / 2, .y = y + (h - a.h * scale) / 2, .w = a.w * scale,
                      .h = a.h * scale, .u1 = 1, .v1 = 1, .color = 0xffffffff, .tex = a.slot, .radius = 8});
}

static port *grid(nav *ui, ui_rect view) {
  static port *visible[8192];
  size_t n = 0;
  for (size_t i = 0; i < port_count && n < COUNTOF(visible); i++)
    if (on_page(&ports[i], page))
      visible[n++] = &ports[i];
  if (n == 0) {
    ui_text(ROW_TEXT, view.x + MARGIN * 2, view.y + MARGIN, page == 1 ? S("No ports installed") : S("Nothing here"),
            DIM);
    return NULL;
  }
  const int cols = MAX(1, (int)((view.w - GAP) / (TILE_W + GAP)));
  const size_t rows = (n + cols - 1) / cols;
  const float left = view.x + (view.w - cols * (TILE_W + GAP) + GAP) / 2;
  const float offset = nav_scroll_begin(ui, nav_str("grid"), view, UI_VERTICAL, rows * (TILE_H + GAP) + GAP, GAP, 0);
  port *chosen = NULL;
  gfx_clip(view.x, view.y, view.w, view.h);
  /* The backgrounds first: the focus's fill goes over them, under the thumbnails */
  for (size_t i = 0; i < n; i++) {
    const ui_rect r = {left + (i % cols) * (TILE_W + GAP), view.y + GAP + (i / cols) * (TILE_H + GAP) - offset,
                       TILE_W, TILE_H};
    if (r.y < view.y + view.h && r.y + r.h > view.y) {
      ui_rounded(r.x, r.y, r.w, r.h, TILE, RADIUS, 0);
      ui_rounded(r.x, r.y, r.w, r.h, EDGE, RADIUS, 1);
    }
  }
  nav_fill(ui, FOCUS, RADIUS);
  for (size_t i = 0; i < n; i++) {
    port *p = visible[i];
    const ui_rect r = {left + (i % cols) * (TILE_W + GAP), view.y + GAP + (i / cols) * (TILE_H + GAP) - offset,
                       TILE_W, TILE_H};
    const unsigned flags = nav_item(ui, nav_int((uint64_t)(p - ports)), r, view);
    nav_ring(ui, r, RADIUS + 2);
    if (flags & NAV_ACTIVATED)
      chosen = p;
    if (r.y >= view.y + view.h || r.y + r.h <= view.y)
      continue;
    thumbnail(p->thumbnail, r.x + (r.w - THUMB_W) / 2, r.y + 8, THUMB_W, THUMB_H);
    int nw;
    const str name = ui_text_fit(TILE_TEXT, p->name, r.w - MARGIN, &nw);
    const float ty = r.y + 8 + THUMB_H + 8;
    ui_text(TILE_TEXT, (int)(r.x + (r.w - nw) / 2), (int)ty, name, FG);
    if (job_active(p))
      progress_bar(r.x + MARGIN, r.y + r.h - 14, r.w - MARGIN * 2, p);
    else if (p->installed)
      ui_icon(ICON_CHECK, 18, (int)(r.x + r.w - 26), (int)(r.y + r.h - 24), p->outdated ? ACCENT : OK);
  }
  gfx_noclip();
  nav_scroll_end(ui);
  return chosen;
}

/* A port's page. The rows: Install / Update / Cancel, Readme, Remove. */
enum { ROW_INSTALL, ROW_README, ROW_REMOVE };
static void detail(nav *ui, int w, int h) {
  port *p = shown;
  const float x0 = MARGIN, y0 = BAR + MARGIN;
  /* The first screenshot, else the thumbnail */
  str shots = p->screenshots, first = S("");
  if (!cfg_item(&shots, &first))
    first = p->thumbnail;
  thumbnail(first, x0, y0, SHOT_W, SHOT_H);

  /* The rows under it */
  struct {
    str label;
    int action, icon;
  } rows[3];
  size_t n = 0;
  if (job_active(p))
    rows[n++] = (typeof(rows[0])){S("Cancel"), ROW_INSTALL, ICON_X};
  else if (!p->installed)
    rows[n++] = (typeof(rows[0])){S("Install"), ROW_INSTALL, ICON_DOWNLOAD};
  else if (p->outdated)
    rows[n++] = (typeof(rows[0])){S("Update"), ROW_INSTALL, ICON_REFRESH_CW};
  if (p->readme.n > 0)
    rows[n++] = (typeof(rows[0])){S("Readme"), ROW_README, ICON_BOOK_OPEN};
  if (p->installed && !job_active(p))
    rows[n++] = (typeof(rows[0])){confirm_remove ? S("A again: remove") : S("Remove"), ROW_REMOVE, ICON_TRASH_2};
  float y = y0 + SHOT_H + MARGIN;
  nav_fill(ui, FOCUS, 6);
  for (size_t i = 0; i < n; i++, y += ROW) {
    const ui_rect r = {x0, y, SHOT_W, ROW - 4};
    const unsigned flags = nav_item(ui, nav_int((uint64_t)rows[i].action), r, (ui_rect){0, 0, w, h});
    nav_ring(ui, r, 8);
    if (!(flags & NAV_FOCUSED) && rows[i].action == ROW_REMOVE)
      confirm_remove = false;
    const gfx_color color = rows[i].action == ROW_REMOVE && confirm_remove ? BAD : FG;
    ui_icon(rows[i].icon, ICON_PX, (int)r.x + MARGIN, (int)(r.y + (r.h - ICON_PX) / 2), color);
    ui_text(ROW_TEXT, (int)r.x + MARGIN * 2 + ICON_PX, (int)(r.y + (r.h - ui_text_height(ROW_TEXT)) / 2), rows[i].label,
            color);
    if (!(flags & NAV_ACTIVATED))
      continue;
    if (rows[i].action == ROW_INSTALL && job_active(p))
      ask(S("cancel"), p->id);
    else if (rows[i].action == ROW_INSTALL && !ask(S("install"), p->id))
      sb_set(p->message, S("downloadd isn't running"));
    else if (rows[i].action == ROW_INSTALL)
      p->message.n = 0;
    else if (rows[i].action == ROW_REMOVE && !confirm_remove)
      confirm_remove = true;
    else if (rows[i].action == ROW_REMOVE) {
      confirm_remove = false;
      ask(S("remove"), p->id);
    } else if (rows[i].action == ROW_README) {
      readme.count = 0;
      free((char *)readme_text.p);
      readme_text = (str){0};
      screen = README;
      nav_forget(ui, nav_str("readme"));
    }
  }

  /* Beside it: who, what, how big, how it's going; then the description */
  const float x = x0 + SHOT_W + MARGIN * 2, tw = w - x - MARGIN;
  float ty = y0;
  ui_text(ROW_TEXT + 2, (int)x, (int)ty, ui_text_fit(ROW_TEXT + 2, p->name, tw, NULL), FG);
  ty += ui_text_height(ROW_TEXT + 2) + 4;
  STRBUF(256) line;
  str porters = p->porter, who;
  sb_set(line, S("by "));
  for (bool first_ = true; cfg_item(&porters, &who); first_ = false)
    sb_add(line, first_ ? S("") : S(", "), who);
  if (line.n > 3) {
    ui_text(SMALL, (int)x, (int)ty, ui_text_fit(SMALL, sb_str(line), tw, NULL), DIM);
    ty += ui_text_height(SMALL) + 2;
  }
  sb_set(line, megabytes(p->size), S(" download, "));
  sb_add(line, megabytes(p->installed_size), S(" installed"));
  ui_text(SMALL, (int)x, (int)ty, ui_text_fit(SMALL, sb_str(line), tw, NULL), DIM);
  ty += ui_text_height(SMALL) + 2;
  if (p->layers.n > 0) {
    str layers = p->layers, l;
    sb_set(line, S("Uses "));
    for (bool first_ = true; cfg_item(&layers, &l); first_ = false)
      sb_add(line, first_ ? S("") : S(", "), l);
    ui_text(SMALL, (int)x, (int)ty, ui_text_fit(SMALL, sb_str(line), tw, NULL), DIM);
    ty += ui_text_height(SMALL) + 2;
  }
  ty += 6;
  /* The job, or what came of it */
  str state = S("");
  gfx_color color = DIM;
  STRBUF(256) status;
  switch (p->job) {
  case JOB_QUEUED: state = S("Waiting…"); break;
  case JOB_DOWNLOADING:
    status.n = 0;
    sb_fmt(status, "Downloading %d%%", p->total ? (int)(p->done * 100 / p->total) : 0);
    state = sb_str(status);
    break;
  case JOB_INSTALLING: state = S("Installing…"); break;
  case JOB_DONE: state = S("Installed"), color = OK; break;
  case JOB_REMOVED: state = S("Removed"); break;
  case JOB_CANCELED: state = S("Canceled"); break;
  case JOB_FAILED:
    sb_set(status, S("Failed: "), sb_str(p->message));
    state = sb_str(status), color = BAD;
    break;
  default:
    if (p->message.n > 0)
      state = sb_str(p->message), color = BAD;
    else if (p->installed)
      state = p->outdated ? S("Installed, an update is here") : S("Installed"), color = p->outdated ? ACCENT : OK;
  }
  if (state.n > 0) {
    ui_lines lines = {0};
    ui_wrap(&lines, state, SMALL, tw);
    for (size_t i = 0; i < MIN(lines.count, 2); i++, ty += ui_text_height(SMALL) + 2)
      ui_text(SMALL, (int)x, (int)ty, lines.lines[i], color);
    free(lines.lines);
  }
  if (job_active(p)) {
    progress_bar(x, ty + 4, tw, p);
    ty += 16;
  }
  ty += 8;
  /* The description, and what the player has to bring */
  STRBUF(1200) text;
  sb_set(text, p->description);
  if (p->needs.n > 0)
    sb_add(text, S("\n\nNeeds the game's own files. "), p->instructions);
  ui_lines lines = {0};
  ui_wrap_text(&lines, sb_str(text), SMALL, tw);
  const float line_h = ui_text_height(SMALL) + 2;
  for (size_t i = 0; i < lines.count && ty + line_h < h - ROW; i++, ty += line_h)
    ui_text(SMALL, (int)x, (int)ty, lines.lines[i], FG);
  free(lines.lines);
}

static void readme_screen(nav *ui, ui_rect view) {
  if (readme_text.p == NULL) {
    const str path = cached(shown->readme);
    if (path.n == 0) {
      ui_text(ROW_TEXT, view.x + MARGIN * 2, view.y + MARGIN, S("Loading…"), DIM);
      return;
    }
    const str md = str_read(fopen(path.p, "r"));
    readme_text = ui_markdown(md);
    free((char *)md.p);
    readme.count = 0;
    ui_wrap_text(&readme, readme_text, README_TEXT, view.w - MARGIN * 4);
  }
  const int line = ui_text_height(README_TEXT) + 2;
  const float offset =
      nav_scroll_begin(ui, nav_str("readme"), view, UI_VERTICAL, readme.count * line + MARGIN * 2, 0, line);
  gfx_clip(view.x, view.y, view.w, view.h);
  for (size_t i = (size_t)MAX(0, (offset - MARGIN) / line); i < readme.count; i++) {
    const float y = view.y + MARGIN + i * line - offset;
    if (y >= view.y + view.h)
      break;
    ui_text(README_TEXT, (int)view.x + MARGIN * 2, (int)y, readme.lines[i], FG);
  }
  gfx_noclip();
  nav_scroll_end(ui);
}

static void open_port(nav *ui, port *p) {
  grid_focus = ui->focus;
  shown = p;
  screen = DETAIL;
  confirm_remove = false;
  nav_focus(ui, 0);
}

static void turn_page(nav *ui, int by) {
  page = (page + page_count + by) % page_count;
  nav_focus(ui, 0);
}

/* False: quit */
static bool input(nav *ui, int button, int h) {
  if (button == APPCTL_B) {
    if (screen == GRID)
      return false;
    screen = screen == README ? DETAIL : GRID;
    nav_focus(ui, screen == GRID ? grid_focus : nav_child(nav_str("detail"), nav_int(ROW_README)));
    return true;
  }
  if (screen == README) {
    const int page_lines = (h - BAR - ROW * 2) / (ui_text_height(README_TEXT) + 2);
    switch (button) {
    case APPCTL_UP: nav_step(ui, -1); break;
    case APPCTL_DOWN: nav_step(ui, 1); break;
    case APPCTL_LEFT: case APPCTL_L1: nav_step(ui, -page_lines); break;
    case APPCTL_RIGHT: case APPCTL_R1: nav_step(ui, page_lines); break;
    }
    return true;
  }
  const bool on_grid = screen == GRID;
  switch (button) {
  case APPCTL_UP: nav_move(ui, NAV_UP); break;
  case APPCTL_DOWN: nav_move(ui, NAV_DOWN); break;
  case APPCTL_LEFT:
  case APPCTL_RIGHT:
    if (!nav_move(ui, button == APPCTL_LEFT ? NAV_LEFT : NAV_RIGHT) && on_grid)
      turn_page(ui, button == APPCTL_LEFT ? -1 : 1);
    break;
  case APPCTL_L1:
  case APPCTL_R1:
    if (on_grid)
      turn_page(ui, button == APPCTL_L1 ? -1 : 1);
    break;
  case APPCTL_A:
    if (catalog_error.n > 0 && port_count == 0)
      fetch_catalog();
    else
      nav_activate(ui);
    break;
  }
  return true;
}

int main(int argc, char **argv) {
  if (argc > 2) {
    fprintf(stderr, "usage: store [PORT]\n");
    return 2;
  }
  if (argc == 2)
    sb_set(open_id, str_c(argv[1]));
  curl_global_init(CURL_GLOBAL_DEFAULT);
  read_config();
  make_cache_dir();
  if (!win_connect(key)) {
    fprintf(stderr, "store: no Wayland display\n");
    return 1;
  }
  win_map(&window);
  gfx_target *target;
  if (!gfx_init(window.title) || (target = gfx_target_create(&window, false)) == NULL || !ui_init("sans-serif")) {
    fprintf(stderr, "store: can't draw\n");
    return 1;
  }
  assets_init();
  spawn("files", file_thread, NULL);
  if ((pad_fd = pad_open(0)) >= 0)
    spawn("pad", pad_thread, NULL);
  connect_daemon();
  fetch_catalog();
  static nav ui = {.animate = true, .duration = 150};
  static ui_context ctx;
  bool animating = true;
  for (;;) {
    struct pollfd fd = {.fd = daemon_fd, .events = POLLIN};
    if (!win_wait(&fd, daemon_fd >= 0 ? 1 : 0, animating ? 16 : daemon_fd >= 0 ? 1000 : 3000))
      return 1;
    if (daemon_fd >= 0 && (fd.revents & (POLLIN | POLLHUP)))
      read_daemon(), animating = true;
    connect_daemon();
    for (int why, value; win_woken(&why, &value); animating = true) {
      if (why == WIN_QUIT)
        return 0;
      if (why == UI_CONFIGURED)
        ui_scheme();
      if (why == STORE_CATALOG) {
        load_catalog();
        port *p = open_id.n > 0 ? find(sb_str(open_id)) : NULL;
        if (p != NULL)
          open_port(&ui, p);
        open_id.n = 0;
      }
      if (why == STORE_BUTTON && (value & 1) && !input(&ui, value >> 1, window.h))
        return 0;
    }
    if (!animating)
      continue;
    assets_frame();
    int w, h;
    if (!gfx_begin(target, &w, &h))
      continue;
    gfx_clear(BG);
    nav_begin(&ui, &ctx, ms_now());
    const ui_rect view = {0, BAR + 1, w, h - BAR - 1 - ROW};
    if (screen == GRID) {
      STRBUF(32) count;
      count.n = 0;
      if (port_count > 0)
        sb_fmt(count, "%zu ports", port_count);
      top_bar(w, port_count > 0 ? page_name(page) : S("Store"), sb_str(count));
      if (port_count == 0) {
        str why = catalog_loading ? S("Loading the catalog…") : sb_str(catalog_error);
        ui_text(ROW_TEXT, MARGIN * 2, BAR + MARGIN * 2, ui_text_fit(ROW_TEXT, why, w - MARGIN * 4, NULL),
                catalog_loading ? DIM : BAD);
        if (!catalog_loading)
          hint(w, h, S("A: try again"));
      } else {
        port *chosen = NULL;
        NAV_SCOPE(&ui, nav_int(page)) { chosen = grid(&ui, view); }
        if (chosen != NULL)
          open_port(&ui, chosen);
        hint(w, h, S("L1/R1: page"));
      }
    } else if (screen == DETAIL) {
      top_bar(w, shown->name, S(""));
      NAV_SCOPE(&ui, nav_str("detail")) { detail(&ui, w, h); }
    } else {
      top_bar(w, shown->name, S("Readme"));
      readme_screen(&ui, view);
      hint(w, h, S("Left, right: page"));
    }
    animating = nav_end(&ui, RING, 2);
    gfx_end(target);
  }
}
