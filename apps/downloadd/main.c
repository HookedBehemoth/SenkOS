/*
 * downloadd: ports from the store and system updates, one at a time, for the
 * store and the launcher's Updates (root, started by senkos.init).
 *
 * Clients talk lines over /run/downloadd.sock (root:player 0660):
 *
 *     install ID | remove ID | cancel ID      a port (install also updates it)
 *     check                                   is there a newer system?
 *     update                                  download it, write it to the other slot
 *
 * and every client hears about every job, all of them when it connects, and every check's answer:
 *
 *     job ID STATE DONE TOTAL [MESSAGE]       ID "os" for the system; STATE queued, downloading,
 *                                             installing, done, removed, failed, canceled;
 *                                             DONE and TOTAL in bytes
 *     os available BUILD SIZE NOTES | os current | os failed MESSAGE
 *
 * A port is done when it's in /data/apps/ports/ID with its layers in /data/OS/layers; the
 * launcher then hears "reload" on its FIFO. The system update is done when the other slot holds
 * it, armed for the next boot (ab write, ab arm): rebooting finishes it, and the launcher commits
 * it once it's up.
 *
 * The work is done by this same program run again, one process a job, which reports on stdout:
 * a port's as player (what it unpacks can't reach past the player's files), the system's as
 * root. The server and what's asked of it come from /etc/store ([Store] Server, Channel: root's,
 * /data/OS/overlay to change it), never from a client: a port is whatever the server says, and
 * the system image is checked against the hash it gives.
 *
 *     downloadd port ID | remove ID | check | os      one job, by hand too
 *     downloadd zone                 the time zone this network's address is in, guessed by
 *                                    ipapi.co (for `clock`, which sets it unless one was chosen)
 *     downloadd send REQUEST...      a client: the request, then what's said until it's over
 */
#include <curl/curl.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <openssl/evp.h>
#include <poll.h>
#include <stdarg.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <zstd.h>

#include "cfg.h"
#include "str.h"
#include "util.h"

#define CONFIG "/etc/store"
#define BUILD "/etc/build"
#define SOCKET "/run/downloadd.sock"
#define PORTS "/data/apps/ports"
#define LAYERS "/data/OS/layers"
#define LAUNCHER_FIFO "/run/launcher"
#define PLAYER "player"

static STRBUF(256) server;
static STRBUF(32) channel;

static void load_config(void) {
  sb_set(server, S("https://store.invalid"));
  sb_set(channel, S("stable"));
  const str text = str_read(fopen(CONFIG, "r"));
  cfg_reader r = cfg_init(text);
  for (cfg_event e; (e = cfg_next(&r)) != CFG_END;) {
    if (e != CFG_VALUE || !str_eq(r.section, S("Store")))
      continue;
    if (str_eq(r.key, S("Server")))
      sb_set(server, r.value);
    else if (str_eq(r.key, S("Channel")))
      sb_set(channel, r.value);
  }
  free((char *)text.p);
  while (server.n > 0 && server.p[server.n - 1] == '/')
    server.p[--server.n] = '\0';
}

/* Ids and file names become paths: as the server allows them */
static bool safe_name(str s) {
  if (s.n == 0 || s.n > 64 || s.p[0] == '.')
    return false;
  for (size_t i = 0; i < s.n; i++)
    if (!((s.p[i] >= 'a' && s.p[i] <= 'z') || (s.p[i] >= 'A' && s.p[i] <= 'Z') ||
          (s.p[i] >= '0' && s.p[i] <= '9') || strchr("._-+", s.p[i]) != NULL))
      return false;
  return true;
}

/* ---- one job, in its own process ---- */

