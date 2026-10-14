#include "perf.h"

#include <glob.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "apps.h"
#include "power.h"
#include "util.h"

/* As saved, and as the rows say */
static const str names[PERF_METRICS] = {S("fps"), S("cpu"), S("gpu"), S("vpu"), S("mem"), S("sd"), S("temp")};
const str perf_labels[PERF_METRICS] = {S("Frame rate"), S("CPU"),      S("GPU"),        S("Video decoder"),
                                       S("Memory"),     S("SD cards"), S("Temperature")};

static const str corners[PERF_CORNERS] = {S("top-left"), S("top-right"), S("bottom-left"), S("bottom-right")};
const str perf_corner_labels[PERF_CORNERS] = {S("Top left"), S("Top right"), S("Bottom left"), S("Bottom right")};

static bool on;
static unsigned shown = (1u << PERF_METRICS) - 1;
static int corner;
static char gpu_freq[128], gpu_busy[128], vpu_busy[128];

/* Since the last sample */
static struct {
  uint64_t ms;
  uint32_t frames;
  unsigned long long busy, total;
  unsigned long long read, written; /* sectors */
  long long gpu_ns, vpu_ns;
} last;

void perf_init(void) {
  str text = str_read(config_file("-perf", "r")), rest = text;
  rest = str_cut(&rest, '\n');
  if (text.n > 0) {
    on = str_eq(str_cut(&rest, ' '), S("on"));
    shown = 0;
    for (str w; rest.n > 0;) {
      w = str_trim(str_cut(&rest, ' '));
      for (int i = 0; i < PERF_METRICS; i++)
        if (str_eq(w, names[i]))
          shown |= 1u << i;
      for (int i = 0; i < PERF_CORNERS; i++)
        if (str_eq(w, corners[i]))
          corner = i;
    }
  }
  free((char *)text.p);
  glob_t g;
  if (glob("/sys/class/devfreq/*.gpu/cur_freq", 0, NULL, &g) == 0) {
    snprintf(gpu_freq, sizeof(gpu_freq), "%s", g.gl_pathv[0]);
    /* Kernel patch 0058: time busy, in ns */
    snprintf(gpu_busy, sizeof(gpu_busy), "%.*s/device/busy_ns", (int)(strrchr(g.gl_pathv[0], '/') - g.gl_pathv[0]),
             g.gl_pathv[0]);
    globfree(&g);
  }
  /* Kernel patch 0059 */
  if (glob("/sys/bus/platform/devices/*.video-codec/busy_ns", 0, NULL, &g) == 0) {
    snprintf(vpu_busy, sizeof(vpu_busy), "%s", g.gl_pathv[0]);
    globfree(&g);
  }
}

bool perf_on(void) { return on; }
bool perf_shows(int metric) { return shown & 1u << metric; }

int perf_corner(void) { return corner; }

static void save(void) {
  FILE *f = config_file("-perf", "w");
  if (f == NULL)
    return;
  fputs(on ? "on" : "off", f);
  for (int i = 0; i < PERF_METRICS; i++)
    if (perf_shows(i))
      str_fput(f, S(" "), names[i]);
  str_fput(f, S(" "), corners[corner], S("\n"));
  fclose(f);
}

void perf_toggle(int metric) {
  if (metric < 0)
    on = !on;
  else
    shown ^= 1u << metric;
  save();
}

void perf_step_corner(int by) {
  corner = (corner + by + PERF_CORNERS) % PERF_CORNERS;
  save();
}

static long long read_ll(const char *path) {
  long long v = -1;
  FILE *f = fopen(path, "r");
  if (f != NULL) {
    if (fscanf(f, "%lld", &v) != 1)
      v = -1;
    fclose(f);
  }
  return v;
}

/* Everything but idle and iowait is busy */
static void cpu_times(unsigned long long *busy, unsigned long long *total) {
  unsigned long long t[8] = {0};
  FILE *f = fopen("/proc/stat", "r");
  if (f != NULL) {
    if (fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &t[0], &t[1], &t[2], &t[3], &t[4], &t[5], &t[6],
               &t[7]) != 8)
      memset(t, 0, sizeof(t));
    fclose(f);
  }
  *total = 0;
  for (int i = 0; i < 8; i++)
    *total += t[i];
  *busy = *total - t[3] - t[4];
}

