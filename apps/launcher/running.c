#include "running.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <linux/capability.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/pidfd.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "util.h"
#include "wake.h"

/* weston.init's cgroup for apps: memory and CPU limits that keep the UI usable */
#define APPS_CGROUP "/sys/fs/cgroup/session/apps/cgroup.procs"
/* Long enough to write a save state to the card */
#define REQUEST_MS 3000

running runs[RUNNING_MAX];
static uint32_t seq;

/* run_drop_root: the apps' environment, user and groups; app_groups < 0: apps run as we do */
extern char **environ;
static char **app_environ;
static uid_t app_uid;
static gid_t app_gid, app_group_list[32];
static int app_groups = -1;

/* Effective and permitted; nothing inheritable */
static bool set_caps(uint32_t caps) {
  struct __user_cap_header_struct h = {.version = _LINUX_CAPABILITY_VERSION_3};
  struct __user_cap_data_struct d[2] = {{.effective = caps, .permitted = caps}};
  return syscall(SYS_capset, &h, d) == 0;
}

bool run_drop_root(void) {
  if (getuid() != 0)
    return true;
  const struct passwd *player = getpwnam("player");
  if (player == NULL)
    return false;
  app_uid = player->pw_uid;
  app_gid = player->pw_gid;
  int n = COUNTOF(app_group_list);
  if (getgrouplist("player", app_gid, app_group_list, &n) < 0)
    return false;
  size_t count = 0;
  while (environ[count] != NULL)
    count++;
  if ((app_environ = calloc(count + 1, sizeof(*app_environ))) == NULL)
    return false;
  for (size_t i = 0; i < count; i++)
    if ((app_environ[i] = strdup(environ[i])) == NULL)
      return false;
  const struct passwd *shell = getpwnam("shell");
  if (shell == NULL)
    return false;
  const uid_t uid = shell->pw_uid;
  const gid_t gid = shell->pw_gid;
  STRBUF(256) home;
  sb_set(home, str_c(shell->pw_dir));
  if (initgroups("shell", gid) != 0 || prctl(PR_SET_KEEPCAPS, 1) != 0 || setresgid(gid, gid, gid) != 0 ||
      setresuid(uid, uid, uid) != 0 || !set_caps(1u << CAP_SETUID | 1u << CAP_SETGID | 1u << CAP_KILL))
    return false;
  app_groups = n;
  prctl(PR_SET_KEEPCAPS, 0);
  /* Weston's socket by its path: the apps' runtime dir lets the player's group in */
  const char *display = getenv("WAYLAND_DISPLAY"), *run = run_app_env("XDG_RUNTIME_DIR");
  STRBUF(256) path;
  if (display != NULL && display[0] != '/' && run != NULL && sb_set(path, str_c(run), S("/"), str_c(display)))
    setenv("WAYLAND_DISPLAY", path.p, 1);
  path.n = 0;
  sb_fmt(path, "/run/user/%u", (unsigned)uid);
  setenv("XDG_RUNTIME_DIR", path.p, 1);
  static const char *const dirs[][2] = {{"XDG_CONFIG_HOME", "/config"}, {"XDG_STATE_HOME", "/state"},
                                        {"XDG_CACHE_HOME", "/cache"}};
  for (size_t i = 0; i < COUNTOF(dirs); i++)
    if (sb_set(path, sb_str(home), str_c(dirs[i][1])))
      setenv(dirs[i][0], path.p, 1);
  return true;
}

const char *run_app_env(const char *name) {
  if (app_environ == NULL)
    return getenv(name);
  const size_t n = strlen(name);
  for (char **e = app_environ; *e != NULL; e++)
    if (strncmp(*e, name, n) == 0 && (*e)[n] == '=')
      return *e + n + 1;
  return NULL;
}

/* Wakes the UI once the app is gone. Ours can't be reaped before that is
 * handled, so the pid stays valid until then. */