static void fail(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
static void fail(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  printf("error ");
  vprintf(fmt, ap);
  printf("\n");
  va_end(ap);
  exit(1);
}

typedef struct {
  char *p;
  size_t n;
} buffer;

static size_t to_buffer(char *p, size_t size, size_t n, void *arg) {
  buffer *b = arg;
  if (b->n + size * n > (1 << 20))
    return 0;
  char *grown = realloc(b->p, b->n + size * n + 1);
  if (grown == NULL)
    return 0;
  memcpy(grown + b->n, p, size * n);
  b->p = grown;
  b->n += size * n;
  b->p[b->n] = '\0';
  return size * n;
}

static CURL *new_curl(const char *url) {
  CURL *c = curl_easy_init();
  if (c == NULL)
    fail("no curl");
  curl_easy_setopt(c, CURLOPT_URL, url);
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
  /* Stalled: under 1 KB/s for 30 s */
  curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1024L);
  curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);
  curl_easy_setopt(c, CURLOPT_USERAGENT, "downloadd");
  return c;
}

/* A server's answer, small: a key file. NULL when it had none (204). */
static str get(const char *url) {
  buffer b = {0};
  CURL *c = new_curl(url);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, to_buffer);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
  const CURLcode rc = curl_easy_perform(c);
  curl_easy_cleanup(c);
  if (rc != CURLE_OK)
    fail("%s: %s", url, curl_easy_strerror(rc));
  return (str){b.p, b.n};
}

/* Downloaded bytes go through the hash, then zstd (or not) into `out` */
typedef struct {
  EVP_MD_CTX *sha;
  ZSTD_DStream *z;
  FILE *out;
  uint64_t done, total;
  uint64_t said_at;
  bool broken;
} sink;

static void progress(sink *s, bool now) {
  const uint64_t t = ms_now();
  if (now || t - s->said_at >= 250) {
    printf("progress %llu %llu\n", (unsigned long long)s->done, (unsigned long long)s->total);
    fflush(stdout);
    s->said_at = t;
  }
}

static size_t to_sink(char *p, size_t size, size_t n, void *arg) {
  sink *s = arg;
  const size_t len = size * n;
  EVP_DigestUpdate(s->sha, p, len);
  if (s->z == NULL) {
    if (fwrite(p, 1, len, s->out) != len)
      return s->broken = true, 0;
  } else {
    static char chunk[1 << 17];
    ZSTD_inBuffer in = {p, len, 0};
    while (in.pos < in.size) {
      ZSTD_outBuffer o = {chunk, sizeof(chunk), 0};
      if (ZSTD_isError(ZSTD_decompressStream(s->z, &o, &in)) || fwrite(chunk, 1, o.pos, s->out) != o.pos)
        return s->broken = true, 0;
    }
  }
  s->done += len;
  progress(s, false);
  return len;
}

/* `url` into `out` (zstd: decompressed), checked against `sha256` (hex) */
static void fetch(const char *url, str sha256, bool zstd, FILE *out, uint64_t *done, uint64_t total) {
  sink s = {.sha = EVP_MD_CTX_new(), .z = zstd ? ZSTD_createDStream() : NULL, .out = out,
            .done = *done, .total = total};
  EVP_DigestInit_ex(s.sha, EVP_sha256(), NULL);
  CURL *c = new_curl(url);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, to_sink);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &s);
  const CURLcode rc = curl_easy_perform(c);
  curl_easy_cleanup(c);
  if (s.broken)
    fail("%s: can't write it: %s", url, strerror(errno));
  if (rc != CURLE_OK)
    fail("%s: %s", url, curl_easy_strerror(rc));
  unsigned char digest[32];
  char hex[65];
  EVP_DigestFinal_ex(s.sha, digest, NULL);
  for (int i = 0; i < 32; i++)
    snprintf(hex + i * 2, 3, "%02x", digest[i]);
  if (!str_ieq(str_c(hex), sha256))
    fail("%s: damaged in transit (SHA-256 %s)", url, hex);
  EVP_MD_CTX_free(s.sha);
  if (s.z != NULL)
    ZSTD_freeDStream(s.z);
  *done = s.done;
  progress(&s, true);
}

