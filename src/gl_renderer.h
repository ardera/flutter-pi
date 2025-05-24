
// SPDX-License-Identifier: MIT
/*
 * EGL/OpenGL renderer
 *
 * Utilities for rendering with EGL/OpenGL.
 * - creating/binding EGL contexts
 * - using EGL extensions
 * - setting up / cleaning up render threads
 *
 * Copyright (c) 2022, Hannes Winkler <hanneswinkler2000@web.de>
 */

#ifndef _FLUTTERPI_SRC_GL_RENDERER_H
#define _FLUTTERPI_SRC_GL_RENDERER_H

#include "pixel_format.h"
#include "util/collection.h"
#include "util/refcounting.h"

#include "config.h"

#if !defined(HAVE_EGL_GLES2)
    #error "gl_renderer requires EGL and OpenGL ES support."
#endif

#include "egl.h"

struct tracer;

struct gl_renderer *gl_renderer_new_from_gbm_device(
    struct tracer *tracer,
    struct gbm_device *gbm_device,
    bool has_forced_pixel_format,
    enum pixfmt pixel_format
);

void gl_renderer_destroy(struct gl_renderer *renderer);

DECLARE_REF_OPS(gl_renderer)

bool gl_renderer_has_forced_pixel_format(struct gl_renderer *renderer);

enum pixfmt gl_renderer_get_forced_pixel_format(struct gl_renderer *renderer);

bool gl_renderer_has_forced_egl_config(struct gl_renderer *renderer);

EGLConfig gl_renderer_get_forced_egl_config(struct gl_renderer *renderer);

struct gbm_device *gl_renderer_get_gbm_device(struct gl_renderer *renderer);

int gl_renderer_make_flutter_rendering_context_current(struct gl_renderer *renderer, EGLSurface surface);

int gl_renderer_make_flutter_resource_uploading_context_current(struct gl_renderer *renderer);

int gl_renderer_make_flutter_setup_context_current(struct gl_renderer *renderer);

int gl_renderer_clear_current(struct gl_renderer *renderer);

EGLContext gl_renderer_create_context(struct gl_renderer *renderer);

void *gl_renderer_get_proc_address(struct gl_renderer *renderer, const char *name);

void *gl_renderer_try_get_proc_address(ASSERTED struct gl_renderer *renderer, const char *name);

EGLDisplay gl_renderer_get_egl_display(struct gl_renderer *renderer);

bool gl_renderer_supports_egl_extension(struct gl_renderer *renderer, const char *name);

bool gl_renderer_supports_gl_extension(struct gl_renderer *renderer, const char *name);

bool gl_renderer_is_llvmpipe(struct gl_renderer *renderer);

int gl_renderer_make_this_a_render_thread(struct gl_renderer *renderer);

void gl_renderer_cleanup_this_render_thread();

ATTR_PURE EGLConfig gl_renderer_choose_config(struct gl_renderer *renderer, bool has_desired_pixel_format, enum pixfmt desired_pixel_format);

ATTR_PURE EGLConfig gl_renderer_choose_config_direct(struct gl_renderer *renderer, enum pixfmt pixel_format);

EGLSurface gl_renderer_create_gbm_window_surface(
    struct gl_renderer *renderer,
    EGLConfig config,
    struct gbm_surface *gbm_surface,
    const EGLAttribKHR *attrib_list,
    const EGLint *int_attrib_list
);

#endif  // _FLUTTERPI_SRC_GL_RENDERER_H
