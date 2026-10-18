/* The daemon: org.senkos.Updates1 on the system bus, the jobs queued, one process at a time */
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "downloadd.h"
#include "updates1.h"

#define PLAYER "player"
#define FAILED_ERROR "org.senkos.Updates1.Error.Failed"

enum { PACK, OS, CHECK };
enum { QUEUED, DOWNLOADING, INSTALLING, DONE, FAILED, CANCELED };
static const str state_words[] = {S("queued"), S("downloading"), S("installing"), S("done"), S("failed"),
                                  S("canceled")};

enum { TASKS_MAX = 64, WAITING_MAX = 4 };
typedef struct {
  STRBUF(96) id;
  STRBUF(96) arg; /* the job's: a pack's name */
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
/* The Check calls the running check answers */
static bus_req waiting[WAITING_MAX];
static size_t waiting_count;
static bus_conn bus;
static bool up;
static char self[PATH_MAX];
static uid_t player_uid;
static gid_t player_gid;

static void changed(const task *t) {
  if (up)
    updates1_emit_job(&bus, sb_str(t->id), state_words[t->state], t->done, t->total, sb_str(t->message));
}

/* The job's process: a pack's as the player, the system's as root */
static bool start(task *t) {
  int p[2];
  if (pipe2(p, O_CLOEXEC) != 0)
    return false;
  static const char *const verbs[] = {[PACK] = "pack", [OS] = "os", [CHECK] = "check"};
  char *argv[4] = {self, (char *)verbs[t->kind], t->kind == PACK ? t->arg.p : NULL, NULL};
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
    t->state = DOWNLOADING;
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
static bool queue(str id, int kind, str arg) {
  task *t = find(id);
  if (t != NULL && (t->state == QUEUED || t->pid > 0)) {
    changed(t);
    return true;
  }
  for (size_t i = 0; t == NULL && i < TASKS_MAX; i++)
    if (tasks[i].id.n == 0 || (tasks[i].state >= DONE && tasks[i].pid == 0))
      t = &tasks[i];
  if (t == NULL)
    return false;
  *t = (task){.kind = kind, .state = QUEUED, .out = -1};
  sb_set(t->id, id);
  sb_set(t->arg, arg);
  changed(t);
  next();
  return true;
}

static int queued(bus_req *req, bool ok) {
  return ok ? bus_reply(req) : bus_reply_error(req, S(FAILED_ERROR), S("too many jobs"));
}

int updates1_pack(bus_req *req, void *userdata, str name) {
  (void)userdata;
  if (!safe_pack(name))
    return bus_reply_error(req, S(BUS_ERROR_INVALID_ARGS), S("Pack: not a pack's name"));
  STRBUF(96) id;
  sb_set(id, S("pack:"), name);
  for (size_t i = 0; i < id.n; i++)
    if (id.p[i] == ' ')
      id.p[i] = '_';
  return queued(req, queue(sb_str(id), PACK, name));
}

int updates1_update(bus_req *req, void *userdata) {
  (void)userdata;
  return queued(req, queue(S("os"), OS, S("")));
}

int updates1_cancel(bus_req *req, void *userdata, str id) {
  (void)userdata;
  task *t = find(id);
  if (t != NULL && t->pid > 0)
    kill(t->pid, SIGTERM);
  else if (t != NULL && t->state == QUEUED) {
    t->state = CANCELED;
    changed(t);
  }
  return bus_reply(req);
}

int updates1_check(bus_req *req, void *userdata) {
  (void)userdata;
  if (waiting_count == WAITING_MAX)
    return bus_reply_error(req, S(FAILED_ERROR), S("checking already"));
  checking.kind = CHECK;
  if (checking.pid == 0 && !start(&checking))
    return bus_reply_error(req, S(FAILED_ERROR), S("can't check"));
  waiting[waiting_count++] = *req;
  return 0;
}

static void put_jobs(bus_wr *w, const void *ctx) {
  (void)ctx;
  const bus_wr_arr a = bus_wr_array(w, 8);
  for (size_t i = 0; i < TASKS_MAX; i++) {
    const task *t = &tasks[i];
    if (t->id.n == 0)
      continue;
    bus_wr_align(w, 8);
    bus_wr_str(w, sb_str(t->id));
    bus_wr_str(w, state_words[t->state]);
    bus_wr_u64(w, t->done);
    bus_wr_u64(w, t->total);
    bus_wr_str(w, sb_str(t->message));
  }
  bus_wr_array_end(w, a);
}

int updates1_jobs(bus_req *req, void *userdata) {
  (void)userdata;
  return updates1_jobs_reply(req, put_jobs, NULL);
}

/* The check's answer: "available BUILD SIZE NOTES", "current" or "error WHY" */
static void answer(str line) {
  const str word = str_cut(&line, ' ');
  str notes = line;
  const str build = str_cut(&notes, ' ');
  const uint64_t size = strtoull(str_cut(&notes, ' ').p, NULL, 10);
  for (size_t i = 0; i < waiting_count && up; i++) {
    if (str_eq(word, S("available")))
      updates1_check_reply(&waiting[i], true, build, size, notes);
    else if (str_eq(word, S("current")))
      updates1_check_reply(&waiting[i], false, S(""), 0, S(""));
    else
      bus_reply_error(&waiting[i], S(FAILED_ERROR), str_eq(word, S("error")) ? line : S("no answer"));
  }
  waiting_count = 0;
}

/* A line from a job's process */
static void report(task *t, str line) {
  if (t == &checking) {
    answer(line);
    return;
  }
  const str word = str_cut(&line, ' ');
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
  if (t == &checking) {
    if (waiting_count > 0)
      answer(S("error stopped"));
    return;
  }
  const bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
  t->state = ok ? DONE : WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM ? CANCELED : FAILED;
  if (t->state == FAILED && t->message.n == 0)
    sb_set(t->message, S("stopped"));
  if (ok && t->kind == OS)
    sb_set(t->message, S("restart to finish"));
  changed(t);
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

/* On the system bus as org.senkos.Updates1, or not */
static bool bus_up(void) {
  if (bus_open(&bus, BUS_SYSTEM) < 0)
    return false;
  updates1_serve(&bus, NULL);
  if (bus_request_name(&bus, S(UPDATES1_NAME)) < 0) {
    bus_close(&bus);
    return false;
  }
  fprintf(stderr, "downloadd: on the system bus\n");
  return true;
}

int daemon_main(void) {
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

  for (;;) {
    /* The bus again a second after it went (init starts it again); the jobs go on meanwhile */
    if (!up)
      up = bus_up();
    struct pollfd fds[3] = {{.fd = up ? bus_fd(&bus) : -1, .events = up ? bus_events(&bus) : 0}};
    size_t count = 1;
    task *running = busy();
    if (running != NULL)
      fds[count++] = (struct pollfd){.fd = running->out, .events = POLLIN};
    if (checking.pid > 0)
      fds[count++] = (struct pollfd){.fd = checking.out, .events = POLLIN};
    if (poll(fds, count, up ? -1 : 1000) < 0)
      continue;
    for (size_t i = 1; i < count; i++)
      if (fds[i].revents)
        read_job(running != NULL && fds[i].fd == running->out ? running : &checking);
    if (up && fds[0].revents) {
      const int r = bus_process(&bus);
      if (r < 0) {
        fprintf(stderr, "downloadd: system bus: %s\n", strerror(-r));
        bus_close(&bus);
        up = false;
        waiting_count = 0;
      }
    }
  }
}
