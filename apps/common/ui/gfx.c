/*
 * Two timeline semaphores order everything: `frames` counts submitted frames
 * (any window), `uploads` counts finished texture copies. A frame waits for
 * the uploads finished so far, so a texture bound after its upload returned
 * is complete for every frame that can sample it; an image is destroyed once
 * `frames` has passed the last frame that could have used it.
 *
 * Descriptors: one set per window and frame in flight. Binding a slot bumps
 * its generation; a set picks up what changed right before its frame is
 * submitted, when its previous submission has finished with it.
 */
#include "gfx.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL_vulkan.h>

#include "util.h"
#include "quad_frag.h"
#include "quad_vert.h"

#define FRAMES 2 /* in flight, per window */
#define MAX_QUADS 4096
#define MAX_SLOTS 256 /* PanVK: maxPerStageDescriptorSampledImages */
#define MAX_TARGETS 2
#define MAX_IMAGES 8 /* swapchain */

typedef struct {
  VkCommandBuffer cmd;
  VkSemaphore acquired;
  uint64_t submitted;
  VkBuffer quads;
  gfx_quad *map;
  VkDescriptorSet set;
  uint32_t seen[MAX_SLOTS];
} frame;

struct gfx_target {
  SDL_Window *window;
  VkSurfaceKHR surface;
  VkSurfaceFormatKHR format;
  bool translucent, stale;
  VkSwapchainKHR swapchain;
  VkExtent2D extent;
  uint32_t image_count, image;
  VkImage images[MAX_IMAGES];
  VkImageView views[MAX_IMAGES];
  /* Per swapchain image: presentation may hold it past its frame */
  VkSemaphore rendered[MAX_IMAGES];
  VkPipeline pipeline;
  frame frames[FRAMES];
  uint32_t current;
};

static VkInstance instance;
static VkPhysicalDevice pdev;
static VkDevice dev;
static uint32_t graphics_family, transfer_family;
static VkQueue graphics_queue, transfer_queue;
/* One lock per VkQueue: the same one twice when there is a single queue */
/* The same one when both queues are one */
static pthread_mutex_t graphics_mutex = PTHREAD_MUTEX_INITIALIZER, transfer_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t *graphics_lock = &graphics_mutex, *transfer_lock = &transfer_mutex;
static VkSemaphore frames_sem, uploads_sem;
static uint64_t frames_submitted;
static uint64_t uploads_submitted; /* under transfer_lock */
static _Atomic uint64_t uploads_done;
static VkCommandPool pool;
static VkDescriptorSetLayout set_layout;
static VkDescriptorPool descriptor_pool;
static VkPipelineLayout layout;
static VkSampler sampler;
static uint32_t slot_count;
static VkImageView slot_view[MAX_SLOTS];
static uint32_t slot_gen[MAX_SLOTS];
static gfx_image white, placeholder;
static gfx_target targets[MAX_TARGETS];
static size_t target_count;

typedef struct {
  gfx_image image;
  uint64_t after;
} retired;
static retired *graveyard;
static size_t graveyard_count, graveyard_capacity;

/* The frame being built */
static frame *building;
static uint32_t quad_count;
static float clip[4];

static bool ok(VkResult r, const char *what) {
  if (r >= 0)
    return true;
  fprintf(stderr, "gfx: %s: %d\n", what, r);
  return false;
}

/* The device is gone or out of memory mid-frame: weston.init restarts us */
static void must(VkResult r, const char *what) {
  if (!ok(r, what))
    abort();
}

static int memory_type(uint32_t bits, VkMemoryPropertyFlags props) {
  VkPhysicalDeviceMemoryProperties mp;
  vkGetPhysicalDeviceMemoryProperties(pdev, &mp);
  for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
    if ((bits & 1u << i) && (mp.memoryTypes[i].propertyFlags & props) == props)
      return (int)i;
  return -1;
}

static VkDeviceMemory allocate(VkMemoryRequirements req,
                               VkMemoryPropertyFlags props) {
  const int type = memory_type(req.memoryTypeBits, props);
  VkMemoryAllocateInfo ai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = req.size,
      .memoryTypeIndex = (uint32_t)type,
  };
  VkDeviceMemory mem = VK_NULL_HANDLE;
  if (type < 0 || !ok(vkAllocateMemory(dev, &ai, NULL, &mem), "allocate"))
    return VK_NULL_HANDLE;
  return mem;
}

