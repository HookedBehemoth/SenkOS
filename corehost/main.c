/*
 * corehost: runs one libretro core with one game, and nothing else. No menu,
 * no overlay: the launcher is the UI and drives it through appctl.h (pause,
 * save states, options). Without the launcher it just plays.
 *
 *   corehost CORE.so ROM
 *
 * Video through SDL_Renderer (handheld cores are small), or for a core that
 * renders with GLES (N64, Dreamcast, PSP) a GLES context and a framebuffer
 * for it to draw into, blitted to the window. Or Vulkan, the way RetroArch
 * offers it: the core makes its device on our instance and surface, renders
 * into its own image and hands that over; we blit it into the swapchain.
 * Cores that can do both are offered GLES unless $COREHOST_RENDER=vulkan. Audio queued to SDL and the
 * pace taken from it, input from the first SDL game controller (the
 * built-in pad, by label: SDL_GAMECONTROLLERCONFIG) or the keyboard.
 *
 * Every game button and the hotkeys (fast forward, a toggle; rewind, held;
 * save and load state 0) are bindings: a button combo each, which the
 * launcher records. The hotkeys default to Menu + a shoulder button.
 *
 * BIOS files and the like: next to the ROM, or in $COREHOST_SYSTEM. What a
 * core saves on its own (PPSSPP's memory stick): there too, or in
 * $COREHOST_SAVE.
 * Next to the ROM: STEM.srm (save RAM; a STEM.sav from the standalone
 * emulator is read when there is no .srm, never written) and STEM.state0.
 * Options are kept in $XDG_CONFIG_HOME/corehost/CORE.opt.
 *
 * SIGTERM, the control socket closing (the launcher went away) and closing
 * the window all quit, after writing the save RAM.
 */
#include <dlfcn.h>
#include <errno.h>
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

#include <SDL.h>
#include <zlib.h>
/* The context's own functions, from SDL_GL_GetProcAddress: nothing linked */
#define GL_GLES_PROTOTYPES 0
#include <GLES3/gl3.h>
#include <SDL_vulkan.h>
#include "libretro_vulkan.h"

#include "../launcher/appctl.h"
#include "libretro.h"

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

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static Uint32 texture_format = SDL_PIXELFORMAT_RGB565;
static int frame_w, frame_h;
/* The frame's shape on screen, width / height; 0: square pixels */
static float aspect;
static SDL_AudioDeviceID audio;
static SDL_GameController *pad;
static double fps = 60;
static int sample_rate;
static const char *core_path;

/* SET_HW_RENDER: the core draws into fbo, at most max_w x max_h */
static struct retro_hw_render_callback hw;
static bool hw_on, vk_on;
static SDL_GLContext gl_context;
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
} gl;

static int ctl = -1;
static appctl_shm *shm;
static bool options_changed;
static char opt_path[512], stem[512];
static uint8_t *sram_shadow;
static size_t sram_shadow_size;

/* Bindings: a combo of pad buttons each (appctl.h), recorded by the launcher.
 * The hotkeys, then the game's buttons: those the core describes (GBA: no X,
 * Y, L2, R2), under its names for them. The keyboard's keys are fixed, it is
 * for testing. */
