// SPDX-License-Identifier: MIT
/*
 * GBM Surface Backing Stores
 *
 * - implements backing store using a gbm surface
 * - ideal way to create backing stores right now
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
#include <backing_store.h>
#include <backing_store_private.h>
#include <gbm_surface_backing_store.h>
#include <tracer.h>
#include <gl_renderer.h>

FILE_DESCR("gbm surface backing store")

#ifndef HAS_EGL
#   error "EGL is needed for GBM surface backing store."
#endif

struct gbm_surface_backing_store;

struct locked_fb {
    atomic_flag is_locked;
    struct gbm_surface_backing_store *store;
    struct gbm_bo *bo;
    refcount_t n_refs;
};

struct gbm_surface_backing_store {
    union {
        struct surface surface;
        struct backing_store backing_store;
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

COMPILE_ASSERT(offsetof(struct gbm_surface_backing_store, surface) == 0);
COMPILE_ASSERT(offsetof(struct gbm_surface_backing_store, backing_store.surface) == 0);

static const uuid_t uuid = CONST_UUID(0xf9, 0xc2, 0x5d, 0xad, 0x2e, 0x3b, 0x4e, 0x2c, 0x9d, 0x26, 0x64, 0x70, 0xfa, 0x9a, 0x25, 0xd9);

#define CAST_THIS(ptr) CAST_GBM_SURFACE_BACKING_STORE(ptr)
#define CAST_THIS_UNCHECKED(ptr) CAST_GBM_SURFACE_BACKING_STORE_UNCHECKED(ptr)

static void locked_fb_destroy(struct locked_fb *fb) {
    struct gbm_surface_backing_store *store;

    store = fb->store;
    fb->store = NULL;
    gbm_surface_release_buffer(store->gbm_surface, fb->bo);
#ifdef DEBUG
    atomic_fetch_sub(&store->n_locked_fbs, 1);
#endif
    atomic_flag_clear(&fb->is_locked);
    surface_unref(CAST_SURFACE(store));
}

DEFINE_STATIC_REF_OPS(locked_fb, n_refs)

#ifdef DEBUG
ATTR_PURE struct gbm_surface_backing_store *__checked_cast_gbm_surface_backing_store(void *ptr) {
    struct gbm_surface_backing_store *store;
    
    store = CAST_GBM_SURFACE_BACKING_STORE_UNCHECKED(ptr);
    DEBUG_ASSERT(uuid_equals(store->uuid, uuid));
    return store;
}
#endif

void gbm_surface_backing_store_deinit(struct surface *s);
static int gbm_surface_backing_store_present_kms(struct surface *s, const struct fl_layer_props *props, struct kms_req_builder *builder);
static int gbm_surface_backing_store_present_fbdev(struct surface *s, const struct fl_layer_props *props, struct fbdev_commit_builder *builder);
static int gbm_surface_backing_store_fill(struct backing_store *store, FlutterBackingStore *fl_store);
static int gbm_surface_backing_store_queue_present(struct backing_store *store, const FlutterBackingStore *fl_store);

int gbm_surface_backing_store_init(
    struct gbm_surface_backing_store *store,
    struct tracer *tracer,
    struct point size,
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
    ok = backing_store_init(CAST_BACKING_STORE_UNCHECKED(store), tracer, size);
    if (ok != 0) {
        goto fail_destroy_egl_surface;
    }

    store->surface.present_kms = gbm_surface_backing_store_present_kms;
    store->surface.present_fbdev = gbm_surface_backing_store_present_fbdev;
    store->surface.deinit = gbm_surface_backing_store_deinit;
    store->backing_store.fill = gbm_surface_backing_store_fill;
    store->backing_store.queue_present = gbm_surface_backing_store_queue_present;
    uuid_copy(&store->uuid, uuid);
    store->pixel_format = pixel_format;
    store->gbm_device = gbm_device;
    store->gbm_surface = gbm_surface;
    store->egl_display = egl_display;
    store->egl_surface = egl_surface;
    store->egl_config = egl_config;
    store->renderer = gl_renderer_ref(renderer);
    for (int i = 0; i < ARRAY_SIZE(store->locked_fbs); i++) {
        store->locked_fbs->is_locked = (atomic_flag) ATOMIC_FLAG_INIT;
    }
    store->locked_front_fb = NULL;
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
 * @brief Create a new gbm_surface based backing store, with an explicit EGL Config for the created EGLSurface.
 * 
 * @param compositor The compositor that this surface will be registered to when calling surface_register.
 * @param size The size of the backing store.
 * @param device The GBM device used to allocate the surface.
 * @param renderer The EGL/OpenGL used to create any GL surfaces.
 * @param pixel_format The pixel format to be used by the framebuffers of the surface.
 * @param egl_config The EGLConfig used for creating the EGLSurface.
 * @return struct gbm_surface_backing_store* 
 */