/* Host-visible and mapped for good; the memory goes with the process. */
static VkBuffer host_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                            void **map, VkDeviceMemory *memory) {
  VkBufferCreateInfo bi = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = size,
      .usage = usage,
  };
  VkBuffer buf;
  if (!ok(vkCreateBuffer(dev, &bi, NULL, &buf), "buffer"))
    return VK_NULL_HANDLE;
  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(dev, buf, &req);
  *memory = allocate(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (*memory == VK_NULL_HANDLE ||
      !ok(vkBindBufferMemory(dev, buf, *memory, 0), "bind buffer") ||
      !ok(vkMapMemory(dev, *memory, 0, size, 0, map), "map")) {
    vkDestroyBuffer(dev, buf, NULL);
    vkFreeMemory(dev, *memory, NULL);
    return VK_NULL_HANDLE;
  }
  return buf;
}

static VkImageMemoryBarrier2 image_barrier(VkImage img, VkImageLayout from,
                                           VkImageLayout to,
                                           VkPipelineStageFlags2 src_stage,
                                           VkAccessFlags2 src_access,
                                           VkPipelineStageFlags2 dst_stage,
                                           VkAccessFlags2 dst_access) {
  return (VkImageMemoryBarrier2){
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
      .srcStageMask = src_stage,
      .srcAccessMask = src_access,
      .dstStageMask = dst_stage,
      .dstAccessMask = dst_access,
      .oldLayout = from,
      .newLayout = to,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = img,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
  };
}

static void barrier(VkCommandBuffer cmd, VkImageMemoryBarrier2 b) {
  VkDependencyInfo di = {
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .imageMemoryBarrierCount = 1,
      .pImageMemoryBarriers = &b,
  };
  vkCmdPipelineBarrier2(cmd, &di);
}

static void destroy_image(gfx_image *image) {
  vkDestroyImageView(dev, image->view, NULL);
  vkDestroyImage(dev, image->image, NULL);
  vkFreeMemory(dev, image->memory, NULL);
  *image = (gfx_image){0};
}

/* Per thread: a pool is only ever used by the thread that made it. */
static _Thread_local VkCommandPool upload_pool;
static _Thread_local VkCommandBuffer upload_cmd;

bool gfx_upload(gfx_image *out, const void *rgba, int w, int h) {
  *out = (gfx_image){.w = w, .h = h};
  const uint32_t families[] = {graphics_family, transfer_family};
  const bool shared = graphics_family != transfer_family;
  VkImageCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .extent = {(uint32_t)w, (uint32_t)h, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
      /* No ownership transfers between the two queues */
      .sharingMode = shared ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE,
      .queueFamilyIndexCount = shared ? 2 : 0,
      .pQueueFamilyIndices = families,
  };
  if (!ok(vkCreateImage(dev, &ci, NULL, &out->image), "image"))
    return false;
  VkMemoryRequirements req;
  vkGetImageMemoryRequirements(dev, out->image, &req);
  out->bytes = req.size;
  out->memory = allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  VkImageViewCreateInfo vi = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = out->image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = ci.format,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
  };
  if (out->memory == VK_NULL_HANDLE ||
      !ok(vkBindImageMemory(dev, out->image, out->memory, 0), "bind image") ||
      !ok(vkCreateImageView(dev, &vi, NULL, &out->view), "view")) {
    destroy_image(out);
    return false;
  }

  const VkDeviceSize size = (VkDeviceSize)w * h * 4;
  void *map;
  VkDeviceMemory staging_mem;
  VkBuffer staging = host_buffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &map,
                                 &staging_mem);
  if (staging == VK_NULL_HANDLE) {
    destroy_image(out);
    return false;
  }
  memcpy(map, rgba, size);

  if (upload_pool == VK_NULL_HANDLE) {
    VkCommandPoolCreateInfo pi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = transfer_family,
    };
    must(vkCreateCommandPool(dev, &pi, NULL, &upload_pool), "upload pool");
    VkCommandBufferAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = upload_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    must(vkAllocateCommandBuffers(dev, &ai, &upload_cmd), "upload cmd");
  }
  vkResetCommandPool(dev, upload_pool, 0);
  VkCommandBufferBeginInfo bi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
  };
  must(vkBeginCommandBuffer(upload_cmd, &bi), "begin upload");
  barrier(upload_cmd,
          image_barrier(out->image, VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COPY_BIT,
                        VK_ACCESS_2_TRANSFER_WRITE_BIT));
  VkBufferImageCopy copy = {
      .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
      .imageExtent = {(uint32_t)w, (uint32_t)h, 1},
  };
  vkCmdCopyBufferToImage(upload_cmd, staging, out->image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
  /* The semaphore carries it to the frames: nothing to wait for here */
  barrier(upload_cmd,
          image_barrier(out->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_PIPELINE_STAGE_2_COPY_BIT,
                        VK_ACCESS_2_TRANSFER_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_NONE, 0));
  must(vkEndCommandBuffer(upload_cmd), "end upload");

  pthread_mutex_lock(transfer_lock);
  const uint64_t value = ++uploads_submitted;
  VkCommandBufferSubmitInfo cbi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
      .commandBuffer = upload_cmd,
  };
  VkSemaphoreSubmitInfo signal = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
      .semaphore = uploads_sem,
      .value = value,
      .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
  };
  VkSubmitInfo2 si = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
      .commandBufferInfoCount = 1,
      .pCommandBufferInfos = &cbi,
      .signalSemaphoreInfoCount = 1,
      .pSignalSemaphoreInfos = &signal,
  };
  must(vkQueueSubmit2(transfer_queue, 1, &si, VK_NULL_HANDLE), "upload");
  pthread_mutex_unlock(transfer_lock);

  VkSemaphoreWaitInfo wi = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
      .semaphoreCount = 1,
      .pSemaphores = &uploads_sem,
      .pValues = &value,
  };
  must(vkWaitSemaphores(dev, &wi, UINT64_MAX), "wait upload");
  /* Timelines are monotonic: the highest finished value covers the rest */
  uint64_t seen = atomic_load(&uploads_done);
  while (seen < value && !atomic_compare_exchange_weak(&uploads_done, &seen, value))
    ;
  vkDestroyBuffer(dev, staging, NULL);
  vkFreeMemory(dev, staging_mem, NULL);
  return true;
}

