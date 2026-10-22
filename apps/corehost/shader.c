/*
 * A preset's passes: each draws a quad with its shaders into an image of its
 * own (the last straight into the window at the game's rectangle, unless it
 * has a scale of its own: then blitted there), sampling what the semantics
 * name: Original (the game's frame), Source (the pass before), OriginalHistoryN
 * (N frames ago), PassOutputN and the passes' aliases, the preset's textures
 * (LUTs). Uniforms by name too, in the UBO or the push constants: MVP, the
 * *Size vec4s (w, h, 1/w, 1/h), FrameCount, FrameDirection, Rotation, the
 * parameters. No PassFeedback.
 *
 * tools/slangc made each X.slang into X.slang.vert.spv, X.slang.frag.spv and
 * X.slang.meta (its pragmas); the bindings and uniforms are read out of the
 * SPIR-V here (its names, decorations and types), where RetroArch has
 * SPIRV-Cross.
 */
#include "shader.h"

#include <math.h>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "border_frag.h"
#include "border_vert.h"
#include "cfg.h"
#include "str.h"
#include "util.h"

#define PASSES 16
#define BINDINGS 16
#define UNIFORMS 128
#define PARAMS 64
#define LUTS 8
#define HISTORY 8
#define TARGETS 8

/* What a sampler binding or a uniform is */
enum { T_ORIGINAL, T_SOURCE, T_HISTORY, T_PASS, T_LUT };
enum {
  U_MVP, U_SIZE, U_FINAL_VIEWPORT, U_FRAME_COUNT, U_FRAME_DIRECTION, U_ROTATION, U_ONE, U_ASPECT, U_PARAM,
  U_ZERO
};

typedef struct {
  uint32_t binding;
  int kind, index; /* T_*; the history frame, pass, LUT */
} texture_use;

typedef struct {
  bool push;
  uint32_t offset, size;
  int kind, which, index; /* U_*; U_SIZE: a T_* texture or -1 output, -2 original */
} uniform_use;

typedef struct {
  VkImage image;
  VkDeviceMemory memory;
  VkImageView view;
  uint32_t w, h;
  VkFormat format;
} image;

typedef struct {
  char alias[64];
  bool linear;
  VkSamplerAddressMode wrap;
  int scale_type[2]; /* 0 source, 1 viewport, 2 absolute; -1 unset */
  float scale[2];
  uint32_t frame_count_mod;
  VkFormat format;
  bool direct; /* the last, into the window */
  VkRenderPass render_pass;
  VkPipelineLayout layout;
  VkPipeline pipeline;
  VkDescriptorSetLayout set_layout;
  VkDescriptorSet sets[SHADER_FRAMES];
  VkBuffer ubo;
  VkDeviceMemory ubo_memory;
  uint8_t *ubo_map;
  uint32_t ubo_binding, ubo_size, ubo_stride;
  bool has_ubo;
  uint32_t push_size;
  texture_use textures[BINDINGS];
  int n_textures;
  uniform_use uniforms[UNIFORMS];
  int n_uniforms;
  image out;
  VkFramebuffer framebuffer;
} pass;

typedef struct {
  char name[64];
  image img;
  bool linear;
  VkSamplerAddressMode wrap;
} lut;

/* The device, and what the preset and the overlay both draw with: the quad,
 * the window's render pass and its images' framebuffers */
static struct {
  shader_vk vk;
  VkPhysicalDeviceMemoryProperties mem;
  VkDeviceSize ubo_align;
  VkBuffer vbo;
  VkDeviceMemory vbo_memory;
  VkRenderPass target_pass;
  struct {
    VkImage image;
    VkImageView view;
    VkFramebuffer framebuffer;
  } targets[TARGETS];
} ctx;

static struct {
  bool loaded;
  pass passes[PASSES];
  int n_passes;
  shader_param params[PARAMS];
  int n_params;
  lut luts[LUTS];
  int n_luts;
  image history[HISTORY];
  int n_history, history_head;
  bool history_clear;
  VkSampler samplers[2][4];
  VkDescriptorPool pool;
  /* Original's view when the input has none */
  VkImage input_image;
  VkImageView input_view;
} sh;

static bool fail(const char *what, const char *path) {
  fprintf(stderr, "corehost: shader: %s%s%s\n", what, path ? ": " : "", path ? path : "");
  return false;
}

/* ---- SPIR-V: the bindings, blocks and members by name ---- */

enum { SPV_NAME = 5, SPV_MEMBER_NAME = 6, SPV_INT = 21, SPV_FLOAT = 22, SPV_VECTOR = 23, SPV_MATRIX = 24,
       SPV_STRUCT = 30, SPV_POINTER = 32, SPV_VARIABLE = 59, SPV_DECORATE = 71, SPV_MEMBER_DECORATE = 72 };
enum { SPV_BINDING = 33, SPV_OFFSET = 35 };
enum { SPV_UNIFORM_CONSTANT = 0, SPV_UNIFORM = 2, SPV_PUSH_CONSTANT = 9 };

typedef struct {
  char name[64];
  uint32_t offset, size;
} member;

typedef struct {
  struct {
    char name[64];
    uint32_t binding;
  } samplers[BINDINGS];
  int n_samplers;
  bool has_ubo;
  uint32_t ubo_binding;
  member ubo[UNIFORMS], push[UNIFORMS];
  int n_ubo, n_push;
} reflection;

typedef struct {
  const uint32_t *w;
  size_t n;
} spirv;

static uint32_t type_size(const spirv *s, const uint32_t *def_at[], uint32_t id) {
  const uint32_t *op = def_at[id];
  if (op == NULL)
    return 0;
  switch (op[0] & 0xffff) {
  case SPV_INT:
  case SPV_FLOAT:
    return op[2] / 8;
  case SPV_VECTOR:
    return type_size(s, def_at, op[2]) * op[3];
  case SPV_MATRIX: /* columns of vec4 at most: std140 rounds them to 16 */
    return 16 * op[3];
  default:
    return 0;
  }
}

/* A block's members: names, offsets, sizes */
static int block_members(const spirv *s, const uint32_t *def_at[], uint32_t type, member *out) {
  const uint32_t *st = def_at[type];
  if (st == NULL || (st[0] & 0xffff) != SPV_STRUCT)
    return 0;
  const int count = MIN((int)(st[0] >> 16) - 2, UNIFORMS);
  for (int m = 0; m < count; m++)
    out[m] = (member){.size = type_size(s, def_at, st[2 + m])};
  for (size_t i = 5; i < s->n;) {
    const uint32_t *op = s->w + i, len = op[0] >> 16;
    if (len == 0)
      break;
    if ((op[0] & 0xffff) == SPV_MEMBER_NAME && op[1] == type && (int)op[2] < count)
      snprintf(out[op[2]].name, sizeof(out[0].name), "%s", (const char *)(op + 3));
    if ((op[0] & 0xffff) == SPV_MEMBER_DECORATE && op[1] == type && (int)op[2] < count && op[3] == SPV_OFFSET)
      out[op[2]].offset = op[4];
    i += len;
  }
  return count;
}

