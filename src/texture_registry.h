// SPDX-License-Identifier: MIT
/*
 * Texture Registry
 *
 * Manages flutter external texture registration, texture frame callbacks.
 *
 * Copyright (c) 2023, Hannes Winkler <hanneswinkler2000@web.de>
 */

#ifndef _FLUTTERPI_SRC_TEXTURE_REGISTRY_H
#define _FLUTTERPI_SRC_TEXTURE_REGISTRY_H

#include <flutter_embedder.h>

#include "config.h"

#ifdef HAVE_EGL_GLES2
    #include "gles.h"
#endif

struct texture_registry_interface {
    int (*register_texture)(void *userdata, int64_t texture_identifier);
    int (*unregister_texture)(void *userdata, int64_t texture_identifier);
    int (*mark_frame_available)(void *userdata, int64_t texture_identifier);
};

struct texture_registry;
struct texture;

#ifdef HAVE_EGL_GLES2
struct gl_texture_frame {
    GLenum target;
    GLuint name;
    GLuint format;
    size_t width;
    size_t height;
};
#endif

struct texture_frame;
struct texture_frame {
    union {
#ifdef HAVE_EGL_GLES2
        struct gl_texture_frame gl;
#endif
    };
    void (*destroy)(const struct texture_frame *frame, void *userdata);
    void *userdata;
};

struct unresolved_texture_frame {
    int (*resolve)(size_t width, size_t height, void *userdata, struct texture_frame *frame_out);
    void (*destroy)(void *userdata);
    void *userdata;
};

struct texture_registry *texture_registry_new(const struct texture_registry_interface *interface, void *userdata);

void texture_registry_destroy(struct texture_registry *reg);

#ifdef HAVE_EGL_GLES2
bool texture_registry_gl_external_texture_frame_callback(
    struct texture_registry *reg,
    int64_t texture_id,
    size_t width,
    size_t height,
    FlutterOpenGLTexture *texture_out
);
#endif

struct texture *texture_new(struct texture_registry *reg);

int64_t texture_get_id(struct texture *texture);

int texture_push_frame(struct texture *texture, const struct texture_frame *frame);

int texture_push_unresolved_frame(struct texture *texture, const struct unresolved_texture_frame *frame);

void texture_destroy(struct texture *texture);

#endif  // _FLUTTERPI_SRC_TEXTURE_REGISTRY_H
