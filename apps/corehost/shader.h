/*
 * libretro slang shader presets on corehost's Vulkan path, as RetroArch runs
 * them (gfx/drivers_shader/shader_vulkan.cpp), built ahead by tools/slangc.
 * One preset at a time.
 */
#pragma once

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

/* Frames in flight at most: per-frame uniforms and descriptor sets */
#define SHADER_FRAMES 2

typedef struct {
  VkPhysicalDevice gpu;
  VkDevice device;
  VkQueue queue;
  uint32_t family;
  pthread_mutex_t *queue_lock;
  VkFormat target_format; /* the swapchain's */
} shader_vk;

/* #pragma parameter: the preset's value, else the default */
typedef struct {
  char name[64], desc[64];
  float value, def, min, max, step;
} shader_param;

/* The game's frame, sampled as Original: in GENERAL or SHADER_READ_ONLY_OPTIMAL
 * (left in it). view: the core's, or VK_NULL_HANDLE for one made here, with
 * alpha 1 (a software core's X of XRGB). */
typedef struct {
  VkImage image;
  VkImageView view;
  VkImageLayout layout;
  VkFormat format;
  uint32_t w, h;
} shader_input;

/* PRESET.slangp; smooth: the filter of passes that don't say */
bool shader_load(const shader_vk *vk, const char *preset, bool smooth);
void shader_unload(void);
bool shader_loaded(void);
int shader_param_count(void);
shader_param *shader_params(void);
/* Records the passes into cmd, the last into target (TRANSFER_DST_OPTIMAL
 * before and after) at rect. frame: the frame in flight, < SHADER_FRAMES. */
void shader_draw(VkCommandBuffer cmd, uint32_t frame, const shader_input *in, VkImage target,
                 VkExtent2D target_size, VkRect2D rect, uint32_t frame_count);
/* The swapchain's images are going: their views and framebuffers too */
void shader_forget_targets(void);
/* The input's image is going (a new one may get the same handle): its view too */
void shader_forget_input(void);

/* The overlay (RetroArch's .cfg: overlay0_overlay, overlay0_viewport): an
 * image drawn over the window, the game showing through (and under its grid,
 * if it has one); its viewport, fractions x, y, w, h of the screen, is where
 * the game goes */
bool border_load(const shader_vk *vk, const char *cfg);
void border_unload(void);
bool border_viewport(float viewport[4]);
/* Over all of target (TRANSFER_DST_OPTIMAL before and after), after the game */
void border_draw(VkCommandBuffer cmd, VkImage target, VkExtent2D size);
