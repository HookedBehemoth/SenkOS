/* What every file wants and libc lacks */
#pragma once

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#undef MIN
#undef MAX
#undef CLAMP
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CLAMP(x, lo, hi) MIN(MAX(x, lo), hi)
#define COUNTOF(a) (sizeof(a) / sizeof((a)[0]))

/* Monotonic milliseconds */
static inline uint64_t ms_now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* A detached thread, named (15 characters at most) for top and gdb */
static inline bool spawn(const char *name, void *(*fn)(void *), void *arg) {
  pthread_t t;
  if (pthread_create(&t, NULL, fn, arg) != 0)
    return false;
  pthread_setname_np(t, name);
  pthread_detach(t);
  return true;
}
