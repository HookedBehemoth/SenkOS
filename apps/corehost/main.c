/*
 * corehost: runs one libretro core with one game, and nothing else. No menu,
 * no overlay: the launcher is the UI and drives it through appctl.h (pause,
 * save states, options). Without the launcher it just plays.
 *
 *   corehost CORE.so ROM
 *
 * Video through Vulkan: a core's frame is copied into a host-visible buffer,
 * from there into an image in its own pixel format, and blitted into the
 * swapchain (handheld cores are small). A core that renders with GLES (N64,
 * Dreamcast, PSP) gets an EGL context and a framebuffer to draw into,
 * blitted to the window. A Vulkan core gets it the way RetroArch offers it:
 * it makes its device on our instance and surface, renders into its own
 * image and hands that over; we blit that instead. Cores that can do both
 * are offered GLES unless $COREHOST_RENDER=vulkan. Paced as RetroArch
 * paces by default: vsync, with the sound resampled to fit (audio_batch_).
 * Input from the built-in pad (pad.h) or the keyboard (win.h).
 *
 * Every game button and the hotkeys (fast forward, a toggle; rewind, held;
 * save and load state 0) are bindings: a button combo each, which the
 * launcher records. The hotkeys default to Menu + a shoulder button.
 *
 * BIOS files and the like: next to the ROM, or in $COREHOST_SYSTEM.
 * Saves: in $COREHOST_SAVE/CORE, else next to the ROM: STEM.srm (save RAM),
 * STEM.state0 and what the core saves on its own (PPSSPP's memory stick).
 * Without a STEM.srm there, the save RAM next to the ROM is read (STEM.srm,
 * else STEM.sav: another emulator's), never written.
 * Options are kept in $XDG_CONFIG_HOME/corehost/CORE.opt, over the image's
 * defaults in /usr/share/corehost/CORE.opt (the N64's bindings).
 *
 * SIGTERM, the control socket closing (the launcher went away) and closing
 * the window all quit, after writing the save RAM.
 *
 * Cores that read their options only when the game starts (mupen's GLideN64)
 * are restarted to apply them: back in the game after a changed option,
 * corehost saves a state, runs itself again in place (same pid, same control
 * channel) and loads it.
 */
#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <linux/input.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <alsa/asoundlib.h>
#include <wayland-client.h>
#include <wayland-egl.h>
#include <zlib.h>
/* The context's own functions, from eglGetProcAddress: nothing linked */
#define GL_GLES_PROTOTYPES 0
#include <GLES3/gl3.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_wayland.h>
#include "libretro_vulkan.h"

#include "appctl.h"
#include "libretro.h"
#include "pad.h"
#include "util.h"
#include "win.h"

/* Frames run per frame shown while fast forwarding */
#define FAST_SPEED 3

static struct {
#define SYM(name) __typeof__(name) *name
  SYM(retro_set_environment);
  SYM(retro_set_video_refresh);
  SYM(retro_set_audio_sample);
  SYM(retro_set_audio_sample_batch);
  SYM(retro_set_input_poll);
  SYM(retro_set_input_state);
  SYM(retro_init);
  SYM(retro_deinit);
  SYM(retro_get_system_info);
  SYM(retro_get_system_av_info);
  SYM(retro_load_game);
  SYM(retro_unload_game);
  SYM(retro_run);
  SYM(retro_reset);
  SYM(retro_serialize_size);
  SYM(retro_serialize);
  SYM(retro_unserialize);
  SYM(retro_get_memory_data);
  SYM(retro_get_memory_size);
#undef SYM
} core;

static win window;
/* A software core's pixels, as libretro and Vulkan name them */
static VkFormat frame_format = VK_FORMAT_R5G6B5_UNORM_PACK16;
static unsigned frame_bytes = 2;
static int frame_w, frame_h;
/* The frame's shape on screen, width / height; 0: square pixels */
static float aspect;
static snd_pcm_t *audio;
static int pad_fd = -1;
/* evdev KEY_*, for the keyboard's bindings */
static bool keys[KEY_MAX + 1];
static double fps = 60;
static const char *core_path;

/* SET_HW_RENDER: the core draws into fbo, at most max_w x max_h */
static struct retro_hw_render_callback hw;
static bool hw_on, vk_on;
static EGLDisplay egl_display;
static EGLSurface egl_surface;
static EGLContext gl_context;
static struct wl_egl_window *egl_window;
static GLuint fbo, fbo_color, fbo_depth;
static unsigned max_w, max_h;
static struct {
  PFNGLGENFRAMEBUFFERSPROC GenFramebuffers;
  PFNGLBINDFRAMEBUFFERPROC BindFramebuffer;
  PFNGLGENRENDERBUFFERSPROC GenRenderbuffers;
  PFNGLBINDRENDERBUFFERPROC BindRenderbuffer;
  PFNGLRENDERBUFFERSTORAGEPROC RenderbufferStorage;
  PFNGLFRAMEBUFFERRENDERBUFFERPROC FramebufferRenderbuffer;
  PFNGLBLITFRAMEBUFFERPROC BlitFramebuffer;
  PFNGLCLEARCOLORPROC ClearColor;
  PFNGLCLEARPROC Clear;
  PFNGLCOLORMASKPROC ColorMask;
  PFNGLDISABLEPROC Disable;
  PFNGLENABLEPROC Enable;
  PFNGLISENABLEDPROC IsEnabled;
  PFNGLGETINTEGERVPROC GetIntegerv;
  PFNGLGETBOOLEANVPROC GetBooleanv;
  PFNGLSCISSORPROC Scissor;
} gl;

static int ctl = -1, ctl_mem = -1;
static appctl_shm *shm;
static bool options_changed;
/* Restarted for options: the state to go on from, in this slot */
#define RESTART_SLOT 99
static char **args;
static bool startup_options; /* the core reads its options only at load */
static uint8_t started_with[APPCTL_OPTIONS_MAX];
static bool restart;
/* stem: the saves' path but for the extension; rom_stem: the ROM's */
static char opt_path[512], stem[512], rom_stem[512], save_dir[512], default_opt_path[512];
/* This game's own options (Y in the launcher: their own is 1), in
 * CORE/ROM.opt beside CORE.opt; the core's value and combo for each are kept
 * to go back to. own: as last seen. */
static char game_opt_path[1100];
static bool own[APPCTL_OPTIONS_MAX];
static uint8_t core_value[APPCTL_OPTIONS_MAX];
static uint16_t core_combo[APPCTL_OPTIONS_MAX];
/* Rewind's ring, a whole state a slot; no slots: no rewind */
static size_t state_size, rewind_slots, rewind_head, rewind_count;
static uint8_t *sram_shadow;
static size_t sram_shadow_size;

/* Bindings: a combo of pad buttons each (appctl.h), recorded by the launcher.
 * The hotkeys, then the game's buttons and each direction of its sticks:
 * those the core describes (GBA: no X, Y, L2, R2; N64: one stick, no
 * second), under its names for them. A row is one of the
 * console's inputs, the combo is what presses it here. The keyboard's keys
 * are fixed, it is for testing. */
#define B(name) (1u << APPCTL_##name)
static const struct {
  const char *key, *label;
  uint16_t buttons;
  int id; /* libretro's joypad id; ANALOG_ID(..) for a stick; -1: a hotkey */
  unsigned keyboard; /* KEY_* */
} bindings[] = {
    {"corehost_hotkey_fast", "Fast forward (toggle)", B(MENU) | B(R1), -1, 0},
    {"corehost_hotkey_rewind", "Rewind (hold)", B(MENU) | B(L1), -1, 0},
    {"corehost_hotkey_save", "Save state", B(MENU) | B(R2), -1, 0},
    {"corehost_hotkey_load", "Load state", B(MENU) | B(L2), -1, 0},
    {"corehost_map_up", "Up", B(UP), RETRO_DEVICE_ID_JOYPAD_UP, KEY_UP},
    {"corehost_map_down", "Down", B(DOWN), RETRO_DEVICE_ID_JOYPAD_DOWN, KEY_DOWN},
    {"corehost_map_left", "Left", B(LEFT), RETRO_DEVICE_ID_JOYPAD_LEFT, KEY_LEFT},
    {"corehost_map_right", "Right", B(RIGHT), RETRO_DEVICE_ID_JOYPAD_RIGHT, KEY_RIGHT},
    {"corehost_map_a", "Button A", B(A), RETRO_DEVICE_ID_JOYPAD_A, KEY_X},
    {"corehost_map_b", "Button B", B(B), RETRO_DEVICE_ID_JOYPAD_B, KEY_Z},
    {"corehost_map_x", "Button X", B(X), RETRO_DEVICE_ID_JOYPAD_X, KEY_S},
    {"corehost_map_y", "Button Y", B(Y), RETRO_DEVICE_ID_JOYPAD_Y, KEY_A},
    {"corehost_map_l", "Button L", B(L1), RETRO_DEVICE_ID_JOYPAD_L, KEY_Q},
    {"corehost_map_r", "Button R", B(R1), RETRO_DEVICE_ID_JOYPAD_R, KEY_W},
    {"corehost_map_l2", "Button L2", B(L2), RETRO_DEVICE_ID_JOYPAD_L2, KEY_1},
    {"corehost_map_r2", "Button R2", B(R2), RETRO_DEVICE_ID_JOYPAD_R2, KEY_2},
    {"corehost_map_select", "Select", B(SELECT), RETRO_DEVICE_ID_JOYPAD_SELECT, KEY_RIGHTSHIFT},
    {"corehost_map_start", "Start", B(START), RETRO_DEVICE_ID_JOYPAD_START, KEY_ENTER},
    /* No sticks to click: unbound until the player binds them */
    {"corehost_map_l3", "Button L3", 0, RETRO_DEVICE_ID_JOYPAD_L3, KEY_3},
    {"corehost_map_r3", "Button R3", 0, RETRO_DEVICE_ID_JOYPAD_R3, KEY_4},
/* Half an axis each: stick (left, right), axis (x, y), direction (-, +) */
#define ANALOG_ID(stick, axis, plus) (16 + (stick) * 4 + (axis) * 2 + (plus))
    {"corehost_map_lstick_left", "Left stick left", 0, ANALOG_ID(0, 0, 0), 0},
    {"corehost_map_lstick_right", "Left stick right", 0, ANALOG_ID(0, 0, 1), 0},
    {"corehost_map_lstick_up", "Left stick up", 0, ANALOG_ID(0, 1, 0), 0},
    {"corehost_map_lstick_down", "Left stick down", 0, ANALOG_ID(0, 1, 1), 0},
    {"corehost_map_rstick_left", "Right stick left", 0, ANALOG_ID(1, 0, 0), 0},
    {"corehost_map_rstick_right", "Right stick right", 0, ANALOG_ID(1, 0, 1), 0},
    {"corehost_map_rstick_up", "Right stick up", 0, ANALOG_ID(1, 1, 0), 0},
    {"corehost_map_rstick_down", "Right stick down", 0, ANALOG_ID(1, 1, 1), 0},
};
enum { HOT_FAST, HOT_REWIND, HOT_SAVE, HOT_LOAD, HOTKEYS, BINDINGS = COUNTOF(bindings) };
enum { ANALOG = BINDINGS - 8 }; /* the first stick row */
_Static_assert(BINDINGS <= 32, "active is a bit per binding");

/* Our own options, after the core's: these choices, then the bindings */
enum { HOST_SCALE, HOST_FILTER, HOST_FPS, HOST_CHOICES };
static const struct {
  const char *key, *label, *values;
} host_options[HOST_CHOICES] = {
    {"corehost_scale", "Scaling", "Integer|Fit|Stretch"},
    {"corehost_filter", "Filter", "Sharp|Smooth"},
    {"corehost_fps", "Show FPS", "Off|On"},
};
static unsigned host_option_at; /* index of the first one in shm */
/* Published once the game is loaded: the input descriptors are in by then */
static bool host_published;
static int binding_at[BINDINGS]; /* index in shm; -1: not shown, never pressed */
/* SET_INPUT_DESCRIPTORS: the ids (joypad, ANALOG_ID) the core uses (none
 * told: all), and its names for them */
static uint32_t described;
static char described_as[ANALOG_ID(2, 0, 0)][APPCTL_TEXT];
/* The ids in the order told, the rows' order */
static uint8_t described_order[ANALOG_ID(2, 0, 0)];
static int described_count;
/* Told after the rows were made (mupen: once its emulation thread starts):
 * the main loop makes them again */