#define B(name) (1u << APPCTL_##name)
static const struct {
  const char *key, *label;
  uint16_t buttons;
  int id; /* libretro's; -1: a hotkey */
  SDL_Scancode keyboard;
} bindings[] = {
    {"corehost_hotkey_fast", "Fast forward (toggle)", B(MENU) | B(R1), -1, 0},
    {"corehost_hotkey_rewind", "Rewind (hold)", B(MENU) | B(L1), -1, 0},
    {"corehost_hotkey_save", "Save state", B(MENU) | B(R2), -1, 0},
    {"corehost_hotkey_load", "Load state", B(MENU) | B(L2), -1, 0},
    {"corehost_map_up", "Up", B(UP), RETRO_DEVICE_ID_JOYPAD_UP, SDL_SCANCODE_UP},
    {"corehost_map_down", "Down", B(DOWN), RETRO_DEVICE_ID_JOYPAD_DOWN, SDL_SCANCODE_DOWN},
    {"corehost_map_left", "Left", B(LEFT), RETRO_DEVICE_ID_JOYPAD_LEFT, SDL_SCANCODE_LEFT},
    {"corehost_map_right", "Right", B(RIGHT), RETRO_DEVICE_ID_JOYPAD_RIGHT, SDL_SCANCODE_RIGHT},
    {"corehost_map_a", "Button A", B(A), RETRO_DEVICE_ID_JOYPAD_A, SDL_SCANCODE_X},
    {"corehost_map_b", "Button B", B(B), RETRO_DEVICE_ID_JOYPAD_B, SDL_SCANCODE_Z},
    {"corehost_map_x", "Button X", B(X), RETRO_DEVICE_ID_JOYPAD_X, SDL_SCANCODE_S},
    {"corehost_map_y", "Button Y", B(Y), RETRO_DEVICE_ID_JOYPAD_Y, SDL_SCANCODE_A},
    {"corehost_map_l", "Button L", B(L1), RETRO_DEVICE_ID_JOYPAD_L, SDL_SCANCODE_Q},
    {"corehost_map_r", "Button R", B(R1), RETRO_DEVICE_ID_JOYPAD_R, SDL_SCANCODE_W},
    {"corehost_map_l2", "Button L2", B(L2), RETRO_DEVICE_ID_JOYPAD_L2, SDL_SCANCODE_1},
    {"corehost_map_r2", "Button R2", B(R2), RETRO_DEVICE_ID_JOYPAD_R2, SDL_SCANCODE_2},
    {"corehost_map_select", "Select", B(SELECT), RETRO_DEVICE_ID_JOYPAD_SELECT, SDL_SCANCODE_RSHIFT},
    {"corehost_map_start", "Start", B(START), RETRO_DEVICE_ID_JOYPAD_START, SDL_SCANCODE_RETURN},
    /* No sticks to click: unbound until the player binds them */
    {"corehost_map_l3", "Button L3", 0, RETRO_DEVICE_ID_JOYPAD_L3, SDL_SCANCODE_3},
    {"corehost_map_r3", "Button R3", 0, RETRO_DEVICE_ID_JOYPAD_R3, SDL_SCANCODE_4},
};
enum { HOT_FAST, HOT_REWIND, HOT_SAVE, HOT_LOAD, HOTKEYS, BINDINGS = SDL_arraysize(bindings) };

/* Our own options, after the core's: these choices, then the bindings */
enum { HOST_SCALE, HOST_FILTER, HOST_CHOICES };
static const struct {
  const char *key, *label, *values;
} host_options[HOST_CHOICES] = {
    {"corehost_scale", "Scaling", "Integer|Fit|Stretch"},
    {"corehost_filter", "Filter", "Sharp|Smooth"},
};
static unsigned host_option_at; /* index of the first one in shm */
/* Published once the game is loaded: the input descriptors are in by then */
static bool host_published;
static int binding_at[BINDINGS]; /* index in shm; -1: not shown, never pressed */
/* SET_INPUT_DESCRIPTORS: the joypad ids the core uses (none told: all), and
 * its names for them */
static uint32_t described;
static char described_as[16][APPCTL_TEXT];

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
  for (int i = 0; i < BINDINGS; i++) {
    const int id = bindings[i].id;
    binding_at[i] = -1;
    if (id >= 0 && described != 0 && !(described >> id & 1))
      continue;
    appctl_option *o = new_option(bindings[i].key,
                                  id >= 0 && described_as[id][0] ? described_as[id] : bindings[i].label,
                                  APPCTL_INPUT);
    if (o == NULL)
      continue;
    o->kind = APPCTL_BINDING;
    atomic_store(&o->buttons, bindings[i].buttons);
    binding_at[i] = (int)atomic_load(&shm->option_count);
    publish();
  }
  host_published = true;
}