static void *exit_thread(void *data) {
  const pid_t pid = (pid_t)(intptr_t)data;
  const int fd = pidfd_open(pid, 0);
  if (fd >= 0) {
    struct pollfd p = {.fd = fd, .events = POLLIN};
    while (poll(&p, 1, -1) < 0 && errno == EINTR)
      ;
    close(fd);
  } else if (errno != ESRCH) {
    fprintf(stderr, "pidfd_open: %s\n", strerror(errno));
  }
  win_wake(UI_EXITED, pid);
  return NULL;
}

running *run_launch(str label, str icon, str exec, str arg, str dir) {
  running *r = NULL;
  for (size_t i = 0; i < RUNNING_MAX && r == NULL; i++)
    if (runs[i].pid == 0)
      r = &runs[i];
  if (r == NULL)
    return NULL;
  /* Before fork too: terminated, for the child's execle */
  *r = (running){0};
  sb_set(r->label, label);
  sb_set(r->icon, icon);
  sb_set(r->exec, exec);
  sb_set(r->arg, arg);
  sb_set(r->dir, dir);
  int pair[2];
  const int mem = memfd_create("appctl", MFD_CLOEXEC);
  void *map = MAP_FAILED;
  if (mem < 0 || ftruncate(mem, sizeof(appctl_shm)) != 0 ||
      (map = mmap(NULL, sizeof(appctl_shm), PROT_READ | PROT_WRITE, MAP_SHARED, mem, 0)) == MAP_FAILED ||
      socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair) != 0) {
    if (map != MAP_FAILED)
      munmap(map, sizeof(appctl_shm));
    if (mem >= 0)
      close(mem);
    return NULL;
  }
  /* Built before fork: the child of a threaded process should not malloc */
  char **const base = app_environ != NULL ? app_environ : environ;
  size_t n = 0;
  while (base[n] != NULL)
    n++;
  char **env = malloc((n + 2) * sizeof(*env));
  char channel[64];
  snprintf(channel, sizeof(channel), APPCTL_ENV "=%d,%d", pair[1], mem);
  if (env != NULL) {
    memcpy(env, base, n * sizeof(*env));
    env[n] = channel;
    env[n + 1] = NULL;
  }
  const pid_t parent = getpid();
  const pid_t pid = env != NULL ? fork() : -1;
  if (pid == 0) {
    setsid();
    /* Without the cgroup the app runs as it is, unlimited. Moved while it's still ours: the
     * cgroups are shell's. */
    const int cg = open(APPS_CGROUP, O_WRONLY | O_CLOEXEC);
    if (cg >= 0) {
      (void)!write(cg, "0", 1);
      close(cg);
    }
    /* The player's, and none of our capabilities: with no root uid on either side, setresuid
     * keeps them */
    if (app_groups >= 0 &&
        (setgroups((size_t)app_groups, app_group_list) != 0 || setresgid(app_gid, app_gid, app_gid) != 0 ||
         setresuid(app_uid, app_uid, app_uid) != 0 || !set_caps(0)))
      _exit(126);
    /* Gone with the launcher, even if it crashes. After the switch, which clears it. */
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    if (getppid() != parent)
      _exit(1);
    /* The app's ends survive exec */
    fcntl(pair[1], F_SETFD, 0);
    fcntl(mem, F_SETFD, 0);
    if (r->dir.n > 0 && chdir(r->dir.p) != 0)
      _exit(127);
    execle("/bin/sh", "sh", "-c", r->exec.p, "sh", r->arg.p, (char *)NULL, env);
    _exit(127);
  }
  free(env);
  close(pair[1]);
  close(mem);
  if (pid < 0) {
    close(pair[0]);
    munmap(map, sizeof(appctl_shm));
    return NULL;
  }
  r->pid = pid;
  r->ctl = pair[0];
  r->shm = map;
  spawn("exit", exit_thread, (void *)(intptr_t)pid);
  return r;
}