static _Atomic bool redescribed;

/* Warnings and errors: cores log at debug level freely */
static void logf_(enum retro_log_level level, const char *fmt, ...) {
  if (level < RETRO_LOG_WARN)
    return;
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
}

static appctl_option *option(const char *key) {
  const uint32_t n = atomic_load(&shm->option_count);
  for (uint32_t i = 0; i < n; i++)
    if (strcmp(shm->options[i].key, key) == 0)
      return &shm->options[i];
  return NULL;
}

static unsigned host_value(int which) {
  const unsigned at = host_option_at + which;
  if (at >= atomic_load(&shm->option_count) ||
      strcmp(shm->options[at].key, host_options[which].key) != 0)
    return 0;
  return atomic_load(&shm->options[at].value);
}

static uint16_t binding(int which) {
  return binding_at[which] < 0 ? 0 : atomic_load(&shm->options[binding_at[which]].buttons);
}

/* The next slot, filled in by the caller and then published; NULL when full */
static appctl_option *new_option(const char *key, const char *label, int category) {
  const uint32_t n = atomic_load(&shm->option_count);
  if (n >= APPCTL_OPTIONS_MAX)
    return NULL;
  appctl_option *o = &shm->options[n];
  snprintf(o->key, sizeof(o->key), "%s", key);
  snprintf(o->label, sizeof(o->label), "%s", label);
  o->count = 0;
  o->kind = APPCTL_CHOICE;
  o->category = category;
  atomic_store(&o->value, 0);
  atomic_store(&o->own, 0);
  atomic_store(&o->buttons, 0);
  return o;
}

static void publish(void) { atomic_fetch_add(&shm->option_count, 1); }

/* "Label; first|second|third", as SET_VARIABLES has it; the first is the
 * default */
static void add_option(const char *key, const char *desc, int category) {
  const char *semi = strchr(desc, ';');
  char label[APPCTL_TEXT];
  snprintf(label, sizeof(label), "%.*s", semi ? (int)(semi - desc) : (int)strlen(desc), desc);
  appctl_option *o = new_option(key, label, category);
  if (o == NULL)
    return;
  const char *v = semi ? semi + 1 : "";
  v += strspn(v, " ");
  while (*v && o->count < APPCTL_VALUES) {
    const size_t len = strcspn(v, "|");
    snprintf(o->values[o->count++], APPCTL_TEXT, "%.*s", (int)len, v);
    v += len + (v[len] == '|');
  }
  publish();
}

/* The libretro categories, as the pause menu has them */
static int category_of(const char *key) {
  if (key == NULL)
    return APPCTL_SYSTEM;
  if (strcmp(key, "video") == 0)
    return APPCTL_GRAPHICS;
  if (strcmp(key, "audio") == 0)
    return APPCTL_AUDIO;
  if (strcmp(key, "input") == 0)
    return APPCTL_INPUT;
  return APPCTL_SYSTEM; /* system, performance, the rest */
}

/* Core options v1 and v2: the values, the default and (v2) a category */
static void add_definition(const char *key, const char *desc, const char *category,
                           const struct retro_core_option_value *values, const char *def) {
  appctl_option *o = new_option(key, desc, category_of(category));
  if (o == NULL)
    return;
  for (; values->value != NULL && o->count < APPCTL_VALUES; values++) {
    if (def != NULL && strcmp(values->value, def) == 0)
      atomic_store(&o->value, o->count);
    snprintf(o->values[o->count++], APPCTL_TEXT, "%s", values->value);
  }
  publish();
}

/* Scaling and filter, then the bindings */
static void publish_host(void) {
  host_option_at = atomic_load(&shm->option_count);
  for (int i = 0; i < HOST_CHOICES; i++) {
    char desc[128];
    snprintf(desc, sizeof(desc), "%s; %s", host_options[i].label, host_options[i].values);
    add_option(host_options[i].key, desc, APPCTL_GRAPHICS);
  }
  /* A pad without sticks: the core's left stick gets the D-pad, its D-pad
   * Menu + the D-pad */
  const uint32_t lstick = 0xfu << ANALOG_ID(0, 0, 0);
  const bool stick_on_dpad = (described & lstick) == lstick;
  /* The game's buttons first, in the core's order (none told: ours), our
   * hotkeys last */
  int order[BINDINGS], n_order = 0;
  for (int k = 0; k < described_count; k++)
    for (int i = HOTKEYS; i < BINDINGS; i++)
      if (bindings[i].id == described_order[k])
        order[n_order++] = i;
  for (int i = HOTKEYS; described_count == 0 && i < BINDINGS; i++)
    order[n_order++] = i;
  for (int i = 0; i < HOTKEYS; i++)
    order[n_order++] = i;
  for (int i = 0; i < BINDINGS; i++)
    binding_at[i] = -1;
  for (int n = 0; n < n_order; n++) {
    const int i = order[n];
    const int id = bindings[i].id;
    if ((id >= 0 && described != 0 && !(described >> id & 1)) ||
        (i == HOT_REWIND && rewind_slots == 0))
      continue;
    appctl_option *o = new_option(bindings[i].key,
                                  id >= 0 && described_as[id][0] ? described_as[id] : bindings[i].label,
                                  APPCTL_INPUT);
    if (o == NULL)
      continue;
    o->kind = APPCTL_BINDING;
    static const uint16_t dpad[4] = {B(LEFT), B(RIGHT), B(UP), B(DOWN)};
    uint16_t def = bindings[i].buttons;
    if (stick_on_dpad && id >= ANALOG_ID(0, 0, 0) && id < ANALOG_ID(1, 0, 0))
      def = dpad[id - ANALOG_ID(0, 0, 0)];
    else if (stick_on_dpad && id >= RETRO_DEVICE_ID_JOYPAD_UP && id <= RETRO_DEVICE_ID_JOYPAD_RIGHT)
      def |= B(MENU);
    atomic_store(&o->buttons, def);
    binding_at[i] = (int)atomic_load(&shm->option_count);
    publish();
  }
  host_published = true;
}

/* "key=value" lines; values that no longer exist are skipped. The game's
 * file: its own. */
static void load_options_from(const char *path, bool game) {
  FILE *f = fopen(path, "r");
  char line[256];
  while (f != NULL && fgets(line, sizeof(line), f)) {
    line[strcspn(line, "\n")] = '\0';
    char *eq = strchr(line, '=');
    if (eq == NULL)
      continue;
    *eq = '\0';
    appctl_option *o = option(line);
    uint16_t combo;
    bool known = false;
    if (o != NULL && o->kind == APPCTL_BINDING && appctl_parse_combo(eq + 1, &combo)) {
      atomic_store(&o->buttons, combo);
      known = true;
    }
    for (unsigned i = 0; o != NULL && o->kind == APPCTL_CHOICE && i < o->count; i++)
      if (strcmp(o->values[i], eq + 1) == 0) {
        atomic_store(&o->value, i);
        known = true;
      }
    if (known && game) {
      atomic_store(&o->own, 1);
      own[o - shm->options] = true;
    }
  }
  if (f != NULL)
    fclose(f);
}

/* The image's defaults, the core's file, then the game's own: before the
 * game loads, so the core starts with them */
static void load_options(void) {
  load_options_from(default_opt_path, false);
  load_options_from(opt_path, false);
  memset(own, 0, sizeof(own));
  for (uint32_t i = 0; i < atomic_load(&shm->option_count); i++) {
    core_combo[i] = atomic_load(&shm->options[i].buttons);
    core_value[i] = atomic_load(&shm->options[i].value);
    atomic_store(&shm->options[i].own, 0);
  }
  load_options_from(game_opt_path, true);
}

/* After the launcher changed something: an option made the game's own keeps
 * the core's value aside, one given back gets it again */
static void sync_own(void) {
  const uint32_t n = atomic_load(&shm->option_count);
  for (uint32_t i = 0; i < n; i++) {
    appctl_option *o = &shm->options[i];
    const bool now = atomic_load(&o->own) != 0;
    if (!now && own[i]) {
      atomic_store(&o->buttons, core_combo[i]);
      atomic_store(&o->value, core_value[i]);
    } else if (!now) {
      core_combo[i] = atomic_load(&o->buttons);
      core_value[i] = atomic_load(&o->value);
    }
    own[i] = now;
  }
}

static void write_option(FILE *f, const appctl_option *o, uint8_t value, uint16_t combo) {
  if (o->kind == APPCTL_BINDING)
    appctl_write_combo(f, o->key, combo);
  else
    fprintf(f, "%s=%s\n", o->key, o->values[value % APPCTL_VALUES]);
}

static void save_options(void) {
  FILE *f = fopen(opt_path, "w");
  if (f == NULL)
    return;
  const uint32_t n = atomic_load(&shm->option_count);
  bool any_own = false;
  for (uint32_t i = 0; i < n; i++) {
    const appctl_option *o = &shm->options[i];
    if (own[i])
      write_option(f, o, core_value[i], core_combo[i]);
    else
      write_option(f, o, atomic_load(&o->value), atomic_load(&o->buttons));
    any_own |= own[i];
  }
  fclose(f);
  if (!any_own) {
    unlink(game_opt_path);
    return;
  }
  if ((f = fopen(game_opt_path, "w")) == NULL)
    return;
  for (uint32_t i = 0; i < n; i++)
    if (own[i])
      write_option(f, &shm->options[i], atomic_load(&shm->options[i].value), atomic_load(&shm->options[i].buttons));
  fclose(f);
}

/* The host's rows anew, with what was saved: the core's file, then the
 * game's own */
static void publish_and_load(void) {
  publish_host();
  load_options();
}

static void sw_frame(const void *data, unsigned w, unsigned h, size_t pitch);

static void video(const void *data, unsigned w, unsigned h, size_t pitch) {
  if (data == NULL) /* a dupe: the last frame stays */
    return;
  if (hw_on) {
    /* In the fbo; anything else from a GL core isn't shown */
    if (data == RETRO_HW_FRAME_BUFFER_VALID) {
      frame_w = (int)w;
      frame_h = (int)h;
    }
    return;
  }
  sw_frame(data, w, h, pitch);
}

/* Fast forward and rewind are silent */
static bool muted;

static int64_t ns_now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

/* RetroArch's audio sync: the display paces the game (vsync, present) and
 * the sound is resampled to fit. A core close to the refresh rate is run at
 * it (GBA's 59.73 fps at 60: 0.45% fast), its samples stretched to match;
 * on top, dynamic rate control nudges the ratio by up to RATE_CONTROL to
 * keep the device's buffer half full, against the two clocks' drift. Then
 * a write finds room and returns at once. A core further off (PAL on 60 Hz)
 * isn't stretched: its writes wait, and the sound paces it. */
#define OUT_RATE 48000 /* PipeWire's (pipewire.conf): one resampling, ours */
#define MAX_SKEW 0.05
#define RATE_CONTROL 0.005
static unsigned swaps = 1; /* presents a frame: 2 for a 30 fps core at 60 Hz */
static double in_rate;     /* the core's samples per second of ours */
static snd_pcm_uframes_t audio_buffer;

/* The core's sound at `step` of its frames per one of ours, linearly;
 * `pos` is where the next one falls past `prev`, the last frame in */
static struct {
  double pos;
  int16_t prev[2];
} rs;

static size_t resample(const int16_t *in, size_t n, double step, int16_t *out, size_t room) {
  size_t k = 0;
  while (rs.pos < n && k < room) {
    const size_t i = (size_t)rs.pos;
    const float t = (float)(rs.pos - i);
    const int16_t *a = i == 0 ? rs.prev : in + (i - 1) * 2, *b = in + i * 2;
    out[k * 2] = (int16_t)(a[0] + (b[0] - a[0]) * t);
    out[k * 2 + 1] = (int16_t)(a[1] + (b[1] - a[1]) * t);
    k++;
    rs.pos += step;
  }
  rs.pos -= (double)n;
  memcpy(rs.prev, in + (n - 1) * 2, sizeof(rs.prev));
  return k;
}