/* "L1+A", names in any order; "" is none. False on a name we don't know. */
static bool parse_combo(const char *text, uint16_t *out) {
  *out = 0;
  while (*text) {
    const size_t len = strcspn(text, "+");
    int b = APPCTL_BUTTONS;
    for (int i = 0; i < APPCTL_BUTTONS; i++)
      if (strlen(appctl_button_names[i]) == len && strncmp(appctl_button_names[i], text, len) == 0)
        b = i;
    if (b == APPCTL_BUTTONS)
      return false;
    *out |= 1u << b;
    text += len + (text[len] == '+');
  }
  return true;
}


/* "key=value" lines; values that no longer exist are skipped */
static void load_options(void) {
  FILE *f = fopen(opt_path, "r");
  char line[256];
  while (f != NULL && fgets(line, sizeof(line), f)) {
    line[strcspn(line, "\n")] = '\0';
    char *eq = strchr(line, '=');
    if (eq == NULL)
      continue;
    *eq = '\0';
    appctl_option *o = option(line);
    uint16_t combo;
    if (o != NULL && o->kind == APPCTL_BINDING && parse_combo(eq + 1, &combo))
      atomic_store(&o->buttons, combo);
    for (unsigned i = 0; o != NULL && i < o->count; i++)
      if (strcmp(o->values[i], eq + 1) == 0)
        atomic_store(&o->value, i);
  }
  if (f != NULL)
    fclose(f);
}

static void save_options(void) {
  FILE *f = fopen(opt_path, "w");
  if (f == NULL)
    return;
  const uint32_t n = atomic_load(&shm->option_count);
  for (uint32_t i = 0; i < n; i++) {
    const appctl_option *o = &shm->options[i];
    fprintf(f, "%s=", o->key);
    if (o->kind == APPCTL_BINDING) {
      const uint16_t combo = atomic_load(&o->buttons);
      for (int b = 0, first = 1; b < APPCTL_BUTTONS; b++)
        if (combo & (1u << b)) {
          fprintf(f, "%s%s", first ? "" : "+", appctl_button_names[b]);
          first = 0;
        }
      fputc('\n', f);
    } else {
      fprintf(f, "%s\n", o->values[atomic_load(&o->value) % APPCTL_VALUES]);
    }
  }
  fclose(f);
}

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
  if (texture == NULL || (int)w != frame_w || (int)h != frame_h) {
    SDL_DestroyTexture(texture);
    texture = SDL_CreateTexture(renderer, texture_format, SDL_TEXTUREACCESS_STREAMING, w, h);
    frame_w = w;
    frame_h = h;
  }
  SDL_UpdateTexture(texture, NULL, data, (int)pitch);
}

/* Fast forward and rewind are silent */
static bool muted;

static void audio_batch_(const int16_t *data, size_t frames) {
  if (audio != 0 && !muted)
    SDL_QueueAudio(audio, data, (Uint32)frames * 4);
}

static size_t audio_batch(const int16_t *data, size_t frames) {
  audio_batch_(data, frames);
  return frames;
}

static void audio_sample(int16_t left, int16_t right) {
  const int16_t frame[2] = {left, right};
  audio_batch_(frame, 1);
}

static void input_poll(void) { SDL_PumpEvents(); }

