#include "power.h"

#include <fcntl.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "apps.h"
#include "running.h"
#include "sysd.h"
#include "util.h"

enum { STEPS_MAX = 16, FPS_MIN = 30, FPS_MAX = 60, FPS_STEP = 5 };
/* Calibration: after a clock change, then seconds measured at each */
enum { SETTLE_MS = 1000, FIRST_SETTLE_MS = 2500, SECONDS = 5 };

/* Ascending, as the kernel lists them: CPU in kHz, GPU in Hz */
static long cpu_steps[STEPS_MAX], gpu_steps[STEPS_MAX];
static int cpu_count, gpu_count;
static long brightness, brightness_max;

/* The app on screen: its key, and its choices; a clock is an index into the
 * steps + 1, 0 for Auto */
static STRBUF(512) app_key;
static int app_cpu, app_gpu, app_fps = FPS_MAX;

static int read_steps(const char *pattern, long *out) {
  glob_t g;
  int n = 0;
  if (glob(pattern, 0, NULL, &g) != 0)
    return 0;
  FILE *f = fopen(g.gl_pathv[0], "r");
  globfree(&g);
  if (f == NULL)
    return 0;
  while (n < STEPS_MAX && fscanf(f, "%ld", &out[n]) == 1)
    n++;
  fclose(f);
  return n;
}

static long read_long(const char *pattern) {
  glob_t g;
  long v = -1;
  if (glob(pattern, 0, NULL, &g) != 0)
    return -1;
  FILE *f = fopen(g.gl_pathv[0], "r");
  globfree(&g);
  if (f != NULL) {
    if (fscanf(f, "%ld", &v) != 1)
      v = -1;
    fclose(f);
  }
  return v;
}

/* To sysd: "WHAT VALUE" */
static void request(const char *what, long value) {
  char line[64];
  sysd((str){line, (size_t)snprintf(line, sizeof(line), "%s %ld", what, value)});
}

/* Index + 1 into the steps; 0: the CPU's highest, the GPU free (sysd) */
static void set_clocks(int cpu, int gpu) {
  if (cpu_count > 0)
    request("cpu", cpu_steps[cpu > 0 ? cpu - 1 : cpu_count - 1]);
  if (gpu_count > 0)
    request("gpu", gpu > 0 ? gpu_steps[gpu - 1] : 0);
}

static int step_of(const long *steps, int count, long value) {
  for (int i = 0; i < count; i++)
    if (steps[i] == value)
      return i + 1;
  return 0;
}

/* The app's line: "CPU_KHZ GPU_HZ FPS KEY" */
static void load(str key) {
  app_cpu = app_gpu = 0;
  app_fps = FPS_MAX;
  str text = str_read(config_file("-power", "r")), rest = text;
  while (rest.n > 0) {
    const str line = str_cut(&rest, '\n');
    char head[64];
    long cpu, gpu, fps;
    int used;
    str_copy(head, sizeof(head), line);
    if (sscanf(head, "%ld %ld %ld %n", &cpu, &gpu, &fps, &used) == 3 && str_eq(str_at(line, (size_t)used), key)) {
      app_cpu = step_of(cpu_steps, cpu_count, cpu);
      app_gpu = step_of(gpu_steps, gpu_count, gpu);
      app_fps = CLAMP((int)fps, FPS_MIN, FPS_MAX);
    }
  }
  free((char *)text.p);
}

/* The others' lines as they were, then the app's */
static void save(void) {
  if (app_key.n == 0)
    return;
  str text = str_read(config_file("-power", "r")), rest = text;
  FILE *f = config_file("-power", "w");
  if (f != NULL) {
    while (rest.n > 0) {
      const str line = str_cut(&rest, '\n');
      char head[64];
      int used = -1;
      str_copy(head, sizeof(head), line);
      if (sscanf(head, "%*ld %*ld %*ld %n", &used) == 0 && used >= 0 && str_eq(str_at(line, (size_t)used), sb_str(app_key)))
        continue;
      if (line.n > 0)
        str_fput(f, line, S("\n"));
    }
    fprintf(f, "%ld %ld %d ", app_cpu > 0 ? cpu_steps[app_cpu - 1] : 0, app_gpu > 0 ? gpu_steps[app_gpu - 1] : 0,
            app_fps);
    str_fput(f, sb_str(app_key), S("\n"));
    fclose(f);
  }
  free((char *)text.p);
}