/* Waits only while the device is full: never when stretched, but for drift */
static void audio_batch_(const int16_t *data, size_t frames) {
  if (audio == NULL || muted || frames == 0)
    return;
  /* Below half full: more of ours per frame of the core's */
  const snd_pcm_sframes_t avail = snd_pcm_avail_update(audio);
  const double half = audio_buffer / 2.0;
  const double direction = avail >= 0 && audio_buffer > 0 ? (avail - half) / half : 0;
  const double step = in_rate / OUT_RATE / (1 + RATE_CONTROL * direction);
  static int16_t out[4096 * 2];
  while (frames > 0) {
    /* As much as `out` takes once resampled */
    const size_t chunk = MIN(frames, MAX((size_t)(4000 * step), (size_t)1));
    size_t n = resample(data, chunk, step, out, 4096);
    const int16_t *at = out;
    while (n > 0) {
      const snd_pcm_sframes_t written = snd_pcm_writei(audio, at, n);
      if (written < 0 && snd_pcm_recover(audio, (int)written, 1) < 0)
        break;
      if (written > 0) {
        at += written * 2;
        n -= (size_t)written;
      }
    }
    data += chunk * 2;
    frames -= chunk;
  }
}

static size_t audio_batch(const int16_t *data, size_t frames) {
  audio_batch_(data, frames);
  return frames;
}

static void audio_sample(int16_t left, int16_t right) {
  const int16_t frame[2] = {left, right};
  audio_batch_(frame, 1);
}

/* Held when the launcher gave the game back (Resume is pressed with A):
 * not the game's until let go */
static uint16_t launcher_held;

/* The window's events were dispatched before the frame (main) */
static void input_poll(void) {}

static void key(unsigned code, bool down) {
  if (code <= KEY_MAX)
    keys[code] = down;
}

/* What is held, as appctl bits */
static uint16_t buttons_held(void) {
  const uint16_t held = pad_held(pad_fd);
  launcher_held &= held;
  return held & ~launcher_held;
}

/* The bindings held this frame, a bit each. One inside a bigger one that is
 * held too is not: L1 + A doesn't also press what A is bound to. */
static uint32_t active;

static void update_bindings(void) {
  const uint16_t held = buttons_held();
  uint16_t combo[BINDINGS];
  uint32_t on = 0;
  for (int i = 0; i < BINDINGS; i++) {
    combo[i] = binding(i);
    if (combo[i] != 0 && (held & combo[i]) == combo[i])
      on |= 1u << i;
  }
  active = on;
  for (int i = 0; i < BINDINGS; i++)
    for (int j = 0; j < BINDINGS; j++)
      if ((on >> i & 1) && (on >> j & 1) && combo[j] != combo[i] &&
          (combo[j] & combo[i]) == combo[i])
        active &= ~(1u << i);
}

/* libretro ids */
static int16_t buttons(void) {
  int16_t mask = 0;
  for (int i = HOTKEYS; i < ANALOG; i++)
    if ((active >> i & 1) || keys[bindings[i].keyboard])
      mask |= 1 << bindings[i].id;
  return mask;
}

/* Full tilt; both axes held: the diagonal of a round stick's gate */
static int16_t stick(unsigned index, unsigned axis) {
  const int at = ANALOG + index * 4;
  const int x = (int)(active >> (at + 1) & 1) - (int)(active >> at & 1);
  const int y = (int)(active >> (at + 3) & 1) - (int)(active >> (at + 2) & 1);
  const int v = axis ? y : x;
  return (int16_t)(v * (x && y ? 23170 : 32767));
}

static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id) {
  if (port == 0 && (device & RETRO_DEVICE_MASK) == RETRO_DEVICE_ANALOG &&
      index <= RETRO_DEVICE_INDEX_ANALOG_RIGHT && id <= RETRO_DEVICE_ID_ANALOG_Y)
    return stick(index, id);
  if (port != 0 || (device & RETRO_DEVICE_MASK) != RETRO_DEVICE_JOYPAD)
    return 0;
  const int16_t mask = buttons();
  return id == RETRO_DEVICE_ID_JOYPAD_MASK ? mask : (mask >> id) & 1;
}

/* The overlay: a few lines of text top left, in a dark box over the game
 * (state saved, fast forward, FPS). Drawn as rectangles, the box and runs of
 * a 5x7 font's pixels, which each video path fills its own way. */
static const char font_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.:%/-><";
static const uint8_t font_rows[][7] = {
    {0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}, /* A */
    {0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e}, /* B */
    {0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e}, /* C */
    {0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e}, /* D */
    {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f}, /* E */
    {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10}, /* F */
    {0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0f}, /* G */
    {0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}, /* H */
    {0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e}, /* I */
    {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0c}, /* J */
    {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}, /* K */
    {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f}, /* L */
    {0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11}, /* M */
    {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11}, /* N */
    {0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}, /* O */
    {0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10}, /* P */
    {0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d}, /* Q */
    {0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11}, /* R */
    {0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e}, /* S */
    {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}, /* T */
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}, /* U */
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04}, /* V */
    {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0a}, /* W */
    {0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11}, /* X */
    {0x11, 0x11, 0x0a, 0x04, 0x04, 0x04, 0x04}, /* Y */
    {0x1f, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1f}, /* Z */
    {0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e}, /* 0 */
    {0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e}, /* 1 */
    {0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f}, /* 2 */
    {0x1f, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0e}, /* 3 */
    {0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02}, /* 4 */
    {0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e}, /* 5 */
    {0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e}, /* 6 */
    {0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}, /* 7 */
    {0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e}, /* 8 */
    {0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c}, /* 9 */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x0c}, /* . */
    {0x00, 0x0c, 0x0c, 0x00, 0x0c, 0x0c, 0x00}, /* : */
    {0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03}, /* % */
    {0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x00}, /* / */
    {0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00}, /* - */
    {0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08}, /* > */
    {0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02}, /* < */
};

#define OVERLAY_LINES 4
#define OVERLAY_RECTS 1024
static char overlay[OVERLAY_LINES][32];
static char notice[32];
static uint64_t notice_until;
typedef struct {
  int x, y, w, h;
} rect;
static struct {
  rect r;
  bool text; /* else the box */
} orects[OVERLAY_RECTS];

static void notify(const char *text) {
  snprintf(notice, sizeof(notice), "%s", text);
  notice_until = ms_now() + 2000;
}

/* The overlay's rectangles for a w x h window; 0 when there is nothing */
static int overlay_rects(int w, int h) {
  (void)w;
  const int px = MAX(1, h / 160); /* a font pixel: 3 at 480 lines */
  int lines = 0, cols = 0;
  for (int l = 0; l < OVERLAY_LINES; l++)
    if (overlay[l][0]) {
      lines++;
      cols = MAX(cols, (int)strlen(overlay[l]));
    }
  if (lines == 0)
    return 0;
  const int x0 = px * 4, y0 = px * 4, pad = px * 2;
  int n = 0;
  orects[n].r = (rect){x0, y0, cols * 6 * px - px + pad * 2, lines * 9 * px - 2 * px + pad * 2};
  orects[n++].text = false;
  int y = y0 + pad;
  for (int l = 0; l < OVERLAY_LINES; l++) {
    if (!overlay[l][0])
      continue;
    for (int c = 0; overlay[l][c]; c++) {
      const char *at = strchr(font_chars, toupper((unsigned char)overlay[l][c]));
      if (at == NULL || *at == '\0')
        continue; /* a space, or one we don't have */
      const uint8_t *rows = font_rows[at - font_chars];
      const int x = x0 + pad + c * 6 * px;
      for (int r = 0; r < 7; r++)
        for (int b = 4; b >= 0; b--) {
          if (!(rows[r] >> b & 1))
            continue;
          int len = 1; /* a run of set pixels, one rectangle */
          while (b - len >= 0 && (rows[r] >> (b - len) & 1))
            len++;
          if (n < OVERLAY_RECTS) {
            orects[n].r = (rect){x + (4 - b) * px, y + r * px, len * px, px};
            orects[n++].text = true;
          }
          b -= len - 1;
        }
    }
    y += 9 * px;
  }
  return n;
}

/* The core's frames in flight: a fence, command buffer and acquire
 * semaphore each. What it hands over for a frame is kept until we present. */
#define VK_FRAMES 2
#define VK_MAX 8
static struct {
  const struct retro_hw_render_context_negotiation_interface_vulkan *negotiation;
  struct retro_hw_render_interface_vulkan iface;
  VkInstance instance;
  VkSurfaceKHR surface;
  VkPhysicalDevice gpu;
  VkDevice device;
  VkQueue queue;
  uint32_t family;
  VkSwapchainKHR swapchain;
  VkImage images[VK_MAX];
  VkSemaphore rendered[VK_MAX]; /* per swapchain image: the present waits */
  uint32_t image_count;
  VkExtent2D extent;
  VkFormat format;
  VkPresentModeKHR mode;
  VkCommandPool pool;
  VkCommandBuffer cmd[VK_FRAMES];
  VkFence fence[VK_FRAMES];
  VkSemaphore acquired[VK_FRAMES];
  uint32_t index;
  pthread_mutex_t queue_lock;
  /* From the core for this frame */
  struct retro_vulkan_image image;
  bool have_image;
  uint32_t src_family, wait_count, cmd_count;
  VkSemaphore wait[VK_MAX], signal;
  VkCommandBuffer core_cmd[VK_MAX];
  /* The overlay at a texel a font pixel: drawn into a host-visible buffer
   * per frame in flight, copied into `ovl` and blitted out scaled, two
   * operations however much text (a blit region is a render pass in PanVK) */
  VkImage ovl;
  VkDeviceMemory ovl_memory;
  VkBuffer ovl_staging[VK_FRAMES];
  VkDeviceMemory ovl_staging_memory[VK_FRAMES];
  uint32_t *ovl_map[VK_FRAMES];
  /* A software core's frame: a host-visible buffer per frame in flight,
   * copied into `frame` (GENERAL, in the core's format) by its present */
  VkBuffer staging[VK_FRAMES];
  VkDeviceMemory staging_memory[VK_FRAMES];
  void *staging_map[VK_FRAMES];
  VkDeviceSize staging_size;
  VkImage frame;
  VkDeviceMemory frame_memory;
  uint32_t row_texels;
  bool frame_new, frame_undefined;
  int win_w, win_h; /* the window's size the swapchain was made for */
} vk;

static bool fail(const char *why) {
  fprintf(stderr, "corehost: %s\n", why);
  return false;
}

static void vk_set_image(void *handle, const struct retro_vulkan_image *image, uint32_t n,
                         const VkSemaphore *semaphores, uint32_t src_family) {
  (void)handle;
  vk.image = *image;
  vk.have_image = true;
  vk.src_family = src_family;
  vk.wait_count = MIN(n, VK_MAX);
  memcpy(vk.wait, semaphores, vk.wait_count * sizeof(*semaphores));
}
static uint32_t vk_sync_index(void *handle) { return (void)handle, vk.index; }
static uint32_t vk_sync_mask(void *handle) { return (void)handle, (1u << VK_FRAMES) - 1; }
static void vk_set_command_buffers(void *handle, uint32_t n, const VkCommandBuffer *cmd) {
  (void)handle;
  vk.cmd_count = MIN(n, VK_MAX);
  memcpy(vk.core_cmd, cmd, vk.cmd_count * sizeof(*cmd));
}
static void vk_wait_sync_index(void *handle) {
  (void)handle;
  vkWaitForFences(vk.device, 1, &vk.fence[vk.index], VK_TRUE, UINT64_MAX);
}
static void vk_lock(void *handle) { (void)handle, pthread_mutex_lock(&vk.queue_lock); }
static void vk_unlock(void *handle) { (void)handle, pthread_mutex_unlock(&vk.queue_lock); }
static void vk_set_signal(void *handle, VkSemaphore semaphore) { (void)handle, vk.signal = semaphore; }

