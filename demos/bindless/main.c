/* 1000 textured rects in one instanced draw of a 4-vertex strip, no vertex buffers. Each rect
 * picks its texture from a runtime-sized sampler array (descriptor indexing, nonuniformEXT).
 * Vulkan 1.3: dynamic rendering, synchronization2. Each swapchain image has a command buffer recorded
 * once; a frame writes that image's time and resubmits it. Stats come from VK_LAYER_MESA_overlay. */
#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>

#include "rect_vert.h"
#include "rect_frag.h"

#define N_RECTS 1000
#define N_IMAGES 64 /* PanVK: maxPerStageDescriptorSampledImages = 256 */
#define IMG_SIZE 64
#define MAX_SC_IMAGES 8

struct rect {
   float x, y, w, h; /* center, size in pixels */
   float u0, v0, u1, v1;
   uint32_t img, tint; /* tint: RGBA8, little endian (R in the low byte) */
   float t_offset, speed;
};

struct push { float px_to_ndc[2]; uint32_t slot; };

#define CHECK(x) do { VkResult r_ = (x); if (r_ < 0) die(#x, r_); } while (0)

static void die(const char *what, int r)
{
   fprintf(stderr, "%s failed: %d\n", what, r);
   exit(1);
}

static VkPhysicalDevice pdev;
static VkDevice dev;

static float frand(float a, float b) { return a + (b - a) * rand() / (float)RAND_MAX; }

static uint32_t find_memory(uint32_t type_bits, VkMemoryPropertyFlags props)
{
   VkPhysicalDeviceMemoryProperties mp;
   vkGetPhysicalDeviceMemoryProperties(pdev, &mp);
   for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
      if ((type_bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props)
         return i;
   die("find_memory", props);
   return 0;
}

static VkDeviceMemory alloc(VkMemoryRequirements req, VkMemoryPropertyFlags props)
{
   VkMemoryAllocateInfo ai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = req.size,
      .memoryTypeIndex = find_memory(req.memoryTypeBits, props),
   };
   VkDeviceMemory mem;
   CHECK(vkAllocateMemory(dev, &ai, NULL, &mem));
   return mem;
}

/* Host-visible buffer, mapped for good */
static VkBuffer host_buffer(VkDeviceSize size, VkBufferUsageFlags usage, void **map)
{
   VkBufferCreateInfo bi = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = size,
      .usage = usage,
   };
   VkBuffer buf;
   CHECK(vkCreateBuffer(dev, &bi, NULL, &buf));
   VkMemoryRequirements req;
   vkGetBufferMemoryRequirements(dev, buf, &req);
   VkDeviceMemory mem = alloc(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   CHECK(vkBindBufferMemory(dev, buf, mem, 0));
   CHECK(vkMapMemory(dev, mem, 0, size, 0, map));
   return buf;
}

/* A different pattern and color per image, alpha included so tints and overlaps show */
static void make_image(uint32_t *px, int i)
{
   uint32_t base = 0xff000000u | (rand() & 0xffffff);
   for (int y = 0; y < IMG_SIZE; y++)
      for (int x = 0; x < IMG_SIZE; x++) {
         int dx = x - IMG_SIZE / 2, dy = y - IMG_SIZE / 2;
         int on;
         switch (i % 4) {
         case 0: on = ((x / 8) ^ (y / 8)) & 1; break;
         case 1: on = (dx * dx + dy * dy) / 48 % 2; break;
         case 2: on = (x + y) / 6 % 2; break;
         default: on = dx * dx + dy * dy < IMG_SIZE * IMG_SIZE / 4; break;
         }
         px[y * IMG_SIZE + x] = on ? base : 0x40ffffffu;
      }
}

static VkShaderModule shader(const uint32_t *code, size_t size)
{
   VkShaderModuleCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = size,
      .pCode = code,
   };
   VkShaderModule m;
   CHECK(vkCreateShaderModule(dev, &ci, NULL, &m));
   return m;
}

static VkSurfaceKHR surface;
static VkSurfaceFormatKHR sc_format;
static VkSwapchainKHR swapchain;
static VkExtent2D extent;
static uint32_t sc_count;
static VkImage sc_images[MAX_SC_IMAGES];
static VkImageView sc_views[MAX_SC_IMAGES];
/* Per swapchain image: presentation may still hold the previous one when a frame slot comes back */
static VkSemaphore rendered[MAX_SC_IMAGES];