void power_apply(void) {
  if (brightness_max > 0 && brightness > 0)
    request("brightness", brightness);
  set_clocks(app_cpu, app_gpu);
}

void power_init(void) {
  cpu_count = read_steps("/sys/devices/system/cpu/cpu0/cpufreq/scaling_available_frequencies", cpu_steps);
  gpu_count = read_steps("/sys/class/devfreq/*.gpu/available_frequencies", gpu_steps);
  brightness_max = read_long("/sys/class/backlight/*/max_brightness");
  brightness = read_long("/sys/class/backlight/*/brightness");
  /* boot.cmd clamps it, so set the saved level */
  FILE *f = fopen("/uboot/brightness.txt", "r");
  long saved;
  if (f != NULL) {
    if (fscanf(f, "brightness=%ld", &saved) == 1 && brightness_max > 0)
      request("brightness", brightness = CLAMP(saved, 1, brightness_max));
    fclose(f);
  }
  set_clocks(0, 0);
}

static struct {
  bool on;
  int stage; /* 0: at the highest, 1: the CPU going down, 2: the GPU */
  int cpu, gpu; /* trying, indices + 1 */
  bool measuring;
  uint64_t at; /* the next step */
  uint64_t last;
  uint32_t frames;
  int seconds;
  double worst;
} cal;

static int frames_fd = -1;

static bool read_frames(uint32_t *out) {
  if (frames_fd < 0) {
    /* Weston's, in the apps' runtime dir */
    const char *dir = run_app_env("XDG_RUNTIME_DIR");
    char path[256];
    snprintf(path, sizeof(path), "%s/frames", dir ? dir : "/tmp");
    frames_fd = open(path, O_RDONLY | O_CLOEXEC);
  }
  return frames_fd >= 0 && pread(frames_fd, out, sizeof(*out), 0) == sizeof(*out);
}

void power_use(str key) {
  cal.on = false;
  sb_set(app_key, key);
  if (key.n > 0)
    load(key);
  else
    app_cpu = app_gpu = 0;
  set_clocks(app_cpu, app_gpu);
}

str power_value(int row) {
  static STRBUF(32) out;
  out.n = 0;
  switch (row) {
  case POWER_CALIBRATE: sb_fmt(out, "%d fps", app_fps); break;
  case POWER_CPU:
    if (app_cpu == 0)
      return S("Auto");
    sb_fmt(out, "%ld MHz", cpu_steps[app_cpu - 1] / 1000);
    break;
  case POWER_GPU:
    if (app_gpu == 0)
      return S("Auto");
    sb_fmt(out, "%ld MHz", gpu_steps[app_gpu - 1] / 1000000);
    break;
  case POWER_BRIGHTNESS:
    if (brightness_max <= 0)
      return S("None");
    sb_fmt(out, "%ld%%", (brightness * 100 + brightness_max / 2) / brightness_max);
    break;
  }
  return sb_str(out);
}

void power_step(int row, int by) {
  switch (row) {
  case POWER_CALIBRATE: app_fps = CLAMP(app_fps + by * FPS_STEP, FPS_MIN, FPS_MAX); break;
  case POWER_CPU: app_cpu = (app_cpu + by + cpu_count + 1) % (cpu_count + 1); break;
  case POWER_GPU: app_gpu = (app_gpu + by + gpu_count + 1) % (gpu_count + 1); break;
  case POWER_BRIGHTNESS: {
    if (brightness_max <= 0)
      return;
    static uint64_t last;
    static int run;
    const uint64_t now = ms_now();
    run = now - last < 150 ? run + 1 : 0;
    last = now;
    const long steps = 100;
    const long step = (brightness * steps + brightness_max / 2) / brightness_max;
    brightness = brightness_max * CLAMP(step + by * (1 << MIN(run / 8, 3)), 1, steps) / steps;
    request("brightness", brightness); /* saved by save-settings, at shutdown and suspend */
    return;
  }
  }
  save();
  if (row != POWER_CALIBRATE)
    set_clocks(app_cpu, app_gpu);
}