uint32_t gfx_slots(void) { return slot_count; }
uint64_t gfx_frame(void) { return frames_submitted; }

bool gfx_done(uint64_t value) {
  uint64_t reached = 0;
  vkGetSemaphoreCounterValue(dev, frames_sem, &reached);
  return reached >= value;
}

void gfx_bind(uint32_t slot, const gfx_image *image) {
  if (slot >= slot_count)
    return;
  slot_view[slot] = image != NULL ? image->view : placeholder.view;
  slot_gen[slot]++;
}

void gfx_release(gfx_image *image) {
  if (image->image == VK_NULL_HANDLE)
    return;
  if (graveyard_count == graveyard_capacity) {
    const size_t capacity = graveyard_capacity ? graveyard_capacity * 2 : 32;
    retired *grown = realloc(graveyard, capacity * sizeof(*grown));
    if (grown == NULL)
      abort();
    graveyard = grown;
    graveyard_capacity = capacity;
  }
  graveyard[graveyard_count++] = (retired){*image, frames_submitted};
  *image = (gfx_image){0};
}

static void bury(void) {
  for (size_t i = 0; i < graveyard_count;)
    if (gfx_done(graveyard[i].after)) {
      destroy_image(&graveyard[i].image);
      graveyard[i] = graveyard[--graveyard_count];
    } else {
      i++;
    }
}

static VkShaderModule shader(const uint32_t *code, size_t size) {
  VkShaderModuleCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = size,
      .pCode = code,
  };
  VkShaderModule m = VK_NULL_HANDLE;
  ok(vkCreateShaderModule(dev, &ci, NULL, &m), "shader");
  return m;
}

static VkSemaphore semaphore(VkSemaphoreType type) {
  VkSemaphoreTypeCreateInfo ti = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
      .semaphoreType = type,
  };
  VkSemaphoreCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
      .pNext = &ti,
  };
  VkSemaphore s;
  must(vkCreateSemaphore(dev, &ci, NULL, &s), "semaphore");
  return s;
}

/* Graphics with present, and a transfer queue of its own when there is one:
 * a transfer-only family, else another family that copies (async compute),
 * else a second queue of the graphics family. */