static bool reflect(const spirv *s, reflection *r) {
  if (s->n < 5 || s->w[0] != 0x07230203)
    return false;
  const uint32_t bound = s->w[3];
  const uint32_t **def_at = calloc(bound, sizeof(*def_at));
  const char **names = calloc(bound, sizeof(*names));
  uint32_t *binding = calloc(bound, sizeof(*binding));
  if (def_at == NULL || names == NULL || binding == NULL)
    return free(def_at), free(names), free(binding), false;
  for (size_t i = 5; i < s->n;) {
    const uint32_t *op = s->w + i, len = op[0] >> 16, code = op[0] & 0xffff;
    if (len == 0 || i + len > s->n)
      break;
    if (code == SPV_NAME && op[1] < bound)
      names[op[1]] = (const char *)(op + 2);
    else if (code == SPV_DECORATE && op[2] == SPV_BINDING && op[1] < bound)
      binding[op[1]] = op[3];
    else if ((code == SPV_INT || code == SPV_FLOAT || code == SPV_VECTOR || code == SPV_MATRIX ||
              code == SPV_STRUCT || code == SPV_POINTER) && op[1] < bound)
      def_at[op[1]] = op;
    i += len;
  }
  for (size_t i = 5; i < s->n;) {
    const uint32_t *op = s->w + i, len = op[0] >> 16;
    if (len == 0)
      break;
    if ((op[0] & 0xffff) == SPV_VARIABLE && op[2] < bound && op[1] < bound && def_at[op[1]] != NULL) {
      const uint32_t var = op[2], storage = op[3], pointee = def_at[op[1]][3];
      if (storage == SPV_UNIFORM_CONSTANT && names[var] && r->n_samplers < BINDINGS) {
        snprintf(r->samplers[r->n_samplers].name, 64, "%s", names[var]);
        r->samplers[r->n_samplers++].binding = binding[var];
      } else if (storage == SPV_UNIFORM) {
        r->has_ubo = true;
        r->ubo_binding = binding[var];
        r->n_ubo = block_members(s, def_at, pointee, r->ubo);
      } else if (storage == SPV_PUSH_CONSTANT) {
        r->n_push = block_members(s, def_at, pointee, r->push);
      }
    }
    i += len;
  }
  free(def_at), free(names), free(binding);
  return true;
}

/* ---- Vulkan helpers ---- */

static void set_device(const shader_vk *vk) {
  ctx.vk = *vk;
  vkGetPhysicalDeviceMemoryProperties(vk->gpu, &ctx.mem);
  VkPhysicalDeviceProperties props;
  vkGetPhysicalDeviceProperties(vk->gpu, &props);
  ctx.ubo_align = MAX(props.limits.minUniformBufferOffsetAlignment, 16);
}

static VkDeviceMemory memory_for(VkMemoryRequirements req, VkMemoryPropertyFlags props) {
  for (uint32_t i = 0; i < ctx.mem.memoryTypeCount; i++)
    if ((req.memoryTypeBits >> i & 1) && (ctx.mem.memoryTypes[i].propertyFlags & props) == props) {
      const VkMemoryAllocateInfo info = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                         .allocationSize = req.size,
                                         .memoryTypeIndex = i};
      VkDeviceMemory m;
      return vkAllocateMemory(ctx.vk.device, &info, NULL, &m) == VK_SUCCESS ? m : VK_NULL_HANDLE;
    }
  return VK_NULL_HANDLE;
}

static bool buffer_new(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer *b, VkDeviceMemory *m, void **map) {
  const VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage};
  if (vkCreateBuffer(ctx.vk.device, &info, NULL, b) != VK_SUCCESS)
    return false;
  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(ctx.vk.device, *b, &req);
  *m = memory_for(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  return *m != VK_NULL_HANDLE && vkBindBufferMemory(ctx.vk.device, *b, *m, 0) == VK_SUCCESS &&
         vkMapMemory(ctx.vk.device, *m, 0, size, 0, map) == VK_SUCCESS;
}

static void image_free(image *img) {
  vkDestroyImageView(ctx.vk.device, img->view, NULL);
  vkDestroyImage(ctx.vk.device, img->image, NULL);
  vkFreeMemory(ctx.vk.device, img->memory, NULL);
  *img = (image){0};
}

static bool image_new(image *img, uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage) {
  image_free(img);
  const VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                  .imageType = VK_IMAGE_TYPE_2D,
                                  .format = format,
                                  .extent = {w, h, 1},
                                  .mipLevels = 1,
                                  .arrayLayers = 1,
                                  .samples = VK_SAMPLE_COUNT_1_BIT,
                                  .tiling = VK_IMAGE_TILING_OPTIMAL,
                                  .usage = usage};
  if (vkCreateImage(ctx.vk.device, &info, NULL, &img->image) != VK_SUCCESS)
    return false;
  VkMemoryRequirements req;
  vkGetImageMemoryRequirements(ctx.vk.device, img->image, &req);
  img->memory = memory_for(req, 0);
  const VkImageViewCreateInfo view = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                      .image = img->image,
                                      .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                      .format = format,
                                      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
  img->w = w, img->h = h, img->format = format;
  return img->memory != VK_NULL_HANDLE && vkBindImageMemory(ctx.vk.device, img->image, img->memory, 0) == VK_SUCCESS &&
         vkCreateImageView(ctx.vk.device, &view, NULL, &img->view) == VK_SUCCESS;
}

static void barrier(VkCommandBuffer cmd, VkImage img, VkImageLayout from, VkImageLayout to) {
  const VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                  .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                                  .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                                  .oldLayout = from,
                                  .newLayout = to,
                                  .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                  .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                  .image = img,
                                  .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
                       NULL, 1, &b);
}

/* One color attachment. The window's: loaded, TRANSFER_DST_OPTIMAL before and
 * after; a pass's: whole, then sampled. */
static VkRenderPass render_pass_new(VkFormat format, bool target) {
  const VkAttachmentDescription color = {
      .format = format,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .loadOp = target ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
      .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .initialLayout = target ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
      .finalLayout = target ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  const VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  const VkSubpassDescription sub = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    .colorAttachmentCount = 1,
                                    .pColorAttachments = &ref};
  /* After whatever came before (the pass before, last frame's reads), before
   * whatever comes after */
  const VkSubpassDependency deps[2] = {
      {VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
       VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
       VK_ACCESS_MEMORY_WRITE_BIT,
       VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT, 0},
      {0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
       VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, 0},
  };
  const VkRenderPassCreateInfo info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                       .attachmentCount = 1,
                                       .pAttachments = &color,
                                       .subpassCount = 1,
                                       .pSubpasses = &sub,
                                       .dependencyCount = 2,
                                       .pDependencies = deps};
  VkRenderPass rp;
  return vkCreateRenderPass(ctx.vk.device, &info, NULL, &rp) == VK_SUCCESS ? rp : VK_NULL_HANDLE;
}

static VkFramebuffer framebuffer_new(VkRenderPass rp, VkImageView view, uint32_t w, uint32_t h) {
  const VkFramebufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                        .renderPass = rp,
                                        .attachmentCount = 1,
                                        .pAttachments = &view,
                                        .width = w,
                                        .height = h,
                                        .layers = 1};
  VkFramebuffer fb;
  return vkCreateFramebuffer(ctx.vk.device, &info, NULL, &fb) == VK_SUCCESS ? fb : VK_NULL_HANDLE;
}

/* The quad: Position -1..1, TexCoord 0..1 (RetroArch's offscreen one; the
 * last pass gets the same, its viewport the game's rectangle), and the
 * window's render pass; once */
static bool shared_new(void) {
  static const float quad[] = {-1, -1, 0, 1, 0, 0, -1, 1, 0, 1, 0, 1, 1, -1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 1};
  void *map;
  if (ctx.vbo == VK_NULL_HANDLE) {
    if (!buffer_new(sizeof(quad), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &ctx.vbo, &ctx.vbo_memory, &map))
      return false;
    memcpy(map, quad, sizeof(quad));
  }
  if (ctx.target_pass == VK_NULL_HANDLE)
    ctx.target_pass = render_pass_new(ctx.vk.target_format, true);
  return ctx.target_pass != VK_NULL_HANDLE;
}