static bool vk_swapchain(void) {
  VkSurfaceCapabilitiesKHR caps;
  vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk.gpu, vk.surface, &caps);
  vk.extent = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent
                                                    : (VkExtent2D){(uint32_t)window.w, (uint32_t)window.h};
  vk.win_w = window.w;
  vk.win_h = window.h;
  if (vk.extent.width == 0 || vk.extent.height == 0)
    return false;
  uint32_t count = caps.minImageCount + 1;
  if (caps.maxImageCount != 0)
    count = MIN(count, caps.maxImageCount);
  VkSwapchainKHR old = vk.swapchain;
  const VkSwapchainCreateInfoKHR info = {
      .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
      .surface = vk.surface,
      .minImageCount = MIN(count, VK_MAX),
      .imageFormat = vk.format,
      .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
      .imageExtent = vk.extent,
      .imageArrayLayers = 1,
      .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      .preTransform = caps.currentTransform,
      .compositeAlpha = caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                            ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                            : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
      .presentMode = vk.mode,
      .clipped = VK_TRUE,
      .oldSwapchain = old,
  };
  vkDeviceWaitIdle(vk.device);
  if (vkCreateSwapchainKHR(vk.device, &info, NULL, &vk.swapchain) != VK_SUCCESS)
    return false;
  if (old != VK_NULL_HANDLE)
    vkDestroySwapchainKHR(vk.device, old, NULL);
  vk.image_count = VK_MAX;
  vkGetSwapchainImagesKHR(vk.device, vk.swapchain, &vk.image_count, vk.images);
  return true;
}

/* Instance, the core's device (or ours), swapchain; then its context_reset */
static bool vk_init(void) {
  static const char *const exts[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME};
  const uint32_t n_ext = COUNTOF(exts);
  const VkApplicationInfo default_app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                         .pApplicationName = "corehost",
                                         .apiVersion = VK_API_VERSION_1_1};
  const VkApplicationInfo *app = vk.negotiation && vk.negotiation->get_application_info
                                     ? vk.negotiation->get_application_info()
                                     : NULL;
  /* PPSSPP's negotiation metadata says 1.0, but its context independently
   * queries the loader version and uses promoted functions (including VMA's
   * memory requirements queries). Expose the loader's supported core API on
   * our instance too, or those device function pointers can be NULL. */
  VkApplicationInfo app_info = app ? *app : default_app;
  uint32_t api_version = VK_API_VERSION_1_0;
  PFN_vkEnumerateInstanceVersion enumerate_version =
      (PFN_vkEnumerateInstanceVersion)vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
  if (enumerate_version != NULL && enumerate_version(&api_version) == VK_SUCCESS)
    app_info.apiVersion = MAX(app_info.apiVersion, api_version);
  const VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                    .pApplicationInfo = &app_info,
                                    .enabledExtensionCount = n_ext,
                                    .ppEnabledExtensionNames = exts};
  const VkWaylandSurfaceCreateInfoKHR wsi = {.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
                                             .display = win_display,
                                             .surface = window.surface};
  if (vkCreateInstance(&ici, NULL, &vk.instance) != VK_SUCCESS ||
      vkCreateWaylandSurfaceKHR(vk.instance, &wsi, NULL, &vk.surface) != VK_SUCCESS)
    return fail("no Vulkan instance or surface");

  static const char *const device_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
  struct retro_vulkan_context ctx = {0};
  if (vk.negotiation && vk.negotiation->create_device) {
    const VkPhysicalDeviceFeatures none = {0};
    if (!vk.negotiation->create_device(&ctx, vk.instance, VK_NULL_HANDLE, vk.surface, vkGetInstanceProcAddr,
                                       (const char **)device_exts, 1, NULL, 0, &none))
      return fail("the core made no Vulkan device");
  } else {
    /* Ours: the first GPU's first queue that draws and presents */
    uint32_t n = 1, families = 8;
    VkQueueFamilyProperties qf[8];
    vkEnumeratePhysicalDevices(vk.instance, &n, &ctx.gpu);
    if (n == 0)
      return fail("no Vulkan device");
    vkGetPhysicalDeviceQueueFamilyProperties(ctx.gpu, &families, qf);
    VkBool32 present = VK_FALSE;
    for (ctx.queue_family_index = 0; ctx.queue_family_index < families; ctx.queue_family_index++) {
      vkGetPhysicalDeviceSurfaceSupportKHR(ctx.gpu, ctx.queue_family_index, vk.surface, &present);
      if (present && (qf[ctx.queue_family_index].queueFlags & VK_QUEUE_GRAPHICS_BIT))
        break;
    }
    const float priority = 1;
    const VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                         .queueFamilyIndex = ctx.queue_family_index,
                                         .queueCount = 1,
                                         .pQueuePriorities = &priority};
    const VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                    .queueCreateInfoCount = 1,
                                    .pQueueCreateInfos = &qci,
                                    .enabledExtensionCount = 1,
                                    .ppEnabledExtensionNames = device_exts};
    if (ctx.queue_family_index == families || vkCreateDevice(ctx.gpu, &dci, NULL, &ctx.device) != VK_SUCCESS)
      return fail("no Vulkan queue for the window");
    vkGetDeviceQueue(ctx.device, ctx.queue_family_index, 0, &ctx.queue);
  }
  vk.gpu = ctx.gpu;
  vk.device = ctx.device;
  vk.queue = ctx.queue;
  vk.family = ctx.queue_family_index;

  /* BGRA8 if there is one; mailbox or immediate: a present never waits
   * (the frame callbacks are the pace, main) */
  uint32_t n = VK_MAX;
  VkSurfaceFormatKHR formats[VK_MAX];
  vkGetPhysicalDeviceSurfaceFormatsKHR(vk.gpu, vk.surface, &n, formats);
  vk.format = n ? formats[0].format : VK_FORMAT_B8G8R8A8_UNORM;
  for (uint32_t i = 0; i < n; i++)
    if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM)
      vk.format = formats[i].format;
  VkPresentModeKHR modes[VK_MAX];
  n = VK_MAX;
  vkGetPhysicalDeviceSurfacePresentModesKHR(vk.gpu, vk.surface, &n, modes);
  vk.mode = VK_PRESENT_MODE_FIFO_KHR;
  for (uint32_t i = 0; i < n; i++)
    if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR ||
        (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR && vk.mode == VK_PRESENT_MODE_FIFO_KHR))
      vk.mode = modes[i];
  if (!vk_swapchain())
    return fail("no swapchain");
  /* A software core's format: blitted, and filtered when asked to (both
   * are required of all three) */
  VkFormatProperties fp;
  vkGetPhysicalDeviceFormatProperties(vk.gpu, frame_format, &fp);
  const VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
  if (!hw_on && (fp.optimalTilingFeatures & needed) != needed)
    return fail("the core's pixel format can't be blitted");

  const VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                       .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                       .queueFamilyIndex = vk.family};
  vkCreateCommandPool(vk.device, &pci, NULL, &vk.pool);
  const VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                           .commandPool = vk.pool,
                                           .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                           .commandBufferCount = VK_FRAMES};
  vkAllocateCommandBuffers(vk.device, &cai, vk.cmd);
  const VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                                 .flags = VK_FENCE_CREATE_SIGNALED_BIT};
  const VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  for (int i = 0; i < VK_FRAMES; i++) {
    vkCreateFence(vk.device, &fci, NULL, &vk.fence[i]);
    vkCreateSemaphore(vk.device, &sci, NULL, &vk.acquired[i]);
  }
  for (int i = 0; i < VK_MAX; i++)
    vkCreateSemaphore(vk.device, &sci, NULL, &vk.rendered[i]);
  pthread_mutex_init(&vk.queue_lock, NULL);

  vk.iface = (struct retro_hw_render_interface_vulkan){
      .interface_type = RETRO_HW_RENDER_INTERFACE_VULKAN,
      .interface_version = RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION,
      .instance = vk.instance,
      .gpu = vk.gpu,
      .device = vk.device,
      .get_device_proc_addr = vkGetDeviceProcAddr,
      .get_instance_proc_addr = vkGetInstanceProcAddr,
      .queue = vk.queue,
      .queue_index = vk.family,
      .set_image = vk_set_image,
      .get_sync_index = vk_sync_index,
      .get_sync_index_mask = vk_sync_mask,
      .set_command_buffers = vk_set_command_buffers,
      .wait_sync_index = vk_wait_sync_index,
      .lock_queue = vk_lock,
      .unlock_queue = vk_unlock,
      .set_signal_semaphore = vk_set_signal,
  };
  if (hw.context_reset != NULL)
    hw.context_reset();
  VkPhysicalDeviceProperties properties;
  vkGetPhysicalDeviceProperties(vk.gpu, &properties);
  fprintf(stderr, "corehost: Vulkan: %s, instance API %u.%u, %ux%u\n", properties.deviceName,
          VK_API_VERSION_MAJOR(app_info.apiVersion), VK_API_VERSION_MINOR(app_info.apiVersion),
          vk.extent.width, vk.extent.height);
  return true;
}

static void vk_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                       uint32_t src_family, uint32_t dst_family, const VkImageSubresourceRange *range) {
  const VkImageMemoryBarrier b = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
      .oldLayout = from,
      .newLayout = to,
      .srcQueueFamilyIndex = src_family,
      .dstQueueFamilyIndex = dst_family,
      .image = image,
      .subresourceRange = *range,
  };
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                       NULL, 0, NULL, 1, &b);
}

/* Memory of a type `req` allows, with `props`, bound to it later */
static VkDeviceMemory vk_memory(VkMemoryRequirements req, VkMemoryPropertyFlags props) {
  VkPhysicalDeviceMemoryProperties mp;
  vkGetPhysicalDeviceMemoryProperties(vk.gpu, &mp);
  uint32_t type = 0;
  while (type < mp.memoryTypeCount &&
         !((req.memoryTypeBits >> type & 1) && (mp.memoryTypes[type].propertyFlags & props) == props))
    type++;
  const VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                    .allocationSize = req.size,
                                    .memoryTypeIndex = type};
  VkDeviceMemory memory = VK_NULL_HANDLE;
  if (type < mp.memoryTypeCount)
    vkAllocateMemory(vk.device, &mai, NULL, &memory);
  return memory;
}

/* A software core's frame for w x h, `size` bytes of it: the staging
 * buffers grow, the image is made again */
static bool sw_resize(unsigned w, unsigned h, VkDeviceSize size) {
  vkDeviceWaitIdle(vk.device);
  for (int f = 0; size > vk.staging_size && f < VK_FRAMES; f++) {
    vkDestroyBuffer(vk.device, vk.staging[f], NULL);
    vkFreeMemory(vk.device, vk.staging_memory[f], NULL);
    const VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                    .size = size,
                                    .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    VkMemoryRequirements req;
    vk.staging_memory[f] = VK_NULL_HANDLE;
    if (vkCreateBuffer(vk.device, &bci, NULL, &vk.staging[f]) != VK_SUCCESS)
      return vk.staging[f] = VK_NULL_HANDLE, vk.staging_size = 0, false;
    vkGetBufferMemoryRequirements(vk.device, vk.staging[f], &req);
    vk.staging_memory[f] = vk_memory(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vk.staging_memory[f] == VK_NULL_HANDLE ||
        vkBindBufferMemory(vk.device, vk.staging[f], vk.staging_memory[f], 0) != VK_SUCCESS ||
        vkMapMemory(vk.device, vk.staging_memory[f], 0, size, 0, &vk.staging_map[f]) != VK_SUCCESS)
      return vk.staging_size = 0, false;
    /* All of them are this big once the last is */
    if (f == VK_FRAMES - 1)
      vk.staging_size = size;
  }
  vkDestroyImage(vk.device, vk.frame, NULL);
  vkFreeMemory(vk.device, vk.frame_memory, NULL);
  vk.frame_memory = VK_NULL_HANDLE;
  const VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                 .imageType = VK_IMAGE_TYPE_2D,
                                 .format = frame_format,
                                 .extent = {w, h, 1},
                                 .mipLevels = 1,
                                 .arrayLayers = 1,
                                 .samples = VK_SAMPLE_COUNT_1_BIT,
                                 .tiling = VK_IMAGE_TILING_OPTIMAL,
                                 .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
  VkMemoryRequirements req;
  if (vkCreateImage(vk.device, &ici, NULL, &vk.frame) != VK_SUCCESS)
    return vk.frame = VK_NULL_HANDLE, false;
  vkGetImageMemoryRequirements(vk.device, vk.frame, &req);
  vk.frame_memory = vk_memory(req, 0);
  vk.frame_undefined = true;
  return vk.frame_memory != VK_NULL_HANDLE &&
         vkBindImageMemory(vk.device, vk.frame, vk.frame_memory, 0) == VK_SUCCESS;
}

/* Into the staging buffer of the frame it will be presented with, once the
 * GPU is done with that one; a few times for one present when fast */
