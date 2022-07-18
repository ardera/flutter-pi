// SPDX-License-Identifier: MIT
/*
 * linux dmabuf surface
 *
 * - present dmabufs on screen as optimally as possible
 *
 * Copyright (c) 2022, Hannes Winkler <hanneswinkler2000@web.de>
 */


#ifndef _FLUTTERPI_INCLUDE_DMABUF_SURFACE_H
#define _FLUTTERPI_INCLUDE_DMABUF_SURFACE_H

#include <pixel_format.h>

struct dmabuf_surface;

#define CAST_DMABUF_SURFACE_UNCHECKED(ptr) ((struct dmabuf_surface*) (ptr))
#ifdef DEBUG
#   define CAST_DMABUF_SURFACE(ptr) __checked_cast_dmabuf_surface(ptr)
ATTR_PURE struct dmabuf_surface *__checked_cast_dmabuf_surface(void *ptr);
#else
#   define CAST_DMABUF_SURFACE(ptr) CAST_DMABUF_SURFACE_UNCHECKED(ptr)
#endif

struct dmabuf {
    enum pixfmt format;
    int width, height;
    int fds[4];
    int offsets[4];
    int strides[4];
    bool has_modifiers;
    uint64_t modifiers[4];
    void *userdata;
};

typedef void (*dmabuf_release_cb_t)(struct dmabuf *buf);

struct texture_registry;

ATTR_MALLOC struct dmabuf_surface *dmabuf_surface_new(struct tracer *tracer, struct texture_registry *texture_registry);

int dmabuf_surface_push_dmabuf(struct dmabuf_surface *s, const struct dmabuf *buf, dmabuf_release_cb_t release_cb);

ATTR_PURE int64_t dmabuf_surface_get_texture_id(struct dmabuf_surface *s);

#endif // _FLUTTERPI_INCLUDE_DMABUF_SURFACE_H