/* A command buffer of its own, waited for: the LUTs' uploads */
static bool submit_once(void (*record)(VkCommandBuffer, void *), void *arg) {
  const VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                             .queueFamilyIndex = ctx.vk.family};
  VkCommandPool pool;
  if (vkCreateCommandPool(ctx.vk.device, &pool_info, NULL, &pool) != VK_SUCCESS)
    return false;
  const VkCommandBufferAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                             .commandPool = pool,
                                             .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                             .commandBufferCount = 1};
  VkCommandBuffer cmd;
  bool ok = vkAllocateCommandBuffers(ctx.vk.device, &alloc, &cmd) == VK_SUCCESS;
  if (ok) {
    const VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cmd, &begin);
    record(cmd, arg);
    vkEndCommandBuffer(cmd);
    const VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
                                 .pCommandBuffers = &cmd};
    pthread_mutex_lock(ctx.vk.queue_lock);
    ok = vkQueueSubmit(ctx.vk.queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS && vkQueueWaitIdle(ctx.vk.queue) == VK_SUCCESS;
    pthread_mutex_unlock(ctx.vk.queue_lock);
  }
  vkDestroyCommandPool(ctx.vk.device, pool, NULL);
  return ok;
}

/* ---- The preset ---- */

typedef struct {
  str key, value;
} kv;

typedef struct {
  str text;
  kv items[512];
  int n;
} keyfile;

static bool keyfile_read(keyfile *k, const char *path) {
  FILE *f = fopen(path, "r");
  if (f == NULL)
    return false;
  k->text = str_read(f); /* closes f */
  k->n = 0;
  cfg_reader r = cfg_init(k->text);
  for (cfg_event e; (e = cfg_next(&r)) != CFG_END && k->n < (int)COUNTOF(k->items);) {
    if (e != CFG_VALUE)
      continue;
    str v = r.value;
    if (v.n >= 2 && v.p[0] == '"' && v.p[v.n - 1] == '"')
      v = str_head(str_at(v, 1), v.n - 2);
    k->items[k->n++] = (kv){r.key, v};
  }
  return true;
}

static str get(const keyfile *k, str key) {
  for (int j = 0; j < k->n; j++)
    if (str_eq(k->items[j].key, key))
      return k->items[j].value;
  return (str){0};
}

/* "shader3", "filter_linear3": a key of pass i */
static str get_n(const keyfile *k, const char *prefix, int i) {
  STRBUF(64) key = {0};
  sb_fmt(key, "%s%d", prefix, i);
  return get(k, sb_str(key));
}

/* "SamplerLUT1_linear": a key of a texture */
static str get_of(const keyfile *k, str name, str suffix) {
  STRBUF(96) key;
  sb_set(key, name, suffix);
  return get(k, sb_str(key));
}

static bool is_true(str v) { return str_eq(v, S("true")) || str_eq(v, S("1")); }

static float to_float(str v, float def) {
  char buf[32];
  if (v.n == 0 || str_copy(buf, sizeof(buf), v) != v.n)
    return def;
  return strtof(buf, NULL);
}

static VkSamplerAddressMode wrap_mode(str v) {
  if (str_eq(v, S("clamp_to_edge")))
    return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  if (str_eq(v, S("repeat")))
    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
  if (str_eq(v, S("mirrored_repeat")))
    return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
  return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
}

static int scale_type(str v) {
  if (str_eq(v, S("source")))
    return 0;
  if (str_eq(v, S("viewport")))
    return 1;
  if (str_eq(v, S("absolute")))
    return 2;
  return -1;
}

static VkFormat pragma_format(str v) {
  static const struct {
    const char *name;
    VkFormat format;
  } formats[] = {
      {"R8G8B8A8_UNORM", VK_FORMAT_R8G8B8A8_UNORM}, {"R8G8B8A8_SRGB", VK_FORMAT_R8G8B8A8_SRGB},
      {"A2B10G10R10_UNORM_PACK32", VK_FORMAT_A2B10G10R10_UNORM_PACK32},
      {"R16G16B16A16_SFLOAT", VK_FORMAT_R16G16B16A16_SFLOAT}, {"R16G16_SFLOAT", VK_FORMAT_R16G16_SFLOAT},
      {"R16_SFLOAT", VK_FORMAT_R16_SFLOAT}, {"R32G32B32A32_SFLOAT", VK_FORMAT_R32G32B32A32_SFLOAT},
      {"R8_UNORM", VK_FORMAT_R8_UNORM}, {"R8G8_UNORM", VK_FORMAT_R8G8_UNORM},
  };
  for (size_t i = 0; i < COUNTOF(formats); i++)
    if (str_eq(v, str_c(formats[i].name)))
      return formats[i].format;
  return VK_FORMAT_UNDEFINED;
}

/* Drawn into and sampled here, else RGBA8 */
static VkFormat usable(VkFormat f) {
  VkFormatProperties p;
  vkGetPhysicalDeviceFormatProperties(ctx.vk.gpu, f, &p);
  const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
  return (p.optimalTilingFeatures & need) == need ? f : VK_FORMAT_R8G8B8A8_UNORM;
}

static bool read_spirv(const char *path, spirv *s) {
  FILE *f = fopen(path, "rb");
  if (f == NULL)
    return fail("no shader", path);
  const str data = str_read(f); /* closes f */
  s->w = (const uint32_t *)data.p;
  s->n = data.n / 4;
  return s->n > 5 || fail("not SPIR-V", path);
}

static VkShaderModule module_new(const spirv *s) {
  const VkShaderModuleCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                         .codeSize = s->n * 4,
                                         .pCode = s->w};
  VkShaderModule m;
  return vkCreateShaderModule(ctx.vk.device, &info, NULL, &m) == VK_SUCCESS ? m : VK_NULL_HANDLE;
}

/* The meta file's parameters, added once each; its name and format */
static void read_meta(const char *path, VkFormat *format, str *name) {
  keyfile *k = malloc(sizeof(*k));
  if (k == NULL || !keyfile_read(k, path))
    return free(k);
  cfg_reader r = cfg_init(k->text);
  shader_param *param = NULL;
  for (cfg_event e; (e = cfg_next(&r)) != CFG_END;) {
    if (e == CFG_SECTION) {
      param = NULL;
      if (r.section.n > 10 && str_eq(str_head(r.section, 10), S("parameter ")) && sh.n_params < PARAMS) {
        const str pname = str_trim(str_at(r.section, 10));
        bool seen = false;
        for (int i = 0; i < sh.n_params; i++)
          seen |= str_eq(str_c(sh.params[i].name), pname);
        if (!seen) {
          param = &sh.params[sh.n_params++];
          *param = (shader_param){0};
          str_copy(param->name, sizeof(param->name), pname);
        }
      }
      continue;
    }
    if (r.section.n == 0 && str_eq(r.key, S("format")))
      *format = pragma_format(r.value);
    else if (r.section.n == 0 && str_eq(r.key, S("name")) && name->n == 0)
      *name = str_dup(r.value);
    else if (param && str_eq(r.key, S("desc")))
      str_copy(param->desc, sizeof(param->desc), r.value);
    else if (param && str_eq(r.key, S("default")))
      param->def = param->value = to_float(r.value, 0);
    else if (param && str_eq(r.key, S("min")))
      param->min = to_float(r.value, 0);
    else if (param && str_eq(r.key, S("max")))
      param->max = to_float(r.value, 0);
    else if (param && str_eq(r.key, S("step")))
      param->step = to_float(r.value, 0);
  }
  free((void *)k->text.p);
  free(k);
}

