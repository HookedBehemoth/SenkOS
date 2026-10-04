/*
 * portrun: runs a port, a folder with a port.ini (docs/ports.md), in a
 * sandbox of its own, without root. Nothing in the port sets the process up:
 *
 * - user, mount, pid and IPC namespaces; network too, unless Network=true
 * - /port: the port folder, read-only, under an overlay whose changes go to
 *   its state (STATE/<id>/upper); /home: STATE/<id>/home, HOME
 * - /layers/<name>: LAYERS/<name>.<arch>.squashfs, served by squashfuse;
 *   LAYERS/<name>.ini ([Layer] Path, Libs, Env) says what goes on PATH and
 *   LD_LIBRARY_PATH and into the environment
 * - the platform read-only (/usr, /etc, /lib and /bin as the host has them),
 *   /sys, /dev/dri, /dev/snd, /dev/input, /run/udev, the Wayland socket,
 *   PipeWire's and Pulse's; the X socket for Display=x11; private /tmp and
 *   /dev/shm
 *
 * [Patch] (patch_args) and [Setup]'s Exec run once before the first start.
 * The pad is mapped to keys and mouse ([Keys], keys.c) out here. The game's
 * stdout and stderr go to STATE/<id>/log.txt.
 *
 * usage: portrun [-q] [-r root] [-s state] [-l layers] <port folder> [args]
 * -q: no sound (no /dev/snd, no PipeWire or Pulse), for testing on a PC
 * -r: the platform (/usr, /lib, /etc, ...) from another root: an aarch64 or
 *     armhf sysroot on a PC, its programs run by qemu-user (binfmt)
 *
 * Exits with the game's status (128 + the signal that ended it), 2 when the
 * port can't run here, 3 when files it Needs aren't there yet (the launcher
 * shows its Instructions).
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>
#include <dirent.h>
#include <glob.h>

#include "cfg.h"
#include "keys.h"
#include "util.h"

#define LAYERS "/data/OS/layers"
#define STATE "/data/OS/saves/ports"
/* The host as the sandbox is built: pivot_root put it here */
#define HOST "/oldroot"

enum { LAYERS_MAX = 8, ARGS_MAX = 64, ENV_MAX = 96, PATCH_MAX = 16 };

typedef struct {
  str name;
  STRBUF(PATH_MAX) image;
  int fd; /* the image, open before /tmp is covered */
  str path, libs, env; /* from its .ini */
} layer;

static struct {
  str id, name, exec, dir, layers, arch, display, libs, env, needs, instructions;
  str setup_exec, setup_layers;
  /* [Patch]'s lines, in order: target, source */
  str patch[PATCH_MAX][2];
  size_t patches;
  bool network;
  keymap keys;
} port;

static char port_dir[PATH_MAX], state_dir[PATH_MAX], layer_dir[PATH_MAX], squashfuse[PATH_MAX];
static int squashfuse_fd = -1;
static STRBUF(16) arch;
static int width = 640, height = 480;
/* Display=x11, the port's or a layer's: DISPLAY and the X socket */
static bool x11;

/* Weston's Xwayland when the session names none */
static const char *display(void) {
  const char *d = getenv("DISPLAY");
  return d != NULL ? d : ":0";
}
/* The X socket and Xauthority, resolved before the pivot: under HOST an
 * absolute symlink (/tmp -> /var/tmp) would point into the new root */
static char x_socket[PATH_MAX], x_auth[PATH_MAX];
/* -r: where the platform comes from, "" for this system's */
static const char *platform = "";
/* What the platform runs: the machine's own (not with -r) and each loader it
 * has; [Setup] gets the first */
static const struct {
  const char *arch, *loader;
} loaders[] = {
    {"aarch64", "/lib/ld-linux-aarch64.so.1"},
    {"armhf", "/lib/ld-linux-armhf.so.3"},
    {"x86_64", "/lib64/ld-linux-x86-64.so.2"},
};
static bool quiet;
/* Where in /port the next start() runs: the game's Dir, [Patch] and [Setup] in /port itself */
static str run_dir;

/* SIGTERM and SIGINT go down the chain to the game: us, the namespaces'
 * maker, pid 1, the game. The launcher's Quit signals all of us; one Quit
 * twice is harmless. */
static volatile pid_t next_pid;

static void forward(int sig) {
  if (next_pid > 0)
    kill(next_pid, sig);
}

static void fail(const char *what, const char *arg) {
  fprintf(stderr, "portrun: %s %s: %s\n", what, arg != NULL ? arg : "", strerror(errno));
  _exit(126);
}

