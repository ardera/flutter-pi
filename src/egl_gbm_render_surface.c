// SPDX-License-Identifier: MIT
/*
 * GBM Surface Backing Stores
 *
 * - implements EGL/GL render surfaces using a gbm surface
 * - ideal way to create render surfaces right now
 *
 * Copyright (c) 2022, Hannes Winkler <hanneswinkler2000@web.de>
 */


#include <stdlib.h>

#include <collection.h>
#include <pixel_format.h>
#include <modesetting.h>
#include <egl.h>
#include <gles.h>
#include <surface.h>
#include <render_surface.h>
#include <render_surface_private.h>
#include <egl_gbm_render_surface.h>
#include <tracer.h>
#include <gl_renderer.h>

FILE_DESCR("EGL/GBM render surface")

#ifndef HAS_EGL
#   error "EGL is needed for EGL/GBM render surface."
#endif

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

    uuid_t uuid;
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
#endif
};

COMPILE_ASSERT(offsetof(struct egl_gbm_render_surface, surface) == 0);
COMPILE_ASSERT(offsetof(struct egl_gbm_render_surface, render_surface.surface) == 0);

static const uuid_t uuid = CONST_UUID(0xf9, 0xc2, 0x5d, 0xad, 0x2e, 0x3b, 0x4e, 0x2c, 0x9d, 0x26, 0x64, 0x70, 0xfa, 0x9a, 0x25, 0xd9);

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
    DEBUG_ASSERT(uuid_equals(s->uuid, uuid));
    return s;
}
#endif

void egl_gbm_render_surface_deinit(struct surface *s);
static int egl_gbm_render_surface_present_kms(struct surface *s, const struct fl_layer_props *props, struct kms_req_builder *builder);
static int egl_gbm_render_surface_present_fbdev(struct surface *s, const struct fl_layer_props *props, struct fbdev_commit_builder *builder);
static int egl_gbm_render_surface_fill(struct render_surface *s, FlutterBackingStore *fl_store);
static int egl_gbm_render_surface_queue_present(struct render_surface *s, const FlutterBackingStore *fl_store);