/* What is held, as appctl bits; the triggers are axes to SDL */
static uint16_t pad_held(void) {
  static const SDL_GameControllerButton map[APPCTL_BUTTONS] = {
      [APPCTL_MENU] = SDL_CONTROLLER_BUTTON_GUIDE,
      [APPCTL_L1] = SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
      [APPCTL_L2] = SDL_CONTROLLER_BUTTON_INVALID,
      [APPCTL_R1] = SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
      [APPCTL_R2] = SDL_CONTROLLER_BUTTON_INVALID,
      [APPCTL_UP] = SDL_CONTROLLER_BUTTON_DPAD_UP,
      [APPCTL_DOWN] = SDL_CONTROLLER_BUTTON_DPAD_DOWN,
      [APPCTL_LEFT] = SDL_CONTROLLER_BUTTON_DPAD_LEFT,
      [APPCTL_RIGHT] = SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
      [APPCTL_START] = SDL_CONTROLLER_BUTTON_START,
      [APPCTL_SELECT] = SDL_CONTROLLER_BUTTON_BACK,
      [APPCTL_A] = SDL_CONTROLLER_BUTTON_A,
      [APPCTL_B] = SDL_CONTROLLER_BUTTON_B,
      [APPCTL_X] = SDL_CONTROLLER_BUTTON_X,
      [APPCTL_Y] = SDL_CONTROLLER_BUTTON_Y,
  };
  if (pad == NULL)
    return 0;
  uint16_t held = 0;
  for (int i = 0; i < APPCTL_BUTTONS; i++)
    if (map[i] != SDL_CONTROLLER_BUTTON_INVALID && SDL_GameControllerGetButton(pad, map[i]))
      held |= 1u << i;
  if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16384)
    held |= B(L2);
  if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16384)
    held |= B(R2);
  return held;
}

/* The bindings held this frame, a bit each. One inside a bigger one that is
 * held too is not: L1 + A doesn't also press what A is bound to. */
static uint32_t active;

static void update_bindings(void) {
  const uint16_t held = pad_held();
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
  const Uint8 *keys = SDL_GetKeyboardState(NULL);
  int16_t mask = 0;
  for (int i = HOTKEYS; i < BINDINGS; i++)
    if ((active >> i & 1) || keys[bindings[i].keyboard])
      mask |= 1 << bindings[i].id;
  return mask;
}

static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id) {
  (void)index;
  if (port != 0 || (device & RETRO_DEVICE_MASK) != RETRO_DEVICE_JOYPAD)
    return 0;
  const int16_t mask = buttons();
  return id == RETRO_DEVICE_ID_JOYPAD_MASK ? mask : (mask >> id) & 1;
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
  SDL_mutex *queue_lock;
  /* From the core for this frame */
  struct retro_vulkan_image image;
  bool have_image;
  uint32_t src_family, wait_count, cmd_count;
  VkSemaphore wait[VK_MAX], signal;
  VkCommandBuffer core_cmd[VK_MAX];
} vk;

static void vk_set_image(void *handle, const struct retro_vulkan_image *image, uint32_t n,
                         const VkSemaphore *semaphores, uint32_t src_family) {
  (void)handle;
  vk.image = *image;
  vk.have_image = true;
  vk.src_family = src_family;
  vk.wait_count = SDL_min(n, VK_MAX);
  memcpy(vk.wait, semaphores, vk.wait_count * sizeof(*semaphores));
}
static uint32_t vk_sync_index(void *handle) { return (void)handle, vk.index; }
static uint32_t vk_sync_mask(void *handle) { return (void)handle, (1u << VK_FRAMES) - 1; }
static void vk_set_command_buffers(void *handle, uint32_t n, const VkCommandBuffer *cmd) {
  (void)handle;
  vk.cmd_count = SDL_min(n, VK_MAX);
  memcpy(vk.core_cmd, cmd, vk.cmd_count * sizeof(*cmd));
}
static void vk_wait_sync_index(void *handle) {
  (void)handle;
  vkWaitForFences(vk.device, 1, &vk.fence[vk.index], VK_TRUE, UINT64_MAX);
}
static void vk_lock(void *handle) { (void)handle, SDL_LockMutex(vk.queue_lock); }
static void vk_unlock(void *handle) { (void)handle, SDL_UnlockMutex(vk.queue_lock); }
static void vk_set_signal(void *handle, VkSemaphore semaphore) { (void)handle, vk.signal = semaphore; }