static void create_swapchain(SDL_Window *win)
{
   VkSurfaceCapabilitiesKHR caps;
   CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pdev, surface, &caps));
   if (caps.currentExtent.width != UINT32_MAX) {
      extent = caps.currentExtent;
   } else {
      int w, h;
      SDL_Vulkan_GetDrawableSize(win, &w, &h);
      extent = (VkExtent2D){ w, h };
   }
   /* The minimum: every image more is another frame of latency under FIFO */
   uint32_t count = caps.minImageCount;

   VkSwapchainKHR old = swapchain;
   VkSwapchainCreateInfoKHR ci = {
      .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
      .surface = surface,
      .minImageCount = count,
      .imageFormat = sc_format.format,
      .imageColorSpace = sc_format.colorSpace,
      .imageExtent = extent,
      .imageArrayLayers = 1,
      .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
      .preTransform = caps.currentTransform,
      .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
      .presentMode = VK_PRESENT_MODE_FIFO_KHR, /* even pacing: every frame is shown once */
      .clipped = VK_TRUE,
      .oldSwapchain = old,
   };
   CHECK(vkCreateSwapchainKHR(dev, &ci, NULL, &swapchain));
   if (old) {
      for (uint32_t i = 0; i < sc_count; i++) {
         vkDestroyImageView(dev, sc_views[i], NULL);
         vkDestroySemaphore(dev, rendered[i], NULL);
      }
      vkDestroySwapchainKHR(dev, old, NULL);
   }

   sc_count = MAX_SC_IMAGES;
   CHECK(vkGetSwapchainImagesKHR(dev, swapchain, &sc_count, sc_images));
   for (uint32_t i = 0; i < sc_count; i++) {
      VkImageViewCreateInfo vi = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
         .image = sc_images[i],
         .viewType = VK_IMAGE_VIEW_TYPE_2D,
         .format = sc_format.format,
         .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
      };
      CHECK(vkCreateImageView(dev, &vi, NULL, &sc_views[i]));
      VkSemaphoreCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
      CHECK(vkCreateSemaphore(dev, &si, NULL, &rendered[i]));
   }
}

static VkImageMemoryBarrier2 image_barrier(VkImage img, VkImageLayout from, VkImageLayout to,
                                           VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                                           VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access)
{
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
      .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
   };
}

static void barriers(VkCommandBuffer cmd, const VkImageMemoryBarrier2 *b, uint32_t n)
{
   VkDependencyInfo di = {
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .imageMemoryBarrierCount = n,
      .pImageMemoryBarriers = b,
   };
   vkCmdPipelineBarrier2(cmd, &di);
}

static VkCommandBuffer cmds[MAX_SC_IMAGES];
static VkPipeline pipeline;
static VkPipelineLayout layout;
static VkDescriptorSet set;

/* The whole frame for swapchain image i. The layout transitions are required: the image comes back
 * from presentation UNDEFINED/PRESENT_SRC and has to be COLOR_ATTACHMENT to render, PRESENT_SRC to present. */
static void record(uint32_t i)
{
   VkCommandBuffer cmd = cmds[i];
   VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
   CHECK(vkBeginCommandBuffer(cmd, &bi));
   VkImageMemoryBarrier2 to_color = image_barrier(sc_images[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                                                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
   barriers(cmd, &to_color, 1);
   VkRenderingAttachmentInfo att = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = sc_views[i],
      .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .clearValue.color.float32 = { 0.05f, 0.05f, 0.08f, 1.0f },
   };
   VkRenderingInfo ri = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
      .renderArea.extent = extent,
      .layerCount = 1,
      .colorAttachmentCount = 1,
      .pColorAttachments = &att,
   };
   vkCmdBeginRendering(cmd, &ri);
   vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
   vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, NULL);
   /* Dynamic so a new swapchain size only needs re-recording, not a new pipeline */
   VkViewport viewport = { 0, 0, extent.width, extent.height, 0, 1 };
   VkRect2D scissor = { { 0, 0 }, extent };
   vkCmdSetViewport(cmd, 0, 1, &viewport);
   vkCmdSetScissor(cmd, 0, 1, &scissor);
   struct push pc = { { 2.0f / extent.width, 2.0f / extent.height }, i };
   vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
   vkCmdDraw(cmd, 4, N_RECTS, 0, 0);
   vkCmdEndRendering(cmd);
   /* Inside the stage the present semaphore waits on */
   VkImageMemoryBarrier2 to_present = image_barrier(sc_images[i], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                                                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0);
   barriers(cmd, &to_present, 1);
   CHECK(vkEndCommandBuffer(cmd));
}

