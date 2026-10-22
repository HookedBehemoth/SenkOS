/* Launcher control FIFO and remote launch requests. */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "launcher_internal.h"
#include "util.h"
#include "wake.h"

static char ctl_fifo[256];
static pthread_mutex_t ctl_lock = PTHREAD_MUTEX_INITIALIZER;
static STRBUF(512) ctl_path;

static void *ctl_thread(void *arg) {
  (void)arg;
  /* Read-write: never at EOF when a writer leaves. Not a FILE: exit() would
   * wait for its lock, held by a read that never returns. */
  const int fd = open(ctl_fifo, O_RDWR | O_CLOEXEC);
  char line[512];
  size_t n = 0;
  for (ssize_t got;
       fd >= 0 && (got = read(fd, line + n, sizeof(line) - n)) > 0;) {
    n += (size_t)got;
    for (char *end; (end = memchr(line, '\n', n)) != NULL;) {
      *end = '\0';
      if (strcmp(line, "reload") == 0) {
        win_wake(UI_RELOAD, 0);
      } else {
        pthread_mutex_lock(&ctl_lock);
        sb_set(ctl_path, str_c(line));
        pthread_mutex_unlock(&ctl_lock);
        win_wake(UI_LAUNCH, 0);
      }
      n -= (size_t)(end + 1 - line);
      memmove(line, end + 1, n);
    }
    if (n == sizeof(line))
      n = 0;
  }
  return NULL;
}

/* /run/launcher, which weston.init makes (shell's; the player may write to it
 * too: launch, reload), else $XDG_RUNTIME_DIR/launcher (on a desktop) */
#define CTL_FIFO "/run/launcher"
static bool ctl_name(void) {
  const char *run = getenv("XDG_RUNTIME_DIR");
  if (access(CTL_FIFO, F_OK) == 0)
    snprintf(ctl_fifo, sizeof(ctl_fifo), "%s", CTL_FIFO);
  else if (run != NULL)
    snprintf(ctl_fifo, sizeof(ctl_fifo), "%s/launcher", run);
  return run != NULL || ctl_fifo[0] != '\0';
}

bool control_open(void) {
  return ctl_name() && (mkfifo(ctl_fifo, 0600) == 0 || errno == EEXIST) &&
         spawn("ctl", ctl_thread, NULL);
}

void control_launch_path(nav *ui) {
  STRBUF(512) path;
  pthread_mutex_lock(&ctl_lock);
  sb_set(path, sb_str(ctl_path));
  pthread_mutex_unlock(&ctl_lock);
  if (launcher.front != NULL ||
      screen_current(launcher.visible) != &home_screen) {
    fprintf(stderr, "launcher: launch %s: not at home\n", path.p);
    return;
  }
  /* What's loaded, then each category read again: pages are read when shown */
  for (int pass = 0; pass < 2; pass++)
    for (size_t i = 0; i < category_count; i++) {
      if (pass == 1)
        apps_scan(&categories[i]);
      for (size_t j = 0; j < categories[i].count; j++) {
        const app *a = &categories[i].apps[j];
        if (a->exec.n > 0 && (str_eq(sb_str(a->arg), sb_str(path)) ||
                              str_eq(sb_str(a->dir), sb_str(path)))) {
          session_start(ui, sb_str(a->name), sb_str(a->icon), sb_str(a->exec),
                        sb_str(a->arg), sb_str(a->dir), category_system(&categories[i]),
                        a->flags);
          return;
        }
      }
    }
  fprintf(stderr, "launcher: launch %s: no such app\n", path.p);
}

int control_request_launch(const char *arg) {
  char path[PATH_MAX];
  if (realpath(arg, path) == NULL || !ctl_name()) {
    fprintf(stderr, "launcher: %s: %s\n", arg,
            ctl_fifo[0] == '\0' ? "no XDG_RUNTIME_DIR" : strerror(errno));
    return 1;
  }
  const int fd = open(ctl_fifo, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0 || dprintf(fd, "%s\n", path) < 0) {
    fprintf(stderr, "launcher: no launcher running (%s)\n", ctl_fifo);
    return 1;
  }
  close(fd);
  return 0;
}
