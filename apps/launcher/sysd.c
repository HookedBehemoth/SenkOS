#include "sysd.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "wake.h"
#include "win.h"

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static STRBUF(128) refused;

typedef struct {
  int fd;
  STRBUF(64) request;
} pending;

static void *wait_reply(void *arg) {
  pending *p = arg;
  char reply[96];
  const ssize_t n = recv(p->fd, reply, sizeof(reply), 0);
  const str r = {reply, n > 0 ? (size_t)n : 0};
  if (!str_eq(r, S("ok"))) {
    str why = n > 0 ? r : S("no reply");
    if (str_eq(str_head(why, 4), S("err ")))
      why = str_at(why, 4);
    str_fput(stderr, S("launcher: sysd: "), sb_str(p->request), S(": "), why, S("\n"));
    pthread_mutex_lock(&lock);
    sb_set(refused, sb_str(p->request), S(": "), why);
    pthread_mutex_unlock(&lock);
    win_wake(UI_REFUSED, 0);
  } else
    win_wake(UI_DONE, 0);
  close(p->fd);
  free(p);
  return NULL;
}

bool sysd(str request) {
  const int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  const struct sockaddr_un addr = {.sun_family = AF_UNIX, .sun_path = "/run/sysd.sock"};
  pending *p = malloc(sizeof(*p));
  pthread_t thread;
  if (fd < 0 || p == NULL || connect(fd, (const struct sockaddr *)&addr, sizeof(addr)) != 0 ||
      send(fd, request.p, request.n, MSG_NOSIGNAL) != (ssize_t)request.n) {
    if (fd >= 0)
      close(fd);
    free(p);
    return false;
  }
  p->fd = fd;
  sb_set(p->request, request);
  if (pthread_create(&thread, NULL, wait_reply, p) != 0) {
    close(fd);
    free(p);
  } else
    pthread_detach(thread);
  return true;
}

str sysd_refused(char *buf, size_t size) {
  pthread_mutex_lock(&lock);
  const size_t n = str_copy(buf, size, sb_str(refused));
  pthread_mutex_unlock(&lock);
  return (str){buf, n};
}