ATTR_MALLOC struct gbm_surface_backing_store *gbm_surface_backing_store_new_with_egl_config(
    struct tracer *tracer,
    struct point size,
    struct gbm_device *device,
    struct gl_renderer *renderer,
    enum pixfmt pixel_format,
    EGLConfig egl_config
) {
    struct gbm_surface_backing_store *store;
    int ok;
    
    store = malloc(sizeof *store);
    if (store == NULL) {
        goto fail_return_null;
    }

    ok = gbm_surface_backing_store_init(store, tracer, size, device, renderer, pixel_format, egl_config);
    if (ok != 0) {
        goto fail_free_store;
    }

    return store;


    fail_free_store:
    free(store);

    fail_return_null:
    return NULL;
}

/**
 * @brief Create a new gbm_surface based backing store.
 * 
 * @param compositor The compositor that this surface will be registered to when calling surface_register.
 * @param size The size of the backing store.
 * @param device The GBM device used to allocate the surface.
 * @param renderer The EGL/OpenGL used to create any GL surfaces.
 * @param pixel_format The pixel format to be used by the framebuffers of the surface.
 * @return struct gbm_surface_backing_store*
 */
ATTR_MALLOC struct gbm_surface_backing_store *gbm_surface_backing_store_new(
    struct tracer *tracer,
    struct point size,
    struct gbm_device *device,
    struct gl_renderer *renderer,
    enum pixfmt pixel_format
) {
    return gbm_surface_backing_store_new_with_egl_config(
        tracer,
        size,
        device,
        renderer,
        pixel_format,
        EGL_NO_CONFIG_KHR
    );
}

void gbm_surface_backing_store_deinit(struct surface *s) {
    struct gbm_surface_backing_store *store;

    store = CAST_GBM_SURFACE_BACKING_STORE(s);

    gl_renderer_unref(store->renderer);
    backing_store_deinit(s);
}

struct gbm_bo_meta {
    struct drmdev *drmdev;
    uint32_t fb_id;

    bool has_opaque_fb;
    enum pixfmt opaque_pixel_format;
    uint32_t opaque_fb_id;
};