static bool pick_queues(VkSurfaceKHR surface, uint32_t *graphics_index,
                        uint32_t *transfer_index) {
  uint32_t n = 16;
  VkQueueFamilyProperties qf[16];
  vkGetPhysicalDeviceQueueFamilyProperties(pdev, &n, qf);
  graphics_family = UINT32_MAX;
  for (uint32_t i = 0; i < n && graphics_family == UINT32_MAX; i++) {
    VkBool32 present = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(pdev, i, surface, &present);
    if (present && (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
      graphics_family = i;
  }
  if (graphics_family == UINT32_MAX)
    return false;
  *graphics_index = *transfer_index = 0;
  transfer_family = graphics_family;
  for (uint32_t i = 0; i < n; i++)
    if ((qf[i].queueFlags & VK_QUEUE_TRANSFER_BIT) &&
        !(qf[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))) {
      transfer_family = i;
      return true;
    }
  /* Compute queues copy too, whether they say so or not */
  for (uint32_t i = 0; i < n; i++)
    if (i != graphics_family && (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
      transfer_family = i;
      return true;
    }
  if (qf[graphics_family].queueCount > 1)
    *transfer_index = 1;
  return true;
}

bool gfx_init(SDL_Window *window) {
  unsigned n_ext = 0;
  if (!SDL_Vulkan_GetInstanceExtensions(window, &n_ext, NULL))
    return false;
  const char **exts = malloc(n_ext * sizeof(*exts));
  SDL_Vulkan_GetInstanceExtensions(window, &n_ext, exts);
  VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = SDL_GetWindowTitle(window),
      .apiVersion = VK_API_VERSION_1_3,
  };
  VkInstanceCreateInfo ici = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app,
      .enabledExtensionCount = n_ext,
      .ppEnabledExtensionNames = exts,
  };
  const bool created = ok(vkCreateInstance(&ici, NULL, &instance), "instance");
  free(exts);
  if (!created)
    return false;
  /* The first window's surface picks the queue; it is kept for its target */
  VkSurfaceKHR surface;
  if (!SDL_Vulkan_CreateSurface(window, instance, &surface))
    return false;

  uint32_t n_pdev = 8;
  VkPhysicalDevice pdevs[8];
  vkEnumeratePhysicalDevices(instance, &n_pdev, pdevs);
  uint32_t graphics_index = 0, transfer_index = 0;
  for (uint32_t i = 0; i < n_pdev && dev == VK_NULL_HANDLE; i++) {
    pdev = pdevs[i];
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pdev, &props);
    VkPhysicalDeviceVulkan13Features f13 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext = &f13};
    VkPhysicalDeviceFeatures2 f = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12};
    vkGetPhysicalDeviceFeatures2(pdev, &f);
    if (props.apiVersion < VK_API_VERSION_1_3 || !f12.runtimeDescriptorArray ||
        !f12.shaderSampledImageArrayNonUniformIndexing ||
        !f12.timelineSemaphore || !f13.dynamicRendering ||
        !f13.synchronization2 ||
        !pick_queues(surface, &graphics_index, &transfer_index))
      continue;
    slot_count = MIN(MAX_SLOTS,
                         MIN(props.limits.maxPerStageDescriptorSampledImages,
                                 props.limits.maxPerStageDescriptorSamplers));
    fprintf(stderr, "gfx: %s, %u slots, transfer queue: family %u queue %u\n",
            props.deviceName, slot_count, transfer_family, transfer_index);

    /* Only what we use */
    f13 = (VkPhysicalDeviceVulkan13Features){
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .dynamicRendering = VK_TRUE,
        .synchronization2 = VK_TRUE,
    };
    f12 = (VkPhysicalDeviceVulkan12Features){
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext = &f13,
        .runtimeDescriptorArray = VK_TRUE,
        .shaderSampledImageArrayNonUniformIndexing = VK_TRUE,
        .timelineSemaphore = VK_TRUE,
    };
    const float prio[] = {1.0f, 0.5f};
    VkDeviceQueueCreateInfo qci[] = {
        {
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = graphics_family,
            .queueCount = transfer_index + 1,
            .pQueuePriorities = prio,
        },
        {
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = transfer_family,
            .queueCount = 1,
            .pQueuePriorities = &prio[1],
        },
    };
    const char *dev_ext = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &f12,
        .queueCreateInfoCount = transfer_family != graphics_family ? 2 : 1,
        .pQueueCreateInfos = qci,
        .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = &dev_ext,
    };
    if (!ok(vkCreateDevice(pdev, &dci, NULL, &dev), "device"))
      dev = VK_NULL_HANDLE;
  }
  if (dev == VK_NULL_HANDLE) {
    fprintf(stderr, "gfx: no Vulkan 1.3 device with descriptor indexing\n");
    return false;
  }
  vkGetDeviceQueue(dev, graphics_family, 0, &graphics_queue);
  vkGetDeviceQueue(dev, transfer_family, transfer_index, &transfer_queue);
  if (transfer_queue == graphics_queue)
    transfer_lock = graphics_lock;
  frames_sem = semaphore(VK_SEMAPHORE_TYPE_TIMELINE);
  uploads_sem = semaphore(VK_SEMAPHORE_TYPE_TIMELINE);

  VkCommandPoolCreateInfo cpi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
      .queueFamilyIndex = graphics_family,
  };
  must(vkCreateCommandPool(dev, &cpi, NULL, &pool), "pool");
  VkSamplerCreateInfo sci = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_LINEAR,
      .minFilter = VK_FILTER_LINEAR,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
  };
  must(vkCreateSampler(dev, &sci, NULL, &sampler), "sampler");

  VkDescriptorSetLayoutBinding bindings[] = {
      {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL},
      {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, slot_count,
       VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
  };
  VkDescriptorSetLayoutCreateInfo dslci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 2,
      .pBindings = bindings,
  };
  must(vkCreateDescriptorSetLayout(dev, &dslci, NULL, &set_layout), "set layout");
  const uint32_t sets = MAX_TARGETS * FRAMES;
  VkDescriptorPoolSize sizes[] = {
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, sets},
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, sets * slot_count},
  };
  VkDescriptorPoolCreateInfo dpci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = sets,
      .poolSizeCount = 2,
      .pPoolSizes = sizes,
  };
  must(vkCreateDescriptorPool(dev, &dpci, NULL, &descriptor_pool), "descriptor pool");
  VkPushConstantRange pcr = {VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float[2])};
  VkPipelineLayoutCreateInfo plci = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &set_layout,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges = &pcr,
  };
  must(vkCreatePipelineLayout(dev, &plci, NULL, &layout), "pipeline layout");

  /* Every slot starts on the placeholder, bound by the first frame */
  const uint32_t white_px = 0xffffffff, placeholder_px = 0xff302c2a;
  if (!gfx_upload(&white, &white_px, 1, 1) ||
      !gfx_upload(&placeholder, &placeholder_px, 1, 1))
    return false;
  for (uint32_t i = 0; i < slot_count; i++)
    gfx_bind(i, NULL);
  gfx_bind(GFX_WHITE, &white);

  targets[0].window = window;
  targets[0].surface = surface;
  return true;
}