static void mkdirs(const char *path) {
  char p[PATH_MAX];
  snprintf(p, sizeof(p), "%s", path);
  for (char *slash = strchr(p + 1, '/');; slash = strchr(slash + 1, '/')) {
    if (slash != NULL)
      *slash = '\0';
    if (mkdir(p, 0755) != 0 && errno != EEXIST)
      fail("mkdir", p);
    if (slash == NULL)
      return;
    *slash = '/';
  }
}

static bool exists(const char *path) { return access(path, F_OK) == 0; }

/* `src` at `dst`, read-only unless `rw`; false when there's no `src` */
static bool bind(const char *src, const char *dst, bool rw) {
  struct stat st;
  if (stat(src, &st) != 0)
    return false;
  if (S_ISDIR(st.st_mode)) {
    mkdirs(dst);
  } else {
    char parent[PATH_MAX];
    snprintf(parent, sizeof(parent), "%s", dst);
    *strrchr(parent, '/') = '\0';
    mkdirs(parent);
    const int fd = open(dst, O_CREAT | O_WRONLY | O_CLOEXEC, 0644);
    if (fd < 0)
      fail("create", dst);
    close(fd);
  }
  if (mount(src, dst, NULL, MS_BIND | MS_REC, NULL) != 0)
    fail("bind", src);
  struct mount_attr ro = {.attr_set = MOUNT_ATTR_RDONLY};
  if (!rw && mount_setattr(AT_FDCWD, dst, AT_RECURSIVE, &ro, sizeof(ro)) != 0)
    fail("read-only", dst);
  return true;
}

/* bind() of a host path, at the same place in the new root */
static bool bind_host(const char *path, bool rw) {
  char src[PATH_MAX], dst[PATH_MAX];
  snprintf(src, sizeof(src), HOST "%s", path);
  snprintf(dst, sizeof(dst), "/newroot%s", path);
  return bind(src, dst, rw);
}

/* `in` with ${ARCH}, ${WIDTH}, ${HEIGHT} put in */
static void expand(str in, char *out, size_t size) {
  size_t n = 0;
  out[0] = '\0';
  while (in.n > 0) {
    const size_t at = str_find(in, '$');
    str_put(out, size, &n, &(str){in.p, at}, 1);
    in = str_at(in, at);
    if (in.n == 0)
      break;
    const size_t end = str_find(in, '}');
    const str var = in.n > 1 && in.p[1] == '{' && end < in.n ? str_head(str_at(in, 2), end - 2) : (str){0};
    if (str_eq(var, S("ARCH")))
      str_put(out, size, &n, &sb_str(arch), 1);
    else if (str_eq(var, S("WIDTH")) || str_eq(var, S("HEIGHT")))
      str_fmt(out, size, &n, "%d", str_eq(var, S("WIDTH")) ? width : height);
    else {
      str_put(out, size, &n, &S("$"), 1);
      in = str_at(in, 1);
      continue;
    }
    in = str_at(in, end + 1);
  }
}

/* The environment, built before the fork: "NAME=value" each, a later one
 * replacing an earlier one of the same name */
static char *env[ENV_MAX + 1];
static size_t env_count;

static void env_put(str item) {
  const size_t eq = str_find(item, '=');
  if (eq == item.n || eq == 0)
    return;
  char value[PATH_MAX * 2];
  expand(item, value, sizeof(value));
  size_t i = 0;
  while (i < env_count && !(strncmp(env[i], value, eq + 1) == 0))
    i++;
  if (i == ENV_MAX)
    return;
  if (i == env_count)
    env_count++;
  else
    free(env[i]);
  env[i] = strdup(value);
}

