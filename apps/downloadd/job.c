/* One job, in its own process: what it does on stdout, "error WHY" and exit 1 when it fails */
#include <curl/curl.h>
#include <errno.h>
#include <limits.h>
#include <openssl/evp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>
#include <zstd.h>

#include "cfg.h"
#include "downloadd.h"
#include "util.h"

#define CONFIG "/etc/updates"
#define BUILD "/etc/build"
#define PACKS "/data/OS/assets/thumbnails"

static STRBUF(256) server;
static STRBUF(32) channel;

static void load_config(void) {
  sb_set(server, S("https://senkos.dev"));
  sb_set(channel, S("stable"));
  const str text = str_read(fopen(CONFIG, "r"));
  cfg_reader r = cfg_init(text);
  for (cfg_event e; (e = cfg_next(&r)) != CFG_END;) {
    if (e != CFG_VALUE || !str_eq(r.section, S("Updates")))
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
  curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
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

/* mkdir -p: a new card, or one wiped, has none of the folders */
static void make_dirs(const char *path) {
  char p[PATH_MAX];
  snprintf(p, sizeof(p), "%s", path);
  for (char *s = p + 1; *s; s++)
    if (*s == '/') {
      *s = '\0';
      mkdir(p, 0755);
      *s = '/';
    }
  if (mkdir(p, 0755) != 0 && errno != EEXIST)
    fail("%s: %s", p, strerror(errno));
}

typedef struct {
  STRBUF(512) url;
  STRBUF(65) sha256;
  uint64_t size;
} file;

/* /v1/packs: a "[NAME]" section a pack, Url, Sha256, Size */
static void install_pack(str name) {
  STRBUF(512) url;
  sb_set(url, sb_str(server), S("/v1/packs"));
  const str text = get(url.p);
  file pack = {0};
  cfg_reader r = cfg_init(text);
  for (cfg_event e; (e = cfg_next(&r)) != CFG_END;) {
    if (e != CFG_VALUE || !str_eq(r.section, name))
      continue;
    if (str_eq(r.key, S("Url")))
      sb_set(pack.url, r.value);
    else if (str_eq(r.key, S("Sha256")))
      sb_set(pack.sha256, r.value);
    else if (str_eq(r.key, S("Size")))
      pack.size = strtoull(r.value.p, NULL, 10);
  }
  if (pack.url.n == 0 || pack.sha256.n != 64)
    fail("the server has no box art for %.*s", (int)name.n, name.p);
  struct statvfs fs;
  make_dirs(PACKS);
  if (statvfs(PACKS, &fs) == 0 && (uint64_t)fs.f_bavail * fs.f_frsize < pack.size + (16 << 20))
    fail("not enough space: %llu MB needed", (unsigned long long)(pack.size >> 20) + 16);
  STRBUF(256) part, path;
  sb_set(part, S(PACKS "/."), name, S(".pack.part"));
  sb_set(path, S(PACKS "/"), name, S(".pack"));
  FILE *out = fopen(part.p, "w");
  if (out == NULL)
    fail("%s: %s", part.p, strerror(errno));
  uint64_t done = 0;
  fetch(pack.url.p, sb_str(pack.sha256), false, out, &done, pack.size);
  if (fclose(out) != 0 || rename(part.p, path.p) != 0)
    fail("%s: %s", path.p, strerror(errno));
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

int job(int argc, char **argv) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  load_config();
  curl_global_init(CURL_GLOBAL_DEFAULT);
  const str id = argc > 2 ? str_c(argv[2]) : S("");
  if (argc == 3 && strcmp(argv[1], "pack") == 0 && safe_pack(id))
    install_pack(id);
  else if (argc == 2 && strcmp(argv[1], "check") == 0)
    check();
  else if (argc == 2 && strcmp(argv[1], "os") == 0)
    update_os();
  else if (argc == 2 && strcmp(argv[1], "zone") == 0)
    zone();
  else {
    fprintf(stderr, "usage: downloadd [pack NAME | check | os | zone]\n");
    return 2;
  }
  return 0;
}