static VkPipeline pipeline(VkFormat format) {
  VkShaderModule vert = shader(quad_vert, sizeof(quad_vert));
  VkShaderModule frag = shader(quad_frag, sizeof(quad_frag));
  VkPipelineShaderStageCreateInfo stages[] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vert, .pName = "main"},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = frag, .pName = "main"},
  };
  VkPipelineVertexInputStateCreateInfo vin = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo ia = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
  };
  VkPipelineViewportStateCreateInfo vp = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1,
      .scissorCount = 1,
  };
  VkPipelineRasterizationStateCreateInfo rs = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .cullMode = VK_CULL_MODE_NONE,
      .lineWidth = 1.0f,
  };
  VkPipelineMultisampleStateCreateInfo ms = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
  };
  /* Premultiplied */
  VkPipelineColorBlendAttachmentState blend = {
      .blendEnable = VK_TRUE,
      .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
      .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
      .colorBlendOp = VK_BLEND_OP_ADD,
      .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
      .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
      .alphaBlendOp = VK_BLEND_OP_ADD,
      .colorWriteMask = 0xf,
  };
  VkPipelineColorBlendStateCreateInfo cb = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &blend,
  };
  VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo ds = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
      .dynamicStateCount = 2,
      .pDynamicStates = dyn,
  };
  VkPipelineRenderingCreateInfo rci = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
      .colorAttachmentCount = 1,
      .pColorAttachmentFormats = &format,
  };
  VkGraphicsPipelineCreateInfo gpci = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .pNext = &rci,
      .stageCount = 2,
      .pStages = stages,
      .pVertexInputState = &vin,
      .pInputAssemblyState = &ia,
      .pViewportState = &vp,
      .pRasterizationState = &rs,
      .pMultisampleState = &ms,
      .pColorBlendState = &cb,
      .pDynamicState = &ds,
      .layout = layout,
  };
  VkPipeline p;
  must(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &p), "pipeline");
  vkDestroyShaderModule(dev, vert, NULL);
  vkDestroyShaderModule(dev, frag, NULL);
  return p;
}