static void sw_frame(const void *data, unsigned w, unsigned h, size_t pitch) {
  if (vk.device == VK_NULL_HANDLE || w == 0 || h == 0)
    return;
  const uint32_t f = vk.index;
  vkWaitForFences(vk.device, 1, &vk.fence[f], VK_TRUE, UINT64_MAX);
  /* The last row may end at its pixels */
  const VkDeviceSize size = (VkDeviceSize)pitch * (h - 1) + (VkDeviceSize)w * frame_bytes;
  if (((int)w != frame_w || (int)h != frame_h || size > vk.staging_size || vk.frame_memory == VK_NULL_HANDLE) &&
      !sw_resize(w, h, size)) {
    vk.have_image = false;
    frame_w = frame_h = 0;
    return;
  }
  memcpy(vk.staging_map[f], data, size);
  frame_w = (int)w;
  frame_h = (int)h;
  vk.row_texels = (uint32_t)(pitch / frame_bytes);
  vk.frame_new = true;
  vk.image = (struct retro_vulkan_image){.image_layout = VK_IMAGE_LAYOUT_GENERAL,
                                         .create_info = {.image = vk.frame}};
  vk.src_family = VK_QUEUE_FAMILY_IGNORED;
  vk.have_image = true;
}

/* The overlay's image and staging buffers, made the first time it is needed */
#define OVL_W 256
#define OVL_H 64
static bool vk_overlay(void) {
  if (vk.ovl != VK_NULL_HANDLE)
    return true;
  for (int f = 0; f < VK_FRAMES; f++) {
    if (vk.ovl_map[f] != NULL)
      continue;
    const VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                    .size = OVL_W * OVL_H * 4,
                                    .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    VkMemoryRequirements req;
    if (vk.ovl_staging[f] == VK_NULL_HANDLE && vkCreateBuffer(vk.device, &bci, NULL, &vk.ovl_staging[f]) != VK_SUCCESS)
      return false;
    vkGetBufferMemoryRequirements(vk.device, vk.ovl_staging[f], &req);
    if (vk.ovl_staging_memory[f] == VK_NULL_HANDLE)
      vk.ovl_staging_memory[f] = vk_memory(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    void *map;
    if (vk.ovl_staging_memory[f] == VK_NULL_HANDLE ||
        vkBindBufferMemory(vk.device, vk.ovl_staging[f], vk.ovl_staging_memory[f], 0) != VK_SUCCESS ||
        vkMapMemory(vk.device, vk.ovl_staging_memory[f], 0, VK_WHOLE_SIZE, 0, &map) != VK_SUCCESS)
      return false;
    vk.ovl_map[f] = map;
  }
  const VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                 .imageType = VK_IMAGE_TYPE_2D,
                                 .format = VK_FORMAT_R8G8B8A8_UNORM,
                                 .extent = {OVL_W, OVL_H, 1},
                                 .mipLevels = 1,
                                 .arrayLayers = 1,
                                 .samples = VK_SAMPLE_COUNT_1_BIT,
                                 .tiling = VK_IMAGE_TILING_OPTIMAL,
                                 .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
  VkImage image;
  if (vkCreateImage(vk.device, &ici, NULL, &image) != VK_SUCCESS)
    return false;
  VkMemoryRequirements req;
  vkGetImageMemoryRequirements(vk.device, image, &req);
  vk.ovl_memory = vk_memory(req, 0);
  if (vk.ovl_memory == VK_NULL_HANDLE || vkBindImageMemory(vk.device, image, vk.ovl_memory, 0) != VK_SUCCESS) {
    vkDestroyImage(vk.device, image, NULL);
    vkFreeMemory(vk.device, vk.ovl_memory, NULL);
    vk.ovl_memory = VK_NULL_HANDLE;
    return false;
  }
  vk.ovl = image;
  return true;
}

/* The core's image onto the next swapchain image at x, y, w, h; the core's
 * command buffers and semaphores go with it */
static void vk_present(int x, int y, int w, int h) {
  const uint32_t f = vk.index;
  if ((window.w != vk.win_w || window.h != vk.win_h) && !vk_swapchain())
    return;
  vkWaitForFences(vk.device, 1, &vk.fence[f], VK_TRUE, UINT64_MAX);
  uint32_t at;
  VkResult r = vkAcquireNextImageKHR(vk.device, vk.swapchain, UINT64_MAX, vk.acquired[f], VK_NULL_HANDLE, &at);
  if (r == VK_ERROR_OUT_OF_DATE_KHR) {
    if (!vk_swapchain())
      return;
    r = vkAcquireNextImageKHR(vk.device, vk.swapchain, UINT64_MAX, vk.acquired[f], VK_NULL_HANDLE, &at);
  }
  if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
    return;
  vkResetFences(vk.device, 1, &vk.fence[f]);

  const VkCommandBuffer cmd = vk.cmd[f];
  vkResetCommandBuffer(cmd, 0);
  const VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                          .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
  vkBeginCommandBuffer(cmd, &begin);
  const VkImageSubresourceRange color = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  const VkImage target = vk.images[at];
  vk_barrier(cmd, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
             VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, &color);
  const VkClearColorValue black = {.float32 = {0, 0, 0, 1}};
  vkCmdClearColorImage(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &color);
  /* A software core's new frame, into its image; the blit's barrier below
   * waits for it */
  if (vk.frame_new) {
    vk_barrier(cmd, vk.frame, vk.frame_undefined ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
               VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, &color);
    const VkBufferImageCopy copy = {.bufferRowLength = vk.row_texels,
                                    .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                    .imageExtent = {(uint32_t)frame_w, (uint32_t)frame_h, 1}};
    vkCmdCopyBufferToImage(cmd, vk.staging[f], vk.frame, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
    vk.frame_new = vk.frame_undefined = false;
  }
  if (vk.have_image && frame_w > 0 && frame_h > 0) {
    const VkImage source = vk.image.create_info.image;
    const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1,
                                           vk.image.create_info.subresourceRange.baseArrayLayer, 1};
    /* GENERAL may be read as it is; any other layout is ours until the fence */
    const VkImageLayout layout = vk.image.image_layout;
    const VkImageLayout read = layout == VK_IMAGE_LAYOUT_GENERAL ? layout : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    const bool transfer = vk.src_family != VK_QUEUE_FAMILY_IGNORED && vk.src_family != vk.family;
    vk_barrier(cmd, source, layout, read, transfer ? vk.src_family : VK_QUEUE_FAMILY_IGNORED,
               transfer ? vk.family : VK_QUEUE_FAMILY_IGNORED, &range);
    const VkImageBlit blit = {
        .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, range.baseArrayLayer, 1},
        .srcOffsets = {{0, 0, 0}, {frame_w, frame_h, 1}},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .dstOffsets = {{x, y, 0}, {x + w, y + h, 1}},
    };
    vkCmdBlitImage(cmd, source, read, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                   host_value(HOST_FILTER) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    vk_barrier(cmd, source, read, layout, transfer ? vk.family : VK_QUEUE_FAMILY_IGNORED,
               transfer ? vk.src_family : VK_QUEUE_FAMILY_IGNORED, &range);
  }
  /* Laid out at 160 lines: a texel a font pixel */
  const int n_rects = overlay_rects(160, 160);
  if (n_rects > 0 && vk_overlay()) {
    const rect box = orects[0].r;
    const int bw = MIN(box.w, OVL_W), bh = MIN(box.h, OVL_H);
    uint32_t *pixels = vk.ovl_map[f];
    for (int i = 0; i < n_rects; i++) {
      const rect r = orects[i].r;
      /* RGBA in memory: the box 0.08, 0.08, 0.1; text white */
      const uint32_t c = orects[i].text ? 0xffffffff : 0xff1a1414;
      for (int y = MAX(r.y - box.y, 0); y < MIN(r.y - box.y + r.h, bh); y++)
        for (int x = MAX(r.x - box.x, 0); x < MIN(r.x - box.x + r.w, bw); x++)
          pixels[y * OVL_W + x] = c;
    }
    vk_barrier(cmd, vk.ovl, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, &color);
    const VkBufferImageCopy copy = {.bufferRowLength = OVL_W,
                                    .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                    .imageExtent = {(uint32_t)bw, (uint32_t)bh, 1}};
    vkCmdCopyBufferToImage(cmd, vk.ovl_staging[f], vk.ovl, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    vk_barrier(cmd, vk.ovl, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, &color);
    /* The same scale as GL's: a font pixel 3 at 480 lines */
    const int px = MAX(1, (int)vk.extent.height / 160);
    const VkImageBlit blit = {
        .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .srcOffsets = {{0, 0, 0}, {bw, bh, 1}},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .dstOffsets = {{box.x * px, box.y * px, 0}, {(box.x + bw) * px, (box.y + bh) * px, 1}},
    };
    vkCmdBlitImage(cmd, vk.ovl, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &blit, VK_FILTER_NEAREST);
  }
  vk_barrier(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
             VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, &color);
  vkEndCommandBuffer(cmd);

  /* The core's command buffers first; with them, no waiting on its semaphores */
  VkSemaphore waits[VK_MAX + 1] = {vk.acquired[f]}, signals[2] = {vk.rendered[at], vk.signal};
  VkPipelineStageFlags stages[VK_MAX + 1];
  VkCommandBuffer cmds[VK_MAX + 1];
  uint32_t n_wait = 1, n_cmd = 0;
  if (vk.cmd_count == 0)
    for (uint32_t i = 0; i < vk.wait_count; i++)
      waits[n_wait++] = vk.wait[i];
  for (uint32_t i = 0; i < n_wait; i++)
    stages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
  for (uint32_t i = 0; i < vk.cmd_count; i++)
    cmds[n_cmd++] = vk.core_cmd[i];
  cmds[n_cmd++] = cmd;
  const VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                               .waitSemaphoreCount = n_wait,
                               .pWaitSemaphores = waits,
                               .pWaitDstStageMask = stages,
                               .commandBufferCount = n_cmd,
                               .pCommandBuffers = cmds,
                               .signalSemaphoreCount = vk.signal != VK_NULL_HANDLE ? 2 : 1,
                               .pSignalSemaphores = signals};
  const VkPresentInfoKHR present = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                                    .waitSemaphoreCount = 1,
                                    .pWaitSemaphores = &vk.rendered[at],
                                    .swapchainCount = 1,
                                    .pSwapchains = &vk.swapchain,
                                    .pImageIndices = &at};
  pthread_mutex_lock(&vk.queue_lock);
  vkQueueSubmit(vk.queue, 1, &submit, vk.fence[f]);
  r = vkQueuePresentKHR(vk.queue, &present);
  pthread_mutex_unlock(&vk.queue_lock);
  if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
    vk_swapchain();
  vk.wait_count = vk.cmd_count = 0;
  vk.signal = VK_NULL_HANDLE;
  vk.index = (f + 1) % VK_FRAMES;
}

static uintptr_t hw_framebuffer(void) { return fbo; }

/* RetroArch's own: lets a core's threads out of the frontend's waits on
 * shutdown. Ours never wait inside the core. */
#define GET_CLEAR_ALL_THREAD_WAITS_CB (3 | 0x800000)
static bool clear_thread_waits(unsigned clear, void *data) {
  (void)clear;
  (void)data;
  return true;
}

static retro_proc_address_t hw_proc(const char *sym) {
  return (retro_proc_address_t)eglGetProcAddress(sym);
}

/* The fbo's storage for frames up to w x h; before the context: noted */
static void hw_resize(unsigned w, unsigned h) {
  max_w = MAX(max_w, w);
  max_h = MAX(max_h, h);
  if (gl_context == EGL_NO_CONTEXT)
    return;
  GLint was;
  gl.GetIntegerv(GL_FRAMEBUFFER_BINDING, &was);
  gl.BindFramebuffer(GL_FRAMEBUFFER, fbo);
  gl.BindRenderbuffer(GL_RENDERBUFFER, fbo_color);
  gl.RenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, max_w, max_h);
  gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, fbo_color);
  if (hw.depth || hw.stencil) {
    gl.BindRenderbuffer(GL_RENDERBUFFER, fbo_depth);
    gl.RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, max_w, max_h);
    gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, fbo_depth);
  }
  gl.BindFramebuffer(GL_FRAMEBUFFER, was);
}

