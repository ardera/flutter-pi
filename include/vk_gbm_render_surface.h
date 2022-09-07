// SPDX-License-Identifier: MIT
/*
 * Vulkan GBM render surface
 *
 * - used as a render target for flutter vulkan rendering
 * - can be scanned out using KMS
 *
 * Copyright (c) 2022, Hannes Winkler <hanneswinkler2000@web.de>
 */


#ifndef _FLUTTERPI_INCLUDE_VK_GBM_RENDER_SURFACE_H
#define _FLUTTERPI_INCLUDE_VK_GBM_RENDER_SURFACE_H

#include <collection.h>
#include <pixel_format.h>
#include <compositor_ng.h>

struct tracer;
struct gbm_device;
struct vk_gbm_render_surface;

#define CAST_VK_GBM_RENDER_SURFACE_UNCHECKED(ptr) ((struct vk_gbm_render_surface*) (ptr))
#ifdef DEBUG
#   define CAST_VK_GBM_RENDER_SURFACE(ptr) __checked_cast_vk_gbm_render_surface (ptr)
ATTR_PURE struct vk_gbm_render_surface *__checked_cast_vk_gbm_render_surface(void *ptr);
#else
#   define CAST_VK_GBM_RENDER_SURFACE(ptr) CAST_VK_GBM_RENDER_SURFACE_UNCHECKED(ptr)
#endif

ATTR_MALLOC struct vk_gbm_render_surface *vk_gbm_render_surface_new(
    struct tracer *tracer,
    struct point size,
    struct gbm_device *device,
    struct vk_renderer *renderer,
    enum pixfmt pixel_format
);

#endif // _FLUTTERPI_INCLUDE_VK_GBM_RENDER_SURFACE_H
