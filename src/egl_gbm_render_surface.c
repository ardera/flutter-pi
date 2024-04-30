// SPDX-License-Identifier: MIT
/*
 * GBM Surface Backing Stores
 *
 * - implements EGL/GL render surfaces using a gbm surface
 * - ideal way to create render surfaces right now
 *
 * Copyright (c) 2022, Hannes Winkler <hanneswinkler2000@web.de>
 */

#include "egl_gbm_render_surface.h"

#include <errno.h>
#include <stdlib.h>

#include "egl.h"
#include "gl_renderer.h"
#include "gles.h"
#include "modesetting.h"
#include "pixel_format.h"
#include "render_surface.h"
#include "render_surface_private.h"
#include "surface.h"
#include "tracer.h"
#include "util/collection.h"
#include "util/logging.h"
#include "util/refcounting.h"

struct egl_gbm_render_surface;

struct locked_fb {
    atomic_flag is_locked;
    struct egl_gbm_render_surface *surface;
    struct gbm_bo *bo;
    refcount_t n_refs;
};

struct egl_gbm_render_surface {
    union {
        struct surface surface;
        struct render_surface render_surface;
    };

#ifdef DEBUG
    uuid_t uuid;
#endif

    enum pixfmt pixel_format;
    struct gbm_device *gbm_device;
    struct gbm_surface *gbm_surface;
    struct gbm_bo *front_buffer;
    EGLDisplay egl_display;
    EGLSurface egl_surface;
    EGLConfig egl_config;
    struct gl_renderer *renderer;

    // Internally mesa supports 4 GBM BOs per surface, so we don't need
    // more than 4 here either.
    struct locked_fb locked_fbs[4];
    struct locked_fb *locked_front_fb;
#ifdef DEBUG
    atomic_int n_locked_fbs;
    bool logged_format_and_modifier;
#endif
};

COMPILE_ASSERT(offsetof(struct egl_gbm_render_surface, surface) == 0);
COMPILE_ASSERT(offsetof(struct egl_gbm_render_surface, render_surface.surface) == 0);

#ifdef DEBUG
static const uuid_t uuid = CONST_UUID(0xf9, 0xc2, 0x5d, 0xad, 0x2e, 0x3b, 0x4e, 0x2c, 0x9d, 0x26, 0x64, 0x70, 0xfa, 0x9a, 0x25, 0xd9);
#endif

#define CAST_THIS(ptr) CAST_EGL_GBM_RENDER_SURFACE(ptr)
#define CAST_THIS_UNCHECKED(ptr) CAST_EGL_GBM_RENDER_SURFACE_UNCHECKED(ptr)

static void locked_fb_destroy(struct locked_fb *fb) {
    struct egl_gbm_render_surface *s;

    s = fb->surface;
    fb->surface = NULL;
    gbm_surface_release_buffer(s->gbm_surface, fb->bo);
#ifdef DEBUG
    atomic_fetch_sub(&s->n_locked_fbs, 1);
#endif
    atomic_flag_clear(&fb->is_locked);
    surface_unref(CAST_SURFACE(s));
}

DEFINE_STATIC_REF_OPS(locked_fb, n_refs)

#ifdef DEBUG
ATTR_PURE struct egl_gbm_render_surface *__checked_cast_egl_gbm_render_surface(void *ptr) {
    struct egl_gbm_render_surface *s;

    s = CAST_EGL_GBM_RENDER_SURFACE_UNCHECKED(ptr);
    assert(uuid_equals(s->uuid, uuid));
    return s;
}
#endif

void egl_gbm_render_surface_deinit(struct surface *s);
static int egl_gbm_render_surface_present_kms(struct surface *s, const struct fl_layer_props *props, struct kms_req_builder *builder);
static int
egl_gbm_render_surface_present_fbdev(struct surface *s, const struct fl_layer_props *props, struct fbdev_commit_builder *builder);
static int egl_gbm_render_surface_fill(struct render_surface *s, FlutterBackingStore *fl_store);
static int egl_gbm_render_surface_queue_present(struct render_surface *s, const FlutterBackingStore *fl_store);