static void on_destroy_gbm_bo_meta(struct gbm_bo *bo, void *meta_void) {
    struct gbm_bo_meta *meta;
    int ok;
    
    DEBUG_ASSERT_NOT_NULL(bo);
    DEBUG_ASSERT_NOT_NULL(meta_void);
    meta = meta_void;

    ok = drmdev_rm_fb(meta->drmdev, meta->fb_id);
    if (ok != 0) {
        LOG_ERROR("Couldn't remove DRM framebuffer.\n");
    }

    if (meta->has_opaque_fb && meta->opaque_fb_id != meta->fb_id) {
        ok = drmdev_rm_fb(meta->drmdev, meta->opaque_fb_id);
        if (ok != 0) {
            LOG_ERROR("Couldn't remove DRM framebuffer.\n");
        }
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

static int gbm_surface_backing_store_present_kms(struct surface *s, const struct fl_layer_props *props, struct kms_req_builder *builder) {
    struct gbm_surface_backing_store *store;
    struct gbm_bo_meta *meta;
    struct drmdev *drmdev;
    struct gbm_bo *bo;
    enum pixfmt pixel_format, opaque_pixel_format;
    uint32_t fb_id, opaque_fb_id;
    int ok;

    /// TODO: Implement by adding the current front bo as a KMS fb (if that's not already done)
    store = CAST_THIS(s);

    /// TODO: Implement non axis-aligned fl_layer_props
    DEBUG_ASSERT_MSG(props->is_aa_rect, "only axis aligned view geometry is supported right now");

    surface_lock(s);

    DEBUG_ASSERT_NOT_NULL_MSG(store->locked_front_fb, "There's no framebuffer available for scanout right now. Make sure you called backing_store_swap_buffers() before presenting.");

    bo = store->locked_front_fb->bo;
    meta = gbm_bo_get_user_data(bo);
    if (meta == NULL) {
        bool has_opaque_fb;

        meta = malloc(sizeof *meta);
        if (meta == NULL) {
            ok = ENOMEM;
            goto fail_unlock;
        }

        drmdev = kms_req_builder_get_drmdev(builder);
        DEBUG_ASSERT_NOT_NULL(drmdev);

        TRACER_BEGIN(store->surface.tracer, "drmdev_add_fb (non-opaque)");
        fb_id = drmdev_add_fb(
            drmdev,
            gbm_bo_get_width(bo),
            gbm_bo_get_height(bo),
            store->pixel_format,
            gbm_bo_get_handle(bo).u32,
            gbm_bo_get_stride(bo),
            gbm_bo_get_offset(bo, 0),
            true, gbm_bo_get_modifier(bo),
            0
        );
        TRACER_END(store->surface.tracer, "drmdev_add_fb (non-opaque)");

        if (fb_id == 0) {
            ok = EIO;
            LOG_ERROR("Couldn't add GBM buffer as DRM framebuffer.\n");
            goto fail_free_meta;
        }

        if (get_pixfmt_info(store->pixel_format)->is_opaque == false) {
            has_opaque_fb = false;
            opaque_pixel_format = pixfmt_opaque(store->pixel_format);
            if (get_pixfmt_info(opaque_pixel_format)->is_opaque) {
                
                TRACER_BEGIN(store->surface.tracer, "drmdev_add_fb (opaque)");
                opaque_fb_id = drmdev_add_fb(
                    drmdev,
                    gbm_bo_get_width(bo),
                    gbm_bo_get_height(bo),
                    opaque_pixel_format,
                    gbm_bo_get_handle(bo).u32,
                    gbm_bo_get_stride(bo),
                    gbm_bo_get_offset(bo, 0),
                    true, gbm_bo_get_modifier(bo),
                    0
                );
                TRACER_END(store->surface.tracer, "drmdev_add_fb (opaque)");

                if (opaque_fb_id != 0) {
                    has_opaque_fb = true;
                }
            }
        } else {
            has_opaque_fb = true;
            opaque_fb_id = fb_id;
            opaque_pixel_format = store->pixel_format;
        }

        meta->drmdev = drmdev_ref(drmdev);
        meta->fb_id = fb_id;
        meta->has_opaque_fb = has_opaque_fb;
        meta->opaque_pixel_format = opaque_pixel_format;
        meta->opaque_fb_id = opaque_fb_id;
        gbm_bo_set_user_data(bo, meta, on_destroy_gbm_bo_meta);
    } else {
        // We can only add this GBM BO to a single KMS device as an fb right now.
        DEBUG_ASSERT_EQUALS_MSG(meta->drmdev, kms_req_builder_get_drmdev(builder), "Currently GBM BOs can only be scanned out on a single KMS device for their whole lifetime.");
    }

    /*
    LOG_DEBUG(
        "gbm_surface_backing_store_present_kms:\n"
        "    src_x, src_y, src_w, src_h: %f %f %f %f\n"
        "    dst_x, dst_y, dst_w, dst_h: %f %f %f %f\n",
        0.0, 0.0,
        store->backing_store.size.x,
        store->backing_store.size.y,
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
    if (kms_req_builder_prefer_next_layer_opaque(builder)) {
        if (meta->has_opaque_fb) {
            fb_id = meta->opaque_fb_id;
            pixel_format = meta->opaque_pixel_format;
        } else {
            LOG_DEBUG("Bottom-most framebuffer layer should be opaque, but an opaque framebuffer couldn't be created.\n");
            LOG_DEBUG("Using non-opaque framebuffer instead, which can result in visual glitches.\n");
            fb_id = meta->fb_id;
            pixel_format = store->pixel_format;
        }
    } else {
        fb_id = meta->fb_id;
        pixel_format = store->pixel_format;
    }

    TRACER_BEGIN(store->surface.tracer, "kms_req_builder_push_fb_layer");
    ok = kms_req_builder_push_fb_layer(
        builder,
        &(const struct kms_fb_layer) {
            .drm_fb_id = fb_id,
            .format = pixel_format,
            .has_modifier = false,
            .modifier = 0,
            
            .dst_x = (int32_t) props->aa_rect.offset.x,
            .dst_y = (int32_t) props->aa_rect.offset.y,
            .dst_w = (uint32_t) props->aa_rect.size.x,
            .dst_h = (uint32_t) props->aa_rect.size.y,
            
            .src_x = 0,
            .src_y = 0,
            .src_w = DOUBLE_TO_FP1616_ROUNDED(store->backing_store.size.x),
            .src_h = DOUBLE_TO_FP1616_ROUNDED(store->backing_store.size.y),
            
            .has_rotation = false,
            .rotation = PLANE_TRANSFORM_ROTATE_0,
            
            .has_in_fence_fd = false,
            .in_fence_fd = 0
        },
        on_release_layer,
        NULL,
        locked_fb_ref(store->locked_front_fb)
    );
    TRACER_END(store->surface.tracer, "kms_req_builder_push_fb_layer");
    if (ok != 0) {
        goto fail_unref_locked_fb;
    }

    surface_unlock(s);
    return ok;

    fail_unref_locked_fb:
    locked_fb_unref(store->locked_front_fb);
    goto fail_unlock;

    fail_free_meta:
    free(meta);

    fail_unlock:
    surface_unlock(s);
    return ok;
}

static int gbm_surface_backing_store_present_fbdev(struct surface *s, const struct fl_layer_props *props, struct fbdev_commit_builder *builder) {
    struct gbm_surface_backing_store *store;

    /// TODO: Implement by mmapping the current front bo, copy it into the fbdev
    /// TODO: Print a warning here if we're not using explicit linear tiling and use glReadPixels instead of gbm_bo_map in that case

    store = CAST_THIS(s);
    (void) store;
    (void) props;
    (void) builder;

    UNIMPLEMENTED();

    return 0;
}

static int gbm_surface_backing_store_fill(struct backing_store *s, FlutterBackingStore *fl_store) {
#if HAS_EGL
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
#else
    (void) s;
    (void) fl_store;
    LOG_ERROR("OpenGL backing stores are not supported if flutter-pi was built without EGL support.");
    return EINVAL;
#endif
}

static int gbm_surface_backing_store_queue_present(struct backing_store *s, const FlutterBackingStore *fl_store) {
    MAYBE_UNUSED struct gbm_surface_backing_store *store;
    struct gbm_bo *bo;
    MAYBE_UNUSED EGLBoolean egl_ok;
    int i, ok;
    
    store = CAST_THIS(s);
    (void) fl_store;

    surface_lock(CAST_SURFACE(s));

    /// TODO: Handle fl_store->did_update == false here

    // Unref the old front fb so potentially one of the locked_fbs entries gets freed 
    if (store->locked_front_fb != NULL) {
        locked_fb_unrefp(&store->locked_front_fb);
    }

    DEBUG_ASSERT(gbm_surface_has_free_buffers(store->gbm_surface));

    // create the in fence here
#ifdef HAS_EGL
    TRACER_BEGIN(s->surface.tracer, "eglSwapBuffers");
    egl_ok = eglSwapBuffers(store->egl_display, store->egl_surface);
    TRACER_END(s->surface.tracer, "eglSwapBuffers");

    if (egl_ok != EGL_TRUE) {
        LOG_EGL_ERROR(eglGetError(), "Couldn't flush rendering. eglSwapBuffers");
        return EIO;
    }
#endif

    TRACER_BEGIN(s->surface.tracer, "gbm_surface_lock_front_buffer");
    bo = gbm_surface_lock_front_buffer(store->gbm_surface);
    TRACER_END(s->surface.tracer, "gbm_surface_lock_front_buffer");

    if (bo == NULL) {
        ok = errno;
        LOG_ERROR("Couldn't lock GBM front buffer. gbm_surface_lock_front_buffer: %s\n", strerror(ok));
        goto fail_unlock;
    }

    // Try to find & lock a locked_fb we can use.
    // Note we use atomics here even though we hold the surfaces' mutex because
    // releasing a locked_fb is possibly done without the mutex.
    for (i = 0; i < ARRAY_SIZE(store->locked_fbs); i++) {
        if (atomic_flag_test_and_set(&store->locked_fbs[i].is_locked) == false) {
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
    //DEBUG_ASSERT_MSG(atomic_fetch_add(&store->n_locked_fbs, 1) <= 1, "sanity check failed: too many locked fbs for double-buffered vsync");
    store->locked_fbs[i].bo = bo;
    store->locked_fbs[i].store = CAST_GBM_SURFACE_BACKING_STORE(surface_ref(CAST_SURFACE(s)));
    store->locked_fbs[i].n_refs = REFCOUNT_INIT_1;
    store->locked_front_fb = store->locked_fbs + i;
    surface_unlock(CAST_SURFACE(s));
    return 0;


    fail_release_bo:
    gbm_surface_release_buffer(store->gbm_surface, bo);

    fail_unlock:
    surface_unlock(CAST_SURFACE(s));
    return ok;
}

/**
 * @brief Get the EGL Surface for rendering into this backing store.
 * 
 * Flutter doesn't really support backing stores to be EGL Surfaces, so we have to hack around this, kinda.
 * 
 * @param s 
 * @return EGLSurface The EGLSurface associated with this backing store. Only valid for the lifetime of this gbm_surface_backing_store.
 */
ATTR_PURE EGLSurface gbm_surface_backing_store_get_egl_surface(struct gbm_surface_backing_store *s) {
    return s->egl_surface;
}

/**
 * @brief Get the EGLConfig that was used to create the EGLSurface for this backing store.
 * 
 * If the display doesn't support EGL_KHR_no_config_context, we need to create the EGL rendering context with
 * the same EGLConfig as every EGLSurface we want to bind to it. So we can just let gbm_surface_backing_store choose a config
 * and let flutter-pi query that config when creating the rendering contexts in that case.
 * 
 * @param s 
 * @return EGLConfig The chosen EGLConfig. Valid forever.
 */
ATTR_PURE EGLConfig gbm_surface_backing_store_get_egl_config(struct gbm_surface_backing_store *s) {
    return s->egl_config;
}
