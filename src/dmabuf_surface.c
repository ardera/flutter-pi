// SPDX-License-Identifier: MIT
/*
 * linux-dmabuf rendering surface
 *
 * A surface:
 * - that plugins can push linux dmabufs into (for example, for video playback)
 * - that'll expose both a texture and a platform view
 * 
 * the exposed flutter texture: (cold path)
 * - is an imported EGL Image, which is created from the dmabuf using the EGL_EXT_image_dma_buf_import extension (if supported)
 * - if that extension is not supported, copy the dmabuf contents into the texture
 * - using a texture is slower than a hardware overlay
 * - (because with a texture, texture contents must be converted into the right pixel format and composited into a single framebuffer,
 *   before the frame can be scanned out => additional memory copy, but with hardware overlay that will be done in realtime, on-the-fly,
 *   while the picture is being scanned out)
 * 
 * the platform view: (hot path)
 * - on KMS present, will add a hardware overlay plane to scanout that fb, if that's possible
 *   (if the rectangle is axis-aligned and pixel format, alpha value etc is supported by KMS)
 * - otherwise, fall back to the cold path (textures)
 * - this needs integration from the dart side, because only the dart side can decide whether to use
 *   texture or platform view
 * 
 * The surface should have a specific counterpart on the dart side, which will decide whether to use
 * texture or platform view. That decision is hard to make consistent, i.e. when dart-side decides on
 * platform view, it's not 100% guaranteed this surface will actually succeed in adding the hw overlay plane.
 * 
 * So best we can do is guess. Not sure how to implement the switching between hot path / cold path though.
 * Maybe, if we fail in adding the hw overlay plane, we could signal that somehow to the
 * dart-side and make it use a texture for the next frame. But also, it could be adding the overlay plane succeeds,
 * and adding a later plane fails. In that case we don't notice the error, but we should still fallback to texture rendering.
 *
 * Copyright (c) 2022, Hannes Winkler <hanneswinkler2000@web.de>
 */


#include <stdlib.h>

#include <collection.h>

#include <stdlib.h>
#include <stdatomic.h>
#include <stdint.h>

#include <collection.h>
#include <surface.h>
#include <surface_private.h>
#include <dmabuf_surface.h>
#include <compositor_ng.h>

FILE_DESCR("dmabuf surface")

struct dmabuf_surface {
    struct surface surface;

    uuid_t uuid;
    EGLDisplay egl_display;
};

COMPILE_ASSERT(offsetof(struct dmabuf_surface, surface) == 0);

static const uuid_t uuid = CONST_UUID(0x68, 0xed, 0xe5, 0x8a, 0x4a, 0x2b, 0x40, 0x76, 0xa1, 0xb8, 0x89, 0x2e, 0x81, 0xfa, 0xe2, 0xb7);

#define CAST_THIS(ptr) CAST_DMABUF_SURFACE(ptr)
#define CAST_THIS_UNCHECKED(ptr) CAST_DMABUF_SURFACE_UNCHECKED(ptr)

#ifdef DEBUG
ATTR_PURE struct dmabuf_surface *__checked_cast_dmabuf_surface(void *ptr) {
    struct dmabuf_surface *s;
    
    s = CAST_DMABUF_SURFACE_UNCHECKED(ptr);
    DEBUG_ASSERT(uuid_equals(s->uuid, uuid));
    return s;
}
#endif

static void dmabuf_surface_deinit(struct surface *s);
static int dmabuf_surface_swap_buffers(struct surface *s);
static int dmabuf_surface_present_kms(struct surface *s, const struct fl_layer_props *props, struct kms_req_builder *builder);
static int dmabuf_surface_present_fbdev(struct surface *s, const struct fl_layer_props *props, struct fbdev_commit_builder *builder);

int dmabuf_surface_init(struct dmabuf_surface *s, struct compositor *compositor, struct tracer *tracer) {
    int ok;

    ok = surface_init(&s->surface, compositor, tracer);
    if (ok != 0) {
        return ok;
    }

    s->surface.deinit = dmabuf_surface_deinit;
    s->surface.swap_buffers = dmabuf_surface_swap_buffers;
    s->surface.present_kms = dmabuf_surface_present_kms;
    s->surface.present_fbdev = dmabuf_surface_present_fbdev;
    uuid_copy(&s->uuid, uuid);
    return 0;
}

static void dmabuf_surface_deinit(struct surface *s) {
    surface_deinit(s);
}

/**
 * @brief Create a new dmabuf surface.
 * 
 * @param compositor The compositor that this surface will be registered to when calling surface_register.
 * @return struct dmabuf_surface* 
 */
ATTR_MALLOC struct dmabuf_surface *dmabuf_surface_new(struct compositor *compositor, struct tracer *tracer) {
    struct dmabuf_surface *s;
    int ok;
    
    s = malloc(sizeof *s);
    if (s == NULL) {
        goto fail_return_null;
    }

    ok = dmabuf_surface_init(s, compositor, tracer);
    if (ok != 0) {
        goto fail_free_surface;
    }

    return s;


    fail_free_surface:
    free(s);

    fail_return_null:
    return NULL;
}

int dmabuf_surface_push_dmabuf(struct dmabuf_surface *s, const struct dmabuf *buf, dmabuf_release_cb_t release_cb) {
    DEBUG_ASSERT_NOT_NULL(s);
    DEBUG_ASSERT_NOT_NULL(buf);
    DEBUG_ASSERT_NOT_NULL(release_cb);

#ifdef HAS_EGL
    DEBUG_ASSERT(eglGetCurrentContext() != EGL_NO_CONTEXT);
#endif

    UNIMPLEMENTED();

    return 0;
}

ATTR_PURE int64_t dmabuf_surface_get_texture_id(struct dmabuf_surface *s) {
    DEBUG_ASSERT_NOT_NULL(s);
    UNIMPLEMENTED();
    return 0;
}

static int dmabuf_surface_swap_buffers(struct surface *_s) {
    struct dmabuf_surface *s;

    s = CAST_THIS(_s);
    (void) s;

    UNIMPLEMENTED();

    return 0;
}

static int dmabuf_surface_present_kms(struct surface *_s, const struct fl_layer_props *props, struct kms_req_builder *builder) {
    struct dmabuf_surface *s;

    s = CAST_THIS(_s);
    (void) s;
    (void) props;
    (void) builder;

    return 0;
}

static int dmabuf_surface_present_fbdev(struct surface *_s, const struct fl_layer_props *props, struct fbdev_commit_builder *builder) {
    struct dmabuf_surface *s;

    s = CAST_THIS(_s);
    (void) s;
    (void) props;
    (void) builder;

    return 0;
}