static int egl_gbm_render_surface_init(
    struct egl_gbm_render_surface *s,
    struct tracer *tracer,
    struct vec2i size,
    struct gbm_device *gbm_device,
    struct gl_renderer *renderer,
    enum pixfmt pixel_format,
    EGLConfig egl_config,
    const uint64_t *allowed_modifiers,
    size_t n_allowed_modifiers
) {
    struct gbm_surface *gbm_surface;
    EGLDisplay egl_display;
    EGLSurface egl_surface;
    EGLBoolean egl_ok;
    int ok;

    ASSERT_NOT_NULL(renderer);
    ASSUME_PIXFMT_VALID(pixel_format);
    egl_display = gl_renderer_get_egl_display(renderer);
    ASSERT_NOT_NULL(egl_display);

#ifdef DEBUG
    if (egl_config != EGL_NO_CONFIG_KHR) {
        EGLint value = 0;

        egl_ok = eglGetConfigAttrib(egl_display, egl_config, EGL_NATIVE_VISUAL_ID, &value);
        if (egl_ok == EGL_FALSE) {
            LOG_EGL_ERROR(eglGetError(), "Couldn't query pixel format of EGL framebuffer config. eglGetConfigAttrib");
            return EIO;
        }

        ASSERT_EQUALS_MSG(
            value,
            get_pixfmt_info(pixel_format)->gbm_format,
            "EGL framebuffer config pixel format doesn't match the argument pixel format."
        );
    }
#endif

    gbm_surface = NULL;
    if (allowed_modifiers != NULL) {
        gbm_surface = gbm_surface_create_with_modifiers(
            gbm_device,
            size.x,
            size.y,
            get_pixfmt_info(pixel_format)->gbm_format,
            allowed_modifiers,
            n_allowed_modifiers
        );
        if (gbm_surface == NULL) {
            ok = errno;
            LOG_ERROR("Couldn't create GBM surface for rendering. gbm_surface_create_with_modifiers: %s\n", strerror(ok));
            LOG_ERROR("Will retry without modifiers\n");
        }
    }
    if (gbm_surface == NULL) {
        gbm_surface = gbm_surface_create(
            gbm_device,
            size.x,
            size.y,
            get_pixfmt_info(pixel_format)->gbm_format,
            GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT
        );
        if (gbm_surface == NULL) {
            ok = errno;
            LOG_ERROR("Couldn't create GBM surface for rendering. gbm_surface_create: %s\n", strerror(ok));
            return ok;
        }
    }

    /// TODO: Think about allowing different tilings / modifiers here
    if (egl_config == EGL_NO_CONFIG_KHR) {
        // choose a config
        egl_config = gl_renderer_choose_config_direct(renderer, pixel_format);
        if (egl_config == EGL_NO_CONFIG_KHR) {
            LOG_ERROR(
                "EGL doesn't supported the specified pixel format %s. Try a different one (ARGB8888 should always work).\n",
                get_pixfmt_info(pixel_format)->name
            );
            ok = EINVAL;
            goto fail_destroy_gbm_surface;
        }
    }

// EGLAttribKHR is defined by EGL_KHR_cl_event2.
#ifndef EGL_KHR_cl_event2
    #error "EGL header definitions for extension EGL_KHR_cl_event2 are required."
#endif

    static const EGLAttribKHR surface_attribs[] = {
        /* EGL_GL_COLORSPACE, GL_LINEAR / GL_SRGB */
        /* EGL_RENDER_BUFFER, EGL_BACK_BUFFER / EGL_SINGLE_BUFFER */
        /* EGL_VG_ALPHA_FORMAT, EGL_VG_ALPHA_FORMAT_NONPRE / EGL_VG_ALPHA_FORMAT_PRE */
        /* EGL_VG_COLORSPACE, EGL_VG_COLORSPACE_sRGB / EGL_VG_COLORSPACE_LINEAR */
        EGL_NONE,
    };

    (void) surface_attribs;

    egl_ok = eglBindAPI(EGL_OPENGL_ES_API);
    if (egl_ok == EGL_FALSE) {
        LOG_EGL_ERROR(eglGetError(), "Couldn't bind OpenGL ES API to EGL. eglBindAPI");
        ok = EIO;
        goto fail_destroy_gbm_surface;
    }

    egl_surface = gl_renderer_create_gbm_window_surface(renderer, egl_config, gbm_surface, NULL, NULL);
    if (egl_surface == EGL_NO_SURFACE) {
        ok = EIO;
        goto fail_destroy_gbm_surface;
    }

    /// TODO: Implement
    ok = render_surface_init(CAST_RENDER_SURFACE_UNCHECKED(s), tracer, size);
    if (ok != 0) {
        goto fail_destroy_egl_surface;
    }

    s->surface.present_kms = egl_gbm_render_surface_present_kms;
    s->surface.present_fbdev = egl_gbm_render_surface_present_fbdev;
    s->surface.deinit = egl_gbm_render_surface_deinit;
    s->render_surface.fill = egl_gbm_render_surface_fill;
    s->render_surface.queue_present = egl_gbm_render_surface_queue_present;
#ifdef DEBUG
    uuid_copy(&s->uuid, uuid);
#endif
    s->pixel_format = pixel_format;
    s->gbm_device = gbm_device;
    s->gbm_surface = gbm_surface;
    s->egl_display = egl_display;
    s->egl_surface = egl_surface;
    s->egl_config = egl_config;
    s->renderer = gl_renderer_ref(renderer);
    for (int i = 0; i < ARRAY_SIZE(s->locked_fbs); i++) {
        s->locked_fbs[i].is_locked = (atomic_flag) ATOMIC_FLAG_INIT;
    }
    s->locked_front_fb = NULL;
#ifdef DEBUG
    s->n_locked_fbs = 0;
    s->logged_format_and_modifier = false;
#endif
    return 0;

fail_destroy_egl_surface:
    eglDestroySurface(egl_display, egl_surface);

fail_destroy_gbm_surface:
    gbm_surface_destroy(gbm_surface);
    return ok;
}

