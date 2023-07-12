// SPDX-License-Identifier: MIT
/*
 * Vulkan GBM render surface
 *
 * - used as a render target for flutter vulkan rendering
 * - can be scanned out using KMS
 *
 * Copyright (c) 2022, Hannes Winkler <hanneswinkler2000@web.de>
 */

#ifndef _FLUTTERPI_INCLUDE_DUMMY_RENDER_SURFACE_H
#define _FLUTTERPI_INCLUDE_DUMMY_RENDER_SURFACE_H

#include "util/geometry.h"

struct tracer;
struct dummy_render_surface;

#define CAST_DUMMY_RENDER_SURFACE_UNCHECKED(ptr) ((struct dummy_render_surface *) (ptr))
#ifdef DEBUG
    #define CAST_DUMMY_RENDER_SURFACE(ptr) __checked_cast_dummy_render_surface(ptr)
ATTR_PURE struct dummy_render_surface *__checked_cast_dummy_render_surface(void *ptr);
#else
    #define CAST_DUMMY_RENDER_SURFACE(ptr) CAST_DUMMY_RENDER_SURFACE_UNCHECKED(ptr)
#endif

struct dummy_render_surface *dummy_render_surface_new(
    struct tracer *tracer,
    struct vec2i size
);

#endif  // _FLUTTERPI_INCLUDE_DUMMY_RENDER_SURFACE_H
