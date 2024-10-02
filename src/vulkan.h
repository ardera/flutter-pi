// SPDX-License-Identifier: MIT
/*
 * Just a shim for including vulkan headers, and disabling vulkan function prototypes if vulkan is not present
 *
 * Copyright (c) 2022, Hannes Winkler <hanneswinkler2000@web.de>
 */

#ifndef _FLUTTERPI_SRC_VULKAN_H
#define _FLUTTERPI_SRC_VULKAN_H

#include "config.h"

#ifndef HAVE_VULKAN
    #error "vulkan.h was included but Vulkan support is disabled."
#endif

#include <vulkan/vulkan.h>

const char *vk_strerror(VkResult result);

#define LOG_VK_ERROR_FMT(result, fmt, ...) LOG_ERROR(fmt ": %s\n", __VA_ARGS__ vk_strerror(result))
#define LOG_VK_ERROR(result, str) LOG_ERROR(str ": %s\n", vk_strerror(result))

#endif  // _FLUTTERPI_SRC_VULKAN_H