static void env_fmt(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void env_fmt(const char *fmt, ...) {
  char item[PATH_MAX * 2];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(item, sizeof(item), fmt, ap);
  va_end(ap);
  env_put(str_c(item));
}

/* What passes from ours into the game's: locale, and settings for the
 * platform's libraries */
static bool passes(const char *item) {
  static const char *const prefixes[] = {"LANG=", "LC_", "TZ=", "SDL_", "MESA_", "PAN_", "VK_", "LIBGL_",
                                         "__GL", "GALLIUM_"};
  for (size_t i = 0; i < COUNTOF(prefixes); i++)
    if (strncmp(item, prefixes[i], strlen(prefixes[i])) == 0)
      return true;
  return false;
}

static layer layers[LAYERS_MAX];
static size_t layer_count;

/* The layers named in `names` for architecture `a`, their files found and
 * .ini read */
static bool add_layers(str names, str a) {
  for (str name; cfg_item(&names, &name);) {
    bool have = false;
    for (size_t i = 0; i < layer_count; i++)
      have |= str_eq(layers[i].name, name);
    if (have)
      continue;
    if (layer_count == LAYERS_MAX) {
      fprintf(stderr, "portrun: more than %d layers\n", LAYERS_MAX);
      return false;
    }
    layer *l = &layers[layer_count++];
    *l = (layer){.name = name};
    if (!sb_set(l->image, str_c(layer_dir), S("/"), name, S("."), a, S(".squashfs")) ||
        (!exists(l->image.p) && !sb_set(l->image, str_c(layer_dir), S("/"), name, S(".squashfs"))) ||
        !exists(l->image.p)) {
      fprintf(stderr, "portrun: no layer %.*s for %.*s in %s\n", (int)name.n, name.p, (int)a.n, a.p, layer_dir);
      return false;
    }
    if ((l->fd = open(l->image.p, O_RDONLY | O_CLOEXEC)) < 0)
      fail("open", l->image.p);
    STRBUF(PATH_MAX) ini;
    sb_set(ini, str_c(layer_dir), S("/"), name, S(".ini"));
    const str text = str_read(fopen(ini.p, "r")); /* kept: the fields point into it */
    cfg_reader r = cfg_init(text);
    for (cfg_event e; (e = cfg_next(&r)) != CFG_END;) {
      if (e != CFG_VALUE || !str_eq(r.section, S("Layer")))
        continue;
      /* "Key[arch]": only for that architecture */
      str key = r.key;
      const size_t open = str_find(key, '[');
      if (open < key.n) {
        if (key.p[key.n - 1] != ']' || !str_eq(str_head(str_at(key, open + 1), key.n - open - 2), a))
          continue;
        key = str_head(key, open);
      }
      if (str_eq(key, S("Path")))
        l->path = r.value;
      else if (str_eq(key, S("Libs")))
        l->libs = r.value;
      else if (str_eq(key, S("Env")))
        l->env = r.value;
      else if (str_eq(key, S("Display")))
        x11 |= str_eq(r.value, S("x11"));
    }
  }
  return true;
}

static void read_port(void) {
  STRBUF(PATH_MAX) ini;
  sb_set(ini, str_c(port_dir), S("/port.ini"));
  const str text = str_read(fopen(ini.p, "r")); /* kept */
  if (text.p == NULL)
    fail("read", ini.p);
  cfg_reader r = cfg_init(text);
  for (cfg_event e; (e = cfg_next(&r)) != CFG_END;) {
    if (e != CFG_VALUE)
      continue;
    if (str_eq(r.section, S("Keys"))) {
      keys_set(&port.keys, r.key, r.value);
      continue;
    }
    if (str_eq(r.section, S("Patch"))) {
      if (port.patches < PATCH_MAX) {
        port.patch[port.patches][0] = r.key;
        port.patch[port.patches++][1] = r.value;
      }
      continue;
    }
    if (str_eq(r.section, S("Setup"))) {
      if (str_eq(r.key, S("Exec")))
        port.setup_exec = r.value;
      else if (str_eq(r.key, S("Layers")))
        port.setup_layers = r.value;
      continue;
    }
    if (!str_eq(r.section, S("Port")))
      continue;
    str *field = str_eq(r.key, S("Name"))           ? &port.name
                 : str_eq(r.key, S("Exec"))         ? &port.exec
                 : str_eq(r.key, S("Layers"))       ? &port.layers
                 : str_eq(r.key, S("Arch"))         ? &port.arch
                 : str_eq(r.key, S("Display"))      ? &port.display
                 : str_eq(r.key, S("Libs"))         ? &port.libs
                 : str_eq(r.key, S("Env"))          ? &port.env
                 : str_eq(r.key, S("Needs"))        ? &port.needs
                 : str_eq(r.key, S("Instructions")) ? &port.instructions
                 : str_eq(r.key, S("Dir"))          ? &port.dir
                                                    : NULL;
    if (field != NULL)
      *field = r.value;
    else if (str_eq(r.key, S("Network")))
      port.network = str_eq(r.value, S("true"));
  }
  if (port.exec.n == 0) {
    fprintf(stderr, "portrun: %s has no Exec\n", ini.p);
    exit(2);
  }
}

static bool runs(str a) {
  struct utsname u;
  uname(&u);
  if (platform[0] == '\0' && str_eq(a, str_c(u.machine)))
    return true;
  for (size_t i = 0; i < COUNTOF(loaders); i++) {
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s%s", platform, loaders[i].loader);
    if (str_eq(a, str_c(loaders[i].arch)) && exists(p))
      return true;
  }
  return false;
}

/* The platform's own architecture: the machine's, or with -r its first loader's */
static str native_arch(void) {
  static struct utsname u;
  uname(&u);
  for (size_t i = 0; platform[0] != '\0' && i < COUNTOF(loaders); i++)
    if (runs(str_c(loaders[i].arch)))
      return str_c(loaders[i].arch);
  return str_c(u.machine);
}

/* The first of the port's architectures the platform runs */
static bool pick_arch(void) {
  if (port.arch.n == 0)
    return sb_set(arch, native_arch());
  for (str a = port.arch, item; cfg_item(&a, &item);)
    if (runs(item))
      return sb_set(arch, item);
  fprintf(stderr, "portrun: the port is for %.*s, this platform runs %.*s\n", (int)port.arch.n, port.arch.p,
          (int)native_arch().n, native_arch().p);
  return false;
}

/* The display's size: PORTRUN_SIZE ("640x480"), else the first connected
 * DRM connector's first mode */
static void display_size(void) {
  const char *size = getenv("PORTRUN_SIZE");
  if (size != NULL && sscanf(size, "%dx%d", &width, &height) == 2)
    return;
  glob_t g;
  if (glob("/sys/class/drm/card*-*/status", 0, NULL, &g) != 0)
    return;
  for (size_t i = 0; i < g.gl_pathc; i++) {
    char line[32] = "", path[PATH_MAX];
    FILE *f = fopen(g.gl_pathv[i], "r");
    if (f == NULL)
      continue;
    (void)!fgets(line, sizeof(line), f);
    fclose(f);
    snprintf(path, sizeof(path), "%.*s/modes", (int)(strrchr(g.gl_pathv[i], '/') - g.gl_pathv[i]), g.gl_pathv[i]);
    if (strncmp(line, "connected", 9) == 0 && (f = fopen(path, "r")) != NULL) {
      const bool got = fscanf(f, "%dx%d", &width, &height) == 2;
      fclose(f);
      if (got)
        break;
    }
  }
  globfree(&g);
}

/* Everything the port says it needs is there, in its folder or its changes */
static bool needs_met(void) {
  for (str needs = port.needs, item; cfg_item(&needs, &item);) {
    char a[PATH_MAX], b[PATH_MAX];
    snprintf(a, sizeof(a), "%s/%.*s", port_dir, (int)item.n, item.p);
    snprintf(b, sizeof(b), "%s/upper/%.*s", state_dir, (int)item.n, item.p);
    if (!exists(a) && !exists(b)) {
      fprintf(stderr, "portrun: %s is missing. %.*s\n", a, (int)port.instructions.n, port.instructions.p);
      return false;
    }
  }
  return true;
}

static void build_env(void) {
  extern char **environ;
  for (char **e = environ; *e != NULL; e++)
    if (passes(*e))
      env_put(str_c(*e));
  const char *runtime = getenv("XDG_RUNTIME_DIR"), *wayland = getenv("WAYLAND_DISPLAY");
  env_fmt("HOME=/home");
  env_fmt("XDG_RUNTIME_DIR=/run/user/%u", getuid());
  if (runtime != NULL && wayland != NULL)
    env_fmt("WAYLAND_DISPLAY=%s", strrchr(wayland, '/') ? strrchr(wayland, '/') + 1 : wayland);
  if (x11) {
    env_fmt("DISPLAY=%s", display());
    env_fmt("SDL_VIDEODRIVER=x11");
    if (getenv("XAUTHORITY") != NULL)
      env_fmt("XAUTHORITY=/run/user/%u/Xauthority", getuid());
  }
  /* Layers' first: the port's own settings win */
  STRBUF(4096) path = {0}, libs = {0};
  for (str l = port.libs, item; cfg_item(&l, &item);)
    sb_add(libs, S("/port/"), item, S(":"));
  for (size_t i = 0; i < layer_count; i++) {
    for (str p = layers[i].path, item; cfg_item(&p, &item);)
      sb_add(path, S("/layers/"), layers[i].name, S("/"), item, S(":"));
    for (str p = layers[i].libs, item; cfg_item(&p, &item);)
      sb_add(libs, S("/layers/"), layers[i].name, S("/"), item, S(":"));
    for (str e = layers[i].env, item; cfg_item(&e, &item);)
      env_put(item);
  }
  sb_add(path, S("/usr/local/bin:/usr/bin:/bin"));
  env_fmt("PATH=%s", path.p);
  if (libs.n > 0) {
    libs.p[--libs.n] = '\0';
    env_fmt("LD_LIBRARY_PATH=%s", libs.p);
  }
  for (str e = port.env, item; cfg_item(&e, &item);)
    env_put(item);
}

/* Exec's words, with ${...} put in: blanks split them, "..." keeps them
 * together */
static char words[4096];
static char *args[ARGS_MAX + 1];

static void split(str exec, char **extra, int extra_count) {
  expand(exec, words, sizeof(words));
  size_t n = 0;
  for (char *p = words, *out = words; *p != '\0' && n < ARGS_MAX;) {
    while (*p == ' ' || *p == '\t')
      p++;
    if (*p == '\0')
      break;
    args[n++] = out;
    for (bool quoted = false; *p != '\0' && (quoted || (*p != ' ' && *p != '\t')); p++)
      if (*p == '"')
        quoted = !quoted;
      else
        *out++ = *p;
    if (*p != '\0')
      p++;
    *out++ = '\0';
  }
  for (int i = 0; i < extra_count && n < ARGS_MAX; i++)
    args[n++] = extra[i];
  args[n] = NULL;
}

/* One [Patch] line as a command of the tools layer's, in /port:
 *   file=old + diff  xdelta3: file is old patched with diff
 *   dir/=archive     7-Zip unpacks archive (zip, 7z, tar, an installer .exe) into dir
 *   archive=dir/     zip stores dir in archive (GameMaker's apk)
 *   dir/=src/        src's files copied over dir's (the port's fixes over the player's files) */
static void patch_args(str target, str source) {
  static char buf[2 * PATH_MAX];
  char *out = buf;
  size_t n = 0;
  /* Each a word of args, the string copied into buf */
  str w[7] = {0};
  const char *plus = memmem(source.p, source.n, " + ", 3);
  char o[PATH_MAX];
  if (plus != NULL) {
    const size_t at = (size_t)(plus - source.p);
    const str x[] = {S("xdelta3"), S("-d"), S("-f"), S("-s"), str_head(source, at), str_at(source, at + 3), target};
    memcpy(w, x, sizeof(x));
  } else if (target.n > 0 && target.p[target.n - 1] == '/' && source.n > 0 && source.p[source.n - 1] == '/') {
    snprintf(o, sizeof(o), "%.*s.", (int)source.n, source.p);
    const str x[] = {S("cp"), S("-R"), str_c(o), target};
    memcpy(w, x, sizeof(x));
  } else if (target.n > 0 && target.p[target.n - 1] == '/') {
    snprintf(o, sizeof(o), "-o%.*s", (int)target.n, target.p);
    const str x[] = {S("7zz"), S("x"), S("-y"), str_c(o), source};
    memcpy(w, x, sizeof(x));
  } else {
    const str x[] = {S("zip"), S("-q"), S("-r"), S("-0"), target, source};
    memcpy(w, x, sizeof(x));
  }
  for (size_t i = 0; i < COUNTOF(w) && w[i].p != NULL && out + w[i].n < buf + sizeof(buf); i++) {
    memcpy(out, w[i].p, w[i].n);
    out[w[i].n] = '\0';
    args[n++] = out;
    out += w[i].n + 1;
  }
  args[n] = NULL;
}

/* A layer's squashfs at /tmp/layers/<name>: we mount /dev/fuse there and
 * squashfuse serves it (it can't mount, it has no capabilities after exec) */
static void serve_layer(const layer *l) {
  char at[PATH_MAX], data[128], fd_path[32], image[32];
  snprintf(at, sizeof(at), "/tmp/layers/%.*s", (int)l->name.n, l->name.p);
  mkdirs(at);
  const int fuse = open("/dev/fuse", O_RDWR);
  if (fuse < 0)
    fail("open", "/dev/fuse");
  snprintf(data, sizeof(data), "fd=%d,rootmode=40000,user_id=%u,group_id=%u", fuse, getuid(), getgid());
  if (mount("squashfuse", at, "fuse.squashfuse", MS_NOSUID | MS_NODEV | MS_RDONLY, data) != 0)
    fail("mount fuse at", at);
  snprintf(fd_path, sizeof(fd_path), "/dev/fd/%d", fuse);
  snprintf(image, sizeof(image), "/proc/self/fd/%d", l->fd);
  const pid_t pid = fork();
  if (pid == 0) {
    fcntl(l->fd, F_SETFD, 0);
    char *const argv[] = {squashfuse, "-f", image, fd_path, NULL};
    extern char **environ;
    fexecve(squashfuse_fd, argv, environ);
    fail("exec", squashfuse);
  }
  close(fuse);
  /* Served once a listing comes back: squashfuse has the image open, the
   * host can go. If it died, the listing fails. */
  DIR *d = opendir(at);
  if (d == NULL || readdir(d) == NULL)
    fail("serve", l->image.p);
  closedir(d);
}

/* The port's process, as pid 1 of its namespace: the new root built, the
 * command run, its status returned */
static int sandbox(int log) {
  if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0)
    fail("make / private", NULL);
  if (mount("tmpfs", "/tmp", "tmpfs", MS_NOSUID | MS_NODEV, "mode=0755") != 0)
    fail("mount tmpfs at", "/tmp");
  for (size_t i = 0; i < layer_count; i++)
    serve_layer(&layers[i]);
  mkdirs("/tmp/oldroot");
  mkdirs("/tmp/newroot");
  if (chdir("/tmp") != 0 || syscall(SYS_pivot_root, ".", "oldroot") != 0)
    fail("pivot_root", NULL);
  if (mount("tmpfs", "/newroot", "tmpfs", MS_NOSUID | MS_NODEV, "mode=0755") != 0)
    fail("mount tmpfs at", "/newroot");

  /* The platform: the host's directories, its /lib and /bin links */
  static const char *const roots[] = {"usr", "bin", "sbin", "lib", "lib32", "lib64", "libx32", "etc"};
  for (size_t i = 0; i < COUNTOF(roots); i++) {
    char src[PATH_MAX], dst[PATH_MAX], target[PATH_MAX];
    snprintf(src, sizeof(src), HOST "%s/%s", platform, roots[i]);
    snprintf(dst, sizeof(dst), "/newroot/%s", roots[i]);
    struct stat st;
    if (lstat(src, &st) != 0)
      continue;
    if (S_ISLNK(st.st_mode)) {
      const ssize_t n = readlink(src, target, sizeof(target) - 1);
      if (n < 0 || (target[n] = '\0', symlink(target, dst) != 0))
        fail("link", dst);
    } else if (S_ISDIR(st.st_mode)) {
      bind(src, dst, false);
    }
  }
  bind_host("/sys", false);
  bind_host("/run/udev", false);
  mkdirs("/newroot/proc");
  if (mount("proc", "/newroot/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) != 0)
    fail("mount", "/proc");

  /* /dev: the harmless nodes, the GPU, sound and input */
  mkdirs("/newroot/dev");
  if (mount("tmpfs", "/newroot/dev", "tmpfs", MS_NOSUID, "mode=0755") != 0)
    fail("mount tmpfs at", "/dev");
  static const char *const nodes[] = {"/dev/null", "/dev/zero", "/dev/full", "/dev/random", "/dev/urandom",
                                      "/dev/tty", "/dev/dri", "/dev/input", "/dev/snd"};
  for (size_t i = 0; i < COUNTOF(nodes) - quiet; i++)
    bind_host(nodes[i], true);
  mkdirs("/newroot/dev/shm");
  if (mount("tmpfs", "/newroot/dev/shm", "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777") != 0)
    fail("mount tmpfs at", "/dev/shm");
  if (symlink("/proc/self/fd", "/newroot/dev/fd") != 0 || symlink("/proc/self/fd/0", "/newroot/dev/stdin") != 0 ||
      symlink("/proc/self/fd/1", "/newroot/dev/stdout") != 0 || symlink("/proc/self/fd/2", "/newroot/dev/stderr") != 0)
    fail("link", "/dev/fd");

  mkdirs("/newroot/tmp");
  if (mount("tmpfs", "/newroot/tmp", "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777") != 0)
    fail("mount tmpfs at", "/tmp");

  /* The sockets: Wayland, PipeWire, Pulse; X for Display=x11 */
  char src[PATH_MAX], dst[PATH_MAX];
  const char *runtime = getenv("XDG_RUNTIME_DIR"), *wayland = getenv("WAYLAND_DISPLAY");
  snprintf(dst, sizeof(dst), "/newroot/run/user/%u", getuid());
  mkdirs(dst);
  if (mount("tmpfs", dst, "tmpfs", MS_NOSUID | MS_NODEV, "mode=0700") != 0)
    fail("mount tmpfs at", dst);
  if (runtime != NULL) {
    static const char *const sockets[] = {"pipewire-0", "pulse"};
    for (size_t i = 0; i < COUNTOF(sockets) + 1; i++) {
      const char *name = i < COUNTOF(sockets) ? sockets[i] : wayland;
      if (name == NULL || (quiet && i < COUNTOF(sockets)))
        continue;
      const char *base = strrchr(name, '/') ? strrchr(name, '/') + 1 : name;
      if (name[0] == '/')
        snprintf(src, sizeof(src), HOST "%s", name);
      else
        snprintf(src, sizeof(src), HOST "%s/%s", runtime, name);
      snprintf(dst, sizeof(dst), "/newroot/run/user/%u/%s", getuid(), base);
      bind(src, dst, true);
    }
  }
  if (x_socket[0] != '\0') {
    snprintf(src, sizeof(src), HOST "%s", x_socket);
    snprintf(dst, sizeof(dst), "/newroot/tmp/.X11-unix/X%d", atoi(display() + 1));
    bind(src, dst, true);
  }
  if (x_auth[0] != '\0') {
    snprintf(src, sizeof(src), HOST "%s", x_auth);
    snprintf(dst, sizeof(dst), "/newroot/run/user/%u/Xauthority", getuid());
    bind(src, dst, false);
  }

  /* The port and its state */
  char data[PATH_MAX * 4];
  snprintf(data, sizeof(data), "lowerdir=" HOST "%s,upperdir=" HOST "%s/upper,workdir=" HOST "%s/work,userxattr",
           port_dir, state_dir, state_dir);
  mkdirs("/newroot/port");
  if (mount("overlay", "/newroot/port", "overlay", MS_NOSUID | MS_NODEV, data) != 0)
    fail("mount overlay", data);
  snprintf(src, sizeof(src), HOST "%s/home", state_dir);
  bind(src, "/newroot/home", true);
  for (size_t i = 0; i < layer_count; i++) {
    snprintf(src, sizeof(src), "/layers/%.*s", (int)layers[i].name.n, layers[i].name.p);
    snprintf(dst, sizeof(dst), "/newroot/layers/%.*s", (int)layers[i].name.n, layers[i].name.p);
    mkdirs(dst);
    if (mount(src, dst, NULL, MS_MOVE, NULL) != 0)
      fail("move", src);
  }

  /* Into it, the host gone, the root read-only */
  if (umount2(HOST, MNT_DETACH) != 0 || chdir("/newroot") != 0 || syscall(SYS_pivot_root, ".", ".") != 0 ||
      umount2(".", MNT_DETACH) != 0 || chdir("/port") != 0)
    fail("enter the sandbox", NULL);
  char dir[PATH_MAX];
  snprintf(dir, sizeof(dir), "%.*s", (int)run_dir.n, run_dir.p);
  if (run_dir.n > 0 && chdir(dir) != 0)
    fail("enter", dir);
  struct mount_attr ro = {.attr_set = MOUNT_ATTR_RDONLY};
  if (mount_setattr(AT_FDCWD, "/", 0, &ro, sizeof(ro)) != 0)
    fail("read-only", "/");

  const pid_t game = fork();
  next_pid = game;
  if (game == 0) {
    signal(SIGTERM, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
    const int null = open("/dev/null", O_RDONLY);
    dup2(null, 0);
    dup2(log, 1);
    dup2(log, 2);
    /* Nothing else of ours: the launcher's control channel, the layers */
    close_range(3, ~0U, 0);
    extern char **environ;
    environ = env;
    execvp(args[0], args);
    fail("exec", args[0]);
  }
  /* pid 1 reaps what the game leaves; the game's status is ours */
  for (;;) {
    int status;
    const pid_t pid = wait(&status);
    if (pid < 0 && errno != EINTR)
      return 126;
    if (pid == game)
      return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
  }
}

static void write_file(const char *path, const char *text) {
  const int fd = open(path, O_WRONLY | O_CLOEXEC);
  if (fd < 0 || write(fd, text, strlen(text)) != (ssize_t)strlen(text))
    fail("write", path);
  close(fd);
}

/* Runs `args` in a new sandbox: a child that makes the namespaces and its
 * child, pid 1 in them. Gone with us. Its pid. */
static pid_t start(void) {
  STRBUF(PATH_MAX) log;
  sb_set(log, str_c(state_dir), S("/log.txt"));
  const int out = open(log.p, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (out < 0)
    fail("open", log.p);
  const pid_t parent = getpid();
  next_pid = 0;
  const pid_t pid = fork();
  if (pid != 0) {
    close(out);
    next_pid = pid;
    return pid;
  }
  prctl(PR_SET_PDEATHSIG, SIGKILL);
  if (getppid() != parent)
    _exit(1);
  const uid_t uid = getuid();
  const gid_t gid = getgid();
  /* X11 clients share images with Xwayland through SysV shared memory (MIT-SHM): the session's
   * IPC namespace, or the server can't attach the segments */
  const int flags = CLONE_NEWUSER | CLONE_NEWNS | CLONE_NEWPID | (x11 ? 0 : CLONE_NEWIPC) |
                    (port.network ? 0 : CLONE_NEWNET);
  if (unshare(flags) != 0)
    fail("unshare", NULL);
  char map[64];
  write_file("/proc/self/setgroups", "deny");
  snprintf(map, sizeof(map), "%u %u 1", uid, uid);
  write_file("/proc/self/uid_map", map);
  snprintf(map, sizeof(map), "%u %u 1", gid, gid);
  write_file("/proc/self/gid_map", map);
  next_pid = 0;
  const pid_t init = fork();
  next_pid = init;
  if (init == 0) {
    /* Its parent is outside its pid namespace: getppid() says 0 */
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    _exit(sandbox(out));
  }
  int status = 0;
  while (waitpid(init, &status, 0) < 0 && errno == EINTR)
    ;
  _exit(WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status));
}

static int finish(pid_t pid) {
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
    ;
  return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

/* The first of `names` on PATH */
static bool find_program(const char *const *names, size_t count, char *out, size_t size) {
  const char *path = getenv("PATH");
  for (size_t i = 0; i < count; i++)
    for (str dirs = str_c(path != NULL ? path : "/usr/bin:/bin"), dir; dirs.n > 0;) {
      dir = str_cut(&dirs, ':');
      snprintf(out, size, "%.*s/%s", (int)dir.n, dir.p, names[i]);
      if (access(out, X_OK) == 0)
        return true;
    }
  return false;
}

int main(int argc, char **argv) {
  const char *state = STATE, *layer_root = LAYERS;
  int opt;
  while ((opt = getopt(argc, argv, "+qr:s:l:")) != -1) {
    if (opt == 'q')
      quiet = true;
    else if (opt == 'r') {
      if ((platform = realpath(optarg, NULL)) == NULL)
        fail("find", optarg);
    }
    else if (opt == 's')
      state = optarg;
    else if (opt == 'l')
      layer_root = optarg;
    else
      return 2;
  }
  if (optind >= argc) {
    fprintf(stderr, "usage: portrun [-q] [-r root] [-s state] [-l layers] <port folder> [args]\n");
    return 2;
  }
  if (realpath(argv[optind], port_dir) == NULL)
    fail("find", argv[optind]);
  port.id = str_at(str_c(port_dir), str_rfind(str_c(port_dir), '/') + 1);
  snprintf(state_dir, sizeof(state_dir), "%s/%.*s", state, (int)port.id.n, port.id.p);
  static const char *const parts[] = {"home", "upper", "work"};
  for (size_t i = 0; i < COUNTOF(parts); i++) {
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s/%s", state_dir, parts[i]);
    mkdirs(p);
  }
  char real[PATH_MAX];
  if (realpath(state_dir, real) == NULL)
    fail("find", state_dir);
  memcpy(state_dir, real, sizeof(real));
  if (realpath(layer_root, layer_dir) == NULL)
    snprintf(layer_dir, sizeof(layer_dir), "%s", layer_root);

  read_port();
  x11 = str_eq(port.display, S("x11"));
  display_size();
  if (!pick_arch())
    return 2;
  if (!needs_met())
    return 3;
  const bool setup = port.setup_exec.n > 0 || port.patches > 0;
  /* [Setup]'s and [Patch]'s tools are the platform's own, whatever the game is built for */
  if (!add_layers(port.layers, sb_str(arch)) ||
      (setup && (!add_layers(port.setup_layers, native_arch()) ||
                 (port.patches > 0 && !add_layers(S("tools"), native_arch())))))
    return 2;
  static const char *const fusers[] = {"squashfuse_ll", "squashfuse"};
  if (layer_count > 0 && (!find_program(fusers, COUNTOF(fusers), squashfuse, sizeof(squashfuse)) ||
                          (squashfuse_fd = open(squashfuse, O_PATH | O_CLOEXEC)) < 0)) {
    fprintf(stderr, "portrun: no squashfuse for the layers\n");
    return 2;
  }
  build_env();
  if (x11 && display()[0] == ':') {
    char p[64];
    snprintf(p, sizeof(p), "/tmp/.X11-unix/X%d", atoi(display() + 1));
    if (realpath(p, x_socket) == NULL)
      x_socket[0] = '\0';
    if (getenv("XAUTHORITY") == NULL || realpath(getenv("XAUTHORITY"), x_auth) == NULL)
      x_auth[0] = '\0';
  }
  struct sigaction sa = {.sa_handler = forward, .sa_flags = SA_RESTART};
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGINT, &sa, NULL);

  /* [Patch], then [Setup]'s Exec, once, with their layers too */
  STRBUF(PATH_MAX) done;
  sb_set(done, str_c(state_dir), S("/setup.done"));
  if (setup && !exists(done.p)) {
    for (size_t i = 0; i <= port.patches; i++) {
      if (i < port.patches)
        patch_args(port.patch[i][0], port.patch[i][1]);
      else if (port.setup_exec.n > 0)
        split(port.setup_exec, NULL, 0);
      else
        break;
      const int status = finish(start());
      if (status != 0) {
        fprintf(stderr, "portrun: %s exited with %d, see %s/log.txt\n", args[0], status, state_dir);
        return status;
      }
    }
    fclose(fopen(done.p, "w"));
  }

  split(port.exec, argv + optind + 1, argc - optind - 1);
  run_dir = port.dir;
  const pid_t pid = start();
  const int child = (int)syscall(SYS_pidfd_open, pid, 0);
  keys_run(&port.keys, child);
  return finish(pid);
}