/* What a sampler's name refers to */
static bool texture_of(const char *name, int pass_index, texture_use *t) {
  int n;
  char rest;
  if (strcmp(name, "Original") == 0)
    return t->kind = T_ORIGINAL, true;
  if (strcmp(name, "Source") == 0)
    return t->kind = T_SOURCE, true;
  if (sscanf(name, "OriginalHistory%d%c", &n, &rest) == 1 && n >= 0 && n <= HISTORY) {
    if (n == 0)
      return t->kind = T_ORIGINAL, true;
    t->kind = T_HISTORY, t->index = n;
    sh.n_history = MAX(sh.n_history, n);
    return true;
  }
  if (sscanf(name, "PassOutput%d%c", &n, &rest) == 1 && n >= 0 && n < pass_index)
    return t->kind = T_PASS, t->index = n, true;
  for (int i = 0; i < pass_index; i++)
    if (sh.passes[i].alias[0] && strcmp(name, sh.passes[i].alias) == 0)
      return t->kind = T_PASS, t->index = i, true;
  for (int i = 0; i < sh.n_luts; i++)
    if (strcmp(name, sh.luts[i].name) == 0)
      return t->kind = T_LUT, t->index = i, true;
  return false;
}

/* What a uniform's name refers to */
static void uniform_of(const char *name, int pass_index, uniform_use *u) {
  texture_use t;
  char tex[64];
  const size_t n = strlen(name);
  u->kind = U_ZERO;
  if (strcmp(name, "MVP") == 0)
    u->kind = U_MVP;
  else if (strcmp(name, "OutputSize") == 0)
    u->kind = U_SIZE, u->which = -1;
  else if (strcmp(name, "FinalViewportSize") == 0)
    u->kind = U_FINAL_VIEWPORT;
  else if (strcmp(name, "FrameCount") == 0)
    u->kind = U_FRAME_COUNT;
  else if (strcmp(name, "FrameDirection") == 0)
    u->kind = U_FRAME_DIRECTION;
  else if (strcmp(name, "Rotation") == 0)
    u->kind = U_ROTATION;
  else if (strcmp(name, "TotalSubFrames") == 0 || strcmp(name, "CurrentSubFrame") == 0)
    u->kind = U_ONE;
  else if (strcmp(name, "OriginalAspect") == 0 || strcmp(name, "OriginalAspectRotated") == 0)
    u->kind = U_ASPECT;
  else if (n > 4 && strcmp(name + n - 4, "Size") == 0 && n - 4 < sizeof(tex) &&
           (snprintf(tex, sizeof(tex), "%.*s", (int)(n - 4), name), texture_of(tex, pass_index, &t)))
    u->kind = U_SIZE, u->which = t.kind, u->index = t.index;
  for (int i = 0; u->kind == U_ZERO && i < sh.n_params; i++)
    if (strcmp(name, sh.params[i].name) == 0)
      u->kind = U_PARAM, u->index = i;
}

static void path_join(char *out, size_t size, const char *dir, str rel) {
  snprintf(out, size, "%s/%.*s", dir, (int)rel.n, rel.p);
}

typedef struct {
  VkBuffer buf;
  image *img;
  VkImageLayout layout;
} upload;

static void record_upload(VkCommandBuffer cmd, void *arg) {
  const upload *up = arg;
  barrier(cmd, up->img->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  const VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                  .imageExtent = {up->img->w, up->img->h, 1}};
  vkCmdCopyBufferToImage(cmd, up->buf, up->img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
  barrier(cmd, up->img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, up->layout);
}

/* RGBA8, left in `layout`; premultiplied: what is transparent is black */
static bool load_png(image *img, const char *path, VkImageUsageFlags usage, VkImageLayout layout, bool premultiply) {
  png_image png = {.version = PNG_IMAGE_VERSION};
  if (!png_image_begin_read_from_file(&png, path))
    return fail("can't read texture", path);
  png.format = PNG_FORMAT_RGBA;
  const size_t size = PNG_IMAGE_SIZE(png);
  VkBuffer buf;
  VkDeviceMemory mem;
  void *map;
  bool ok = buffer_new(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &buf, &mem, &map) &&
            png_image_finish_read(&png, NULL, map, 0, NULL) &&
            image_new(img, png.width, png.height, VK_FORMAT_R8G8B8A8_UNORM, usage | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
  png_image_free(&png);
  for (uint8_t *px = map; ok && premultiply && px < (uint8_t *)map + size; px += 4)
    for (int c = 0; c < 3; c++)
      px[c] = (uint8_t)(px[c] * px[3] / 255);
  upload up = {buf, img, layout};
  ok = ok && submit_once(record_upload, &up);
  vkDestroyBuffer(ctx.vk.device, buf, NULL);
  vkFreeMemory(ctx.vk.device, mem, NULL);
  return ok || fail("can't load texture", path);
}

static void add_uniforms(pass *p, const member *m, int n, bool push, int index) {
  for (int i = 0; i < n && p->n_uniforms < UNIFORMS; i++) {
    bool dup = false;
    for (int j = 0; j < p->n_uniforms; j++)
      dup |= p->uniforms[j].push == push && p->uniforms[j].offset == m[i].offset;
    if (dup || m[i].size == 0)
      continue;
    uniform_use *u = &p->uniforms[p->n_uniforms++];
    *u = (uniform_use){.push = push, .offset = m[i].offset, .size = m[i].size};
    uniform_of(m[i].name, index, u);
    if (push)
      p->push_size = MAX(p->push_size, m[i].offset + m[i].size);
    else
      p->ubo_size = MAX(p->ubo_size, m[i].offset + m[i].size);
  }
}

/* A quad's pipeline: Position, TexCoord in; viewport and scissor set when drawn */
static VkPipeline pipeline_new(VkShaderModule vm, VkShaderModule fm, VkPipelineLayout layout,
                               VkRenderPass render_pass, bool blend) {
  const VkPipelineShaderStageCreateInfo stages[2] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
       .module = vm, .pName = "main"},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
       .module = fm, .pName = "main"}};
  /* Position (vec4), TexCoord (vec2) */
  const VkVertexInputBindingDescription vb = {0, 6 * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX};
  const VkVertexInputAttributeDescription va[2] = {{0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0},
                                                    {1, 0, VK_FORMAT_R32G32_SFLOAT, 4 * sizeof(float)}};
  const VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                                   .vertexBindingDescriptionCount = 1,
                                                   .pVertexBindingDescriptions = &vb,
                                                   .vertexAttributeDescriptionCount = 2,
                                                   .pVertexAttributeDescriptions = va};
  const VkPipelineInputAssemblyStateCreateInfo ia = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP};
  const VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                                .viewportCount = 1,
                                                .scissorCount = 1};
  const VkPipelineRasterizationStateCreateInfo rs = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL,
      .cullMode = VK_CULL_MODE_NONE,
      .lineWidth = 1};
  const VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                   .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
  /* Blended: premultiplied alpha over what is there */
  const VkPipelineColorBlendAttachmentState over = {
      .blendEnable = blend,
      .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
      .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
      .colorBlendOp = VK_BLEND_OP_ADD,
      .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
      .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
      .alphaBlendOp = VK_BLEND_OP_ADD,
      .colorWriteMask = 0xf};
  const VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                                  .attachmentCount = 1,
                                                  .pAttachments = &over};
  const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  const VkPipelineDynamicStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                               .dynamicStateCount = 2,
                                               .pDynamicStates = dyn};
  const VkGraphicsPipelineCreateInfo gpi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                            .stageCount = 2,
                                            .pStages = stages,
                                            .pVertexInputState = &vi,
                                            .pInputAssemblyState = &ia,
                                            .pViewportState = &vp,
                                            .pRasterizationState = &rs,
                                            .pMultisampleState = &ms,
                                            .pColorBlendState = &cb,
                                            .pDynamicState = &ds,
                                            .layout = layout,
                                            .renderPass = render_pass};
  VkPipeline pipeline;
  return vkCreateGraphicsPipelines(ctx.vk.device, VK_NULL_HANDLE, 1, &gpi, NULL, &pipeline) == VK_SUCCESS
             ? pipeline
             : VK_NULL_HANDLE;
}