static bool vk_swapchain(void) {
  VkSurfaceCapabilitiesKHR caps;
  vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk.gpu, vk.surface, &caps);
  int w, h;
  SDL_Vulkan_GetDrawableSize(window, &w, &h);
  vk.extent = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent : (VkExtent2D){(uint32_t)w, (uint32_t)h};
  if (vk.extent.width == 0 || vk.extent.height == 0)
    return false;
  uint32_t count = caps.minImageCount + 1;
  if (caps.maxImageCount != 0)
    count = SDL_min(count, caps.maxImageCount);
  VkSwapchainKHR old = vk.swapchain;
  const VkSwapchainCreateInfoKHR info = {
      .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
      .surface = vk.surface,
      .minImageCount = SDL_min(count, VK_MAX),
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

/* Window, instance, the core's device, swapchain; then its context_reset */
static bool vk_init(const char *title, int w, int h, Uint32 flags) {
  window = SDL_CreateWindow(title, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, w, h,
                            flags | SDL_WINDOW_VULKAN);
  unsigned n_ext = 0;
  if (window == NULL || !SDL_Vulkan_GetInstanceExtensions(window, &n_ext, NULL))
    return false;
  const char *exts[16];
  n_ext = SDL_min(n_ext, SDL_arraysize(exts));
  SDL_Vulkan_GetInstanceExtensions(window, &n_ext, exts);
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
    app_info.apiVersion = SDL_max(app_info.apiVersion, api_version);
  const VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                    .pApplicationInfo = &app_info,
                                    .enabledExtensionCount = n_ext,
                                    .ppEnabledExtensionNames = exts};
  if (vkCreateInstance(&ici, NULL, &vk.instance) != VK_SUCCESS ||
      !SDL_Vulkan_CreateSurface(window, vk.instance, &vk.surface))
    return SDL_SetError("no Vulkan instance or surface"), false;

  static const char *const device_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
  struct retro_vulkan_context ctx = {0};
  if (vk.negotiation && vk.negotiation->create_device) {
    const VkPhysicalDeviceFeatures none = {0};
    if (!vk.negotiation->create_device(&ctx, vk.instance, VK_NULL_HANDLE, vk.surface, vkGetInstanceProcAddr,
                                       (const char **)device_exts, 1, NULL, 0, &none))
      return SDL_SetError("the core made no Vulkan device"), false;
  } else {
    /* Ours: the first GPU's first queue that draws and presents */
    uint32_t n = 1, families = 8;
    VkQueueFamilyProperties qf[8];
    vkEnumeratePhysicalDevices(vk.instance, &n, &ctx.gpu);
    if (n == 0)
      return SDL_SetError("no Vulkan device"), false;
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
      return SDL_SetError("no Vulkan queue for the window"), false;
    vkGetDeviceQueue(ctx.device, ctx.queue_family_index, 0, &ctx.queue);
  }
  vk.gpu = ctx.gpu;
  vk.device = ctx.device;
  vk.queue = ctx.queue;
  vk.family = ctx.queue_family_index;

  /* BGRA8 if there is one; immediate or mailbox: the pace is the audio's */
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
    return SDL_SetError("no swapchain"), false;

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
  vk.queue_lock = SDL_CreateMutex();

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

/* The core's image onto the next swapchain image at x, y, w, h; the core's
 * command buffers and semaphores go with it */
static void vk_present(int x, int y, int w, int h) {
  const uint32_t f = vk.index;
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
  SDL_LockMutex(vk.queue_lock);
  vkQueueSubmit(vk.queue, 1, &submit, vk.fence[f]);
  r = vkQueuePresentKHR(vk.queue, &present);
  SDL_UnlockMutex(vk.queue_lock);
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
  return (retro_proc_address_t)SDL_GL_GetProcAddress(sym);
}

/* The fbo's storage for frames up to w x h; before the context: noted */
static void hw_resize(unsigned w, unsigned h) {
  max_w = SDL_max(max_w, w);
  max_h = SDL_max(max_h, h);
  if (gl_context == NULL)
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

/* Window and context for SET_HW_RENDER's core, then its context_reset */
static bool hw_init(const char *title, int w, int h, Uint32 flags) {
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,
                      hw.context_type == RETRO_HW_CONTEXT_OPENGLES_VERSION ? (int)hw.version_major
                      : hw.context_type == RETRO_HW_CONTEXT_OPENGLES3      ? 3
                                                                           : 2);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,
                      hw.context_type == RETRO_HW_CONTEXT_OPENGLES_VERSION ? (int)hw.version_minor : 0);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
  /* Opaque: Weston can scan it out instead of compositing it */
  SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);
  window = SDL_CreateWindow(title, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, w, h,
                            flags | SDL_WINDOW_OPENGL);
  if (window == NULL || (gl_context = SDL_GL_CreateContext(window)) == NULL)
    return false;
  /* The pace is the audio's; and a hidden window would block a swap */
  SDL_GL_SetSwapInterval(0);
