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
 * - if that extension is not supported, copy the dmabuf contents into the texture (glTexImage2D)
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

#include "dmabuf_surface.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>

#include "kms/resources.h"
#include "kms/drmdev.h"
#include "kms/req_builder.h"
#include "compositor_ng.h"
#include "surface.h"
#include "surface_private.h"
#include "texture_registry.h"
#include "util/collection.h"
#include "util/logging.h"
#include "util/refcounting.h"

#include "config.h"

struct refcounted_dmabuf {
    refcount_t n_refs;
    struct dmabuf buf;
    dmabuf_release_cb_t release_callback;

    struct drmdev *drmdev;
    uint32_t drm_fb_id;
};

void refcounted_dmabuf_destroy(struct refcounted_dmabuf *dmabuf) {
    dmabuf->release_callback(&dmabuf->buf);
    if (DRM_ID_IS_VALID(dmabuf->drm_fb_id)) {
        drmdev_rm_fb(dmabuf->drmdev, dmabuf->drm_fb_id);
    }
    drmdev_unref(dmabuf->drmdev);
    free(dmabuf);
}

DEFINE_STATIC_REF_OPS(refcounted_dmabuf, n_refs)

struct dmabuf_surface {
    struct surface surface;

#ifdef DEBUG
    uuid_t uuid;
#endif

#ifdef HAVE_EGL_GLES2
    EGLDisplay egl_display;
#endif

    struct texture *texture;
    struct refcounted_dmabuf *next_buf;
};

COMPILE_ASSERT(offsetof(struct dmabuf_surface, surface) == 0);

#ifdef DEBUG
static const uuid_t uuid = CONST_UUID(0x68, 0xed, 0xe5, 0x8a, 0x4a, 0x2b, 0x40, 0x76, 0xa1, 0xb8, 0x89, 0x2e, 0x81, 0xfa, 0xe2, 0xb7);
#endif

#define CAST_THIS(ptr) CAST_DMABUF_SURFACE(ptr)
#define CAST_THIS_UNCHECKED(ptr) CAST_DMABUF_SURFACE_UNCHECKED(ptr)

#ifdef DEBUG
ATTR_PURE struct dmabuf_surface *__checked_cast_dmabuf_surface(void *ptr) {
    struct dmabuf_surface *s;

    s = CAST_DMABUF_SURFACE_UNCHECKED(ptr);
    assert(uuid_equals(s->uuid, uuid));
    return s;
}
#endif

static void dmabuf_surface_deinit(struct surface *s);
static int dmabuf_surface_present_kms(struct surface *s, const struct fl_layer_props *props, struct kms_req_builder *builder);
static int dmabuf_surface_present_fbdev(struct surface *s, const struct fl_layer_props *props, struct fbdev_commit_builder *builder);

int dmabuf_surface_init(struct dmabuf_surface *s, struct tracer *tracer, struct texture_registry *texture_registry) {
    struct texture *texture;
    int ok;

    texture = texture_new(texture_registry);
    if (texture == NULL) {
        return EIO;
    }

    ok = surface_init(&s->surface, tracer);
    if (ok != 0) {
        return ok;
    }

    s->surface.deinit = dmabuf_surface_deinit;
    s->surface.present_kms = dmabuf_surface_present_kms;
    s->surface.present_fbdev = dmabuf_surface_present_fbdev;

#ifdef DEBUG
    uuid_copy(&s->uuid, uuid);
#endif

#ifdef HAVE_EGL_GLES2
    s->egl_display = EGL_NO_DISPLAY;
#endif

    s->texture = texture;
    s->next_buf = NULL;
    return 0;
}

static void dmabuf_surface_deinit(struct surface *s) {
    texture_destroy(CAST_THIS_UNCHECKED(s)->texture);
    surface_deinit(s);
}

/**
 * @brief Create a new dmabuf surface.
 *
 * @return struct dmabuf_surface*
 */
MUST_CHECK struct dmabuf_surface *dmabuf_surface_new(struct tracer *tracer, struct texture_registry *texture_registry) {
    struct dmabuf_surface *s;
    int ok;

    s = malloc(sizeof *s);
    if (s == NULL) {
        goto fail_return_null;
    }

    ok = dmabuf_surface_init(s, tracer, texture_registry);
    if (ok != 0) {
        goto fail_free_surface;
    }

    return s;

fail_free_surface:
    free(s);

fail_return_null:
    return NULL;
}

int on_resolve_texture_frame(size_t width, size_t height, void *userdata, struct texture_frame *frame_out) {
    /// TODO: Implement
    (void) width;
    (void) height;
    (void) userdata;

#ifdef HAVE_GLES2
    frame_out->gl.target = GL_TEXTURE_2D;
    frame_out->gl.name = 0;
    frame_out->gl.format = GL_RGBA8_OES;
    frame_out->gl.width = 0;
    frame_out->gl.height = 0;
#endif

    frame_out->userdata = NULL;
    frame_out->destroy = NULL;
    return 0;
}