/**
 * @brief Create a new gbm_surface based render surface, with an explicit EGL Config for the created EGLSurface.
 *
 * @param compositor The compositor that this surface will be registered to when calling surface_register.
 * @param size The size of the surface.
 * @param device The GBM device used to allocate the surface.
 * @param renderer The EGL/OpenGL used to create any GL surfaces.
 * @param pixel_format The pixel format to be used by the framebuffers of the surface.
 * @param egl_config The EGLConfig used for creating the EGLSurface.
 * @param allowed_modifiers The list of modifiers that gbm_surface_create_with_modifiers can choose from.
 *                          NULL if not specified. (In that case, gbm_surface_create will be used)
 * @param n_allowed_modifiers The number of modifiers in @param allowed_modifiers.
 * @return struct egl_gbm_render_surface*
 */
struct egl_gbm_render_surface *egl_gbm_render_surface_new_with_egl_config(
    struct tracer *tracer,
    struct vec2i size,
    struct gbm_device *device,
    struct gl_renderer *renderer,
    enum pixfmt pixel_format,
    EGLConfig egl_config,
    const uint64_t *allowed_modifiers,
    size_t n_allowed_modifiers
) {
    struct egl_gbm_render_surface *surface;
    int ok;

    surface = malloc(sizeof *surface);
    if (surface == NULL) {
        goto fail_return_null;
    }

    ok = egl_gbm_render_surface_init(
        surface,
        tracer,
        size,
        device,
        renderer,
        pixel_format,
        egl_config,
        allowed_modifiers,
        n_allowed_modifiers
    );
    if (ok != 0) {
        goto fail_free_surface;
    }

    return surface;

fail_free_surface:
    free(surface);

fail_return_null:
    return NULL;
}

/**
 * @brief Create a new gbm_surface based render surface.
 *
 * @param compositor The compositor that this surface will be registered to when calling surface_register.
 * @param size The size of the surface.
 * @param device The GBM device used to allocate the surface.
 * @param renderer The EGL/OpenGL used to create any GL surfaces.
 * @param pixel_format The pixel format to be used by the framebuffers of the surface.
 * @return struct egl_gbm_render_surface*
 */
struct egl_gbm_render_surface *egl_gbm_render_surface_new(
    struct tracer *tracer,
    struct vec2i size,
    struct gbm_device *device,
    struct gl_renderer *renderer,
    enum pixfmt pixel_format
) {
    return egl_gbm_render_surface_new_with_egl_config(tracer, size, device, renderer, pixel_format, EGL_NO_CONFIG_KHR, NULL, 0);
}

void egl_gbm_render_surface_deinit(struct surface *s) {
    struct egl_gbm_render_surface *egl_surface;

    egl_surface = CAST_EGL_GBM_RENDER_SURFACE(s);

    gl_renderer_unref(egl_surface->renderer);
    render_surface_deinit(s);
}

struct gbm_bo_meta {
    struct drmdev *drmdev;

    bool has_nonopaque_fb_id;
    uint32_t nonopaque_fb_id;

    bool has_opaque_fb_id;
    uint32_t opaque_fb_id;
};

static void on_destroy_gbm_bo_meta(struct gbm_bo *bo, void *meta_void) {
    struct gbm_bo_meta *meta;

    ASSERT_NOT_NULL(bo);
    ASSERT_NOT_NULL(meta_void);
    (void) bo;
    meta = meta_void;

    if (meta->has_nonopaque_fb_id) {
        drmdev_rm_fb(meta->drmdev, meta->nonopaque_fb_id);
    }
    if (meta->has_opaque_fb_id) {
        drmdev_rm_fb(meta->drmdev, meta->opaque_fb_id);
    }
    drmdev_unref(meta->drmdev);
    free(meta);
}