#define GL(name) (*(void **)&gl.name = SDL_GL_GetProcAddress("gl" #name)) != NULL
  if (!(GL(GenFramebuffers) && GL(BindFramebuffer) && GL(GenRenderbuffers) && GL(BindRenderbuffer) &&
        GL(RenderbufferStorage) && GL(FramebufferRenderbuffer) && GL(BlitFramebuffer) &&
        GL(ClearColor) && GL(Clear) && GL(ColorMask) && GL(Disable) && GL(Enable) &&
        GL(IsEnabled) && GL(GetIntegerv) && GL(GetBooleanv)))
    return SDL_SetError("no GLES 3 functions"), false;
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
static void hw_present(int x, int y, int w, int h, int win_h) {
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
  SDL_GL_SwapWindow(window);
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
    case RETRO_PIXEL_FORMAT_XRGB8888: texture_format = SDL_PIXELFORMAT_XRGB8888; return true;
    case RETRO_PIXEL_FORMAT_RGB565: texture_format = SDL_PIXELFORMAT_RGB565; return true;
    case RETRO_PIXEL_FORMAT_0RGB1555: texture_format = SDL_PIXELFORMAT_XRGB1555; return true;
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
    if (host_published)
      publish_host();
    load_options();
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
    /* The ROM's own directory unless told: BIOS files and the like go next to it */
    static char dir[2][512];
    const bool system = cmd == RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY;
    const char *env = getenv(system ? "COREHOST_SYSTEM" : "COREHOST_SAVE");
    snprintf(dir[system], sizeof(dir[system]), "%s", env != NULL ? env : stem);
    char *slash = strrchr(dir[system], '/');
    if (env == NULL && slash != NULL)
      *slash = '\0';
    *(const char **)data = dir[system];
    return true;
  }
  case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    for (const struct retro_input_descriptor *d = data; d->description != NULL; d++)
      if (d->port == 0 && d->device == RETRO_DEVICE_JOYPAD && d->id < 16) {
        described |= 1u << d->id;
        snprintf(described_as[d->id], APPCTL_TEXT, "%s", d->description);
      }
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

