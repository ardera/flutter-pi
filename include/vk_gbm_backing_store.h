// SPDX-License-Identifier: MIT
/*
 * Vulkan GBM backing store
 *
 * - used as a render target for flutter vulkan rendering
 * - can be scanned out using KMS
 *
 * Copyright (c) 2022, Hannes Winkler <hanneswinkler2000@web.de>
 */


#ifndef _FLUTTERPI_INCLUDE_VK_GBM_BACKING_STORE_H
#define _FLUTTERPI_INCLUDE_VK_GBM_BACKING_STORE_H

#include <collection.h>
#include <pixel_format.h>
#include <compositor_ng.h>

struct tracer;
struct gbm_device;
struct vk_gbm_backing_store;

#define CAST_VK_GBM_BACKING_STORE_UNCHECKED(ptr) ((struct vk_gbm_backing_store*) (ptr))
#ifdef DEBUG
#   define CAST_VK_GBM_BACKING_STORE(ptr) __checked_cast_vk_gbm_backing_store (ptr)
ATTR_PURE struct vk_gbm_backing_store *__checked_cast_vk_gbm_backing_store(void *ptr);
#else
#   define CAST_VK_GBM_BACKING_STORE(ptr) CAST_VK_GBM_BACKING_STORE_UNCHECKED(ptr)
#endif

ATTR_MALLOC struct vk_gbm_backing_store *vk_gbm_backing_store_new(
    struct tracer *tracer,
    struct point size,
    struct gbm_device *device,
    struct vk_renderer *renderer,
    enum pixfmt pixel_format
);

#endif // _FLUTTERPI_INCLUDE_VK_GBM_BACKING_STORE_H
