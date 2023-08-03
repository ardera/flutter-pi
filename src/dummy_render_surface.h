// SPDX-License-Identifier: MIT
/*
 * Dummy render surface
 *
 * Just a render surface that does nothing when presenting.
 *
 * Copyright (c) 2023, Hannes Winkler <hanneswinkler2000@web.de>
 */

#ifndef _FLUTTERPI_SRC_DUMMY_RENDER_SURFACE_H
#define _FLUTTERPI_SRC_DUMMY_RENDER_SURFACE_H

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

struct dummy_render_surface *dummy_render_surface_new(struct tracer *tracer, struct vec2i size);

#endif  // _FLUTTERPI_SRC_DUMMY_RENDER_SURFACE_H