/* The pass's shaders, bindings, uniforms and pipeline */
static bool load_pass(int index, const char *shader_path) {
  pass *p = &sh.passes[index];
  char path[1024];
  spirv vs = {0}, fs = {0};
  snprintf(path, sizeof(path), "%s.meta", shader_path);
  str name = {0};
  VkFormat format = VK_FORMAT_UNDEFINED;
  read_meta(path, &format, &name);
  /* The preset's float or sRGB, else the shader's #pragma format */
  p->format = usable(p->format != VK_FORMAT_UNDEFINED ? p->format
                     : format != VK_FORMAT_UNDEFINED  ? format
                                                      : VK_FORMAT_R8G8B8A8_UNORM);
  if (p->alias[0] == '\0' && name.n > 0)
    str_copy(p->alias, sizeof(p->alias), name);
  free((void *)name.p);
  snprintf(path, sizeof(path), "%s.vert.spv", shader_path);
  bool ok = read_spirv(path, &vs);
  snprintf(path, sizeof(path), "%s.frag.spv", shader_path);
  ok = ok && read_spirv(path, &fs);
  reflection *r = calloc(2, sizeof(*r));
  ok = ok && r != NULL && ((reflect(&vs, &r[0]) && reflect(&fs, &r[1])) || fail("bad SPIR-V", shader_path));
  for (int s = 0; ok && s < 2; s++) {
    if (r[s].has_ubo)
      p->has_ubo = true, p->ubo_binding = r[s].ubo_binding;
    add_uniforms(p, r[s].ubo, r[s].n_ubo, false, index);
    add_uniforms(p, r[s].push, r[s].n_push, true, index);
    for (int i = 0; i < r[s].n_samplers; i++) {
      texture_use t = {.binding = r[s].samplers[i].binding};
      bool dup = false;
      for (int j = 0; j < p->n_textures; j++)
        dup |= p->textures[j].binding == t.binding;
      if (dup)
        continue;
      if (!texture_of(r[s].samplers[i].name, index, &t) || p->n_textures >= BINDINGS) {
        ok = fail("unknown texture", r[s].samplers[i].name);
        break;
      }
      p->textures[p->n_textures++] = t;
    }
  }
  free(r);

  /* The set: the UBO, then the samplers */
  VkDescriptorSetLayoutBinding b[BINDINGS + 1];
  uint32_t nb = 0;
  if (p->has_ubo)
    b[nb++] = (VkDescriptorSetLayoutBinding){p->ubo_binding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                                             VK_SHADER_STAGE_ALL_GRAPHICS, NULL};
  for (int i = 0; i < p->n_textures; i++)
    b[nb++] = (VkDescriptorSetLayoutBinding){p->textures[i].binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                             VK_SHADER_STAGE_ALL_GRAPHICS, NULL};
  const VkDescriptorSetLayoutCreateInfo sli = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                               .bindingCount = nb,
                                               .pBindings = b};
  ok = ok && vkCreateDescriptorSetLayout(ctx.vk.device, &sli, NULL, &p->set_layout) == VK_SUCCESS;
  const VkPushConstantRange push = {VK_SHADER_STAGE_ALL_GRAPHICS, 0, p->push_size};
  const VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                          .setLayoutCount = 1,
                                          .pSetLayouts = &p->set_layout,
                                          .pushConstantRangeCount = p->push_size > 0,
                                          .pPushConstantRanges = &push};
  ok = ok && vkCreatePipelineLayout(ctx.vk.device, &pli, NULL, &p->layout) == VK_SUCCESS;
  if (ok && p->has_ubo) {
    p->ubo_stride = (MAX(p->ubo_size, 16) + ctx.ubo_align - 1) / ctx.ubo_align * ctx.ubo_align;
    ok = buffer_new(p->ubo_stride * SHADER_FRAMES, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &p->ubo, &p->ubo_memory,
                    (void **)&p->ubo_map);
  }
  p->render_pass = p->direct ? VK_NULL_HANDLE : render_pass_new(p->format, false);
  ok = ok && (p->direct || p->render_pass != VK_NULL_HANDLE);

  const VkShaderModule vm = ok ? module_new(&vs) : VK_NULL_HANDLE, fm = ok ? module_new(&fs) : VK_NULL_HANDLE;
  ok = ok && vm != VK_NULL_HANDLE && fm != VK_NULL_HANDLE &&
       (p->pipeline = pipeline_new(vm, fm, p->layout, p->direct ? ctx.target_pass : p->render_pass, false)) !=
           VK_NULL_HANDLE;
  vkDestroyShaderModule(ctx.vk.device, vm, NULL);
  vkDestroyShaderModule(ctx.vk.device, fm, NULL);
  free((void *)vs.w), free((void *)fs.w);
  return ok || fail("can't make the pass", shader_path);
}

bool shader_load(const shader_vk *vk, const char *preset, bool smooth) {
  shader_unload();
  set_device(vk);

  keyfile *k = malloc(sizeof(*k));
  if (k == NULL || !keyfile_read(k, preset))
    return free(k), fail("can't read preset", preset);
  char dir[512];
  snprintf(dir, sizeof(dir), "%s", preset);
  char *slash = strrchr(dir, '/');
  if (slash != NULL)
    *slash = '\0';
  else
    snprintf(dir, sizeof(dir), ".");

  /* Samplers: nearest and linear, each of the wrap modes */
  static const VkSamplerAddressMode wraps[4] = {
      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT};
  bool ok = true;
  for (int l = 0; l < 2; l++)
    for (int w = 0; w < 4; w++) {
      const VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                      .magFilter = l ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                                      .minFilter = l ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                                      .addressModeU = wraps[w],
                                      .addressModeV = wraps[w],
                                      .addressModeW = wraps[w],
                                      .maxLod = 0,
                                      .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK};
      ok = ok && vkCreateSampler(ctx.vk.device, &si, NULL, &sh.samplers[l][w]) == VK_SUCCESS;
    }

  ok = ok && shared_new();

  /* The textures first: the passes' bindings name them */
  str names = get(k, S("textures")), name;
  while (ok && cfg_item(&names, &name) && sh.n_luts < LUTS) {
    lut *l = &sh.luts[sh.n_luts++];
    str_copy(l->name, sizeof(l->name), name);
    char path[1024];
    path_join(path, sizeof(path), dir, get(k, name));
    l->linear = is_true(get_of(k, name, S("_linear")));
    l->wrap = wrap_mode(get_of(k, name, S("_wrap_mode")));
    ok = load_png(&l->img, path, VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false);
  }

  sh.n_passes = CLAMP((int)to_float(get(k, S("shaders")), 0), 0, PASSES);
  ok = ok && (sh.n_passes > 0 || fail("no passes", preset));
  for (int i = 0; ok && i < sh.n_passes; i++) {
    pass *p = &sh.passes[i];
    const str linear = get_n(k, "filter_linear", i);
    p->linear = linear.n > 0 ? is_true(linear) : smooth;
    p->wrap = wrap_mode(get_n(k, "wrap_mode", i));
    str_copy(p->alias, sizeof(p->alias), get_n(k, "alias", i));
    const int both = scale_type(get_n(k, "scale_type", i));
    p->scale_type[0] = both >= 0 ? both : scale_type(get_n(k, "scale_type_x", i));
    p->scale_type[1] = both >= 0 ? both : scale_type(get_n(k, "scale_type_y", i));
    const float s = to_float(get_n(k, "scale", i), 1);
    p->scale[0] = to_float(get_n(k, "scale_x", i), s);
    p->scale[1] = to_float(get_n(k, "scale_y", i), s);
    p->frame_count_mod = (uint32_t)to_float(get_n(k, "frame_count_mod", i), 0);
    p->format = is_true(get_n(k, "float_framebuffer", i))   ? VK_FORMAT_R16G16B16A16_SFLOAT
                : is_true(get_n(k, "srgb_framebuffer", i)) ? VK_FORMAT_R8G8B8A8_SRGB
                                                                    : VK_FORMAT_UNDEFINED;
    /* The last into the window, unless it has a size of its own */
    const bool last = i == sh.n_passes - 1;
    p->direct = last && ((p->scale_type[0] < 0 && p->scale_type[1] < 0) ||
                         (p->scale_type[0] == 1 && p->scale_type[1] == 1 && p->scale[0] == 1 && p->scale[1] == 1));
    if (last && p->scale_type[0] < 0 && p->scale_type[1] < 0)
      p->scale_type[0] = p->scale_type[1] = 1;
    for (int a = 0; a < 2; a++)
      if (p->scale_type[a] < 0)
        p->scale_type[a] = 0;
    char path[1024];
    path_join(path, sizeof(path), dir, get_n(k, "shader", i));
    ok = load_pass(i, path);
  }
  /* The preset's values over the parameters' defaults */
  for (int i = 0; ok && i < sh.n_params; i++)
    sh.params[i].value = to_float(get(k, str_c(sh.params[i].name)), sh.params[i].def);

  /* The sets, a frame in flight each */
  uint32_t samplers = 0;
  for (int i = 0; i < sh.n_passes; i++)
    samplers += sh.passes[i].n_textures;
  const VkDescriptorPoolSize sizes[2] = {
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, (uint32_t)sh.n_passes * SHADER_FRAMES},
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX(samplers, 1) * SHADER_FRAMES}};
  const VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                          .maxSets = (uint32_t)sh.n_passes * SHADER_FRAMES,
                                          .poolSizeCount = 2,
                                          .pPoolSizes = sizes};
  ok = ok && vkCreateDescriptorPool(ctx.vk.device, &dpi, NULL, &sh.pool) == VK_SUCCESS;
  for (int i = 0; ok && i < sh.n_passes; i++) {
    VkDescriptorSetLayout layouts[SHADER_FRAMES];
    for (int f = 0; f < SHADER_FRAMES; f++)
      layouts[f] = sh.passes[i].set_layout;
    const VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                            .descriptorPool = sh.pool,
                                            .descriptorSetCount = SHADER_FRAMES,
                                            .pSetLayouts = layouts};
    ok = vkAllocateDescriptorSets(ctx.vk.device, &ai, sh.passes[i].sets) == VK_SUCCESS;
  }
  free((void *)k->text.p);
  free(k);
  sh.history_clear = true;
  sh.loaded = ok;
  if (!ok)
    shader_unload();
  return ok;
}