static void destroy_swapchain_images(gfx_target *t) {
  for (uint32_t i = 0; i < t->image_count; i++) {
    vkDestroyImageView(dev, t->views[i], NULL);
    vkDestroySemaphore(dev, t->rendered[i], NULL);
  }
  t->image_count = 0;
}

/* Waits for this window's frames, so their images and semaphores are free */
static bool create_swapchain(gfx_target *t) {
  int w, h;
  SDL_Vulkan_GetDrawableSize(t->window, &w, &h);
  VkSurfaceCapabilitiesKHR caps;
  if (w <= 0 || h <= 0 ||
      !ok(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pdev, t->surface, &caps), "caps"))
    return false;
  t->extent = caps.currentExtent.width != UINT32_MAX
                  ? caps.currentExtent
                  : (VkExtent2D){(uint32_t)w, (uint32_t)h};
  if (t->extent.width == 0 || t->extent.height == 0)
    return false;
  for (int i = 0; i < FRAMES; i++) {
    const uint64_t value = t->frames[i].submitted;
    VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                              .semaphoreCount = 1,
                              .pSemaphores = &frames_sem,
                              .pValues = &value};
    must(vkWaitSemaphores(dev, &wi, UINT64_MAX), "wait frame");
  }
  /* Presentation may still read the old images */
  pthread_mutex_lock(graphics_lock);
  vkQueueWaitIdle(graphics_queue);
  pthread_mutex_unlock(graphics_lock);

  /* Mailbox: never waits for the compositor, which stops sending frame
   * callbacks to a hidden window. The loop paces itself. */
  uint32_t n = 8;
  VkPresentModeKHR modes[8], mode = VK_PRESENT_MODE_FIFO_KHR;
  vkGetPhysicalDeviceSurfacePresentModesKHR(pdev, t->surface, &n, modes);
  for (uint32_t i = 0; i < n; i++)
    if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR)
      mode = modes[i];
  const VkCompositeAlphaFlagBitsKHR wanted =
      t->translucent ? VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR
                     : VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  const VkCompositeAlphaFlagBitsKHR alpha =
      caps.supportedCompositeAlpha & wanted
          ? wanted
          : (VkCompositeAlphaFlagBitsKHR)(caps.supportedCompositeAlpha &
                                          -caps.supportedCompositeAlpha);
  uint32_t count = caps.minImageCount + 1;
  if (caps.maxImageCount != 0)
    count = MIN(count, caps.maxImageCount);

  VkSwapchainKHR old = t->swapchain;
  VkSwapchainCreateInfoKHR ci = {
      .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
      .surface = t->surface,
      .minImageCount = count,
      .imageFormat = t->format.format,
      .imageColorSpace = t->format.colorSpace,
      .imageExtent = t->extent,
      .imageArrayLayers = 1,
      .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
      .preTransform = caps.currentTransform,
      .compositeAlpha = alpha,
      .presentMode = mode,
      .clipped = VK_TRUE,
      .oldSwapchain = old,
  };
  const VkResult r = vkCreateSwapchainKHR(dev, &ci, NULL, &t->swapchain);
  destroy_swapchain_images(t);
  if (old != VK_NULL_HANDLE)
    vkDestroySwapchainKHR(dev, old, NULL);
  if (!ok(r, "swapchain")) {
    t->swapchain = VK_NULL_HANDLE;
    return false;
  }
  t->image_count = MAX_IMAGES;
  must(vkGetSwapchainImagesKHR(dev, t->swapchain, &t->image_count, t->images),
       "swapchain images");
  for (uint32_t i = 0; i < t->image_count; i++) {
    VkImageViewCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = t->images[i],
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = t->format.format,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    must(vkCreateImageView(dev, &vi, NULL, &t->views[i]), "swapchain view");
    t->rendered[i] = semaphore(VK_SEMAPHORE_TYPE_BINARY);
  }
  t->stale = false;
  return true;
}