int egl_gbm_render_surface_init(
    struct egl_gbm_render_surface *s,
    struct tracer *tracer,
    struct vec2f size,
    struct gbm_device *gbm_device,
    struct gl_renderer *renderer,
    enum pixfmt pixel_format,
    EGLConfig egl_config
) {
    struct gbm_surface *gbm_surface;
    EGLDisplay egl_display;
    EGLSurface egl_surface;
    EGLBoolean egl_ok;
    int ok;

    DEBUG_ASSERT_NOT_NULL(renderer);
    DEBUG_ASSERT_PIXFMT_VALID(pixel_format);
    egl_display = gl_renderer_get_egl_display(renderer);
    DEBUG_ASSERT_NOT_NULL(egl_display);

#ifdef DEBUG
    if (egl_config != EGL_NO_CONFIG_KHR) {
        EGLint value = 0;
        
        egl_ok = eglGetConfigAttrib(egl_display, egl_config, EGL_NATIVE_VISUAL_ID, &value);
        if (egl_ok == EGL_FALSE) {
            LOG_ERROR("Couldn't query pixel format of EGL framebuffer config. eglGetConfigAttrib: 0x%08X\n", eglGetError());
            return EIO;
        }

        DEBUG_ASSERT_EQUALS_MSG(value, get_pixfmt_info(pixel_format)->gbm_format, "EGL framebuffer config pixel format doesn't match the argument pixel format.");
    }
#endif
    /// TODO: Think about allowing different tilings / modifiers here
    gbm_surface = gbm_surface_create(
        gbm_device,
        (uint32_t) size.x, (uint32_t) size.y,
        get_pixfmt_info(pixel_format)->gbm_format,
        GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT
    );
    if (gbm_surface == NULL) {
        ok = errno;
        LOG_ERROR("Couldn't create GBM surface for rendering. gbm_surface_create_with_modifiers: %s\n", strerror(ok));
        return ok;
    }

    if (egl_config == EGL_NO_CONFIG_KHR) {
        // choose a config
        egl_config = gl_renderer_choose_config_direct(renderer, pixel_format);
        if (egl_config == EGL_NO_CONFIG_KHR) {
            LOG_ERROR("EGL doesn't supported the specified pixel format %s. Try a different one (ARGB8888 should always work).\n", get_pixfmt_info(pixel_format)->name);
            ok = EINVAL;
            goto fail_destroy_gbm_surface;
        }
    }

    static const EGLAttrib surface_attribs[] = {
        /* EGL_GL_COLORSPACE, GL_LINEAR / GL_SRGB */
        /* EGL_RENDER_BUFFER, EGL_BACK_BUFFER / EGL_SINGLE_BUFFER */
        /* EGL_VG_ALPHA_FORMAT, EGL_VG_ALPHA_FORMAT_NONPRE / EGL_VG_ALPHA_FORMAT_PRE */
        /* EGL_VG_COLORSPACE, EGL_VG_COLORSPACE_sRGB / EGL_VG_COLORSPACE_LINEAR */
        EGL_NONE
    };

    (void) surface_attribs;

    egl_ok = eglBindAPI(EGL_OPENGL_ES_API);
    if (egl_ok == EGL_FALSE) {
        LOG_ERROR("Couldn't bind OpenGL ES API to EGL. eglBindAPI: 0x%08X\n", eglGetError());
        ok = EIO;
        goto fail_destroy_gbm_surface;
    }

    egl_surface = eglCreatePlatformWindowSurface(
        egl_display,
        egl_config,
        gbm_surface,
        NULL
    );
    if (egl_surface == EGL_NO_SURFACE) {
        LOG_ERROR("Could not create EGL rendering surface. eglCreatePlatformWindowSurface: 0x%08X\n", eglGetError());
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
    uuid_copy(&s->uuid, uuid);
    s->pixel_format = pixel_format;
    s->gbm_device = gbm_device;
    s->gbm_surface = gbm_surface;
    s->egl_display = egl_display;
    s->egl_surface = egl_surface;
    s->egl_config = egl_config;
    s->renderer = gl_renderer_ref(renderer);
    for (int i = 0; i < ARRAY_SIZE(s->locked_fbs); i++) {
        s->locked_fbs->is_locked = (atomic_flag) ATOMIC_FLAG_INIT;
    }
    s->locked_front_fb = NULL;
    return 0;


    fail_destroy_egl_surface:
#ifdef HAS_EGL
    eglDestroySurface(egl_display, egl_surface);
    
    fail_destroy_gbm_surface:
#endif
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
 * @return struct egl_gbm_render_surface* 
 */
ATTR_MALLOC struct egl_gbm_render_surface *egl_gbm_render_surface_new_with_egl_config(
    struct tracer *tracer,
    struct vec2f size,
    struct gbm_device *device,
    struct gl_renderer *renderer,
    enum pixfmt pixel_format,
    EGLConfig egl_config
) {
    struct egl_gbm_render_surface *surface;
    int ok;
    
    surface = malloc(sizeof *surface);
    if (surface == NULL) {
        goto fail_return_null;
    }

    ok = egl_gbm_render_surface_init(surface, tracer, size, device, renderer, pixel_format, egl_config);
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
ATTR_MALLOC struct egl_gbm_render_surface *egl_gbm_render_surface_new(
    struct tracer *tracer,
    struct vec2f size,
    struct gbm_device *device,
    struct gl_renderer *renderer,
    enum pixfmt pixel_format
) {
    return egl_gbm_render_surface_new_with_egl_config(
        tracer,
        size,
        device,
        renderer,
        pixel_format,
        EGL_NO_CONFIG_KHR
    );
}

void egl_gbm_render_surface_deinit(struct surface *s) {
    struct egl_gbm_render_surface *egl_surface;

    egl_surface = CAST_EGL_GBM_RENDER_SURFACE(s);

    gl_renderer_unref(egl_surface->renderer);
    render_surface_deinit(s);
}

struct gbm_bo_meta {
    struct drmdev *drmdev;
    uint32_t fb_id;
};

static void on_destroy_gbm_bo_meta(struct gbm_bo *bo, void *meta_void) {
    struct gbm_bo_meta *meta;
    int ok;
    
    DEBUG_ASSERT_NOT_NULL(bo);
    DEBUG_ASSERT_NOT_NULL(meta_void);
    (void) bo;
    meta = meta_void;

    ok = drmdev_rm_fb(meta->drmdev, meta->fb_id);
    if (ok != 0) {
        LOG_ERROR("Couldn't remove DRM framebuffer.\n");
    }

    drmdev_unref(meta->drmdev);
    free(meta);
}

static void on_release_layer(void *userdata) {
    struct locked_fb *fb;

    DEBUG_ASSERT_NOT_NULL(userdata);

    fb = userdata;
    locked_fb_unref(fb);
}

static int egl_gbm_render_surface_present_kms(struct surface *s, const struct fl_layer_props *props, struct kms_req_builder *builder) {
    struct egl_gbm_render_surface *egl_surface;
    struct gbm_bo_meta *meta;
    struct drmdev *drmdev;
    struct gbm_bo *bo;
    enum pixfmt pixel_format;
    uint32_t fb_id;
    int ok;

    egl_surface = CAST_THIS(s);

    /// TODO: Implement non axis-aligned fl_layer_props
    DEBUG_ASSERT_MSG(props->is_aa_rect, "only axis aligned view geometry is supported right now");

    surface_lock(s);

    DEBUG_ASSERT_NOT_NULL_MSG(egl_surface->locked_front_fb, "There's no framebuffer available for scanout right now. Make sure you called render_surface_queue_present() before presenting.");

    bo = egl_surface->locked_front_fb->bo;
    meta = gbm_bo_get_user_data(bo);
    if (meta == NULL) {
        meta = malloc(sizeof *meta);
        if (meta == NULL) {
            ok = ENOMEM;
            goto fail_unlock;
        }

        drmdev = kms_req_builder_get_drmdev(builder);
        DEBUG_ASSERT_NOT_NULL(drmdev);

        TRACER_BEGIN(egl_surface->surface.tracer, "drmdev_add_fb (non-opaque)");
        fb_id = drmdev_add_fb(
            drmdev,
            gbm_bo_get_width(bo),
            gbm_bo_get_height(bo),
            egl_surface->pixel_format,
            gbm_bo_get_handle(bo).u32,
            gbm_bo_get_stride(bo),
            gbm_bo_get_offset(bo, 0),
            true, gbm_bo_get_modifier(bo)
        );
        TRACER_END(egl_surface->surface.tracer, "drmdev_add_fb (non-opaque)");

        if (fb_id == 0) {
            ok = EIO;
            LOG_ERROR("Couldn't add GBM buffer as DRM framebuffer.\n");
            goto fail_free_meta;
        }

        meta->drmdev = drmdev_ref(drmdev);
        meta->fb_id = fb_id;
        gbm_bo_set_user_data(bo, meta, on_destroy_gbm_bo_meta);
    } else {
        // We can only add this GBM BO to a single KMS device as an fb right now.
        DEBUG_ASSERT_EQUALS_MSG(meta->drmdev, kms_req_builder_get_drmdev(builder), "Currently GBM BOs can only be scanned out on a single KMS device for their whole lifetime.");
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

    // The bottom-most layer should preferably be an opaque layer.
    // For example, on Pi 4, even though ARGB8888 is listed as supported for the primary plane,
    // rendering is completely off.
    // So we just cast our fb to an XRGB8888 framebuffer and scanout that instead.
    fb_id = meta->fb_id;
    pixel_format = egl_surface->pixel_format;

    TRACER_BEGIN(egl_surface->surface.tracer, "kms_req_builder_push_fb_layer");
    ok = kms_req_builder_push_fb_layer(
        builder,
        &(const struct kms_fb_layer) {
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
            .in_fence_fd = 0
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

    fail_free_meta:
    free(meta);

    fail_unlock:
    surface_unlock(s);
    return ok;
}

static int egl_gbm_render_surface_present_fbdev(struct surface *s, const struct fl_layer_props *props, struct fbdev_commit_builder *builder) {
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
    fl_store->open_gl = (FlutterOpenGLBackingStore) {
        .type = kFlutterOpenGLTargetTypeFramebuffer,
        .framebuffer = {
            /* for some reason flutter wants this to be GL_BGRA8_EXT, contrary to what the docs say */
            .target = GL_BGRA8_EXT, 

            /* 0 refers to the window surface, instead of to an FBO */
            .name = 0, 

            /*
             * even though the compositor will call surface_ref too to fill the FlutterBackingStore.user_data,
             * we need to ref two times because flutter will call both this destruction callback and the
             * compositor collect callback
             */
            .user_data = surface_ref(CAST_SURFACE_UNCHECKED(s)),
            .destruction_callback = surface_unref_void
        }
    };
    return 0;
}

static int egl_gbm_render_surface_queue_present(struct render_surface *s, const FlutterBackingStore *fl_store) {
    MAYBE_UNUSED struct egl_gbm_render_surface *egl_surface;
    struct gbm_bo *bo;
    MAYBE_UNUSED EGLBoolean egl_ok;
    int i, ok;
    
    egl_surface = CAST_THIS(s);
    (void) fl_store;

    surface_lock(CAST_SURFACE(s));

    /// TODO: Handle fl_store->did_update == false here

    // Unref the old front fb so potentially one of the locked_fbs entries gets freed 
    if (egl_surface->locked_front_fb != NULL) {
        locked_fb_unrefp(&egl_surface->locked_front_fb);
    }

    DEBUG_ASSERT(gbm_surface_has_free_buffers(egl_surface->gbm_surface));

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
    DEBUG_ASSERT_MSG(false, "Couldn't find a free slot to lock the surfaces front framebuffer.");
    ok = EIO;
    goto fail_release_bo;

    locked:
    /// TODO: Remove this once we're using triple buffering
    //DEBUG_ASSERT_MSG(atomic_fetch_add(&render_surface->n_locked_fbs, 1) <= 1, "sanity check failed: too many locked fbs for double-buffered vsync");
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