/* EGL on the window for SET_HW_RENDER's core, then its context_reset */
static bool hw_init(void) {
  const EGLint major = hw.context_type == RETRO_HW_CONTEXT_OPENGLES_VERSION ? (EGLint)hw.version_major
                       : hw.context_type == RETRO_HW_CONTEXT_OPENGLES3      ? 3
                                                                            : 2;
  const EGLint minor = hw.context_type == RETRO_HW_CONTEXT_OPENGLES_VERSION ? (EGLint)hw.version_minor : 0;
  const EGLint wanted[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                           EGL_RENDERABLE_TYPE, major >= 3 ? EGL_OPENGL_ES3_BIT : EGL_OPENGL_ES2_BIT,
                           EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
  const EGLint context[] = {EGL_CONTEXT_MAJOR_VERSION, major, EGL_CONTEXT_MINOR_VERSION, minor, EGL_NONE};
  EGLConfig configs[64], config;
  EGLint n = 0;
  egl_display = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, win_display, NULL);
  if (egl_display == EGL_NO_DISPLAY || !eglInitialize(egl_display, NULL, NULL) ||
      !eglBindAPI(EGL_OPENGL_ES_API) || !eglChooseConfig(egl_display, wanted, configs, 64, &n) || n == 0)
    return fail("no EGL config");
  /* XRGB8888 exactly, without depth: what the display's planes take, so
   * Weston scans it out instead of compositing it on the GPU every frame
   * (behind the core's frame: missed vblanks). EGL sorts deeper colour
   * first: "at least 8" gets XRGB2101010. The core draws into its own
   * framebuffer. */
  config = configs[0];
  for (EGLint i = n - 1; i >= 0; i--) {
    EGLint r, g, b, alpha, depth;
    eglGetConfigAttrib(egl_display, configs[i], EGL_RED_SIZE, &r);
    eglGetConfigAttrib(egl_display, configs[i], EGL_GREEN_SIZE, &g);
    eglGetConfigAttrib(egl_display, configs[i], EGL_BLUE_SIZE, &b);
    eglGetConfigAttrib(egl_display, configs[i], EGL_ALPHA_SIZE, &alpha);
    eglGetConfigAttrib(egl_display, configs[i], EGL_DEPTH_SIZE, &depth);
    if (r == 8 && g == 8 && b == 8 && alpha == 0 && depth == 0)
      config = configs[i];
  }
  egl_window = wl_egl_window_create(window.surface, window.w, window.h);
  egl_surface = eglCreatePlatformWindowSurface(egl_display, config, egl_window, NULL);
  gl_context = eglCreateContext(egl_display, config, EGL_NO_CONTEXT, context);
  if (egl_surface == EGL_NO_SURFACE || gl_context == EGL_NO_CONTEXT ||
      !eglMakeCurrent(egl_display, egl_surface, egl_surface, gl_context))
    return fail("no GLES context");
  /* A swap never waits: the frame callbacks are the pace (main) */
  eglSwapInterval(egl_display, 0);
#define GL(name) (*(void **)&gl.name = eglGetProcAddress("gl" #name)) != NULL
  if (!(GL(GenFramebuffers) && GL(BindFramebuffer) && GL(GenRenderbuffers) && GL(BindRenderbuffer) &&
        GL(RenderbufferStorage) && GL(FramebufferRenderbuffer) && GL(BlitFramebuffer) &&
        GL(ClearColor) && GL(Clear) && GL(ColorMask) && GL(Disable) && GL(Enable) &&
        GL(IsEnabled) && GL(GetIntegerv) && GL(GetBooleanv) && GL(Scissor)))
    return fail("no GLES 3 functions");
#undef GL
  gl.GenFramebuffers(1, &fbo);
  gl.GenRenderbuffers(1, &fbo_color);
  gl.GenRenderbuffers(1, &fbo_depth);
  hw_resize(max_w, max_h);
  gl.BindFramebuffer(GL_FRAMEBUFFER, fbo);
  if (hw.context_reset != NULL)
    hw.context_reset();
  return true;
}

/* The frame onto the window, as present() places it. The core keeps its GL
 * state across frames: what this touches is put back. */
static void hw_present(int x, int y, int w, int h, int win_w, int win_h) {
  GLint draw, read;
  GLboolean mask[4];
  gl.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
  gl.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
  gl.GetBooleanv(GL_COLOR_WRITEMASK, mask);
  const GLboolean scissor = gl.IsEnabled(GL_SCISSOR_TEST);
  gl.Disable(GL_SCISSOR_TEST);
  gl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  gl.ClearColor(0, 0, 0, 1);
  gl.Clear(GL_COLOR_BUFFER_BIT);
  if (frame_w > 0 && frame_h > 0) {
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    /* GL's rows go up; a core with top-left origin drew them down */
    const int top = hw.bottom_left_origin ? frame_h : 0, bottom = frame_h - top;
    gl.BlitFramebuffer(0, bottom, frame_w, top, x, win_h - y - h, x + w, win_h - y, GL_COLOR_BUFFER_BIT,
                       host_value(HOST_FILTER) ? GL_LINEAR : GL_NEAREST);
  }
  /* The overlay: a scissored clear a rectangle, nothing of the core's touched
   * but the scissor box */
  const int n_rects = overlay_rects(win_w, win_h);
  GLint box[4];
  gl.GetIntegerv(GL_SCISSOR_BOX, box);
  gl.Enable(GL_SCISSOR_TEST);
  for (int i = 0; i < n_rects; i++) {
    const rect r = orects[i].r;
    const float v = orects[i].text ? 1 : 0.08f;
    gl.Scissor(r.x, win_h - r.y - r.h, r.w, r.h);
    gl.ClearColor(v, v, orects[i].text ? 1 : 0.1f, 1);
    gl.Clear(GL_COLOR_BUFFER_BIT);
  }
  gl.Disable(GL_SCISSOR_TEST);
  gl.Scissor(box[0], box[1], box[2], box[3]);
  int egl_w, egl_h;
  wl_egl_window_get_attached_size(egl_window, &egl_w, &egl_h);
  if (egl_w != win_w || egl_h != win_h)
    wl_egl_window_resize(egl_window, win_w, win_h, 0, 0);
  eglSwapBuffers(egl_display, egl_surface);
  gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, draw);
  gl.BindFramebuffer(GL_READ_FRAMEBUFFER, read);
  gl.ColorMask(mask[0], mask[1], mask[2], mask[3]);
  if (scissor)
    gl.Enable(GL_SCISSOR_TEST);
}

static bool environment(unsigned cmd, void *data) {
  switch (cmd) {
  case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
    switch (*(const enum retro_pixel_format *)data) {
    /* X as A: the swapchain is opaque */
    case RETRO_PIXEL_FORMAT_XRGB8888: frame_format = VK_FORMAT_B8G8R8A8_UNORM, frame_bytes = 4; return true;
    case RETRO_PIXEL_FORMAT_RGB565: frame_format = VK_FORMAT_R5G6B5_UNORM_PACK16, frame_bytes = 2; return true;
    case RETRO_PIXEL_FORMAT_0RGB1555: frame_format = VK_FORMAT_A1R5G5B5_UNORM_PACK16, frame_bytes = 2; return true;
    default: return false;
    }
  case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
    ((struct retro_log_callback *)data)->log = logf_;
    return true;
  case RETRO_ENVIRONMENT_GET_CAN_DUPE:
  case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
    if (data != NULL)
      *(bool *)data = true;
    return true;
  /* 2: the options come with a category (and a default). Any of the three
   * ways a core sets them replaces the core's; ours follow once published,
   * then the values from the last run. */
  case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
    *(unsigned *)data = 2;
    return true;
  case RETRO_ENVIRONMENT_SET_VARIABLES:
  case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
  case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL:
  case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
  case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL: {
    atomic_store(&shm->option_count, 0);
    if (cmd == RETRO_ENVIRONMENT_SET_VARIABLES) {
      for (const struct retro_variable *v = data; v->key != NULL; v++)
        add_option(v->key, v->value, APPCTL_SYSTEM);
    } else if (cmd == RETRO_ENVIRONMENT_SET_CORE_OPTIONS ||
               cmd == RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL) {
      const struct retro_core_option_definition *d =
          cmd == RETRO_ENVIRONMENT_SET_CORE_OPTIONS
              ? data
              : ((const struct retro_core_options_intl *)data)->us;
      for (; d != NULL && d->key != NULL; d++)
        add_definition(d->key, d->desc, NULL, d->values, d->default_value);
    } else {
      const struct retro_core_options_v2 *o =
          cmd == RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2
              ? data
              : ((const struct retro_core_options_v2_intl *)data)->us;
      for (const struct retro_core_option_v2_definition *d = o ? o->definitions : NULL;
           d != NULL && d->key != NULL; d++)
        add_definition(d->key, d->desc_categorized ? d->desc_categorized : d->desc,
                       d->category_key, d->values, d->default_value);
    }
    load_options();
    if (host_published)
      publish_and_load();
    /* true for v2: the categories are used */
    return true;
  }
  case RETRO_ENVIRONMENT_GET_VARIABLE: {
    struct retro_variable *v = data;
    const appctl_option *o = option(v->key);
    v->value = o != NULL ? o->values[atomic_load(&o->value) % APPCTL_VALUES] : NULL;
    return o != NULL;
  }
  case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
    *(bool *)data = options_changed;
    options_changed = false;
    return true;
  case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
  case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: {
    /* System: the ROM's own directory unless told, BIOS files and the like go next to it */
    static char dir[512];
    if (cmd == RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY) {
      *(const char **)data = save_dir;
      return true;
    }
    const char *env = getenv("COREHOST_SYSTEM");
    snprintf(dir, sizeof(dir), "%s", env != NULL ? env : rom_stem);
    char *slash = strrchr(dir, '/');
    if (env == NULL && slash != NULL)
      *slash = '\0';
    *(const char **)data = dir;
    return true;
  }
  case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS: /* again on a mapping change */
    described = 0;
    described_count = 0;
    for (const struct retro_input_descriptor *d = data; d->description != NULL; d++)
      if (d->port == 0 && d->device == RETRO_DEVICE_JOYPAD && d->id < 16) {
        if (!(described >> d->id & 1))
          described_order[described_count++] = (uint8_t)d->id;
        described |= 1u << d->id;
        snprintf(described_as[d->id], APPCTL_TEXT, "%s", d->description);
      } else if (d->port == 0 && d->device == RETRO_DEVICE_ANALOG &&
                 d->index <= RETRO_DEVICE_INDEX_ANALOG_RIGHT && d->id <= RETRO_DEVICE_ID_ANALOG_Y) {
        /* "Control Stick X": a row for each way, "Control Stick X -" .. */
        for (int plus = 0; plus < 2; plus++) {
          const int a = ANALOG_ID(d->index, d->id, plus);
          if (!(described >> a & 1))
            described_order[described_count++] = (uint8_t)a;
          described |= 1u << a;
          snprintf(described_as[a], APPCTL_TEXT, "%s %s", d->description,
                   d->id == RETRO_DEVICE_ID_ANALOG_X ? (plus ? "right" : "left") : (plus ? "down" : "up"));
        }
      }
    if (host_published)
      atomic_store(&redescribed, true);
    return true;
  case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
  case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
  case RETRO_ENVIRONMENT_SET_HW_SHARED_CONTEXT:
    return true;
  case RETRO_ENVIRONMENT_SET_GEOMETRY: /* the frame's size comes with it */
    aspect = ((const struct retro_game_geometry *)data)->aspect_ratio;
    /* A core may replace its output images when geometry changes. Don't
     * repeat the previous image until it hands us a fresh one. */
    if (vk_on)
      vk.have_image = false;
    return true;
  case GET_CLEAR_ALL_THREAD_WAITS_CB:
    *(retro_environment_t *)data = clear_thread_waits;
    return true;
  case RETRO_ENVIRONMENT_GET_LIBRETRO_PATH:
    *(const char **)data = core_path;
    return true;
  /* GLES: what Mesa does best here, and what the cores do on ARM */
  case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER: {
    const char *render = getenv("COREHOST_RENDER");
    *(unsigned *)data = render != NULL && strcmp(render, "vulkan") == 0 ? RETRO_HW_CONTEXT_VULKAN
                                                                         : RETRO_HW_CONTEXT_OPENGLES3;
    return true;
  }
  case RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE: {
    const struct retro_hw_render_context_negotiation_interface_vulkan *n = data;
    if (n->interface_type != RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN)
      return false;
    vk.negotiation = n;
    return true;
  }
  case RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE:
    if (!vk_on || vk.device == VK_NULL_HANDLE)
      return false;
    *(const struct retro_hw_render_interface **)data = (const void *)&vk.iface;
    return true;
  case RETRO_ENVIRONMENT_SET_HW_RENDER: {
    struct retro_hw_render_callback *cb = data;
    vk_on = cb->context_type == RETRO_HW_CONTEXT_VULKAN;
    if (!vk_on && cb->context_type != RETRO_HW_CONTEXT_OPENGLES2 &&
        cb->context_type != RETRO_HW_CONTEXT_OPENGLES3 && cb->context_type != RETRO_HW_CONTEXT_OPENGLES_VERSION)
      return false;
    cb->get_current_framebuffer = hw_framebuffer;
    cb->get_proc_address = hw_proc;
    hw = *cb;
    hw_on = true;
    return true;
  }
  case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO: {
    /* ponytail: a new rate or sample rate is taken on the next start */
    const struct retro_system_av_info *av = data;
    aspect = av->geometry.aspect_ratio;
    if (vk_on)
      vk.have_image = false;
    if (av->geometry.max_width > max_w || av->geometry.max_height > max_h)
      hw_resize(av->geometry.max_width, av->geometry.max_height);
    return true;
  }
  default:
    return false;
  }
}

