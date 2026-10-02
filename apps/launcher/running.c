#include "running.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/pidfd.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "wake.h"

/* weston.init's cgroup for apps: memory and CPU limits that keep the UI usable */
#define APPS_CGROUP "/sys/fs/cgroup/session/apps/cgroup.procs"
/* Long enough to write a save state to the card */
#define REQUEST_MS 3000

running runs[RUNNING_MAX];
static uint32_t seq;

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

extern char **environ;

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
  size_t n = 0;
  while (environ[n] != NULL)
    n++;
  char **env = malloc((n + 2) * sizeof(*env));
  char channel[64];
  snprintf(channel, sizeof(channel), APPCTL_ENV "=%d,%d", pair[1], mem);
  if (env != NULL) {
    memcpy(env, environ, n * sizeof(*env));
    env[n] = channel;
    env[n + 1] = NULL;
  }
  const pid_t parent = getpid();
  const pid_t pid = env != NULL ? fork() : -1;
  if (pid == 0) {
    setsid();
    /* Gone with the launcher, even if it crashes */
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    if (getppid() != parent)
      _exit(1);
    /* Without the cgroup the app runs as it is, unlimited */
    const int cg = open(APPS_CGROUP, O_WRONLY | O_CLOEXEC);
    if (cg >= 0) {
      (void)!write(cg, "0", 1);
      close(cg);
    }
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
  /* v4 only appends option slots. v3 clients (e.g. the image's media player)
   * still share the same messages and the first 64 slots in our larger memfd. */
  return r->shm != NULL && r->shm->magic == APPCTL_MAGIC &&
         (r->shm->version == APPCTL_VERSION || r->shm->version == 3);
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