/* Save RAM: in from STEM.srm, or else STEM.sav */
static void load_sram(void) {
  uint8_t *ram = core.retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
  const size_t room = core.retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
  if (ram == NULL || room == 0)
    return;
  char path[600];
  size_t size = 0;
  snprintf(path, sizeof(path), "%s.srm", stem);
  uint8_t *data = read_all(path, &size);
  if (data == NULL) {
    snprintf(path, sizeof(path), "%s.sav", stem);
    data = read_all(path, &size);
  }
  if (data != NULL) {
    memcpy(ram, data, SDL_min(size, room));
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
    memcpy(sram_shadow, ram, SDL_min(size, sram_shadow_size));
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
static size_t state_size, rewind_slots, rewind_head, rewind_count;

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

/* The window re-mapped: the kiosk shell puts what maps on top */
static void raise_window(void) {
  SDL_HideWindow(window);
  SDL_ShowWindow(window);
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
    SDL_PauseAudioDevice(audio, 1);
    SDL_ClearQueuedAudio(audio);
    /* Home may be the last the game sees for a while */
    flush_sram();
    break;
  case APPCTL_RESUME:
    *paused = false;
    if (m.arg)
      raise_window();
    /* Presses that were the launcher's */
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
    SDL_PauseAudioDevice(audio, 0);
    break;
  case APPCTL_SAVE_STATE: m.result = save_state(m.arg); break;
  case APPCTL_LOAD_STATE:
    m.result = load_state(m.arg);
    rewind_count = 0;
    break;
  case APPCTL_RESET: core.retro_reset(); break;
  case APPCTL_OPTIONS:
    options_changed = true;
    save_options();
    if (texture != NULL)
      SDL_SetTextureScaleMode(texture, host_value(HOST_FILTER) ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
    break;
  default: m.result = -ENOSYS;
  }
  send(ctl, &m, sizeof(m), MSG_NOSIGNAL);
  return true;
}

static void present(void) {
  int w, h;
  if (vk_on)
    w = (int)vk.extent.width, h = (int)vk.extent.height;
  else if (hw_on)
    SDL_GL_GetDrawableSize(window, &w, &h);
  else
    SDL_GetRendererOutputSize(renderer, &w, &h);
  SDL_Rect dst = {0, 0, w, h};
  if (frame_w > 0 && frame_h > 0) {
    /* Integer scaling goes by the height: N64's 640x240 is a 4:3 picture */
    const float shown_w = aspect > 0 ? frame_h * aspect : frame_w;
    const float fit = SDL_min(w / shown_w, (float)h / frame_h);
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
  if (hw_on) {
    hw_present(dst.x, dst.y, dst.w, dst.h, h);
    return;
  }
  SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
  SDL_RenderClear(renderer);
  if (texture != NULL && frame_w > 0 && frame_h > 0) {
    SDL_SetTextureScaleMode(texture, host_value(HOST_FILTER) ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
    SDL_RenderCopy(renderer, texture, NULL, &dst);
  }
  SDL_RenderPresent(renderer);
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
    close(mem);
    if (map != MAP_FAILED) {
      ctl = sock;
      shm = map;
    }
  }
  unsetenv(APPCTL_ENV);
  /* Alone: the same table, ours only */
  if (shm == NULL && (shm = calloc(1, sizeof(*shm))) == NULL)
    return false;
  shm->magic = APPCTL_MAGIC;
  shm->version = APPCTL_VERSION;
  shm->slots = 1;
  return true;
}

int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: %s CORE.so ROM\n", argv[0]);
    return 2;
  }
  const char *rom = argv[2];
  core_path = argv[1];
  snprintf(stem, sizeof(stem), "%s", rom);
  char *dot = strrchr(stem, '.');
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
  const size_t len = strlen(opt_path);
  snprintf(opt_path + len, sizeof(opt_path) - len, "/%s.opt", info.library_name);

  /* A core that registers no options still gets ours */
  struct retro_variable none = {NULL, NULL};
  environment(RETRO_ENVIRONMENT_SET_VARIABLES, &none);
  core.retro_set_environment(environment);
  core.retro_set_video_refresh(video);
  core.retro_set_audio_sample(audio_sample);
  core.retro_set_audio_sample_batch(audio_batch);
  core.retro_set_input_poll(input_poll);
  core.retro_set_input_state(input_state);

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
    fprintf(stderr, "corehost: %s\n", SDL_GetError());
    return 1;
  }
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
  publish_host();
  load_options();
  rewind_init();

  struct retro_system_av_info av;
  core.retro_get_system_av_info(&av);
  fps = av.timing.fps > 0 ? av.timing.fps : 60;
  aspect = av.geometry.aspect_ratio;
  sample_rate = (int)(av.timing.sample_rate + 0.5);

  const Uint32 flags = SDL_WINDOW_RESIZABLE | (ctl >= 0 ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
  if (vk_on) {
    if (!vk_init(info.library_name, av.geometry.base_width * 3, av.geometry.base_height * 3, flags)) {
      fprintf(stderr, "corehost: Vulkan: %s\n", SDL_GetError());
      return 1;
    }
  } else if (hw_on) {
    hw_resize(av.geometry.max_width, av.geometry.max_height);
    if (!hw_init(info.library_name, av.geometry.base_width * 3, av.geometry.base_height * 3, flags)) {
      fprintf(stderr, "corehost: GLES: %s\n", SDL_GetError());
      return 1;
    }
  } else {
    window = SDL_CreateWindow(info.library_name, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                              av.geometry.base_width * 3, av.geometry.base_height * 3, flags);
    renderer = window ? SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED) : NULL;
  }
  if (window == NULL || (!hw_on && renderer == NULL)) {
    fprintf(stderr, "corehost: %s\n", SDL_GetError());
    return 1;
  }
  SDL_AudioSpec want = {.freq = sample_rate, .format = AUDIO_S16SYS, .channels = 2, .samples = 512}, got;
  audio = SDL_OpenAudioDevice(NULL, 0, &want, &got, 0);
  if (audio != 0)
    SDL_PauseAudioDevice(audio, 0);
  /* Three frames of sound queued, at most: the pace and the latency */
  const Uint32 queue_max = (Uint32)(sample_rate / fps * 4 * 3);

  bool paused = false, running = true, fast = false;
  unsigned frame = 0;
  Uint64 next = SDL_GetPerformanceCounter(), flushed = SDL_GetTicks64();
  const Uint64 frame_ticks = (Uint64)(SDL_GetPerformanceFrequency() / fps);
  while (running) {
    SDL_Event ev;
    /* Paused: wake for the launcher now and then, and for the window */
    if (paused && SDL_WaitEventTimeout(&ev, 50))
      SDL_PushEvent(&ev);
    while (SDL_PollEvent(&ev)) {
      if (ev.type == SDL_QUIT)
        running = false;
      else if (ev.type == SDL_CONTROLLERDEVICEADDED && pad == NULL)
        pad = SDL_GameControllerOpen(ev.cdevice.which);
    }
    if (ctl >= 0 && !control(&paused))
      running = false;
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
      save_state(0);
    if ((pressed & (1u << HOT_LOAD)) && load_state(0) == 0)
      rewind_count = 0;
    const bool rewinding = active & (1u << HOT_REWIND);
    muted = fast || rewinding;

    for (int i = 0; i < (fast && !rewinding ? FAST_SPEED : 1); i++) {
      if (rewinding)
        rewind_pop();
      else if (++frame % REWIND_EVERY == 0)
        rewind_push();
      core.retro_run();
    }
    present();
    if (audio != 0 && !muted) {
      while (SDL_GetQueuedAudioSize(audio) > queue_max)
        SDL_Delay(1);
    } else {
      next += frame_ticks;
      const Uint64 now = SDL_GetPerformanceCounter();
      if (next > now)
        SDL_Delay((Uint32)((next - now) * 1000 / SDL_GetPerformanceFrequency()));
      else
        next = now;
    }
    /* A crash costs at most this much play */
    if (SDL_GetTicks64() - flushed > 10000) {
      flush_sram();
      flushed = SDL_GetTicks64();
    }
  }
  flush_sram();
  /* RetroArch's order, which cores count on: PPSSPP's unload frees what its
   * context_destroy uses */
  if (vk_on && vk.device != VK_NULL_HANDLE)
    vkDeviceWaitIdle(vk.device);
  if (hw_on && hw.context_destroy != NULL)
    hw.context_destroy();
  core.retro_unload_game();
  core.retro_deinit();
  free(data);
  SDL_Quit();
  return 0;
}
