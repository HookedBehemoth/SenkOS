#include "updates.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "util.h"
#include "wake.h"
#include "win.h"

#define SOCKET "/run/downloadd.sock"

update_state updates;
pthread_mutex_t updates_lock = PTHREAD_MUTEX_INITIALIZER;
static int fd = -1;

str updates_reason(str why) {
  const size_t scheme = str_find(why, ':');
  if (scheme + 2 < why.n && why.p[scheme + 1] == '/' && why.p[scheme + 2] == '/')
    for (size_t i = scheme + 3; i + 1 < why.n; i++)
      if (why.p[i] == ':' && why.p[i + 1] == ' ')
        return str_at(why, i + 2);
  return why;
}

str updates_build(void) {
  static STRBUF(32) build;
  if (build.n == 0) {
    char line[32] = "";
    FILE *f = fopen("/etc/build", "r");
    if (f != NULL && fgets(line, sizeof(line), f) == NULL)
      line[0] = '\0';
    if (f != NULL)
      fclose(f);
    line[strcspn(line, "\n")] = '\0';
    sb_set(build, line[0] ? str_c(line) : S("unknown"));
  }
  return sb_str(build);
}

/* "os available BUILD SIZE NOTES | os current | os failed WHY" and "job os STATE DONE TOTAL [MESSAGE]" */
static void line(str l) {
  const str what = str_cut(&l, ' ');
  if (str_eq(what, S("os"))) {
    const str word = str_cut(&l, ' ');
    if (str_eq(word, S("available"))) {
      updates.check = UPDATE_AVAILABLE;
      sb_set(updates.build, str_cut(&l, ' '));
      updates.size = strtoull(str_cut(&l, ' ').p, NULL, 10);
      sb_set(updates.notes, l);
    } else if (str_eq(word, S("current"))) {
      updates.check = UPDATE_CURRENT;
    } else {
      updates.check = UPDATE_FAILED;
      sb_set(updates.notes, l);
    }
    return;
  }
  if (!str_eq(what, S("job")))
    return;
  const str id = str_cut(&l, ' ');
  if (id.n > 5 && memcmp(id.p, "pack:", 5) == 0) {
    sb_set(updates.pack_news, str_cut(&l, ' '));
    str_cut(&l, ' ');
    str_cut(&l, ' ');
    sb_set(updates.pack_why, l);
  } else if (str_eq(id, S("os"))) {
    const str state = str_cut(&l, ' ');
    sb_set(updates.job_state, state);
    updates.done = strtoull(str_cut(&l, ' ').p, NULL, 10);
    updates.total = strtoull(str_cut(&l, ' ').p, NULL, 10);
    sb_set(updates.job_message, l);
    updates.job = str_eq(state, S("done"))                                            ? UPDATE_READY
                  : str_eq(state, S("failed")) || str_eq(state, S("canceled")) ? UPDATE_IDLE
                                                                                     : UPDATE_RUNNING;
  }
}

static void ask(const char *request, size_t n);

/* Under updates_lock */
static void ask_pack(void) {
  STRBUF(128) request;
  sb_set(request, S("pack "), sb_str(updates.pack), S("\n"));
  ask(request.p, request.n);
  updates.pack.n = 0;
}

static void *thread(void *arg) {
  (void)arg;
  for (;;) {
    const int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un addr = {.sun_family = AF_UNIX, .sun_path = SOCKET};
    if (s < 0 || connect(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
      if (s >= 0)
        close(s);
      sleep(3);
      continue;
    }
    pthread_mutex_lock(&updates_lock);
    fd = s;
    updates.connected = true;
    /* Asked before it was there */
    if (updates.check == UPDATE_CHECKING && write(fd, "check\n", 6) != 6)
      updates.check = UPDATE_FAILED;
    if (updates.pack.n > 0)
      ask_pack();
    pthread_mutex_unlock(&updates_lock);
    win_wake(UI_UPDATES, 0);
    char buf[2048];
    size_t n = 0;
    for (ssize_t got; (got = read(s, buf + n, sizeof(buf) - n)) > 0;) {
      n += (size_t)got;
      pthread_mutex_lock(&updates_lock);
      for (char *end; (end = memchr(buf, '\n', n)) != NULL;) {
        line((str){buf, (size_t)(end - buf)});
        n -= (size_t)(end + 1 - buf);
        memmove(buf, end + 1, n);
      }
      pthread_mutex_unlock(&updates_lock);
      if (n == sizeof(buf))
        n = 0;
      win_wake(UI_UPDATES, 0);
    }
    pthread_mutex_lock(&updates_lock);
    close(s);
    fd = -1;
    updates.connected = false;
    pthread_mutex_unlock(&updates_lock);
    win_wake(UI_UPDATES, 0);
    sleep(1);
  }
  return NULL;
}

static void start(void) {
  static bool started;
  if (!started)
    started = spawn("updates", thread, NULL);
}

static void ask(const char *request, size_t n) {
  if (fd >= 0 && write(fd, request, n) != (ssize_t)n)
    fprintf(stderr, "launcher: downloadd: %s\n", strerror(errno));
}

void updates_check(void) {
  start();
  pthread_mutex_lock(&updates_lock);
  updates.check = UPDATE_CHECKING;
  ask("check\n", 6);
  pthread_mutex_unlock(&updates_lock);
}

void updates_install(void) {
  start();
  pthread_mutex_lock(&updates_lock);
  updates.job = UPDATE_RUNNING;
  sb_set(updates.job_state, S("queued"));
  ask("update\n", 7);
  pthread_mutex_unlock(&updates_lock);
}

void updates_pack(str name) {
  start();
  pthread_mutex_lock(&updates_lock);
  sb_set(updates.pack, name);
  if (fd >= 0)
    ask_pack();
  pthread_mutex_unlock(&updates_lock);
}