int main(void)
{
   if (SDL_Init(SDL_INIT_VIDEO) < 0)
      die(SDL_GetError(), 0);
   SDL_Window *win = SDL_CreateWindow("bindless-vk", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                      640, 480, SDL_WINDOW_VULKAN | SDL_WINDOW_FULLSCREEN_DESKTOP);
   if (!win)
      die(SDL_GetError(), 0);

   /* Instance, surface, device */
   unsigned n_ext = 0;
   SDL_Vulkan_GetInstanceExtensions(win, &n_ext, NULL);
   const char **exts = malloc(n_ext * sizeof(*exts));
   SDL_Vulkan_GetInstanceExtensions(win, &n_ext, exts);
   VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "bindless-vk",
      .apiVersion = VK_API_VERSION_1_3,
   };
   VkInstanceCreateInfo ici = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app,
      .enabledExtensionCount = n_ext,
      .ppEnabledExtensionNames = exts,
   };
   VkInstance inst;
   CHECK(vkCreateInstance(&ici, NULL, &inst));
   if (!SDL_Vulkan_CreateSurface(win, inst, &surface))
      die(SDL_GetError(), 0);

   uint32_t n_pdev = 1;
   vkEnumeratePhysicalDevices(inst, &n_pdev, &pdev);
   if (!n_pdev)
      die("no Vulkan device", 0);
   VkPhysicalDeviceProperties props;
   vkGetPhysicalDeviceProperties(pdev, &props);
   if (props.apiVersion < VK_API_VERSION_1_3)
      die("Vulkan 1.3", props.apiVersion);

   uint32_t qf = 0, n_qf = 8;
   VkQueueFamilyProperties qfp[8];
   vkGetPhysicalDeviceQueueFamilyProperties(pdev, &n_qf, qfp);
   for (;; qf++) {
      if (qf == n_qf)
         die("no graphics+present queue", 0);
      VkBool32 present;
      vkGetPhysicalDeviceSurfaceSupportKHR(pdev, qf, surface, &present);
      if (present && (qfp[qf].queueFlags & VK_QUEUE_GRAPHICS_BIT))
         break;
   }

   VkPhysicalDeviceVulkan13Features f13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
   VkPhysicalDeviceVulkan12Features f12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &f13 };
   VkPhysicalDeviceFeatures2 f = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12 };
   vkGetPhysicalDeviceFeatures2(pdev, &f);
   if (!f12.runtimeDescriptorArray || !f12.shaderSampledImageArrayNonUniformIndexing ||
       !f13.dynamicRendering || !f13.synchronization2)
      die("descriptor indexing / dynamic rendering / synchronization2 support", 0);
   /* Enable only what we use */
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
   };

   float prio = 1.0f;
   VkDeviceQueueCreateInfo qci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = qf,
      .queueCount = 1,
      .pQueuePriorities = &prio,
   };
   const char *dev_ext = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
   VkDeviceCreateInfo dci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .pNext = &f12,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &qci,
      .enabledExtensionCount = 1,
      .ppEnabledExtensionNames = &dev_ext,
   };
   CHECK(vkCreateDevice(pdev, &dci, NULL, &dev));
   VkQueue queue;
   vkGetDeviceQueue(dev, qf, 0, &queue);

   /* Swapchain: UNORM if offered (tints are plain RGBA8), BGRA first: with RGBA (DRM ABGR8888)
    * the H700's display planes swap red and blue when Weston scans the buffer out directly */
   uint32_t n_fmt = 32;
   VkSurfaceFormatKHR fmts[32];
   CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(pdev, surface, &n_fmt, fmts));
   sc_format = fmts[0];
   for (uint32_t i = 0; i < n_fmt; i++)
      if (fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM && fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
         sc_format = fmts[i];
   for (uint32_t i = 0; i < n_fmt; i++)
      if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM && fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
         sc_format = fmts[i];
   create_swapchain(win);

   /* Instances */
   void *map;
   VkBuffer rect_buf = host_buffer(N_RECTS * sizeof(struct rect), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &map);
   struct rect *rects = map;
   for (int i = 0; i < N_RECTS; i++) {
      float u0 = frand(0, 0.5f), v0 = frand(0, 0.5f), rep = frand(0.5f, 2);
      float w = frand(16, 72);
      rects[i] = (struct rect){
         .x = frand(0, extent.width), .y = frand(0, extent.height),
         .w = w, .h = w * frand(0.5f, 2),
         .u0 = u0, .v0 = v0, .u1 = u0 + rep, .v1 = v0 + rep,
         .img = rand() % N_IMAGES,
         .tint = 0xff000000u | (rand() & 0xffffff) | 0x404040,
         .t_offset = frand(0, 6.2832f),
         .speed = frand(-3, 3),
      };
   }

   VkBuffer time_buf = host_buffer(MAX_SC_IMAGES * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &map);
   volatile float *times = map;

   /* Textures: one VkImage each, uploaded through one staging buffer */
   VkCommandPoolCreateInfo cpi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
      .queueFamilyIndex = qf,
   };
   VkCommandPool pool;
   CHECK(vkCreateCommandPool(dev, &cpi, NULL, &pool));
   VkCommandBufferAllocateInfo cai = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = MAX_SC_IMAGES,
   };
   CHECK(vkAllocateCommandBuffers(dev, &cai, cmds));

   const VkDeviceSize img_bytes = IMG_SIZE * IMG_SIZE * 4;
   VkBuffer staging = host_buffer(N_IMAGES * img_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &map);
   VkImage images[N_IMAGES];
   VkImageMemoryBarrier2 ib[N_IMAGES];
   VkDescriptorImageInfo tex_info[N_IMAGES];
   VkSamplerCreateInfo sci = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_LINEAR,
      .minFilter = VK_FILTER_LINEAR,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
   };
   VkSampler sampler;
   CHECK(vkCreateSampler(dev, &sci, NULL, &sampler));

   VkCommandBuffer cmd = cmds[0];
   VkCommandBufferBeginInfo bi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   CHECK(vkBeginCommandBuffer(cmd, &bi));
   for (int i = 0; i < N_IMAGES; i++) {
      make_image((uint32_t *)((char *)map + i * img_bytes), i);
      VkImageCreateInfo ci = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
         .imageType = VK_IMAGE_TYPE_2D,
         .format = VK_FORMAT_R8G8B8A8_UNORM,
         .extent = { IMG_SIZE, IMG_SIZE, 1 },
         .mipLevels = 1,
         .arrayLayers = 1,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .tiling = VK_IMAGE_TILING_OPTIMAL,
         .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
      };
      CHECK(vkCreateImage(dev, &ci, NULL, &images[i]));
      VkMemoryRequirements req;
      vkGetImageMemoryRequirements(dev, images[i], &req);
      CHECK(vkBindImageMemory(dev, images[i], alloc(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT), 0));
      VkImageViewCreateInfo vi = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
         .image = images[i],
         .viewType = VK_IMAGE_VIEW_TYPE_2D,
         .format = ci.format,
         .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
      };
      tex_info[i] = (VkDescriptorImageInfo){ sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
      CHECK(vkCreateImageView(dev, &vi, NULL, &tex_info[i].imageView));
      ib[i] = image_barrier(images[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_PIPELINE_STAGE_2_NONE, 0,
                            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
   }
   barriers(cmd, ib, N_IMAGES);
   for (int i = 0; i < N_IMAGES; i++) {
      VkBufferImageCopy copy = {
         .bufferOffset = i * img_bytes,
         .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
         .imageExtent = { IMG_SIZE, IMG_SIZE, 1 },
      };
      vkCmdCopyBufferToImage(cmd, staging, images[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
      ib[i] = image_barrier(images[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
   }
   barriers(cmd, ib, N_IMAGES);
   CHECK(vkEndCommandBuffer(cmd));
   VkCommandBufferSubmitInfo cbsi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cmd };
   VkSubmitInfo2 upload = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = 1, .pCommandBufferInfos = &cbsi };
   CHECK(vkQueueSubmit2(queue, 1, &upload, VK_NULL_HANDLE));
   CHECK(vkQueueWaitIdle(queue));

   /* Descriptors: the instance SSBO and every texture, bound once */
   VkDescriptorSetLayoutBinding bindings[] = {
      { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL },
      { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, N_IMAGES, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
      { 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL },
   };
   VkDescriptorSetLayoutCreateInfo dslci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 3,
      .pBindings = bindings,
   };
   VkDescriptorSetLayout dsl;
   CHECK(vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl));
   VkDescriptorPoolSize sizes[] = {
      { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 },
      { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, N_IMAGES },
   };
   VkDescriptorPoolCreateInfo dpci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1,
      .poolSizeCount = 2,
      .pPoolSizes = sizes,
   };
   VkDescriptorPool dpool;
   CHECK(vkCreateDescriptorPool(dev, &dpci, NULL, &dpool));
   VkDescriptorSetAllocateInfo dsai = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = dpool,
      .descriptorSetCount = 1,
      .pSetLayouts = &dsl,
   };
   CHECK(vkAllocateDescriptorSets(dev, &dsai, &set));
   VkDescriptorBufferInfo rect_info = { rect_buf, 0, VK_WHOLE_SIZE }, time_info = { time_buf, 0, VK_WHOLE_SIZE };
   VkWriteDescriptorSet writes[] = {
      { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &rect_info },
      { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 1, .descriptorCount = N_IMAGES,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = tex_info },
      { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 2, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &time_info },
   };
   vkUpdateDescriptorSets(dev, 3, writes, 0, NULL);

   /* Pipeline: no vertex input, 4-vertex strip per instance, alpha blended */
   VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(struct push) };
   VkPipelineLayoutCreateInfo plci = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &dsl,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges = &pcr,
   };
   CHECK(vkCreatePipelineLayout(dev, &plci, NULL, &layout));
   VkPipelineShaderStageCreateInfo stages[] = {
      { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
        .module = shader(rect_vert, sizeof(rect_vert)), .pName = "main" },
      { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
        .module = shader(rect_frag, sizeof(rect_frag)), .pName = "main" },
   };
   VkPipelineVertexInputStateCreateInfo vin = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
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
   VkPipelineColorBlendAttachmentState blend = {
      .blendEnable = VK_TRUE,
      .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
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
   VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
   VkPipelineDynamicStateCreateInfo ds = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
      .dynamicStateCount = 2,
      .pDynamicStates = dyn,
   };
   VkPipelineRenderingCreateInfo rci = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
      .colorAttachmentCount = 1,
      .pColorAttachmentFormats = &sc_format.format,
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
   CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipeline));

   /* Frame loop. Fences and acquire semaphores are per swapchain image: once an image's fence has
    * signaled, its command buffer, time slot and semaphore are free again. The acquire has to name a
    * semaphore before the image is known, so it takes the spare and trades it in. */
   VkSemaphore acquired[MAX_SC_IMAGES], spare;
   VkFence done[MAX_SC_IMAGES];
   VkSemaphoreCreateInfo semci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
   VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
   CHECK(vkCreateSemaphore(dev, &semci, NULL, &spare));
   for (int i = 0; i < MAX_SC_IMAGES; i++) {
      CHECK(vkCreateSemaphore(dev, &semci, NULL, &acquired[i]));
      CHECK(vkCreateFence(dev, &fci, NULL, &done[i]));
   }
   for (uint32_t i = 0; i < sc_count; i++)
      record(i);

   Uint64 t0 = SDL_GetPerformanceCounter();
   for (;;) {
      SDL_Event ev;
      while (SDL_PollEvent(&ev))
         if (ev.type == SDL_QUIT || (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE))
            goto out;

      uint32_t idx;
      VkResult r = vkAcquireNextImageKHR(dev, swapchain, UINT64_MAX, spare, VK_NULL_HANDLE, &idx);
      if (r == VK_ERROR_OUT_OF_DATE_KHR) {
         CHECK(vkDeviceWaitIdle(dev));
         create_swapchain(win);
         for (uint32_t i = 0; i < sc_count; i++)
            record(i);
         continue;
      }
      CHECK(r);
      CHECK(vkWaitForFences(dev, 1, &done[idx], VK_TRUE, UINT64_MAX));
      CHECK(vkResetFences(dev, 1, &done[idx]));
      VkSemaphore s = acquired[idx];
      acquired[idx] = spare;
      spare = s;

      times[idx] = (SDL_GetPerformanceCounter() - t0) / (float)SDL_GetPerformanceFrequency();

      VkSemaphoreSubmitInfo wait = {
         .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
         .semaphore = acquired[idx],
         .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
      };
      VkSemaphoreSubmitInfo signal = {
         .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
         .semaphore = rendered[idx],
         .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
      };
      cbsi.commandBuffer = cmds[idx];
      VkSubmitInfo2 si = {
         .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
         .waitSemaphoreInfoCount = 1,
         .pWaitSemaphoreInfos = &wait,
         .commandBufferInfoCount = 1,
         .pCommandBufferInfos = &cbsi,
         .signalSemaphoreInfoCount = 1,
         .pSignalSemaphoreInfos = &signal,
      };
      CHECK(vkQueueSubmit2(queue, 1, &si, done[idx]));

      VkPresentInfoKHR pi = {
         .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
         .waitSemaphoreCount = 1,
         .pWaitSemaphores = &rendered[idx],
         .swapchainCount = 1,
         .pSwapchains = &swapchain,
         .pImageIndices = &idx,
      };
      r = vkQueuePresentKHR(queue, &pi);
      if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
         CHECK(vkDeviceWaitIdle(dev));
         create_swapchain(win);
         for (uint32_t i = 0; i < sc_count; i++)
            record(i);
      } else {
         CHECK(r);
      }
   }
out:
   /* The process exit frees the rest */
   vkDeviceWaitIdle(dev);
   SDL_Quit();
   return 0;
}