static void on_release_layer(void *userdata) {
    struct locked_fb *fb;

    ASSERT_NOT_NULL(userdata);

    fb = userdata;
    locked_fb_unref(fb);
}

static int egl_gbm_render_surface_present_kms(struct surface *s, const struct fl_layer_props *props, struct kms_req_builder *builder) {
    struct egl_gbm_render_surface *egl_surface;
    struct gbm_bo_meta *meta;
    struct drmdev *drmdev;
    struct gbm_bo *bo;
    enum pixfmt pixel_format;
    uint32_t fb_id, opaque_fb_id;
    int ok;

    egl_surface = CAST_THIS(s);

    /// TODO: Implement non axis-aligned fl_layer_props
    ASSERT_MSG(props->is_aa_rect, "only axis aligned view geometry is supported right now");

    surface_lock(s);

    ASSERT_NOT_NULL_MSG(
        egl_surface->locked_front_fb,
        "There's no framebuffer available for scanout right now. Make sure you called render_surface_queue_present() before presenting."
    );

    bo = egl_surface->locked_front_fb->bo;
    meta = gbm_bo_get_user_data(bo);
    if (meta == NULL) {
        meta = malloc(sizeof *meta);
        if (meta == NULL) {
            ok = ENOMEM;
            goto fail_unlock;
        }

        drmdev = kms_req_builder_get_drmdev(builder);
        ASSERT_NOT_NULL(drmdev);

        struct drm_crtc *crtc = kms_req_builder_get_crtc(builder);
        ASSERT_NOT_NULL(crtc);

        if (drm_crtc_any_plane_supports_format(drmdev, crtc, egl_surface->pixel_format)) {
            TRACER_BEGIN(egl_surface->surface.tracer, "drmdev_add_fb (non-opaque)");
            fb_id = drmdev_add_fb_from_gbm_bo(
                drmdev,
                bo,
                /* cast_opaque */ false
            );
            TRACER_END(egl_surface->surface.tracer, "drmdev_add_fb (non-opaque)");

            if (fb_id == 0) {
                ok = EIO;
                LOG_ERROR("Couldn't add GBM buffer as DRM framebuffer.\n");
                goto fail_free_meta;
            }

            meta->has_nonopaque_fb_id = true;
            meta->nonopaque_fb_id = fb_id;
        } else {
            meta->has_nonopaque_fb_id = false;
            meta->nonopaque_fb_id = 0;
        }

        // if this EGL surface is non-opaque and has an opaque equivalent
        if (!get_pixfmt_info(egl_surface->pixel_format)->is_opaque &&
            pixfmt_opaque(egl_surface->pixel_format) != egl_surface->pixel_format &&
            drm_crtc_any_plane_supports_format(drmdev, crtc, pixfmt_opaque(egl_surface->pixel_format))) {
            opaque_fb_id = drmdev_add_fb_from_gbm_bo(
                drmdev,
                bo,
                /* cast_opaque */ true
            );
            if (opaque_fb_id == 0) {
                ok = EIO;
                LOG_ERROR("Couldn't add GBM buffer as opaque DRM framebuffer.\n");
                goto fail_remove_fb;
            }

            meta->has_opaque_fb_id = true;
            meta->opaque_fb_id = opaque_fb_id;
        } else {
            meta->has_opaque_fb_id = false;
            meta->opaque_fb_id = 0;
        }

        if (!meta->has_nonopaque_fb_id && !meta->has_opaque_fb_id) {
            ok = EIO;
            LOG_ERROR("Couldn't add GBM buffer as DRM framebuffer.\n");
            goto fail_free_meta;
        }

        meta->drmdev = drmdev_ref(drmdev);
        meta->nonopaque_fb_id = fb_id;
        gbm_bo_set_user_data(bo, meta, on_destroy_gbm_bo_meta);
    } else {
        // We can only add this GBM BO to a single KMS device as an fb right now.
        ASSERT_EQUALS_MSG(
            meta->drmdev,
            kms_req_builder_get_drmdev(builder),
            "Currently GBM BOs can only be scanned out on a single KMS device for their whole lifetime."
        );
    }

    /*
    LOG_DEBUG(
        "egl_gbm_render_surface_present_kms:\n"
        "    src_x, src_y, src_w, src_h: %f %f %f %f\n"
        "    dst_x, dst_y, dst_w, dst_h: %f %f %f %f\n",
        0.0, 0.0,
        s->render_surface.size.x,
        s->render_surface.size.y,
        props->aa_rect.offset.x,
        props->aa_rect.offset.y,
        props->aa_rect.size.x,
        props->aa_rect.size.y
    );
    */

    // So we just cast our fb to an XRGB8888 framebuffer and scanout that instead.
    if (meta->has_nonopaque_fb_id && !meta->has_opaque_fb_id) {
        fb_id = meta->nonopaque_fb_id;
        pixel_format = egl_surface->pixel_format;
    } else if (meta->has_opaque_fb_id && !meta->has_nonopaque_fb_id) {
        fb_id = meta->opaque_fb_id;
        pixel_format = pixfmt_opaque(egl_surface->pixel_format);
    } else {
        // We have both an opaque and a non-opaque framebuffer.

        // The bottom-most layer should preferably be an opaque layer.
        // We could also try non-opaque first and fallback to opaque if not supported though.
        if (kms_req_builder_prefer_next_layer_opaque(builder)) {
            fb_id = meta->opaque_fb_id;
            pixel_format = pixfmt_opaque(egl_surface->pixel_format);
        } else {
            fb_id = meta->nonopaque_fb_id;
            pixel_format = egl_surface->pixel_format;
        }
    }

    TRACER_BEGIN(egl_surface->surface.tracer, "kms_req_builder_push_fb_layer");
    ok = kms_req_builder_push_fb_layer(
        builder,
        &(const struct kms_fb_layer){
            .drm_fb_id = fb_id,
            .format = pixel_format,
            .has_modifier = gbm_bo_get_modifier(bo) != DRM_FORMAT_MOD_INVALID,
            .modifier = gbm_bo_get_modifier(bo),

            .dst_x = (int32_t) props->aa_rect.offset.x,
            .dst_y = (int32_t) props->aa_rect.offset.y,
            .dst_w = (uint32_t) props->aa_rect.size.x,
            .dst_h = (uint32_t) props->aa_rect.size.y,

            .src_x = 0,
            .src_y = 0,
            .src_w = DOUBLE_TO_FP1616_ROUNDED(egl_surface->render_surface.size.x),
            .src_h = DOUBLE_TO_FP1616_ROUNDED(egl_surface->render_surface.size.y),

            .has_rotation = false,
            .rotation = PLANE_TRANSFORM_ROTATE_0,

            .has_in_fence_fd = false,
            .in_fence_fd = 0,
        },
        on_release_layer,
        NULL,
        locked_fb_ref(egl_surface->locked_front_fb)
    );
    TRACER_END(egl_surface->surface.tracer, "kms_req_builder_push_fb_layer");
    if (ok != 0) {
        goto fail_unref_locked_fb;
    }

    surface_unlock(s);
    return ok;

fail_unref_locked_fb:
    locked_fb_unref(egl_surface->locked_front_fb);
    goto fail_unlock;

fail_remove_fb:
    drmdev_rm_fb(drmdev, fb_id);

fail_free_meta:
    free(meta);

fail_unlock:
    surface_unlock(s);
    return ok;
}