void power_calibrate(void) {
  uint32_t frames;
  if (app_key.n == 0 || (cpu_count == 0 && gpu_count == 0) || !read_frames(&frames))
    return;
  cal = (typeof(cal)){.on = true, .cpu = cpu_count, .gpu = gpu_count, .at = ms_now() + FIRST_SETTLE_MS};
  set_clocks(cal.cpu, cal.gpu);
}

bool power_calibrating(void) { return cal.on; }

void power_cancel(void) {
  if (!cal.on)
    return;
  cal.on = false;
  set_clocks(app_cpu, app_gpu);
}

/* "Testing CPU 1104 · GPU 648 MHz": what calibration has set */
static str clocks_text(str what) {
  static STRBUF(64) text;
  text.n = 0;
  sb_add(text, what);
  if (cpu_count > 0)
    sb_fmt(text, " CPU %ld", cpu_steps[(cal.cpu > 0 ? cal.cpu : cpu_count) - 1] / 1000);
  if (gpu_count > 0)
    sb_fmt(text, "%s GPU %ld", cpu_count > 0 ? " ·" : "", gpu_steps[(cal.gpu > 0 ? cal.gpu : gpu_count) - 1] / 1000000);
  sb_add(text, S(" MHz"));
  return sb_str(text);
}

str power_progress(void) { return clocks_text(S("Testing")); }

/* Done: what held, kept as if chosen */
static void finish(str *say) {
  cal.on = false;
  app_cpu = cal.cpu;
  app_gpu = cal.gpu;
  save();
  set_clocks(app_cpu, app_gpu);
  *say = clocks_text(S("Set"));
}

/* Down the same clock while it holds (one that didn't: back up), then the
 * next; stage 3: done */
static void next_step(bool held) {
  if (!held && cal.stage == 1)
    cal.cpu++;
  if (!held && cal.stage == 2)
    cal.gpu++;
  if (cal.stage == 0 || !held || (cal.stage == 1 ? cal.cpu : cal.gpu) <= 1)
    cal.stage++;
  if (cal.stage == 1 && cal.cpu <= 1)
    cal.stage = 2;
  if (cal.stage == 2 && cal.gpu <= 1)
    cal.stage = 3;
  if (cal.stage == 3)
    return;
  if (cal.stage == 1)
    cal.cpu--;
  else
    cal.gpu--;
  set_clocks(cal.cpu, cal.gpu);
}

int power_tick(uint64_t now, str *say) {
  if (!cal.on)
    return -1;
  if (now < cal.at)
    return (int)(cal.at - now);
  uint32_t frames;
  if (!read_frames(&frames)) {
    power_cancel();
    return -1;
  }
  if (!cal.measuring) {
    cal.measuring = true;
    cal.seconds = 0;
    cal.worst = 1e9;
  } else {
    cal.worst = MIN(cal.worst, (double)(frames - cal.frames) * 1000 / (double)(now - cal.last));
    cal.seconds++;
  }
  cal.frames = frames;
  cal.last = now;
  if (cal.seconds < SECONDS) {
    cal.at = now + 1000;
    return 1000;
  }
  cal.measuring = false;
  /* Every second of five at the rate, a frame short at most (vsync's count
   * wobbles by one). ponytail: one scene's load; a heavier one later can
   * still drop below it, and then the clocks are a step up by hand. */
  const bool held = cal.worst >= app_fps - 1.5;
  if (cal.stage == 0 && !held) {
    static STRBUF(64) text;
    text.n = 0;
    sb_fmt(text, "Can't hold %d fps", app_fps);
    *say = sb_str(text);
    power_cancel();
    return -1;
  }
  next_step(held);
  if (cal.stage == 3) {
    finish(say);
    return -1;
  }
  *say = power_progress();
  cal.at = now + SETTLE_MS;
  return SETTLE_MS;
}
