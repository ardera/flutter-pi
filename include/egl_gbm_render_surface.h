// SPDX-License-Identifier: MIT
/*
 * GBM Surface backing store
 *
 * - public interface for backing stores which are based on GBM surfaces
 *
 * Copyright (c) 2022, Hannes Winkler <hanneswinkler2000@web.de>
 */


#ifndef _FLUTTERPI_INCLUDE_EGL_GBM_RENDER_SURFACE_H
#define _FLUTTERPI_INCLUDE_EGL_GBM_RENDER_SURFACE_H

#include <collection.h>
#include <pixel_format.h>
#include <compositor_ng.h>

struct render_surface;
struct gbm_device;
struct egl_gbm_render_surface;

#define CAST_EGL_GBM_RENDER_SURFACE_UNCHECKED(ptr) ((struct egl_gbm_render_surface*) (ptr))
#ifdef DEBUG
#   define CAST_EGL_GBM_RENDER_SURFACE(ptr) __checked_cast_egl_gbm_render_surface(ptr)
ATTR_PURE struct egl_gbm_render_surface *__checked_cast_egl_gbm_render_surface(void *ptr);
#else
#   define CAST_EGL_GBM_RENDER_SURFACE(ptr) CAST_EGL_GBM_RENDER_SURFACE_UNCHECKED(ptr)
#endif

struct egl_gbm_render_surface *egl_gbm_render_surface_new_with_egl_config(
    struct tracer *tracer,
    struct vec2i size,
    struct gbm_device *device,
    struct gl_renderer *renderer,
    enum pixfmt pixel_format,
    EGLConfig egl_config,
    const uint64_t *allowed_modifiers,
    size_t n_allowed_modifiers
);

struct egl_gbm_render_surface *egl_gbm_render_surface_new(
    struct tracer *tracer,
    struct vec2i size,
    struct gbm_device *device,
    struct gl_renderer *renderer,
    enum pixfmt pixel_format
);

ATTR_PURE EGLSurface egl_gbm_render_surface_get_egl_surface(struct egl_gbm_render_surface *s);

ATTR_PURE EGLConfig egl_gbm_render_surface_get_egl_config(struct egl_gbm_render_surface *s);

#endif // _FLUTTERPI_INCLUDE_EGL_GBM_RENDER_SURFACE_H