running *run_find(pid_t pid) {
  for (size_t i = 0; i < RUNNING_MAX; i++)
    if (runs[i].pid != 0 && runs[i].pid == pid)
      return &runs[i];
  return NULL;
}

running *run_same(str exec, str arg, str dir) {
  for (size_t i = 0; i < RUNNING_MAX; i++)
    if (runs[i].pid != 0 && !runs[i].quitting && str_eq(sb_str(runs[i].exec), exec) &&
        str_eq(sb_str(runs[i].arg), arg) && str_eq(sb_str(runs[i].dir), dir))
      return &runs[i];
  return NULL;
}

size_t run_count(void) {
  size_t n = 0;
  for (size_t i = 0; i < RUNNING_MAX; i++)
    n += runs[i].pid != 0;
  return n;
}

bool run_controlled(const running *r) {
  return r->shm != NULL && r->shm->magic == APPCTL_MAGIC && r->shm->version == APPCTL_VERSION;
}

int run_request(running *r, uint32_t op, int32_t arg) {
  appctl_msg m = {.op = op, .seq = ++seq, .arg = arg};
  if (send(r->ctl, &m, sizeof(m), MSG_NOSIGNAL) != sizeof(m))
    return -EPIPE;
  const uint64_t until = ms_now() + REQUEST_MS;
  for (;;) {
    const uint64_t now = ms_now();
    struct pollfd p = {.fd = r->ctl, .events = POLLIN};
    if (now >= until || poll(&p, 1, (int)(until - now)) == 0)
      return -ETIMEDOUT;
    appctl_msg answer;
    const ssize_t got = recv(r->ctl, &answer, sizeof(answer), MSG_DONTWAIT);
    if (got == 0 || (got < 0 && errno != EAGAIN && errno != EINTR))
      return -EPIPE;
    /* An answer to an earlier request that timed out */
    if (got == sizeof(answer) && answer.seq == m.seq)
      return answer.result;
  }
}

void run_pause(running *r) {
  /* setsid made it a group leader: signals go to all it started */
  if (!run_controlled(r) || run_request(r, APPCTL_PAUSE, 0) != 0)
    kill(-r->pid, SIGSTOP);
}

void run_resume(running *r, bool raise) {
  /* Continued either way: a stop is cheap to undo and may be what happened */
  kill(-r->pid, SIGCONT);
  if (run_controlled(r))
    run_request(r, APPCTL_RESUME, raise);
}

/* Ours (win.h) and SDL's turn SIGTERM into a clean exit (corehost and mGBA
 * write their saves) */
void run_quit(running *r) {
  r->quitting = true;
  kill(-r->pid, SIGTERM);
  kill(-r->pid, SIGCONT);
}

bool run_forget(running *r, str *why) {
  static STRBUF(96) text;
  int status = 0;
  sb_set(text, S(""));
  /* Exited already: the pidfd said so */
  if (waitpid(r->pid, &status, 0) == r->pid) {
    if (WIFSIGNALED(status))
      sb_fmt(text, "killed by signal %d (%s)", WTERMSIG(status), strsignal(WTERMSIG(status)));
    else if (WIFEXITED(status) && WEXITSTATUS(status) == 127)
      sb_set(text, S("exited with status 127 (command not found)"));
    else if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
      sb_fmt(text, "exited with status %d", WEXITSTATUS(status));
  }
  *why = sb_str(text);
  if (text.n > 0)
    fprintf(stderr, "%s (%d) %s%s\n", r->label.p, r->pid, text.p, r->quitting ? ", after Quit" : "");
  /* Whatever an app does on SIGTERM is fine */
  const bool ok = text.n == 0 || r->quitting;
  close(r->ctl);
  munmap(r->shm, sizeof(appctl_shm));
  *r = (running){0};
  return ok;
}

void run_quit_all(void) {
  for (size_t i = 0; i < RUNNING_MAX; i++)
    if (runs[i].pid != 0)
      run_quit(&runs[i]);
}