static uint8_t *read_all(const char *path, size_t *size) {
  FILE *f = fopen(path, "rb");
  if (f == NULL)
    return NULL;
  uint8_t *data = NULL;
  long n = 0;
  if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) >= 0 && fseek(f, 0, SEEK_SET) == 0 &&
      (data = malloc(n ? n : 1)) != NULL && fread(data, 1, n, f) != (size_t)n) {
    free(data);
    data = NULL;
  }
  fclose(f);
  *size = (size_t)n;
  return data;
}

/* Written beside and renamed over, so a crash leaves the old one */
static int write_all(const char *path, const void *data, size_t size) {
  char tmp[600];
  snprintf(tmp, sizeof(tmp), "%s.new", path);
  FILE *f = fopen(tmp, "wb");
  if (f == NULL)
    return -errno;
  const bool ok = fwrite(data, 1, size, f) == size;
  if (fclose(f) != 0 || !ok || rename(tmp, path) != 0) {
    unlink(tmp);
    return -EIO;
  }
  return 0;
}

/* A zip's first file: stored or deflated */
static uint8_t *unzip(uint8_t *zip, size_t size, size_t *out_size) {
  uint32_t sig, dir = 0, local = 0, csize = 0, usize = 0;
  uint16_t method = 0, name_len, extra_len;
  for (size_t i = size >= 22 ? size - 22 : 0; i > 0 && dir == 0; i--) {
    memcpy(&sig, zip + i, 4);
    if (sig == 0x06054b50)
      memcpy(&dir, zip + i + 16, 4);
  }
  if (dir == 0 || dir + 46 > size)
    return NULL;
  memcpy(&method, zip + dir + 10, 2);
  memcpy(&csize, zip + dir + 20, 4);
  memcpy(&usize, zip + dir + 24, 4);
  memcpy(&local, zip + dir + 42, 4);
  if ((uint64_t)local + 30 > size)
    return NULL;
  memcpy(&name_len, zip + local + 26, 2);
  memcpy(&extra_len, zip + local + 28, 2);
  const size_t at = (size_t)local + 30 + name_len + extra_len;
  uint8_t *out = malloc(usize ? usize : 1);
  if (out == NULL || at + csize > size)
    goto fail;
  if (method == 0 && csize == usize) {
    memcpy(out, zip + at, usize);
  } else if (method == 8) {
    z_stream z = {.next_in = zip + at, .avail_in = csize, .next_out = out, .avail_out = usize};
    const bool ok = inflateInit2(&z, -MAX_WBITS) == Z_OK && inflate(&z, Z_FINISH) == Z_STREAM_END;
    inflateEnd(&z);
    if (!ok)
      goto fail;
  } else {
    goto fail;
  }
  *out_size = usize;
  return out;
fail:
  free(out);
  return NULL;
}

/* Save RAM: in from STEM.srm, or else from next to the ROM */
static void load_sram(void) {
  uint8_t *ram = core.retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
  const size_t room = core.retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
  if (ram == NULL || room == 0)
    return;
  char path[600];
  size_t size = 0;
  snprintf(path, sizeof(path), "%s.srm", stem);
  uint8_t *data = read_all(path, &size);
  for (int i = 0; i < 2 && data == NULL; i++) {
    snprintf(path, sizeof(path), "%s.%s", rom_stem, i == 0 ? "srm" : "sav");
    data = read_all(path, &size);
  }
  if (data != NULL) {
    memcpy(ram, data, MIN(size, room));
    free(data);
  }
  /* What is on disk, so an unchanged one is never written */
  sram_shadow = malloc(room);
  if (sram_shadow != NULL) {
    memcpy(sram_shadow, ram, room);
    sram_shadow_size = room;
  }
}

/* The core may only know the real size once the game used it */
static void flush_sram(void) {
  uint8_t *ram = core.retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
  const size_t size = core.retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
  if (ram == NULL || size == 0 || sram_shadow == NULL ||
      (size <= sram_shadow_size && memcmp(ram, sram_shadow, size) == 0))
    return;
  char path[600];
  snprintf(path, sizeof(path), "%s.srm", stem);
  if (write_all(path, ram, size) == 0)
    memcpy(sram_shadow, ram, MIN(size, sram_shadow_size));
}

static int save_state(int slot) {
  const size_t size = core.retro_serialize_size();
  void *buf = size ? malloc(size) : NULL;
  int result = -ENOTSUP;
  if (buf != NULL && core.retro_serialize(buf, size)) {
    char path[600];
    snprintf(path, sizeof(path), "%s.state%d", stem, slot);
    result = write_all(path, buf, size);
  }
  free(buf);
  return result;
}

static int load_state(int slot) {
  char path[600];
  snprintf(path, sizeof(path), "%s.state%d", stem, slot);
  size_t size;
  uint8_t *buf = read_all(path, &size);
  if (buf == NULL)
    return -ENOENT;
  const bool ok = core.retro_unserialize(buf, size);
  free(buf);
  return ok ? 0 : -EINVAL;
}

/* The last seconds, a whole state every REWIND_EVERY frames.
 * ponytail: plain states in a fixed budget (GBA: a few seconds); XOR against
 * the previous one and compress when longer rewinds matter. */
#define REWIND_BUDGET (32u << 20)
#define REWIND_EVERY 3
static uint8_t *rewind_ring;

static void rewind_init(void) {
  state_size = core.retro_serialize_size();
  rewind_slots = state_size ? REWIND_BUDGET / state_size : 0;
  /* Less than a second's worth (N64, PS1 and up): none, and no copying a
   * big state every few frames */
  if (rewind_slots < 20 || (rewind_ring = malloc(rewind_slots * state_size)) == NULL)
    rewind_slots = 0;
}

static void rewind_push(void) {
  if (rewind_slots == 0 || !core.retro_serialize(rewind_ring + rewind_head * state_size, state_size))
    return;
  rewind_head = (rewind_head + 1) % rewind_slots;
  rewind_count += rewind_count < rewind_slots;
}

static void rewind_pop(void) {
  if (rewind_count == 0)
    return;
  rewind_head = (rewind_head + rewind_slots - 1) % rewind_slots;
  rewind_count--;
  core.retro_unserialize(rewind_ring + rewind_head * state_size, state_size);
}

/* One request, answered; false once the launcher is gone */
static bool control(bool *paused) {
  appctl_msg m;
  const ssize_t n = recv(ctl, &m, sizeof(m), MSG_DONTWAIT);
  if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR))
    return false;
  if (n != sizeof(m))
    return true;
  m.result = 0;
  switch (m.op) {
  case APPCTL_PAUSE:
    *paused = true;
    if (audio != NULL)
      snd_pcm_drop(audio);
    /* Home may be the last the game sees for a while */
    flush_sram();
    break;
  case APPCTL_RESUME:
    *paused = false;
    for (unsigned i = 0; startup_options && i < host_option_at; i++)
      restart |= atomic_load(&shm->options[i].value) != started_with[i];
    /* Mapped again: the kiosk shell puts what maps on top */
    if (m.arg)
      win_map(&window);
    /* What is held now was pressed for the launcher */
    launcher_held = pad_held(pad_fd);
    if (audio != NULL)
      snd_pcm_prepare(audio);
    break;
  case APPCTL_SAVE_STATE:
    m.result = save_state(m.arg);
    notify(m.result == 0 ? "State saved" : "Save failed");
    break;
  case APPCTL_LOAD_STATE:
    m.result = load_state(m.arg);
    notify(m.result == 0 ? "State loaded" : m.result == -ENOENT ? "No state saved" : "Load failed");
    rewind_count = 0;
    break;
  case APPCTL_RESET: core.retro_reset(); break;
  case APPCTL_OPTIONS:
    options_changed = true;
    sync_own();
    save_options();
    break;
  default: m.result = -ENOSYS;
  }
  send(ctl, &m, sizeof(m), MSG_NOSIGNAL);
  return true;
}

static void present(void) {
  const int w = vk_on ? (int)vk.extent.width : window.w, h = vk_on ? (int)vk.extent.height : window.h;
  rect dst = {0, 0, w, h};
  if (frame_w > 0 && frame_h > 0) {
    /* Integer scaling goes by the height: N64's 640x240 is a 4:3 picture */
    const float shown_w = aspect > 0 ? frame_h * aspect : frame_w;
    const float fit = MIN(w / shown_w, (float)h / frame_h);
    const unsigned scale = host_value(HOST_SCALE);
    if (scale != 2) {
      const float s = scale == 0 && fit >= 1 ? (float)(int)fit : fit;
      dst.w = (int)(shown_w * s);
      dst.h = (int)(frame_h * s);
      dst.x = (w - dst.w) / 2;
      dst.y = (h - dst.h) / 2;
    }
  }
  if (vk_on) {
    vk_present(dst.x, dst.y, dst.w, dst.h);
    return;
  }
  hw_present(dst.x, dst.y, dst.w, dst.h, w, h);
}

static bool open_ctl(void) {
  int sock, mem;
  const char *env = getenv(APPCTL_ENV);
  if (env != NULL && sscanf(env, "%d,%d", &sock, &mem) == 2) {
    struct stat st;
    if (fstat(mem, &st) != 0 || st.st_size != sizeof(appctl_shm)) {
      fprintf(stderr, "corehost: incompatible launcher control table; restart the updated launcher\n");
      close(mem);
      return false;
    }
    void *map = mmap(NULL, sizeof(appctl_shm), PROT_READ | PROT_WRITE, MAP_SHARED, mem, 0);
    if (map != MAP_FAILED) {
      ctl = sock;
      ctl_mem = mem; /* kept for a restart */
      shm = map;
      /* Ours again after a restart: options are published anew */
      atomic_store(&shm->option_count, 0);
    } else {
      close(mem);
    }
  }
  unsetenv(APPCTL_ENV);
  /* Alone: the same table, ours only */
  if (shm == NULL && (shm = calloc(1, sizeof(*shm))) == NULL)
    return false;
  shm->magic = APPCTL_MAGIC;
  shm->version = APPCTL_VERSION;
  shm->slots = 1;
  shm->flags = APPCTL_OWN_OPTIONS;
  return true;
}

static void shut_down(void) {
  flush_sram();
  /* RetroArch's order, which cores count on: PPSSPP's unload frees what its
   * context_destroy uses */
  if (vk_on && vk.device != VK_NULL_HANDLE)
    vkDeviceWaitIdle(vk.device);
  if (hw_on && hw.context_destroy != NULL)
    hw.context_destroy();
  core.retro_unload_game();
  core.retro_deinit();
  if (audio != NULL)
    snd_pcm_close(audio);
  audio = NULL;
}

/* Run again in place with the options as they are now, where we are */
static void restart_core(void) {
  if (save_state(RESTART_SLOT) != 0)
    return; /* nothing to go on from: keep playing with the old options */
  shut_down();
  char env[32];
  snprintf(env, sizeof(env), "%d,%d", ctl, ctl_mem);
  setenv(APPCTL_ENV, env, 1);
  setenv("COREHOST_RESTARTED", "1", 1);
  execv("/proc/self/exe", args);
  fprintf(stderr, "corehost: restart: %s\n", strerror(errno));
  exit(1);
}