void shader_forget_targets(void) {
  /* Before any preset: nothing made, no device to destroy with */
  if (ctx.vk.device == VK_NULL_HANDLE)
    return;
  for (int i = 0; i < TARGETS; i++) {
    vkDestroyFramebuffer(ctx.vk.device, ctx.targets[i].framebuffer, NULL);
    vkDestroyImageView(ctx.vk.device, ctx.targets[i].view, NULL);
    ctx.targets[i].image = VK_NULL_HANDLE, ctx.targets[i].view = VK_NULL_HANDLE,
    ctx.targets[i].framebuffer = VK_NULL_HANDLE;
  }
}

void shader_unload(void) {
  if (ctx.vk.device == VK_NULL_HANDLE)
    return;
  vkDeviceWaitIdle(ctx.vk.device);
  for (int i = 0; i < PASSES; i++) {
    pass *p = &sh.passes[i];
    vkDestroyFramebuffer(ctx.vk.device, p->framebuffer, NULL);
    image_free(&p->out);
    vkDestroyPipeline(ctx.vk.device, p->pipeline, NULL);
    vkDestroyPipelineLayout(ctx.vk.device, p->layout, NULL);
    vkDestroyDescriptorSetLayout(ctx.vk.device, p->set_layout, NULL);
    vkDestroyRenderPass(ctx.vk.device, p->render_pass, NULL);
    vkDestroyBuffer(ctx.vk.device, p->ubo, NULL);
    vkFreeMemory(ctx.vk.device, p->ubo_memory, NULL);
    *p = (pass){0};
  }
  for (int i = 0; i < LUTS; i++)
    image_free(&sh.luts[i].img);
  for (int i = 0; i < HISTORY; i++)
    image_free(&sh.history[i]);
  for (int l = 0; l < 2; l++)
    for (int w = 0; w < 4; w++)
      vkDestroySampler(ctx.vk.device, sh.samplers[l][w], NULL), sh.samplers[l][w] = VK_NULL_HANDLE;
  vkDestroyDescriptorPool(ctx.vk.device, sh.pool, NULL);
  vkDestroyImageView(ctx.vk.device, sh.input_view, NULL);
  memset(&sh, 0, sizeof(sh));
}

bool shader_loaded(void) { return sh.loaded; }

void shader_forget_input(void) {
  if (sh.input_view != VK_NULL_HANDLE)
    vkDestroyImageView(ctx.vk.device, sh.input_view, NULL);
  sh.input_view = VK_NULL_HANDLE;
  sh.input_image = VK_NULL_HANDLE;
}
int shader_param_count(void) { return sh.n_params; }
shader_param *shader_params(void) { return sh.params; }

static int wrap_index(VkSamplerAddressMode m) {
  switch (m) {
  case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE: return 1;
  case VK_SAMPLER_ADDRESS_MODE_REPEAT: return 2;
  case VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT: return 3;
  default: return 0;
  }
}

/* A texture's view, layout, size and sampler as pass p sees it */
typedef struct {
  VkImageView view;
  VkImageLayout layout;
  uint32_t w, h;
  VkSampler sampler;
} bound;

static bound texture_for(int p, int kind, int index, const shader_input *in, VkImageView in_view,
                         VkImageLayout in_layout) {
  /* Original and history: pass 0's filter; a pass's output: the next one's */
  const pass *first = &sh.passes[0];
  VkSampler first_sampler = sh.samplers[first->linear][wrap_index(first->wrap)];
  switch (kind) {
  case T_SOURCE:
    if (p > 0) {
      const pass *src = &sh.passes[p - 1];
      return (bound){src->out.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, src->out.w, src->out.h,
                     sh.samplers[sh.passes[p].linear][wrap_index(sh.passes[p].wrap)]};
    }
    /* fall through: pass 0's Source is Original */
  case T_ORIGINAL:
    return (bound){in_view, in_layout, in->w, in->h, first_sampler};
  case T_HISTORY: {
    const image *h = &sh.history[(sh.history_head + index - 1) % sh.n_history];
    return (bound){h->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, h->w, h->h, first_sampler};
  }
  case T_PASS: {
    const pass *src = &sh.passes[index], *next = &sh.passes[index + 1];
    return (bound){src->out.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, src->out.w, src->out.h,
                   sh.samplers[next->linear][wrap_index(next->wrap)]};
  }
  default: {
    const lut *l = &sh.luts[index];
    return (bound){l->img.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, l->img.w, l->img.h,
                   sh.samplers[l->linear][wrap_index(l->wrap)]};
  }
  }
}

static void put_size(uint8_t *at, uint32_t w, uint32_t h) {
  const float v[4] = {(float)w, (float)h, 1.0f / (float)MAX(w, 1), 1.0f / (float)MAX(h, 1)};
  memcpy(at, v, sizeof(v));
}

/* Made anew when the size or format changed: the GPU may still use the old */
static bool ensure(image *img, uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage) {
  if (img->image != VK_NULL_HANDLE && img->w == w && img->h == h && img->format == format)
    return true;
  vkDeviceWaitIdle(ctx.vk.device);
  return image_new(img, w, h, format, usage);
}