static int
egl_gbm_render_surface_present_fbdev(struct surface *s, const struct fl_layer_props *props, struct fbdev_commit_builder *builder) {
    struct egl_gbm_render_surface *egl_surface;

    /// TODO: Implement by mmapping the current front bo, copy it into the fbdev
    /// TODO: Print a warning here if we're not using explicit linear tiling and use glReadPixels instead of gbm_bo_map in that case

    egl_surface = CAST_THIS(s);
    (void) egl_surface;
    (void) props;
    (void) builder;

    UNIMPLEMENTED();

    return 0;
}

static int egl_gbm_render_surface_fill(struct render_surface *s, FlutterBackingStore *fl_store) {
    fl_store->type = kFlutterBackingStoreTypeOpenGL;
    fl_store->open_gl = (FlutterOpenGLBackingStore
    ){ .type = kFlutterOpenGLTargetTypeFramebuffer,
       .framebuffer = { /* for some reason flutter wants this to be GL_BGRA8_EXT, contrary to what the docs say */
                        .target = GL_BGRA8_EXT,

                        /* 0 refers to the window surface, instead of to an FBO */
                        .name = 0,

                        /*
             * even though the compositor will call surface_ref too to fill the FlutterBackingStore.user_data,
             * we need to ref two times because flutter will call both this destruction callback and the
             * compositor collect callback
             */
                        .user_data = surface_ref(CAST_SURFACE_UNCHECKED(s)),
                        .destruction_callback = surface_unref_void } };
    return 0;
}