/* The whole MMC disks (mmcblkN, not their partitions) */
static void sd_sectors(unsigned long long *read, unsigned long long *written) {
  *read = *written = 0;
  FILE *f = fopen("/proc/diskstats", "r");
  if (f == NULL)
    return;
  char line[256], name[32];
  unsigned long long r, w;
  while (fgets(line, sizeof(line), f))
    if (sscanf(line, "%*u %*u %31s %*u %*u %llu %*u %*u %*u %llu", name, &r, &w) == 3 &&
        strncmp(name, "mmcblk", 6) == 0 && strchr(name + 6, 'p') == NULL) {
      *read += r;
      *written += w;
    }
  fclose(f);
}

/* MemTotal - MemAvailable, and MemTotal, in MiB */
static void memory(long long *used, long long *total) {
  long long kb, avail = 0;
  char key[32];
  *total = 0;
  FILE *f = fopen("/proc/meminfo", "r");
  if (f == NULL)
    return;
  while (fscanf(f, "%31s %lld kB\n", key, &kb) == 2)
    if (strcmp(key, "MemTotal:") == 0)
      *total = kb;
    else if (strcmp(key, "MemAvailable:") == 0)
      avail = kb;
  fclose(f);
  *used = (*total - avail) / 1024;
  *total /= 1024;
}

static long long hottest(void) {
  long long best = -1;
  glob_t g;
  if (glob("/sys/class/thermal/thermal_zone*/temp", 0, NULL, &g) != 0)
    return -1;
  for (size_t i = 0; i < g.gl_pathc; i++)
    best = MAX(best, read_ll(g.gl_pathv[i]));
  globfree(&g);
  return best;
}

size_t perf_sample(str lines[PERF_METRICS]) {
  static STRBUF(48) text[PERF_METRICS];
  const uint64_t now = ms_now();
  const double secs = last.ms > 0 && now > last.ms ? (double)(now - last.ms) / 1000 : 0;
  uint32_t f = last.frames;
  const bool counted = power_frames(&f);
  unsigned long long busy, total, read, written;
  cpu_times(&busy, &total);
  sd_sectors(&read, &written);
  const long long gpu_ns = read_ll(gpu_busy), vpu_ns = read_ll(vpu_busy);
  size_t n = 0;
  for (int i = 0; i < PERF_METRICS; i++) {
    if (!perf_shows(i))
      continue;
    text[i].n = 0;
    switch (i) {
    case PERF_FPS:
      if (counted && secs > 0)
        sb_fmt(text[i], "%.0f fps", (f - last.frames) / secs);
      else
        sb_fmt(text[i], "- fps");
      break;
    case PERF_CPU:
      sb_fmt(text[i], "CPU %3.0f%%  %lld MHz", total > last.total ? 100.0 * (busy - last.busy) / (total - last.total) : 0.0,
             read_ll("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq") / 1000);
      break;
    case PERF_GPU:
      if (gpu_ns >= 0 && last.gpu_ns >= 0 && secs > 0)
        sb_fmt(text[i], "GPU %3.0f%%  ", MIN(100.0, (gpu_ns - last.gpu_ns) / 1e7 / secs));
      else
        sb_fmt(text[i], "GPU ");
      sb_fmt(text[i], "%lld MHz", read_ll(gpu_freq) / 1000000);
      break;
    case PERF_VPU:
      if (vpu_ns >= 0 && last.vpu_ns >= 0 && secs > 0)
        sb_fmt(text[i], "VPU %3.0f%%", MIN(100.0, (vpu_ns - last.vpu_ns) / 1e7 / secs));
      else
        sb_fmt(text[i], "VPU -");
      break;
    case PERF_MEM: {
      long long used = 0, all = 0;
      memory(&used, &all);
      sb_fmt(text[i], "RAM %lld / %lld MB", used, all);
      break;
    }
    /* Sectors are 512 bytes */
    case PERF_SD:
      sb_fmt(text[i], "SD  r %.1f  w %.1f MB/s", secs > 0 ? (read - last.read) / 2048.0 / secs : 0.0,
             secs > 0 ? (written - last.written) / 2048.0 / secs : 0.0);
      break;
    case PERF_TEMP: sb_fmt(text[i], "%lld °C", hottest() / 1000); break;
    }
    lines[n++] = sb_str(text[i]);
  }
  last.ms = now;
  last.frames = f;
  last.busy = busy;
  last.total = total;
  last.read = read;
  last.written = written;
  last.gpu_ns = gpu_ns;
  last.vpu_ns = vpu_ns;
  return n;
}