gfx_target *gfx_target_create(SDL_Window *window, bool translucent) {
  gfx_target *t = NULL;
  for (size_t i = 0; i < target_count; i++)
    if (targets[i].window == window)
      t = &targets[i];
  if (t == NULL && target_count < MAX_TARGETS)
    t = &targets[target_count];
  if (t == NULL)
    return NULL;
  t->window = window;
  t->translucent = translucent;
  if (t->surface == VK_NULL_HANDLE &&
      !SDL_Vulkan_CreateSurface(window, instance, &t->surface))
    return NULL;
  VkBool32 present = VK_FALSE;
  vkGetPhysicalDeviceSurfaceSupportKHR(pdev, graphics_family, t->surface, &present);
  if (!present)
    return NULL;

  /* UNORM: colours are used as they are. BGRA (DRM ARGB8888) first: what
   * Weston and EGL use. With RGBA (ABGR8888) the H700's display planes swap
   * red and blue whenever Weston scans the buffer out directly instead of
   * compositing it: a blue focus ring flashed orange. */
  uint32_t n = 32;
  VkSurfaceFormatKHR formats[32];
  must(vkGetPhysicalDeviceSurfaceFormatsKHR(pdev, t->surface, &n, formats), "formats");
  t->format = formats[0];
  const VkFormat wanted[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};
  for (int w = COUNTOF(wanted) - 1; w >= 0; w--)
    for (uint32_t i = 0; i < n; i++)
      if (formats[i].format == wanted[w] &&
          formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        t->format = formats[i];
  t->pipeline = pipeline(t->format.format);

  VkCommandBuffer cmds[FRAMES];
  VkCommandBufferAllocateInfo cai = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = FRAMES,
  };
  must(vkAllocateCommandBuffers(dev, &cai, cmds), "command buffers");
  VkDescriptorSetLayout layouts[FRAMES];
  for (int i = 0; i < FRAMES; i++)
    layouts[i] = set_layout;
  VkDescriptorSet sets[FRAMES];
  VkDescriptorSetAllocateInfo dsai = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = descriptor_pool,
      .descriptorSetCount = FRAMES,
      .pSetLayouts = layouts,
  };
  must(vkAllocateDescriptorSets(dev, &dsai, sets), "descriptor sets");
  for (int i = 0; i < FRAMES; i++) {
    frame *f = &t->frames[i];
    f->cmd = cmds[i];
    f->set = sets[i];
    f->acquired = semaphore(VK_SEMAPHORE_TYPE_BINARY);
    VkDeviceMemory mem;
    void *map;
    f->quads = host_buffer(MAX_QUADS * sizeof(gfx_quad),
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &map, &mem);
    if (f->quads == VK_NULL_HANDLE)
      abort();
    f->map = map;
    VkDescriptorBufferInfo bi = {f->quads, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = f->set,
        .dstBinding = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &bi,
    };
    vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);
  }
  if (t == &targets[target_count])
    target_count++;
  return t;
}

void gfx_clip(float x, float y, float w, float h) {
  clip[0] = x;
  clip[1] = y;
  clip[2] = x + w;
  clip[3] = y + h;
}

void gfx_noclip(void) { gfx_clip(-1e9f, -1e9f, 2e9f, 2e9f); }

void gfx_push(gfx_quad q) {
  if (building == NULL || quad_count == MAX_QUADS)
    return;
  memcpy(q.clip, clip, sizeof(clip));
  building->map[quad_count++] = q;
}

bool gfx_begin(gfx_target *t, int *w, int *h) {
  int dw, dh;
  SDL_Vulkan_GetDrawableSize(t->window, &dw, &dh);
  if (t->swapchain == VK_NULL_HANDLE || t->stale ||
      (uint32_t)dw != t->extent.width || (uint32_t)dh != t->extent.height)
    if (!create_swapchain(t))
      return false;
  frame *f = &t->frames[t->current];
  VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                            .semaphoreCount = 1,
                            .pSemaphores = &frames_sem,
                            .pValues = &f->submitted};
  must(vkWaitSemaphores(dev, &wi, UINT64_MAX), "wait frame");
  bury();
  VkResult r = vkAcquireNextImageKHR(dev, t->swapchain, UINT64_MAX, f->acquired,
                                     VK_NULL_HANDLE, &t->image);
  if (r == VK_ERROR_OUT_OF_DATE_KHR) {
    if (!create_swapchain(t))
      return false;
    r = vkAcquireNextImageKHR(dev, t->swapchain, UINT64_MAX, f->acquired,
                              VK_NULL_HANDLE, &t->image);
  }
  if (!ok(r, "acquire")) {
    t->stale = true;
    return false;
  }
  building = f;
  quad_count = 0;
  gfx_noclip();
  *w = (int)t->extent.width;
  *h = (int)t->extent.height;
  return true;
}