static int egl_gbm_render_surface_queue_present(struct render_surface *s, const FlutterBackingStore *fl_store) {
    UNUSED struct egl_gbm_render_surface *egl_surface;
    struct gbm_bo *bo;
    UNUSED EGLBoolean egl_ok;
    int i, ok;

    egl_surface = CAST_THIS(s);
    (void) fl_store;

    surface_lock(CAST_SURFACE(s));

    /// TODO: Handle fl_store->did_update == false here

    // Unref the old front fb so potentially one of the locked_fbs entries gets freed
    if (egl_surface->locked_front_fb != NULL) {
        locked_fb_unrefp(&egl_surface->locked_front_fb);
    }

    assert(gbm_surface_has_free_buffers(egl_surface->gbm_surface));

    // create the in fence here
    TRACER_BEGIN(s->surface.tracer, "eglSwapBuffers");
    egl_ok = eglSwapBuffers(egl_surface->egl_display, egl_surface->egl_surface);
    TRACER_END(s->surface.tracer, "eglSwapBuffers");

    if (egl_ok != EGL_TRUE) {
        LOG_EGL_ERROR(eglGetError(), "Couldn't flush rendering. eglSwapBuffers");
        return EIO;
    }

    TRACER_BEGIN(s->surface.tracer, "gbm_surface_lock_front_buffer");
    bo = gbm_surface_lock_front_buffer(egl_surface->gbm_surface);
    TRACER_END(s->surface.tracer, "gbm_surface_lock_front_buffer");

#ifdef DEBUG
    if (!egl_surface->logged_format_and_modifier) {
        uint32_t fourcc = gbm_bo_get_format(bo);
        uint64_t modifier = gbm_bo_get_modifier(bo);

        bool has_format = has_pixfmt_for_gbm_format(fourcc);
        enum pixfmt format = has_format ? get_pixfmt_for_gbm_format(fourcc) : PIXFMT_RGB565;

        LOG_DEBUG(
            "using fourcc %c%c%c%c (%s) with modifier 0x%" PRIx64 "\n",
            fourcc & 0xFF,
            (fourcc >> 8) & 0xFF,
            (fourcc >> 16) & 0xFF,
            (fourcc >> 24) & 0xFF,
            has_format ? get_pixfmt_info(format)->name : "?",
            modifier
        );

        egl_surface->logged_format_and_modifier = true;
    }
#endif

    if (bo == NULL) {
        ok = errno;
        LOG_ERROR("Couldn't lock GBM front buffer. gbm_surface_lock_front_buffer: %s\n", strerror(ok));
        goto fail_unlock;
    }

    // Try to find & lock a locked_fb we can use.
    // Note we use atomics here even though we hold the surfaces' mutex because
    // releasing a locked_fb is possibly done without the mutex.
    for (i = 0; i < ARRAY_SIZE(egl_surface->locked_fbs); i++) {
        if (atomic_flag_test_and_set(&egl_surface->locked_fbs[i].is_locked) == false) {
            goto locked;
        }
    }

    // If we reached this point, we couldn't find lock one of the 4 locked_fbs.
    // Which shouldn't happen except we have an application bug.

    /// FIXME: This sometimes (rarely) fails with EGL surface backing stores.
    ///   See below:
    ///
    ///   pi@hpi4:~ $ LD_LIBRARY_PATH=~/mesa-install/lib/arm-linux-gnueabihf/ LIBGL_DRIVERS_PATH=~/mesa-install/lib/arm-linux-gnueabihf/dri/ ~/devel/flutterpi-install/bin/flutter-pi ~/devel/flutterpi_platform_view_test_debug/ --vm-service-host=192.168.178.11
    ///   ==============Locale==============
    ///   Flutter locale:
    ///     default: de_DE
    ///     locales: de_DE de.UTF-8 de.UTF-8 de.UTF-8 de_DE de de.UTF-8
    ///   ===================================
    ///   ===================================
    ///   EGL information:
    ///     version: 1.4
    ///     vendor: Mesa Project
    ///     client extensions: EGL_EXT_client_extensions EGL_EXT_device_base EGL_EXT_device_enumeration EGL_EXT_device_query EGL_EXT_platform_base EGL_KHR_client_get_all_proc_addresses EGL_KHR_debug EGL_EXT_platform_device EGL_MESA_platform_gbm EGL_KHR_platform_gbm EGL_MESA_platform_surfaceless
    ///     display extensions: EGL_ANDROID_blob_cache EGL_EXT_buffer_age EGL_EXT_image_dma_buf_import EGL_EXT_image_dma_buf_import_modifiers EGL_KHR_cl_event2 EGL_KHR_config_attribs EGL_KHR_context_flush_control EGL_KHR_create_context EGL_KHR_create_context_no_error EGL_KHR_fence_sync EGL_KHR_get_all_proc_addresses EGL_KHR_gl_colorspace EGL_KHR_gl_renderbuffer_image EGL_KHR_gl_texture_2D_image EGL_KHR_gl_texture_3D_image EGL_KHR_gl_texture_cubemap_image EGL_KHR_image EGL_KHR_image_base EGL_KHR_image_pixmap EGL_KHR_no_config_context EGL_KHR_reusable_sync EGL_KHR_surfaceless_context EGL_EXT_pixel_format_float EGL_KHR_wait_sync EGL_MESA_configless_context EGL_MESA_drm_image EGL_MESA_image_dma_buf_export EGL_MESA_query_driver
    ///   ===================================
    ///   ===================================
    ///   OpenGL ES information:
    ///     version: "OpenGL ES 3.1 Mesa 23.2.0-devel (git-8bfd18b8c5)"
    ///     shading language version: "OpenGL ES GLSL ES 3.10"
    ///     vendor: "Broadcom"
    ///     renderer: "V3D 4.2"
    ///     extensions: "GL_EXT_blend_minmax GL_EXT_multi_draw_arrays GL_EXT_texture_filter_anisotropic GL_EXT_texture_compression_s3tc GL_EXT_texture_compression_dxt1 GL_EXT_texture_compression_rgtc GL_EXT_texture_format_BGRA8888 GL_OES_compressed_ETC1_RGB8_texture GL_OES_depth24 GL_OES_element_index_uint GL_OES_fbo_render_mipmap GL_OES_mapbuffer GL_OES_rgb8_rgba8 GL_OES_standard_derivatives GL_OES_stencil8 GL_OES_texture_3D GL_OES_texture_float GL_OES_texture_half_float GL_OES_texture_half_float_linear GL_OES_texture_npot GL_OES_vertex_half_float GL_EXT_draw_instanced GL_EXT_texture_sRGB_decode GL_OES_EGL_image GL_OES_depth_texture GL_AMD_performance_monitor GL_OES_packed_depth_stencil GL_EXT_texture_type_2_10_10_10_REV GL_NV_conditional_render GL_OES_get_program_binary GL_APPLE_texture_max_level GL_EXT_discard_framebuffer GL_EXT_read_format_bgra GL_NV_pack_subimage GL_EXT_frag_depth GL_NV_fbo_color_attachments GL_OES_EGL_image_external GL_OES_EGL_sync GL_OES_vertex_array_object GL_ANGLE_pack_reverse_row_order GL_ANGLE_texture_compression_dxt3 GL_ANGLE_texture_compression_dxt5 GL_EXT_occlusion_query_boolean GL_EXT_texture_rg GL_EXT_unpack_subimage GL_NV_draw_buffers GL_NV_read_buffer GL_NV_read_depth GL_NV_read_depth_stencil GL_NV_read_stencil GL_EXT_draw_buffers GL_EXT_instanced_arrays GL_EXT_map_buffer_range GL_KHR_debug GL_KHR_texture_compression_astc_ldr GL_NV_generate_mipmap_sRGB GL_NV_pixel_buffer_object GL_OES_depth_texture_cube_map GL_OES_required_internalformat GL_OES_surfaceless_context GL_EXT_color_buffer_float GL_EXT_debug_label GL_EXT_sRGB_write_control GL_EXT_separate_shader_objects GL_EXT_shader_implicit_conversions GL_EXT_shader_integer_mix GL_EXT_base_instance GL_EXT_compressed_ETC1_RGB8_sub_texture GL_EXT_copy_image GL_EXT_draw_buffers_indexed GL_EXT_draw_elements_base_vertex GL_EXT_polygon_offset_clamp GL_EXT_primitive_bounding_box GL_EXT_shader_io_blocks GL_EXT_texture_border_clamp GL_EXT_texture_cube_map_array GL_EXT_texture_view GL_KHR_context_flush_control GL_NV_image_formats GL_NV_shader_noperspective_interpolation GL_OES_copy_image GL_OES_draw_buffers_indexed GL_OES_draw_elements_base_vertex GL_OES_primitive_bounding_box GL_OES_shader_io_blocks GL_OES_texture_border_clamp GL_OES_texture_cube_map_array GL_OES_texture_stencil8 GL_OES_texture_storage_multisample_2d_array GL_OES_texture_view GL_EXT_buffer_storage GL_EXT_float_blend GL_EXT_geometry_point_size GL_EXT_geometry_shader GL_KHR_no_error GL_KHR_texture_compression_astc_sliced_3d GL_OES_EGL_image_external_essl3 GL_OES_geometry_point_size GL_OES_geometry_shader GL_OES_shader_image_atomic GL_EXT_texture_compression_s3tc_srgb GL_MESA_shader_integer_functions GL_EXT_texture_mirror_clamp_to_edge GL_KHR_parallel_shader_compile GL_EXT_EGL_image_storage GL_MESA_framebuffer_flip_y GL_EXT_texture_query_lod GL_MESA_bgra "
    ///   ===================================
    ///   window.c: INFO: display has non-square pixels. Non-square-pixels are not supported by flutter.
    ///   display mode:
    ///     resolution: 800 x 480
    ///     refresh rate: 59.928489Hz
    ///     physical size: 154mm x 86mm
    ///     flutter device pixel ratio: 1.367054
    ///     pixel format: (any)
    ///   pluginregistry.c: Initialized plugins: services, text input, raw keyboard plugin, gstreamer video_player, audioplayers,
    ///   flutter: The Dart VM service is listening on http://192.168.178.11:44515/F3wK7cUNFd0=/
    ///   gl_renderer.c: Choosing EGL config with pixel format ARGB 8:8:8:8...
    ///   gl_renderer.c: Choosing EGL config with pixel format ARGB 8:8:8:8...
    ///   compositor_ng.c: fl_layer_props[1]:
    ///     is_aa_rect: yes
    ///     aa_rect: offset: (0,000000, 76,555023), size: (799,999992, 403,444973)
    ///     quad: top left: (0,000000, 76,555023), top right: (799,999992, 76,555023), bottom left: (0,000000, 479,999996), bottom right: (799,999992, 479,999996)
    ///     opacity: 1,000000
    ///     rotation: 0,000000
    ///     n_clip_rects: 0
    ///     clip_rects: (nil)
    ///   egl_gbm_render_surface.c: egl_gbm_render_surface_present_kms:
    ///       src_x, src_y, src_w, src_h: 0 0 800 480
    ///       dst_x, dst_y, dst_w, dst_h: 0,000000 0,000000 800,000000 480,000000
    ///   egl_gbm_render_surface.c: egl_gbm_render_surface_present_kms:
    ///       src_x, src_y, src_w, src_h: 0 0 800 480
    ///       dst_x, dst_y, dst_w, dst_h: 0,000000 76,555023 799,999992 403,444973
    ///   egl_gbm_render_surface.c: egl_gbm_render_surface_present_kms:
    ///       src_x, src_y, src_w, src_h: 0 0 800 480
    ///       dst_x, dst_y, dst_w, dst_h: 0,000000 0,000000 800,000000 480,000000
    ///   flutter-pi: egl_gbm_render_surface.c:641: int egl_gbm_render_surface_queue_present(struct render_surface *, const FlutterBackingStore *): Zusicherung »(0) && ("Couldn't find a free slot to lock the surfaces front framebuffer.")« nicht erfüllt.

    ASSERT_MSG(false, "Couldn't find a free slot to lock the surfaces front framebuffer.");
    ok = EIO;
    goto fail_release_bo;

locked:
    /// TODO: Remove this once we're using triple buffering
    //ASSERT_MSG(atomic_fetch_add(&render_surface->n_locked_fbs, 1) <= 1, "sanity check failed: too many locked fbs for double-buffered vsync");
    egl_surface->locked_fbs[i].bo = bo;
    egl_surface->locked_fbs[i].surface = CAST_THIS(surface_ref(CAST_SURFACE(s)));
    egl_surface->locked_fbs[i].n_refs = REFCOUNT_INIT_1;
    egl_surface->locked_front_fb = egl_surface->locked_fbs + i;
    surface_unlock(CAST_SURFACE(s));
    return 0;

fail_release_bo:
    gbm_surface_release_buffer(egl_surface->gbm_surface, bo);

fail_unlock:
    surface_unlock(CAST_SURFACE(s));
    return ok;
}

/**
 * @brief Get the EGL Surface for rendering into this render surface.
 *
 * Flutter doesn't really support backing stores to be EGL Surfaces, so we have to hack around this, kinda.
 *
 * @param s
 * @return EGLSurface The EGLSurface associated with this render surface. Only valid for the lifetime of this egl_gbm_render_surface.
 */
ATTR_PURE EGLSurface egl_gbm_render_surface_get_egl_surface(struct egl_gbm_render_surface *s) {
    return s->egl_surface;
}

/**
 * @brief Get the EGLConfig that was used to create the EGLSurface for this render surface.
 *
 * If the display doesn't support EGL_KHR_no_config_context, we need to create the EGL rendering context with
 * the same EGLConfig as every EGLSurface we want to bind to it. So we can just let egl_gbm_render_surface choose a config
 * and let flutter-pi query that config when creating the rendering contexts in that case.
 *
 * @param s
 * @return EGLConfig The chosen EGLConfig. Valid forever.
 */
ATTR_PURE EGLConfig egl_gbm_render_surface_get_egl_config(struct egl_gbm_render_surface *s) {
    return s->egl_config;
}