static VkFramebuffer target_framebuffer(VkImage target, VkExtent2D size) {
  int free_at = -1;
  for (int i = 0; i < TARGETS; i++) {
    if (ctx.targets[i].image == target)
      return ctx.targets[i].framebuffer;
    if (ctx.targets[i].image == VK_NULL_HANDLE && free_at < 0)
      free_at = i;
  }
  if (free_at < 0)
    return VK_NULL_HANDLE;
  const VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                    .image = target,
                                    .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                    .format = ctx.vk.target_format,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
  if (vkCreateImageView(ctx.vk.device, &vi, NULL, &ctx.targets[free_at].view) != VK_SUCCESS)
    return VK_NULL_HANDLE;
  ctx.targets[free_at].image = target;
  ctx.targets[free_at].framebuffer = framebuffer_new(ctx.target_pass, ctx.targets[free_at].view, size.width, size.height);
  return ctx.targets[free_at].framebuffer;
}

void shader_draw(VkCommandBuffer cmd, uint32_t frame, const shader_input *in, VkImage target, VkExtent2D target_size,
                 VkRect2D rect, uint32_t frame_count) {
  if (!sh.loaded || in->w == 0 || in->h == 0 || rect.extent.width == 0 || rect.extent.height == 0)
    return;
  frame %= SHADER_FRAMES;
  /* Original's view: the core's, or ours of its image with alpha 1 */
  VkImageView in_view = in->view;
  if (in_view == VK_NULL_HANDLE) {
    if (sh.input_image != in->image) {
      vkDeviceWaitIdle(ctx.vk.device);
      vkDestroyImageView(ctx.vk.device, sh.input_view, NULL);
      const VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                        .image = in->image,
                                        .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                        .format = in->format,
                                        .components = {.a = VK_COMPONENT_SWIZZLE_ONE},
                                        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
      sh.input_image = vkCreateImageView(ctx.vk.device, &vi, NULL, &sh.input_view) == VK_SUCCESS ? in->image
                                                                                                  : VK_NULL_HANDLE;
    }
    in_view = sh.input_view;
  }
  if (in_view == VK_NULL_HANDLE)
    return;
  const VkImageLayout in_layout = in->layout;

  /* History: N frames, cleared the first time */
  for (int i = 0; i < sh.n_history; i++)
    if (sh.history[i].w != in->w || sh.history[i].h != in->h || sh.history[i].format != in->format) {
      if (!ensure(&sh.history[i], in->w, in->h, in->format,
                  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))
        return;
      sh.history_clear = true;
    }
  if (sh.history_clear) {
    const VkClearColorValue black = {0};
    const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    for (int i = 0; i < sh.n_history; i++) {
      barrier(cmd, sh.history[i].image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
      vkCmdClearColorImage(cmd, sh.history[i].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
      barrier(cmd, sh.history[i].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    sh.history_clear = false;
  }

  /* The passes' sizes: of the one before (Original first), of the viewport, or as given */
  uint32_t src_w = in->w, src_h = in->h;
  for (int i = 0; i < sh.n_passes; i++) {
    pass *p = &sh.passes[i];
    uint32_t size[2];
    for (int a = 0; a < 2; a++) {
      const float base = p->scale_type[a] == 0   ? (float)(a ? src_h : src_w)
                         : p->scale_type[a] == 1 ? (float)(a ? rect.extent.height : rect.extent.width)
                                                 : 1.0f;
      size[a] = (uint32_t)MAX(1.0f, roundf(base * p->scale[a]));
    }
    if (p->direct) {
      size[0] = rect.extent.width, size[1] = rect.extent.height;
    } else if (p->out.w != size[0] || p->out.h != size[1] || p->framebuffer == VK_NULL_HANDLE) {
      vkDeviceWaitIdle(ctx.vk.device);
      vkDestroyFramebuffer(ctx.vk.device, p->framebuffer, NULL);
      p->framebuffer = VK_NULL_HANDLE;
      if (!image_new(&p->out, size[0], size[1], p->format,
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ||
          (p->framebuffer = framebuffer_new(p->render_pass, p->out.view, size[0], size[1])) == VK_NULL_HANDLE)
        return;
    }
    p->out.w = size[0], p->out.h = size[1];
    src_w = size[0], src_h = size[1];
  }
  const VkFramebuffer target_fb = sh.passes[sh.n_passes - 1].direct ? target_framebuffer(target, target_size)
                                                                    : VK_NULL_HANDLE;
  if (sh.passes[sh.n_passes - 1].direct && target_fb == VK_NULL_HANDLE)
    return;

  for (int i = 0; i < sh.n_passes; i++) {
    pass *p = &sh.passes[i];
    /* The uniforms */
    uint8_t *ubo = p->has_ubo ? p->ubo_map + frame * p->ubo_stride : NULL;
    uint8_t push[128] = {0};
    if (ubo != NULL)
      memset(ubo, 0, p->ubo_size);
    for (int u = 0; u < p->n_uniforms; u++) {
      const uniform_use *use = &p->uniforms[u];
      uint8_t *at = use->push ? push + use->offset : ubo + use->offset;
      if (use->push ? use->offset + use->size > sizeof(push) : ubo == NULL)
        continue;
      switch (use->kind) {
      case U_MVP: {
        static const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        memcpy(at, identity, MIN(use->size, sizeof(identity)));
        break;
      }
      case U_SIZE:
        if (use->which == -1) {
          put_size(at, p->out.w, p->out.h);
        } else {
          const bound b = texture_for(i, use->which, use->index, in, in_view, in_layout);
          put_size(at, b.w, b.h);
        }
        break;
      case U_FINAL_VIEWPORT:
        put_size(at, rect.extent.width, rect.extent.height);
        break;
      case U_FRAME_COUNT: {
        const uint32_t n = p->frame_count_mod ? frame_count % p->frame_count_mod : frame_count;
        memcpy(at, &n, 4);
        break;
      }
      case U_FRAME_DIRECTION: {
        const int32_t one = 1;
        memcpy(at, &one, 4);
        break;
      }
      case U_ONE: {
        const uint32_t one = 1;
        memcpy(at, &one, 4);
        break;
      }
      case U_ASPECT: {
        const float a = (float)in->w / (float)in->h;
        memcpy(at, &a, 4);
        break;
      }
      case U_PARAM:
        memcpy(at, &sh.params[use->index].value, 4);
        break;
      default: /* U_ROTATION, U_ZERO: 0 */
        break;
      }
    }
    /* The set */
    VkDescriptorImageInfo images[BINDINGS];
    VkWriteDescriptorSet writes[BINDINGS + 1];
    uint32_t nw = 0;
    const VkDescriptorBufferInfo ubo_info = {p->ubo, frame * p->ubo_stride, MAX(p->ubo_size, 16)};
    if (p->has_ubo)
      writes[nw++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                            .dstSet = p->sets[frame],
                                            .dstBinding = p->ubo_binding,
                                            .descriptorCount = 1,
                                            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                            .pBufferInfo = &ubo_info};
    for (int t = 0; t < p->n_textures; t++) {
      const bound b = texture_for(i, p->textures[t].kind, p->textures[t].index, in, in_view, in_layout);
      images[t] = (VkDescriptorImageInfo){b.sampler, b.view, b.layout};
      writes[nw++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                            .dstSet = p->sets[frame],
                                            .dstBinding = p->textures[t].binding,
                                            .descriptorCount = 1,
                                            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                            .pImageInfo = &images[t]};
    }
    vkUpdateDescriptorSets(ctx.vk.device, nw, writes, 0, NULL);

    /* The draw */
    const VkRect2D area = p->direct ? rect : (VkRect2D){{0, 0}, {p->out.w, p->out.h}};
    const VkRenderPassBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = p->direct ? ctx.target_pass : p->render_pass,
        .framebuffer = p->direct ? target_fb : p->framebuffer,
        .renderArea = p->direct ? (VkRect2D){{0, 0}, target_size} : area};
    vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport viewport = {(float)area.offset.x, (float)area.offset.y, (float)area.extent.width,
                                 (float)area.extent.height, 0, 1};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &area);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->layout, 0, 1, &p->sets[frame], 0, NULL);
    if (p->push_size > 0)
      vkCmdPushConstants(cmd, p->layout, VK_SHADER_STAGE_ALL_GRAPHICS, 0, MIN(p->push_size, sizeof(push)), push);
    const VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &ctx.vbo, &zero);
    vkCmdDraw(cmd, 4, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
  }

  /* A last pass with a size of its own: blitted to the rectangle */
  pass *last = &sh.passes[sh.n_passes - 1];
  if (!last->direct) {
    barrier(cmd, last->out.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    const VkImageBlit blit = {
        .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .srcOffsets = {{0, 0, 0}, {(int32_t)last->out.w, (int32_t)last->out.h, 1}},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .dstOffsets = {{rect.offset.x, rect.offset.y, 0},
                       {rect.offset.x + (int32_t)rect.extent.width, rect.offset.y + (int32_t)rect.extent.height, 1}}};
    vkCmdBlitImage(cmd, last->out.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, last->linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    barrier(cmd, last->out.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  }

  /* This frame into the history, for the next: the oldest goes */
  if (sh.n_history > 0) {
    const int next = (sh.history_head + sh.n_history - 1) % sh.n_history;
    image *h = &sh.history[next];
    const VkImageLayout copy_from = in_layout == VK_IMAGE_LAYOUT_GENERAL ? in_layout : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    if (copy_from != in_layout)
      barrier(cmd, in->image, in_layout, copy_from);
    barrier(cmd, h->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkImageCopy copy = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                              .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                              .extent = {in->w, in->h, 1}};
    vkCmdCopyImage(cmd, in->image, copy_from, h->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barrier(cmd, h->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (copy_from != in_layout)
      barrier(cmd, in->image, copy_from, in_layout);
    sh.history_head = next;
  }
}

/* ---- The overlay: an image over the window, the game under it ---- */

static struct {
  image img;
  bool loaded, has_viewport;
  float viewport[4];
  VkSampler sampler;
  VkDescriptorSetLayout set_layout;
  VkPipelineLayout layout;
  VkPipeline pipeline;
  VkDescriptorPool pool;
  VkDescriptorSet set;
} ov;

/* The image's pipeline and its set */
static bool border_pipeline(void) {
  const VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                  .magFilter = VK_FILTER_LINEAR,
                                  .minFilter = VK_FILTER_LINEAR,
                                  .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                  .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                  .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
  const VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                          VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
  const VkDescriptorSetLayoutCreateInfo sli = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                               .bindingCount = 1,
                                               .pBindings = &b};
  const VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
  const VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                          .maxSets = 1,
                                          .poolSizeCount = 1,
                                          .pPoolSizes = &size};
  if (vkCreateSampler(ctx.vk.device, &si, NULL, &ov.sampler) != VK_SUCCESS ||
      vkCreateDescriptorSetLayout(ctx.vk.device, &sli, NULL, &ov.set_layout) != VK_SUCCESS ||
      vkCreateDescriptorPool(ctx.vk.device, &dpi, NULL, &ov.pool) != VK_SUCCESS)
    return false;
  const VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                          .setLayoutCount = 1,
                                          .pSetLayouts = &ov.set_layout};
  const VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                          .descriptorPool = ov.pool,
                                          .descriptorSetCount = 1,
                                          .pSetLayouts = &ov.set_layout};
  if (vkCreatePipelineLayout(ctx.vk.device, &pli, NULL, &ov.layout) != VK_SUCCESS ||
      vkAllocateDescriptorSets(ctx.vk.device, &ai, &ov.set) != VK_SUCCESS)
    return false;
  const VkDescriptorImageInfo info = {ov.sampler, ov.img.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  const VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                  .dstSet = ov.set,
                                  .dstBinding = 0,
                                  .descriptorCount = 1,
                                  .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                  .pImageInfo = &info};
  vkUpdateDescriptorSets(ctx.vk.device, 1, &w, 0, NULL);
  const spirv vs = {border_vert, sizeof(border_vert) / 4}, fs = {border_frag, sizeof(border_frag) / 4};
  const VkShaderModule vm = module_new(&vs), fm = module_new(&fs);
  ov.pipeline = vm != VK_NULL_HANDLE && fm != VK_NULL_HANDLE
                    ? pipeline_new(vm, fm, ov.layout, ctx.target_pass, true)
                    : VK_NULL_HANDLE;
  vkDestroyShaderModule(ctx.vk.device, vm, NULL);
  vkDestroyShaderModule(ctx.vk.device, fm, NULL);
  return ov.pipeline != VK_NULL_HANDLE;
}

bool border_load(const shader_vk *vk, const char *cfg) {
  border_unload();
  set_device(vk);
  keyfile *k = malloc(sizeof(*k));
  if (k == NULL || !keyfile_read(k, cfg))
    return free(k), fail("can't read overlay", cfg);
  char dir[512], path[1024];
  snprintf(dir, sizeof(dir), "%s", cfg);
  char *slash = strrchr(dir, '/');
  if (slash != NULL)
    *slash = '\0';
  path_join(path, sizeof(path), dir, get(k, S("overlay0_overlay")));
  /* "x,y,w,h": where the game goes, as fractions of the screen */
  char vp[64];
  const str v = get(k, S("overlay0_viewport"));
  ov.has_viewport = v.n > 0 && str_copy(vp, sizeof(vp), v) == v.n &&
                    sscanf(vp, "%f,%f,%f,%f", &ov.viewport[0], &ov.viewport[1], &ov.viewport[2], &ov.viewport[3]) == 4;
  free((void *)k->text.p);
  free(k);
  ov.loaded = shared_new() &&
              load_png(&ov.img, path, VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, true) &&
              border_pipeline();
  if (!ov.loaded)
    border_unload();
  return ov.loaded;
}

void border_unload(void) {
  if (ctx.vk.device == VK_NULL_HANDLE)
    return;
  if (ov.img.image != VK_NULL_HANDLE)
    vkDeviceWaitIdle(ctx.vk.device);
  image_free(&ov.img);
  vkDestroyPipeline(ctx.vk.device, ov.pipeline, NULL);
  vkDestroyPipelineLayout(ctx.vk.device, ov.layout, NULL);
  vkDestroyDescriptorPool(ctx.vk.device, ov.pool, NULL);
  vkDestroyDescriptorSetLayout(ctx.vk.device, ov.set_layout, NULL);
  vkDestroySampler(ctx.vk.device, ov.sampler, NULL);
  ov = (typeof(ov)){0};
}

bool border_viewport(float viewport[4]) {
  if (!ov.loaded || !ov.has_viewport)
    return false;
  memcpy(viewport, ov.viewport, sizeof(ov.viewport));
  return true;
}

void border_draw(VkCommandBuffer cmd, VkImage target, VkExtent2D size) {
  const VkFramebuffer fb = ov.loaded ? target_framebuffer(target, size) : VK_NULL_HANDLE;
  if (fb == VK_NULL_HANDLE)
    return;
  const VkRect2D area = {{0, 0}, size};
  const VkRenderPassBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                       .renderPass = ctx.target_pass,
                                       .framebuffer = fb,
                                       .renderArea = area};
  vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
  const VkViewport viewport = {0, 0, (float)size.width, (float)size.height, 0, 1};
  vkCmdSetViewport(cmd, 0, 1, &viewport);
  vkCmdSetScissor(cmd, 0, 1, &area);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ov.pipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ov.layout, 0, 1, &ov.set, 0, NULL);
  const VkDeviceSize zero = 0;
  vkCmdBindVertexBuffers(cmd, 0, 1, &ctx.vbo, &zero);
  vkCmdDraw(cmd, 4, 1, 0, 0);
  vkCmdEndRenderPass(cmd);
}