static int run(char *const argv[]) {
  const pid_t pid = fork();
  if (pid == 0) {
    execvp(argv[0], argv);
    _exit(127);
  }
  int status = 1;
  if (pid < 0 || waitpid(pid, &status, 0) < 0)
    return -1;
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void remove_tree(const char *path) {
  if (access(path, F_OK) == 0)
    run((char *[]){"rm", "-rf", (char *)path, NULL});
}

enum { FILES_MAX = 16 };
typedef struct {
  STRBUF(96) name;
  STRBUF(512) url;
  STRBUF(65) sha256;
  uint64_t size;
  STRBUF(96) layer;
} file;

/* /v1/ports/ID: [Port] and its layers' [File NAME] sections */
static void install_port(str id) {
  STRBUF(512) url;
  sb_set(url, sb_str(server), S("/v1/ports/"), id);
  const str text = get(url.p);
  file port = {0}, files[FILES_MAX];
  size_t count = 0;
  STRBUF(64) version = {0};
  STRBUF(128) arches = {0};
  uint64_t installed = 0;
  cfg_reader r = cfg_init(text);
  for (cfg_event e; (e = cfg_next(&r)) != CFG_END;) {
    if (e == CFG_SECTION && r.section.n > 5 && memcmp(r.section.p, "File ", 5) == 0 && count < FILES_MAX) {
      files[count] = (file){0};
      sb_set(files[count].name, str_at(r.section, 5));
      count++;
      continue;
    }
    if (e != CFG_VALUE)
      continue;
    const bool in_port = str_eq(r.section, S("Port"));
    file *f = in_port ? &port : count > 0 ? &files[count - 1] : NULL;
    if (f == NULL)
      continue;
    if (str_eq(r.key, S("Url")))
      sb_set(f->url, r.value);
    else if (str_eq(r.key, S("Sha256")))
      sb_set(f->sha256, r.value);
    else if (str_eq(r.key, S("Size")))
      f->size = strtoull(r.value.p, NULL, 10);
    else if (str_eq(r.key, S("Layer")))
      sb_set(f->layer, r.value);
    else if (in_port && str_eq(r.key, S("Version")))
      sb_set(version, r.value);
    else if (in_port && str_eq(r.key, S("Arch")))
      sb_set(arches, r.value);
    else if (in_port && str_eq(r.key, S("InstalledSize")))
      installed = strtoull(r.value.p, NULL, 10);
  }
  if (port.url.n == 0 || port.sha256.n != 64)
    fail("the server has no such port");
  /* What portrun will run: the first of the port's that the handheld can */
  str arch = S(""), list = sb_str(arches), item;
  while (arch.n == 0 && cfg_item(&list, &item))
    if (str_eq(item, S("aarch64")) || str_eq(item, S("armhf")))
      arch = item;
  if (arch.n == 0)
    fail("not made for this handheld (%.*s)", (int)arches.n, arches.p);

  /* The layer files it needs that aren't here yet: the .ini, the squashfs for that arch */
  uint64_t total = port.size, need = port.size + installed;
  bool wanted[FILES_MAX] = {0};
  for (size_t i = 0; i < count; i++) {
    file *f = &files[i];
    STRBUF(128) ini, image;
    sb_set(ini, sb_str(f->layer), S(".ini"));
    sb_set(image, sb_str(f->layer), S("."), arch, S(".squashfs"));
    if (!safe_name(sb_str(f->name)) || f->sha256.n != 64 ||
        !(str_eq(sb_str(f->name), sb_str(ini)) || str_eq(sb_str(f->name), sb_str(image))))
      continue;
    STRBUF(256) path;
    sb_set(path, S(LAYERS "/"), sb_str(f->name));
    struct stat st;
    /* Layers are named by their version: one of the same size is that one */
    if (stat(path.p, &st) == 0 && (uint64_t)st.st_size == f->size)
      continue;
    wanted[i] = true;
    total += f->size;
    need += f->size;
  }
  struct statvfs fs;
  if (statvfs(PORTS, &fs) == 0 && (uint64_t)fs.f_bavail * fs.f_frsize < need + (16 << 20))
    fail("not enough space: %llu MB needed", (unsigned long long)(need >> 20) + 16);

  uint64_t done = 0;
  for (size_t i = 0; i < count; i++) {
    if (!wanted[i])
      continue;
    STRBUF(256) part, path;
    sb_set(part, S(LAYERS "/."), sb_str(files[i].name), S(".part"));
    sb_set(path, S(LAYERS "/"), sb_str(files[i].name));
    FILE *out = fopen(part.p, "w");
    if (out == NULL)
      fail("%s: %s", part.p, strerror(errno));
    fetch(files[i].url.p, sb_str(files[i].sha256), false, out, &done, total);
    if (fclose(out) != 0 || rename(part.p, path.p) != 0)
      fail("%s: %s", path.p, strerror(errno));
  }

  /* Unpacked as it comes, into a folder of its own, then put in place */
  STRBUF(256) staging, dest, cmd;
  sb_set(staging, S(PORTS "/.store-"), id);
  sb_set(dest, S(PORTS "/"), id);
  remove_tree(staging.p);
  if (mkdir(staging.p, 0755) != 0)
    fail("%s: %s", staging.p, strerror(errno));
  sb_set(cmd, S("tar x -C "), sb_str(staging));
  FILE *tar = popen(cmd.p, "w");
  if (tar == NULL)
    fail("tar: %s", strerror(errno));
  fetch(port.url.p, sb_str(port.sha256), true, tar, &done, total);
  if (pclose(tar) != 0)
    fail("the port's archive is broken");
  printf("installing\n");
  fflush(stdout);
  STRBUF(256) marker;
  sb_set(marker, sb_str(staging), S("/.store"));
  FILE *m = fopen(marker.p, "w");
  if (m != NULL) {
    str_fput(m, S("[Store]\nVersion="), sb_str(version), S("\n"));
    fclose(m);
  }
  if (rename(staging.p, dest.p) != 0) {
    /* An update: over what's there, so the files the player brought stay */
    sb_add(staging, S("/."));
    if (run((char *[]){"cp", "-a", staging.p, dest.p, NULL}) != 0)
      fail("can't put it in %s", dest.p);
    staging.p[staging.n -= 2] = '\0';
    remove_tree(staging.p);
  }
  free((char *)text.p);
}

static str build(void) {
  static STRBUF(32) b;
  char line[32] = "";
  FILE *f = fopen(BUILD, "r");
  if (f != NULL && fgets(line, sizeof(line), f) != NULL)
    line[strcspn(line, "\n")] = '\0';
  if (f != NULL)
    fclose(f);
  sb_set(b, str_c(line));
  return sb_str(b);
}

typedef struct {
  file f;
  STRBUF(32) build;
  STRBUF(512) notes;
} release;

/* The newest release past this build: [Release] Build, Notes, Url, Sha256, Size */
static bool newer(release *rel) {
  STRBUF(512) url;
  sb_set(url, sb_str(server), S("/v1/os?channel="), sb_str(channel), S("&build="), build());
  const str text = get(url.p);
  *rel = (release){0};
  file *f = &rel->f;
  cfg_reader r = cfg_init(text);
  for (cfg_event e; (e = cfg_next(&r)) != CFG_END;) {
    if (e != CFG_VALUE || !str_eq(r.section, S("Release")))
      continue;
    if (str_eq(r.key, S("Build")))
      sb_set(rel->build, r.value);
    else if (str_eq(r.key, S("Notes")))
      sb_set(rel->notes, r.value);
    else if (str_eq(r.key, S("Url")))
      sb_set(f->url, r.value);
    else if (str_eq(r.key, S("Sha256")))
      sb_set(f->sha256, r.value);
    else if (str_eq(r.key, S("Size")))
      f->size = strtoull(r.value.p, NULL, 10);
  }
  free((char *)text.p);
  return f->url.n > 0 && f->sha256.n == 64;
}

static void check(void) {
  release r;
  if (newer(&r))
    printf("available %s %llu %s\n", r.build.p, (unsigned long long)r.f.size, r.notes.p);
  else
    printf("current\n");
}

/* Into the other slot, armed once the hash matched */
static void update_os(void) {
  release r;
  if (!newer(&r))
    fail("the system is up to date");
  FILE *slot = popen("ab write", "w");
  if (slot == NULL)
    fail("ab: %s", strerror(errno));
  uint64_t done = 0;
  fetch(r.f.url.p, sb_str(r.f.sha256), true, slot, &done, r.f.size);
  if (pclose(slot) != 0)
    fail("couldn't write the other slot");
  printf("installing\n");
  fflush(stdout);
  if (run((char *[]){"ab", "arm", NULL}) != 0)
    fail("the image isn't a system");
}

/* "Europe/Berlin": a name under /usr/share/zoneinfo, `clock` checks it's there */
static void zone(void) {
  const str z = str_trim(get("https://ipapi.co/timezone"));
  if (z.n == 0 || z.n > 64 || z.p[0] == '/' || strstr(z.p, "..") != NULL ||
      strspn(z.p, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789/_+-") < z.n)
    fail("no zone");
  str_fput(stdout, z, S("\n"));
}

static int job(int argc, char **argv) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  load_config();
  curl_global_init(CURL_GLOBAL_DEFAULT);
  const str id = argc > 2 ? str_c(argv[2]) : S("");
  if (argc == 3 && strcmp(argv[1], "port") == 0 && safe_name(id))
    install_port(id);
  else if (argc == 3 && strcmp(argv[1], "remove") == 0 && safe_name(id)) {
    STRBUF(256) dest;
    sb_set(dest, S(PORTS "/"), id);
    remove_tree(dest.p);
  } else if (argc == 2 && strcmp(argv[1], "check") == 0)
    check();
  else if (argc == 2 && strcmp(argv[1], "os") == 0)
    update_os();
  else if (argc == 2 && strcmp(argv[1], "zone") == 0)
    zone();
  else {
    fprintf(stderr, "usage: downloadd [port ID | remove ID | check | os | zone]\n");
    return 2;
  }
  return 0;
}

/* ---- the daemon ---- */

enum { INSTALL, REMOVE, OS, CHECK };
enum { QUEUED, DOWNLOADING, INSTALLING, DONE, FAILED, CANCELED };
static const char *const state_words[] = {"queued", "downloading", "installing", "done", "failed", "canceled"};

enum { TASKS_MAX = 64, CLIENTS_MAX = 16 };
typedef struct {
  STRBUF(68) id;
  int kind, state;
  unsigned long long done, total;
  STRBUF(200) message;
  pid_t pid; /* running: its process, its stdout */
  int out;
  char line[600];
  size_t n;
} task;
static task tasks[TASKS_MAX];
static task checking = {.out = -1};
static int clients[CLIENTS_MAX];
static size_t client_count;
static STRBUF(600) os_status;
static char self[PATH_MAX];
static uid_t player_uid;
static gid_t player_gid;

static void tell(int fd, str line) {
  /* A client that doesn't read is dropped (its next read sees the end) rather than waited for */
  if (write(fd, line.p, line.n) != (ssize_t)line.n)
    shutdown(fd, SHUT_RDWR);
}

static void tell_all(str line) {
  for (size_t i = client_count; i > 0; i--)
    tell(clients[i - 1], line);
}

static str task_line(const task *t) {
  static STRBUF(400) line;
  const char *word = t->state == DONE && t->kind == REMOVE ? "removed" : state_words[t->state];
  sb_set(line, S("job "), sb_str(t->id), S(" "), str_c(word));
  sb_fmt(line, " %llu %llu", t->done, t->total);
  if (t->message.n > 0)
    sb_add(line, S(" "), sb_str(t->message));
  sb_add(line, S("\n"));
  return sb_str(line);
}

static void changed(const task *t) { tell_all(task_line(t)); }

/* The job's process: a port's as the player, the system's as root */
static bool start(task *t) {
  int p[2];
  if (pipe2(p, O_CLOEXEC) != 0)
    return false;
  char *argv[4] = {self, t->kind == INSTALL ? "port" : t->kind == REMOVE ? "remove" : t->kind == OS ? "os" : "check",
                   t->kind == INSTALL || t->kind == REMOVE ? t->id.p : NULL, NULL};
  const pid_t pid = fork();
  if (pid == 0) {
    dup2(p[1], STDOUT_FILENO);
    const bool as_player = t->kind != OS;
    if (as_player && (setgroups(0, NULL) != 0 || setgid(player_gid) != 0 || setuid(player_uid) != 0))
      _exit(1);
    signal(SIGPIPE, SIG_DFL);
    execv(self, argv);
    _exit(127);
  }
  close(p[1]);
  if (pid < 0) {
    close(p[0]);
    return false;
  }
  t->pid = pid;
  t->out = p[0];
  t->n = 0;
  return true;
}

static task *busy(void) {
  for (size_t i = 0; i < TASKS_MAX; i++)
    if (tasks[i].pid > 0)
      return &tasks[i];
  return NULL;
}

/* The next queued job, if nothing runs */
static void next(void) {
  if (busy() != NULL)
    return;
  for (size_t i = 0; i < TASKS_MAX; i++) {
    task *t = &tasks[i];
    if (t->state != QUEUED || t->id.n == 0)
      continue;
    t->state = t->kind == REMOVE ? INSTALLING : DOWNLOADING;
    if (!start(t)) {
      t->state = FAILED;
      sb_set(t->message, S("can't start"));
    }
    changed(t);
    if (t->pid > 0)
      return;
  }
}

static task *find(str id) {
  for (size_t i = 0; i < TASKS_MAX; i++)
    if (tasks[i].id.n > 0 && str_eq(sb_str(tasks[i].id), id))
      return &tasks[i];
  return NULL;
}

/* A new job, or the finished one of the same id again */
static void queue(str id, int kind) {
  task *t = find(id);
  if (t != NULL && (t->state == QUEUED || t->pid > 0)) {
    changed(t);
    return;
  }
  for (size_t i = 0; t == NULL && i < TASKS_MAX; i++)
    if (tasks[i].id.n == 0 || (tasks[i].state >= DONE && tasks[i].pid == 0))
      t = &tasks[i];
  if (t == NULL)
    return;
  *t = (task){.kind = kind, .state = QUEUED, .out = -1};
  sb_set(t->id, id);
  changed(t);
  next();
}

static void request(int fd, str line) {
  const str verb = str_cut(&line, ' ');
  const str id = str_trim(line);
  if (str_eq(verb, S("install")) && safe_name(id))
    queue(id, INSTALL);
  else if (str_eq(verb, S("remove")) && safe_name(id))
    queue(id, REMOVE);
  else if (str_eq(verb, S("update")))
    queue(S("os"), OS);
  else if (str_eq(verb, S("cancel"))) {
    task *t = find(id);
    if (t != NULL && t->pid > 0)
      kill(t->pid, SIGTERM);
    else if (t != NULL && t->state == QUEUED) {
      t->state = CANCELED;
      changed(t);
    }
  } else if (str_eq(verb, S("check")) && checking.pid == 0) {
    checking.kind = CHECK;
    if (!start(&checking))
      tell(fd, S("os failed can't check\n"));
  } else
    tell(fd, S("error unknown request\n"));
}

/* The launcher's FIFO: a port came or went */
static void reload_launcher(void) {
  const int fd = open(LAUNCHER_FIFO, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd >= 0) {
    if (write(fd, "reload\n", 7) != 7)
      fprintf(stderr, "downloadd: %s: %s\n", LAUNCHER_FIFO, strerror(errno));
    close(fd);
  }
}

/* A line from a job's process */
static void report(task *t, str line) {
  const str word = str_cut(&line, ' ');
  if (t == &checking) {
    if (str_eq(word, S("available")) || str_eq(word, S("current")))
      sb_set(os_status, S("os "), word, line.n ? S(" ") : S(""), line, S("\n"));
    else if (str_eq(word, S("error")))
      sb_set(os_status, S("os failed "), line, S("\n"));
    tell_all(sb_str(os_status));
    return;
  }
  if (str_eq(word, S("progress"))) {
    t->done = strtoull(line.p, NULL, 10);
    t->total = strtoull(line.p + str_find(line, ' '), NULL, 10);
  } else if (str_eq(word, S("installing"))) {
    t->state = INSTALLING;
  } else if (str_eq(word, S("error"))) {
    sb_set(t->message, line);
  }
  changed(t);
}

/* Its stdout closed: it's gone */
static void finished(task *t) {
  int status = 0;
  waitpid(t->pid, &status, 0);
  close(t->out);
  t->out = -1;
  t->pid = 0;
  if (t == &checking)
    return;
  const bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
  t->state = ok ? DONE : WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM ? CANCELED : FAILED;
  if (t->state == FAILED && t->message.n == 0)
    sb_set(t->message, S("stopped"));
  if (ok && t->kind == OS)
    sb_set(t->message, S("restart to finish"));
  changed(t);
  if (ok && (t->kind == INSTALL || t->kind == REMOVE))
    reload_launcher();
  next();
}

static void read_job(task *t) {
  const ssize_t got = read(t->out, t->line + t->n, sizeof(t->line) - 1 - t->n);
  if (got <= 0) {
    finished(t);
    return;
  }
  t->n += (size_t)got;
  for (char *end; (end = memchr(t->line, '\n', t->n)) != NULL;) {
    report(t, (str){t->line, (size_t)(end - t->line)});
    t->n -= (size_t)(end + 1 - t->line);
    memmove(t->line, end + 1, t->n);
  }
  if (t->n == sizeof(t->line) - 1)
    t->n = 0;
}

static int daemon_main(void) {
  signal(SIGPIPE, SIG_IGN);
  const struct passwd *pw = getpwnam(PLAYER);
  if (pw == NULL) {
    fprintf(stderr, "downloadd: no user %s\n", PLAYER);
    return 1;
  }
  player_uid = pw->pw_uid;
  player_gid = pw->pw_gid;
  const ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
  if (n <= 0)
    return 1;
  self[n] = '\0';

  const int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  struct sockaddr_un addr = {.sun_family = AF_UNIX, .sun_path = SOCKET};
  unlink(SOCKET);
  if (listener < 0 || bind(listener, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
      chown(SOCKET, 0, player_gid) != 0 || chmod(SOCKET, 0660) != 0 || listen(listener, 8) != 0) {
    fprintf(stderr, "downloadd: %s: %s\n", SOCKET, strerror(errno));
    return 1;
  }
  char lines[CLIENTS_MAX][256];
  size_t fill[CLIENTS_MAX] = {0};
  for (;;) {
    struct pollfd fds[1 + CLIENTS_MAX + 2];
    size_t count = 0;
    fds[count++] = (struct pollfd){.fd = listener, .events = POLLIN};
    for (size_t i = 0; i < client_count; i++)
      fds[count++] = (struct pollfd){.fd = clients[i], .events = POLLIN};
    task *running = busy();
    const size_t jobs_at = count;
    if (running != NULL)
      fds[count++] = (struct pollfd){.fd = running->out, .events = POLLIN};
    if (checking.pid > 0)
      fds[count++] = (struct pollfd){.fd = checking.out, .events = POLLIN};
    if (poll(fds, count, -1) < 0)
      continue;
    for (size_t i = jobs_at; i < count; i++)
      if (fds[i].revents)
        read_job(running != NULL && fds[i].fd == running->out ? running : &checking);
    /* Clients last: a dropped one moves the others */
    for (size_t i = client_count; i > 0; i--) {
      if (!fds[i].revents)
        continue;
      const int fd = fds[i].fd;
      size_t k = i - 1;
      const ssize_t got = read(fd, lines[k] + fill[k], sizeof(lines[k]) - fill[k]);
      if (got <= 0 || (fill[k] += (size_t)got) == sizeof(lines[k])) {
        close(fd);
        clients[k] = clients[--client_count];
        memcpy(lines[k], lines[client_count], fill[client_count]);
        fill[k] = fill[client_count];
        continue;
      }
      for (char *end; (end = memchr(lines[k], '\n', fill[k])) != NULL;) {
        request(fd, (str){lines[k], (size_t)(end - lines[k])});
        fill[k] -= (size_t)(end + 1 - lines[k]);
        memmove(lines[k], end + 1, fill[k]);
      }
    }
    if (fds[0].revents & POLLIN) {
      const int fd = accept4(listener, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
      if (fd >= 0 && client_count < CLIENTS_MAX) {
        fill[client_count] = 0;
        clients[client_count++] = fd;
        for (size_t i = 0; i < TASKS_MAX; i++)
          if (tasks[i].id.n > 0)
            tell(fd, task_line(&tasks[i]));
      } else if (fd >= 0) {
        close(fd);
      }
    }
  }
}

/* `send install ID`: until that job ends; `send check`: until the answer */
static int client(int argc, char **argv) {
  STRBUF(256) request;
  request.n = 0;
  for (int i = 2; i < argc; i++)
    sb_add(request, i > 2 ? S(" ") : S(""), str_c(argv[i]));
  sb_add(request, S("\n"));
  const str verb = str_head(sb_str(request), str_find(sb_str(request), ' '));
  STRBUF(80) id;
  sb_set(id, argc > 3 ? str_c(argv[3]) : S("os"));
  const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  struct sockaddr_un addr = {.sun_family = AF_UNIX, .sun_path = SOCKET};
  if (fd < 0 || connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
      write(fd, request.p, request.n) != (ssize_t)request.n) {
    fprintf(stderr, "downloadd: %s: %s\n", SOCKET, strerror(errno));
    return 1;
  }
  FILE *in = fdopen(fd, "r");
  char line[600];
  bool asked = false;
  while (fgets(line, sizeof(line), in) != NULL) {
    fputs(line, stdout);
    fflush(stdout);
    str rest = str_c(line);
    const str what = str_cut(&rest, ' ');
    if (str_eq(what, S("os")) && str_eq(verb, S("check\n")))
      return 0;
    if (!str_eq(what, S("job")) || !str_eq(str_cut(&rest, ' '), sb_str(id)))
      continue;
    const str state = str_cut(&rest, ' ');
    /* Its state as of connecting comes first: what this request started comes after */
    if (str_eq(state, S("queued")) || str_eq(state, S("downloading")) || str_eq(state, S("installing")))
      asked = true;
    else if (asked || str_eq(verb, S("cancel")))
      return str_eq(state, S("done")) || str_eq(state, S("removed")) || str_eq(state, S("canceled")) ? 0 : 1;
  }
  return 1;
}

int main(int argc, char **argv) {
  if (argc > 2 && strcmp(argv[1], "send") == 0)
    return client(argc, argv);
  return argc > 1 ? job(argc, argv) : daemon_main();
}