int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: %s CORE.so ROM\n", argv[0]);
    return 2;
  }
  args = argv;
  const char *rom = argv[2];
  core_path = argv[1];
  snprintf(rom_stem, sizeof(rom_stem), "%s", rom);
  char *dot = strrchr(rom_stem, '.');
  if (dot != NULL && strchr(dot, '/') == NULL)
    *dot = '\0';
  if (!open_ctl())
    return 1;

  void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (lib == NULL) {
    fprintf(stderr, "corehost: %s\n", dlerror());
    return 1;
  }
#define LOAD(name)                                                             \
  if ((*(void **)&core.name = dlsym(lib, #name)) == NULL) {                    \
    fprintf(stderr, "corehost: no " #name "\n");                               \
    return 1;                                                                  \
  }
  LOAD(retro_set_environment) LOAD(retro_set_video_refresh) LOAD(retro_set_audio_sample)
  LOAD(retro_set_audio_sample_batch) LOAD(retro_set_input_poll) LOAD(retro_set_input_state)
  LOAD(retro_init) LOAD(retro_deinit) LOAD(retro_get_system_info) LOAD(retro_get_system_av_info)
  LOAD(retro_load_game) LOAD(retro_unload_game) LOAD(retro_run) LOAD(retro_reset)
  LOAD(retro_serialize_size) LOAD(retro_serialize) LOAD(retro_unserialize)
  LOAD(retro_get_memory_data) LOAD(retro_get_memory_size)
#undef LOAD

  struct retro_system_info info = {0};
  core.retro_get_system_info(&info);
  const char *config = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
  if (config != NULL && config[0] == '/')
    snprintf(opt_path, sizeof(opt_path), "%s/corehost", config);
  else
    snprintf(opt_path, sizeof(opt_path), "%s/.config/corehost", home ? home : "/tmp");
  for (char *slash = strchr(opt_path + 1, '/'); slash; slash = strchr(slash + 1, '/')) {
    *slash = '\0';
    mkdir(opt_path, 0755);
    *slash = '/';
  }
  mkdir(opt_path, 0755);
  snprintf(game_opt_path, sizeof(game_opt_path), "%s/%s", opt_path, info.library_name);
  mkdir(game_opt_path, 0755);
  const char *base = strrchr(rom_stem, '/');
  base = base ? base + 1 : rom_stem;
  snprintf(game_opt_path, sizeof(game_opt_path), "%s/%s/%s.opt", opt_path, info.library_name, base);
  const char *saves = getenv("COREHOST_SAVE");
  if (saves != NULL && saves[0] == '/') {
    snprintf(save_dir, sizeof(save_dir), "%s/%s", saves, info.library_name);
    for (char *slash = strchr(save_dir + 1, '/'); slash; slash = strchr(slash + 1, '/')) {
      *slash = '\0';
      mkdir(save_dir, 0755);
      *slash = '/';
    }
    mkdir(save_dir, 0755);
  } else {
    snprintf(save_dir, sizeof(save_dir), "%.*s", (int)(base > rom_stem ? base - 1 - rom_stem : 1),
             base > rom_stem ? rom_stem : ".");
  }
  if (snprintf(stem, sizeof(stem), "%s/%s", save_dir, base) >= (int)sizeof(stem)) {
    fprintf(stderr, "corehost: %s/%s: too long a path\n", save_dir, base);
    return 1;
  }
  const size_t len = strlen(opt_path);
  snprintf(opt_path + len, sizeof(opt_path) - len, "/%s.opt", info.library_name);
  snprintf(default_opt_path, sizeof(default_opt_path), "%s/%s.opt", COREHOST_DEFAULTS, info.library_name);

  /* A core that registers no options still gets ours */
  struct retro_variable none = {NULL, NULL};
  environment(RETRO_ENVIRONMENT_SET_VARIABLES, &none);
  core.retro_set_environment(environment);
  core.retro_set_video_refresh(video);
  core.retro_set_audio_sample(audio_sample);
  core.retro_set_audio_sample_batch(audio_batch);
  core.retro_set_input_poll(input_poll);
  core.retro_set_input_state(input_state);

  if (!win_connect(key)) {
    fprintf(stderr, "corehost: no Wayland display\n");
    return 1;
  }
  pad_fd = pad_open(0);
  core.retro_init();

  struct retro_game_info game = {.path = rom};
  size_t size = 0;
  uint8_t *data = NULL;
  if (!info.need_fullpath) {
    data = read_all(rom, &size);
    const size_t n = strlen(rom);
    if (data != NULL && n > 4 && strcasecmp(rom + n - 4, ".zip") == 0) {
      uint8_t *inner = unzip(data, size, &size);
      free(data);
      data = inner;
    }
    if (data == NULL) {
      fprintf(stderr, "corehost: can't read %s\n", rom);
      return 1;
    }
    game.data = data;
    game.size = size;
  }
  if (!core.retro_load_game(&game)) {
    fprintf(stderr, "corehost: %s doesn't load %s\n", info.library_name, rom);
    return 1;
  }
  load_sram();
  /* What the core read its options as */
  startup_options = strcmp(info.library_name, "Mupen64Plus-Next") == 0 && ctl_mem >= 0;
  for (unsigned i = 0; i < atomic_load(&shm->option_count); i++)
    started_with[i] = atomic_load(&shm->options[i].value);
  rewind_init(); /* before the bindings: no rewind, no row for it */
  publish_and_load();
  /* Restarted: the state is loaded once the core takes it (mupen: not while
   * it boots), for a few seconds at most */
  int resume_tries = getenv("COREHOST_RESTARTED") ? 300 : 0;
  unsetenv("COREHOST_RESTARTED");

  struct retro_system_av_info av;
  core.retro_get_system_av_info(&av);
  fps = av.timing.fps > 0 ? av.timing.fps : 60;
  aspect = av.geometry.aspect_ratio;

  window = (win){.app_id = "corehost",
                 .title = info.library_name,
                 .fullscreen = ctl >= 0,
                 .w = (int)av.geometry.base_width * 3,
                 .h = (int)av.geometry.base_height * 3};
  win_map(&window);
  if (hw_on && !vk_on) {
    hw_resize(av.geometry.max_width, av.geometry.max_height);
    if (!hw_init())
      return 1;
  } else {
    /* A software core's frames go the Vulkan core's way */
    vk_on = true;
    if (!vk_init())
      return 1;
  }
  /* Swaps per frame and the stretch, as RetroArch picks them (its
   * audio_max_timing_skew): the refresh rate's nearest multiple of the
   * core's, if it is close */
  const double refresh = win_refresh_mhz > 0 ? win_refresh_mhz / 1000.0 : fps;
  const int64_t refresh_ns = (int64_t)(1e9 / refresh);
  swaps = (unsigned)CLAMP((int)(refresh / fps + 0.5), 1, 4);
  const double target = refresh / swaps;
  const bool stretched = fabs(1 - fps / target) <= MAX_SKEW;
  if (!stretched)
    swaps = 1;
  in_rate = av.timing.sample_rate * (stretched ? target / fps : 1);
  fprintf(stderr, "corehost: %.3f fps at %.3f Hz: %u swap%s a frame, %s\n", fps, refresh, swaps,
          swaps > 1 ? "s" : "", stretched ? "sound stretched to fit" : "paced by the sound");
  /* 64 ms of sound in the device, as RetroArch has it */
  if (snd_pcm_open(&audio, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0 ||
      snd_pcm_set_params(audio, SND_PCM_FORMAT_S16, SND_PCM_ACCESS_RW_INTERLEAVED, 2, OUT_RATE, 1, 64000) < 0) {
    fprintf(stderr, "corehost: no sound\n");
    if (audio != NULL)
      snd_pcm_close(audio);
    audio = NULL;
  } else {
    snd_pcm_uframes_t period;
    snd_pcm_get_params(audio, &audio_buffer, &period);
  }

  bool paused = false, running = true, fast = false;
  unsigned frame = 0;
  uint64_t flushed = ms_now();
  while (running) {
    /* Paused: asleep until the launcher, the window or a signal */
    struct pollfd fds[1] = {{ctl, POLLIN, 0}};
    if (!win_wait(fds, ctl >= 0, paused ? -1 : 0))
      running = false;
    for (int why, value; win_woken(&why, &value);)
      running &= why != WIN_QUIT;
    if (ctl >= 0 && !control(&paused))
      running = false;
    if (restart && running && !paused)
      restart_core();
    restart = false;
    if (paused || !running)
      continue;

    /* Hotkeys on the press: toggles and one-shots */
    static uint32_t was;
    update_bindings();
    const uint32_t pressed = active & ~was;
    was = active;
    if (pressed & (1u << HOT_FAST))
      fast = !fast;
    if (pressed & (1u << HOT_SAVE))
      notify(save_state(0) == 0 ? "State saved" : "Save failed");
    if (pressed & (1u << HOT_LOAD)) {
      const int r = load_state(0);
      notify(r == 0 ? "State loaded" : r == -ENOENT ? "No state saved" : "Load failed");
      if (r == 0)
        rewind_count = 0;
    }
    const bool rewinding = active & (1u << HOT_REWIND);
    muted = fast || rewinding;

    /* The overlay: FPS (frames run a second, and the share of the time the
     * core took), what is going on, the last notice */
    static int64_t second_at, core_ns;
    static unsigned frames_run;
    static char fps_line[32];
    const int64_t run_at = ns_now();
    if (run_at - second_at >= 1000000000) {
      snprintf(fps_line, sizeof(fps_line), "%u FPS %d%%", frames_run,
               (int)(core_ns * 100 / (run_at - second_at)));
      /* For measuring from outside (tools, ssh) */
      static int log_fps = -1;
      if (log_fps < 0)
        log_fps = getenv("COREHOST_LOG_FPS") != NULL;
      if (log_fps)
        fprintf(stderr, "corehost: %s\n", fps_line);
      second_at = run_at;
      frames_run = 0;
      core_ns = 0;
    }
    int l = 0;
    memset(overlay, 0, sizeof(overlay));
    if (host_value(HOST_FPS))
      snprintf(overlay[l++], sizeof(overlay[0]), "%s", fps_line);
    if (rewinding)
      snprintf(overlay[l++], sizeof(overlay[0]), "<< Rewind");
    else if (fast)
      snprintf(overlay[l++], sizeof(overlay[0]), ">> Fast forward");
    if (notice[0] && ms_now() < notice_until)
      snprintf(overlay[l++], sizeof(overlay[0]), "%s", notice);

    for (int i = 0; i < (fast && !rewinding ? FAST_SPEED : 1); i++, frames_run++) {
      if (rewinding)
        rewind_pop();
      else if (++frame % REWIND_EVERY == 0)
        rewind_push();
      core.retro_run();
    }
    core_ns += ns_now() - run_at;
    if (atomic_exchange(&redescribed, false)) {
      atomic_store(&shm->option_count, host_option_at);
      publish_and_load();
    }
    if (resume_tries > 0 && (load_state(RESTART_SLOT) == 0 || --resume_tries == 0)) {
      char path[600];
      snprintf(path, sizeof(path), "%s.state%d", stem, RESTART_SLOT);
      unlink(path);
      resume_tries = 0;
    }
    /* Vsync, as the compositor's frame callbacks come: one each repaint
     * (vblank) that showed our last commit. Waited for before the next
     * commit, not after this one: the core's next frame runs while this one
     * is shown, so a frame a little over a refresh costs a little, not a
     * whole refresh (double buffering). Waited for in the loop, so the
     * launcher, the window and signals are heard meanwhile. A hidden window
     * gets none: two refreshes past the last commit at most, then on. */
    static int64_t committed;
    for (unsigned i = 0; i < swaps && running; i++) {
      const int64_t until = committed + 2 * refresh_ns;
      for (int64_t left; window.frame_pending && (left = until - ns_now()) > 0;)
        if (!win_wait(fds, ctl >= 0, (int)(left / 1000000) + 1))
          running = false;
      win_frame(&window);
      present();
      committed = ns_now();
    }
    /* A crash costs at most this much play */
    if (ms_now() - flushed > 10000) {
      flush_sram();
      flushed = ms_now();
    }
  }
  shut_down();
  free(data);
  return 0;
}
