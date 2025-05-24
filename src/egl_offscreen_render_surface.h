// SPDX-License-Identifier: MIT
/*
 * Offscreen (MESA surfaceless) backing store
 *
 * Copyright (c) 2022, Hannes Winkler <hanneswinkler2000@web.de>
 */

#ifndef _FLUTTERPI_SRC_EGL_OFFSCREEN_RENDER_SURFACE_H
#define _FLUTTERPI_SRC_EGL_OFFSCREEN_RENDER_SURFACE_H

#include "compositor_ng.h"
#include "pixel_format.h"
#include "util/collection.h"

struct render_surface;
struct gbm_device;
struct egl_offscreen_render_surface;

#define CAST_EGL_OFFSCREEN_RENDER_SURFACE_UNCHECKED(ptr) ((struct egl_offscreen_render_surface *) (ptr))
#ifdef DEBUG
    #define CAST_EGL_OFFSCREEN_RENDER_SURFACE(ptr) __checked_cast_egl_offscreen_render_surface(ptr)
ATTR_PURE struct egl_offscreen_render_surface *__checked_cast_egl_offscreen_render_surface(void *ptr);
#else
    #define CAST_EGL_OFFSCREEN_RENDER_SURFACE(ptr) CAST_EGL_OFFSCREEN_RENDER_SURFACE_UNCHECKED(ptr)
#endif

struct egl_offscreen_render_surface *egl_offscreen_render_surface_new(
    struct tracer *tracer,
    struct vec2i size,
    struct gl_renderer *renderer
);

ATTR_PURE EGLSurface egl_offscreen_render_surface_get_egl_surface(struct egl_offscreen_render_surface *s);

ATTR_PURE EGLConfig egl_offscreen_render_surface_get_egl_config(struct egl_offscreen_render_surface *s);

#endif  // _FLUTTERPI_SRC_EGL_GBM_RENDER_SURFACE_H
