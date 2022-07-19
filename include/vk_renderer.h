// SPDX-License-Identifier: MIT
/*
 * Vulkan Renderer
 *
 * - provides a vulkan renderer object
 * - a vulkan renderer object is basically a combination of:
 *   - a vulkan instance (VkInstance)
 *   - a vulkan physical device (VkPhysicalDevice)
 *   - a vulkan logical device (VkDevice)
 *   - a vulkan graphics queue (VkQueue)
 *   - a vulkan command buffer pool (VkCommandPool or something)
 * - and utilities for using those
 *
 * Copyright (c) 2022, Hannes Winkler <hanneswinkler2000@web.de>
 */


#ifndef _FLUTTERPI_INCLUDE_VK_RENDERER_H
#define _FLUTTERPI_INCLUDE_VK_RENDERER_H

#include <vulkan.h>
#include <collection.h>

struct vk_renderer;

/**
 * @brief Create a new vulkan renderer with some reasonable defaults.
 * 
 * Creates a vulkan instance with:
 * - app name `flutter-pi`, version 1.0.0
 * - engine `flutter-pi`, version 1.0.0
 * - vulkan version 1.1.0
 * - khronos validation layers and debug utils enabled, if supported and VULKAN_DEBUG is defined
 * 
 * Selects a good physical device (dedicated GPU > integrated GPU > software) that has a
 * graphics queue family and supports the following device extensions:
 * - `VK_KHR_external_memory`
 * - `VK_KHR_external_memory_fd`
 * - `VK_KHR_external_semaphore`
 * - `VK_KHR_external_semaphore_fd`
 * - `VK_EXT_external_memory_dma_buf`
 * - `VK_KHR_image_format_list`
 * - `VK_EXT_image_drm_format_modifier`
 * 
 * Those extensions will also be enabled when create the logical device of course.
 * 
 * Will also create a graphics queue.
 * 
 * @return New vulkan renderer instance.
 */
ATTR_MALLOC struct vk_renderer *vk_renderer_new();

void vk_renderer_destroy();

DECLARE_REF_OPS(vk_renderer)

/**
 * @brief Get the vulkan version of this renderer. This is unconditionally VK_MAKE_VERSION(1, 1, 0) for now.
 * 
 * @param renderer renderer instance
 * @return VK_MAKE_VERSION(1, 1, 0)
 */
ATTR_CONST uint32_t vk_renderer_get_vk_version(struct vk_renderer *renderer);

/**
 * @brief Get the vulkan instance of this renderer. See @ref vk_renderer_new for details on this instance.
 * 
 * @param renderer renderer instance
 * @return vulkan instance
 */
ATTR_PURE VkInstance vk_renderer_get_instance(struct vk_renderer *renderer);

/**
 * @brief Get the physical device that's used by this renderer. See @ref vk_renderer_new for details.
 * 
 * @param renderer renderer instance
 * @return vulkan physical device
 */
ATTR_PURE VkPhysicalDevice vk_renderer_get_physical_device(struct vk_renderer *renderer);

/**
 * @brief Get the logical device that's used by this renderer. See @ref vk_renderer_new for details.
 * 
 * @param renderer renderer instance
 * @return vulkan logical device
 */
ATTR_PURE VkDevice vk_renderer_get_device(struct vk_renderer *renderer);

/**
 * @brief Get the index of the graphics queue family.
 * 
 * @param renderer renderer instance
 * @return instance of the graphics queue family.
 */
ATTR_PURE uint32_t vk_renderer_get_queue_family_index(struct vk_renderer *renderer);

/**
 * @brief Get the graphics queue of this renderer.
 * 
 * @param renderer renderer instance
 * @return graphics queue
 */
ATTR_PURE VkQueue vk_renderer_get_queue(struct vk_renderer *renderer);

ATTR_PURE int vk_renderer_get_enabled_instance_extension_count(struct vk_renderer *renderer);

ATTR_PURE const char **vk_renderer_get_enabled_instance_extensions(struct vk_renderer *renderer);

ATTR_PURE int vk_renderer_get_enabled_device_extension_count(struct vk_renderer *renderer);

ATTR_PURE const char **vk_renderer_get_enabled_device_extensions(struct vk_renderer *renderer);

/**
 * @brief Find the index of a memory type for which the following conditions are true:
 * - (1 < 32) & @param req_bits is not 0
 * - the memory types property flags support the flags that are given in @param flags
 * 
 * @param renderer renderer instance
 * @param flags Which property flags the memory type should support.
 * @param req_bits Which memory types are allowed to choose from.
 * @return index of the found memory type or -1 if none was found.
 */
ATTR_PURE int vk_renderer_find_mem_type(struct vk_renderer *renderer, VkMemoryPropertyFlags flags, uint32_t req_bits);

#endif // _FLUTTERPI_INCLUDE_VK_RENDERER_H