/* The slots whose binding changed since this set last went out */
static void update_set(frame *f) {
  VkDescriptorImageInfo infos[MAX_SLOTS];
  VkWriteDescriptorSet writes[MAX_SLOTS];
  uint32_t n = 0;
  for (uint32_t i = 0; i < slot_count; i++) {
    if (f->seen[i] == slot_gen[i])
      continue;
    f->seen[i] = slot_gen[i];
    infos[n] = (VkDescriptorImageInfo){sampler, slot_view[i],
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    writes[n] = (VkWriteDescriptorSet){
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = f->set,
        .dstBinding = 1,
        .dstArrayElement = i,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &infos[n],
    };
    n++;
  }
  if (n > 0)
    vkUpdateDescriptorSets(dev, n, writes, 0, NULL);
}

void gfx_end(gfx_target *t) {
  frame *f = &t->frames[t->current];
  if (building != f)
    return;
  building = NULL;
  update_set(f);
  VkCommandBuffer cmd = f->cmd;
  VkCommandBufferBeginInfo bi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
  };
  must(vkBeginCommandBuffer(cmd, &bi), "begin frame");
  VkImage image = t->images[t->image];
  barrier(cmd, image_barrier(image, VK_IMAGE_LAYOUT_UNDEFINED,
                             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                             VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                             VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT));
  VkRenderingAttachmentInfo att = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = t->views[t->image],
      .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
  };
  VkRenderingInfo ri = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
      .renderArea.extent = t->extent,
      .layerCount = 1,
      .colorAttachmentCount = 1,
      .pColorAttachments = &att,
  };
  vkCmdBeginRendering(cmd, &ri);
  if (quad_count > 0) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, t->pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1,
                            &f->set, 0, NULL);
    VkViewport viewport = {0, 0, t->extent.width, t->extent.height, 0, 1};
    VkRect2D scissor = {{0, 0}, t->extent};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    const float px_to_ndc[2] = {2.0f / t->extent.width, 2.0f / t->extent.height};
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                       sizeof(px_to_ndc), px_to_ndc);
    vkCmdDraw(cmd, 4, quad_count, 0, 0);
  }
  vkCmdEndRendering(cmd);
  barrier(cmd, image_barrier(image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                             VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                             VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                             VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0));
  must(vkEndCommandBuffer(cmd), "end frame");

  f->submitted = ++frames_submitted;
  VkSemaphoreSubmitInfo waits[] = {
      {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
       .semaphore = f->acquired,
       .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT},
      /* Already reached: makes the uploads' writes visible to this frame */
      {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
       .semaphore = uploads_sem,
       .value = atomic_load(&uploads_done),
       .stageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT},
  };
  VkSemaphoreSubmitInfo signals[] = {
      {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
       .semaphore = t->rendered[t->image],
       .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT},
      {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
       .semaphore = frames_sem,
       .value = f->submitted,
       .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT},
  };
  VkCommandBufferSubmitInfo cbi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
      .commandBuffer = cmd,
  };
  VkSubmitInfo2 si = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
      .waitSemaphoreInfoCount = 2,
      .pWaitSemaphoreInfos = waits,
      .commandBufferInfoCount = 1,
      .pCommandBufferInfos = &cbi,
      .signalSemaphoreInfoCount = 2,
      .pSignalSemaphoreInfos = signals,
  };
  VkPresentInfoKHR pi = {
      .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
      .waitSemaphoreCount = 1,
      .pWaitSemaphores = &t->rendered[t->image],
      .swapchainCount = 1,
      .pSwapchains = &t->swapchain,
      .pImageIndices = &t->image,
  };
  pthread_mutex_lock(graphics_lock);
  must(vkQueueSubmit2(graphics_queue, 1, &si, VK_NULL_HANDLE), "submit");
  const VkResult r = vkQueuePresentKHR(graphics_queue, &pi);
  pthread_mutex_unlock(graphics_lock);
  if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
    t->stale = true;
  else
    must(r, "present");
  t->current = (t->current + 1) % FRAMES;
}