int dmabuf_surface_push_dmabuf(struct dmabuf_surface *s, const struct dmabuf *buf, dmabuf_release_cb_t release_cb) {
    struct refcounted_dmabuf *b;
    int ok;

    ASSERT_NOT_NULL(s);
    ASSERT_NOT_NULL(buf);
    ASSERT_NOT_NULL(release_cb);

#ifdef HAVE_EGL_GLES2
    assert(eglGetCurrentContext() != EGL_NO_CONTEXT);
#endif

    UNIMPLEMENTED();

    b = malloc(sizeof *b);
    if (b == NULL) {
        return ENOMEM;
    }

    b->n_refs = REFCOUNT_INIT_0;
    b->buf = *buf;
    b->release_callback = release_cb;
    b->drmdev = NULL;
    b->drm_fb_id = DRM_ID_NONE;

    surface_lock(CAST_SURFACE_UNCHECKED(s));

    ok = texture_push_unresolved_frame(
        s->texture,
        &(const struct unresolved_texture_frame){
            .resolve = on_resolve_texture_frame,
            .destroy = surface_unref_void,
            .userdata = surface_ref(CAST_SURFACE_UNCHECKED(s)),
        }
    );
    if (ok != 0) {
        LOG_ERROR("Couldn't post new frame to texture.\n");
        surface_unref(CAST_SURFACE_UNCHECKED(s));
        surface_unlock(CAST_SURFACE_UNCHECKED(s));
        free(b);
        return ok;
    }

    refcounted_dmabuf_swap_ptrs(&s->next_buf, b);

    surface_unlock(CAST_SURFACE_UNCHECKED(s));

    return 0;
}

ATTR_PURE int64_t dmabuf_surface_get_texture_id(struct dmabuf_surface *s) {
    ASSERT_NOT_NULL(s);
    return texture_get_id(s->texture);
}

static int dmabuf_surface_present_kms(struct surface *_s, const struct fl_layer_props *props, struct kms_req_builder *builder) {
    struct dmabuf_surface *s;
    uint32_t fb_id;
    int ok;

    ASSERT_MSG(props->is_aa_rect, "Only axis-aligned rectangles supported right now.");
    s = CAST_THIS(_s);
    (void) s;
    (void) props;
    (void) builder;

    surface_lock(_s);

    ASSERT_NOT_NULL_MSG(s->next_buf, "dmabuf_surface_present_kms was called, but no dmabuf is queued to be presented.");

    if (DRM_ID_IS_VALID(s->next_buf->drm_fb_id)) {
        ASSERT_EQUALS_MSG(s->next_buf->drmdev, kms_req_builder_get_drmdev(builder), "Only 1 KMS instance per dmabuf supported right now.");
        fb_id = s->next_buf->drm_fb_id;
    } else {
        fb_id = drmdev_add_fb_from_dmabuf(
            kms_req_builder_get_drmdev(builder),
            s->next_buf->buf.width,
            s->next_buf->buf.height,
            s->next_buf->buf.format,
            s->next_buf->buf.fds[0],
            s->next_buf->buf.strides[0],
            s->next_buf->buf.offsets[0],
            s->next_buf->buf.has_modifiers,
            s->next_buf->buf.modifiers[0]
        );
        if (!DRM_ID_IS_VALID(fb_id)) {
            LOG_ERROR("Couldn't add dmabuf as framebuffer.\n");
        }

        s->next_buf->drm_fb_id = fb_id;
        s->next_buf->drmdev = drmdev_ref(kms_req_builder_get_drmdev(builder));
    }

    ok = kms_req_builder_push_fb_layer(
        builder,
        &(struct kms_fb_layer){
            .drm_fb_id = fb_id,
            .format = s->next_buf->buf.format,

            .has_modifier = s->next_buf->buf.has_modifiers,
            .modifier = s->next_buf->buf.modifiers[0],

            .src_x = 0,
            .src_y = 0,
            .src_w = DOUBLE_TO_FP1616_ROUNDED(s->next_buf->buf.width),
            .src_h = DOUBLE_TO_FP1616_ROUNDED(s->next_buf->buf.height),

            .dst_x = (int) round(props->aa_rect.offset.x),
            .dst_y = (int) round(props->aa_rect.offset.y),
            .dst_w = (int) round(props->aa_rect.size.x),
            .dst_h = (int) round(props->aa_rect.size.y),

            .has_rotation = false,
            .rotation = PLANE_TRANSFORM_ROTATE_0,
            .has_in_fence_fd = false,
            .in_fence_fd = 0,
        },
        refcounted_dmabuf_unref_void,
        NULL,
        refcounted_dmabuf_ref(s->next_buf)
    );
    if (ok != 0) {
        LOG_ERROR("Couldn't push KMS fb layer. kms_req_builder_push_fb_layer: %s\n", strerror(ok));
        refcounted_dmabuf_unref(s->next_buf);
        surface_unlock(_s);
        return ok;
    }

    surface_unlock(_s);

    return 0;
}

static int dmabuf_surface_present_fbdev(struct surface *_s, const struct fl_layer_props *props, struct fbdev_commit_builder *builder) {
    struct dmabuf_surface *s;

    s = CAST_THIS(_s);
    (void) s;
    (void) props;
    (void) builder;
    UNIMPLEMENTED();

    return 0;
}
