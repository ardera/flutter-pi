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

ATTR_MALLOC struct vk_renderer *vk_renderer_new();

void vk_renderer_destroy();

DECLARE_REF_OPS(vk_renderer)

ATTR_CONST uint32_t vk_renderer_get_vk_version(struct vk_renderer *renderer);

ATTR_PURE VkInstance vk_renderer_get_instance(struct vk_renderer *renderer);

ATTR_PURE VkPhysicalDevice vk_renderer_get_physical_device(struct vk_renderer *renderer);

ATTR_PURE VkDevice vk_renderer_get_device(struct vk_renderer *renderer);

ATTR_PURE uint32_t vk_renderer_get_queue_family_index(struct vk_renderer *renderer);

ATTR_PURE VkQueue vk_renderer_get_queue(struct vk_renderer *renderer);

ATTR_PURE int vk_renderer_get_enabled_instance_extension_count(struct vk_renderer *renderer);

ATTR_PURE const char **vk_renderer_get_enabled_instance_extensions(struct vk_renderer *renderer);

ATTR_PURE int vk_renderer_get_enabled_device_extension_count(struct vk_renderer *renderer);

ATTR_PURE const char **vk_renderer_get_enabled_device_extensions(struct vk_renderer *renderer);

ATTR_PURE int vk_renderer_find_mem_type(struct vk_renderer *renderer, VkMemoryPropertyFlags flags, uint32_t req_bits);

#endif // _FLUTTERPI_INCLUDE_VK_RENDERER_H
