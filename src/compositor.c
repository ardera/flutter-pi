#include <stdio.h>
#include <errno.h>
#include <sys/mman.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <gbm.h>
#include <EGL/egl.h>
#define  EGL_EGLEXT_PROTOTYPES
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#define  GL_GLEXT_PROTOTYPES
#include <GLES2/gl2ext.h>
#include <flutter_embedder.h>

#include <flutter-pi.h>
#include <collection.h>
#include <compositor.h>
#include <cursor.h>

struct view_cb_data {
    int64_t view_id;
    platform_view_mount_cb mount;
    platform_view_unmount_cb unmount;
    platform_view_update_view_cb update_view;
    platform_view_present_cb present;
    void *userdata;

    bool was_present_last_frame;
    int last_zpos;
    FlutterSize last_size;
    FlutterPoint last_offset;
    int last_num_mutations;
    FlutterPlatformViewMutation last_mutations[16];
};

/*
struct plane_data {
    int type;
    const struct drm_plane *plane;
    bool is_reserved;
    int zpos;
};
*/

struct compositor compositor = {
    .drmdev = NULL,
    .cbs = CPSET_INITIALIZER(CPSET_DEFAULT_MAX_SIZE),
    .has_applied_modeset = false,
    .should_create_window_surface_backing_store = true,
    .stale_rendertargets = CPSET_INITIALIZER(CPSET_DEFAULT_MAX_SIZE),
    .cursor = {
        .is_enabled = false,
        .width = 0,
        .height = 0,
        .bpp = 0,
        .depth = 0,
        .pitch = 0,
        .size = 0,
        .drm_fb_id = 0,
        .gem_bo_handle = 0,
        .buffer = NULL,
        .x = 0,
        .y = 0
    }
};

static struct view_cb_data *get_cbs_for_view_id_locked(int64_t view_id) {
    struct view_cb_data *data;
    
    for_each_pointer_in_cpset(&compositor.cbs, data) {
        if (data->view_id == view_id) {
            return data;
        }
    }

    return NULL;
}

static struct view_cb_data *get_cbs_for_view_id(int64_t view_id) {
    struct view_cb_data *data;
    
    cpset_lock(&compositor.cbs);
    data = get_cbs_for_view_id_locked(view_id);
    cpset_unlock(&compositor.cbs);
    
    return data;
}

/**
 * @brief Destroy all the rendertargets in the stale rendertarget cache.
 */
static int destroy_stale_rendertargets(void) {
    struct rendertarget *target;

    cpset_lock(&compositor.stale_rendertargets);

    for_each_pointer_in_cpset(&compositor.stale_rendertargets, target) {
        target->destroy(target);
        target = NULL;
    }

    cpset_unlock(&compositor.stale_rendertargets);
}

static void destroy_gbm_bo(
    struct gbm_bo *bo,
    void *userdata
) {
	struct drm_fb *fb = userdata;

	if (fb && fb->fb_id)
		drmModeRmFB(flutterpi.drm.drmdev->fd, fb->fb_id);
	
	free(fb);
}

/**
 * @brief Get a DRM FB id for this GBM BO, so we can display it.
 */
static uint32_t gbm_bo_get_drm_fb_id(struct gbm_bo *bo) {
	uint32_t width, height, format, strides[4] = {0}, handles[4] = {0}, offsets[4] = {0}, flags = 0;
	int ok = -1;

	// if the buffer object already has some userdata associated with it,
	//   it's the framebuffer we allocated.
	struct drm_fb *fb = gbm_bo_get_user_data(bo);
	if (fb) return fb->fb_id;

	// if there's no framebuffer for the bo, we need to create one.
	fb = calloc(1, sizeof(struct drm_fb));
	fb->bo = bo;

	width = gbm_bo_get_width(bo);
	height = gbm_bo_get_height(bo);
	format = gbm_bo_get_format(bo);

	uint64_t modifiers[4] = {0};
	modifiers[0] = gbm_bo_get_modifier(bo);
	const int num_planes = gbm_bo_get_plane_count(bo);

	for (int i = 0; i < num_planes; i++) {
		strides[i] = gbm_bo_get_stride_for_plane(bo, i);
		handles[i] = gbm_bo_get_handle(bo).u32;
		offsets[i] = gbm_bo_get_offset(bo, i);
		modifiers[i] = modifiers[0];
	}

	if (modifiers[0]) {
		flags = DRM_MODE_FB_MODIFIERS;
	}

	ok = drmModeAddFB2WithModifiers(flutterpi.drm.drmdev->fd, width, height, format, handles, strides, offsets, modifiers, &fb->fb_id, flags);

	if (ok) {
		if (flags)
			fprintf(stderr, "drm_fb_get_from_bo: modifiers failed!\n");
		
		memcpy(handles, (uint32_t [4]){gbm_bo_get_handle(bo).u32,0,0,0}, 16);
		memcpy(strides, (uint32_t [4]){gbm_bo_get_stride(bo),0,0,0}, 16);
		memset(offsets, 0, 16);

		ok = drmModeAddFB2(flutterpi.drm.drmdev->fd, width, height, format, handles, strides, offsets, &fb->fb_id, 0);
	}

	if (ok) {
		fprintf(stderr, "drm_fb_get_from_bo: failed to create fb: %s\n", strerror(errno));
		free(fb);
		return 0;
	}

	gbm_bo_set_user_data(bo, fb, destroy_gbm_bo);

	return fb->fb_id;
}


/**
 * @brief Create a GL renderbuffer that is backed by a DRM buffer-object and registered as a DRM framebuffer
 */
static int create_drm_rbo(
    size_t width,
    size_t height,
    struct drm_rbo *out
) {
    struct drm_rbo fbo;
    EGLint egl_error;
    GLenum gl_error;
    int ok;

    eglGetError();
    glGetError();

    fbo.egl_image = flutterpi.egl.createDRMImageMESA(flutterpi.egl.display, (const EGLint[]) {
        EGL_WIDTH, width,
        EGL_HEIGHT, height,
        EGL_DRM_BUFFER_FORMAT_MESA, EGL_DRM_BUFFER_FORMAT_ARGB32_MESA,
        EGL_DRM_BUFFER_USE_MESA, EGL_DRM_BUFFER_USE_SCANOUT_MESA,
        EGL_NONE
    });
    if ((egl_error = eglGetError()) != EGL_SUCCESS) {
        fprintf(stderr, "[compositor] error creating DRM EGL Image for flutter backing store, eglCreateDRMImageMESA: %ld\n", egl_error);
        return EINVAL;
    }

    flutterpi.egl.exportDRMImageMESA(flutterpi.egl.display, fbo.egl_image, NULL, &fbo.gem_handle, &fbo.gem_stride);
    if ((egl_error = eglGetError()) != EGL_SUCCESS) {
        fprintf(stderr, "[compositor] error getting handle & stride for DRM EGL Image, eglExportDRMImageMESA: %d\n", egl_error);
        return EINVAL;
    }

    glGenRenderbuffers(1, &fbo.gl_rbo_id);
    if (gl_error = glGetError()) {
        fprintf(stderr, "[compositor] error generating renderbuffers for flutter backing store, glGenRenderbuffers: %ld\n", gl_error);
        return EINVAL;
    }

    glBindRenderbuffer(GL_RENDERBUFFER, fbo.gl_rbo_id);
    if (gl_error = glGetError()) {
        fprintf(stderr, "[compositor] error binding renderbuffer, glBindRenderbuffer: %d\n", gl_error);
        return EINVAL;
    }

    flutterpi.gl.EGLImageTargetRenderbufferStorageOES(GL_RENDERBUFFER, fbo.egl_image);
    if (gl_error = glGetError()) {
        fprintf(stderr, "[compositor] error binding DRM EGL Image to renderbuffer, glEGLImageTargetRenderbufferStorageOES: %ld\n", gl_error);
        return EINVAL;
    }

    /*
    glGenFramebuffers(1, &fbo.gl_fbo_id);
    if (gl_error = glGetError()) {
        fprintf(stderr, "[compositor] error generating FBOs for flutter backing store, glGenFramebuffers: %d\n", gl_error);
        return EINVAL;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, fbo.gl_fbo_id);
    if (gl_error = glGetError()) {
        fprintf(stderr, "[compositor] error binding FBO for attaching the renderbuffer, glBindFramebuffer: %d\n", gl_error);
        return EINVAL;
    }

    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, fbo.gl_rbo_id);
    if (gl_error = glGetError()) {
        fprintf(stderr, "[compositor] error attaching renderbuffer to FBO, glFramebufferRenderbuffer: %d\n", gl_error);
        return EINVAL;
    }

    GLenum fb_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);

    */

    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    
    // glBindFramebuffer(GL_FRAMEBUFFER, 0);

    ok = drmModeAddFB2(
        flutterpi.drm.drmdev->fd,
        width,
        height,
        DRM_FORMAT_ARGB8888,
        (const uint32_t*) &(uint32_t[4]) {
            fbo.gem_handle,
            0,
            0,
            0
        },
        (const uint32_t*) &(uint32_t[4]) {
            fbo.gem_stride, 0, 0, 0
        },
        (const uint32_t*) &(uint32_t[4]) {
            0, 0, 0, 0
        },
        &fbo.drm_fb_id,
        0
    );
    if (ok == -1) {
        perror("[compositor] Could not make DRM fb from EGL Image, drmModeAddFB2");
        return errno;
    }

    *out = fbo;

    return 0;
}

/**
 * @brief Set the color attachment of a GL FBO to this DRM RBO.
 */
static int attach_drm_rbo_to_fbo(
    GLuint fbo_id,
    struct drm_rbo *rbo
) {
    EGLint egl_error;
    GLenum gl_error;

    eglGetError();
    glGetError();

    glBindFramebuffer(GL_FRAMEBUFFER, fbo_id);
    if (gl_error = glGetError()) {
        fprintf(stderr, "[compositor] error binding FBO for attaching the renderbuffer, glBindFramebuffer: %d\n", gl_error);
        return EINVAL;
    }

    glBindRenderbuffer(GL_RENDERBUFFER, rbo->gl_rbo_id);
    if (gl_error = glGetError()) {
        fprintf(stderr, "[compositor] error binding renderbuffer, glBindRenderbuffer: %d\n", gl_error);
        return EINVAL;
    }

    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rbo->gl_rbo_id);
    if (gl_error = glGetError()) {
        fprintf(stderr, "[compositor] error attaching renderbuffer to FBO, glFramebufferRenderbuffer: %d\n", gl_error);
        return EINVAL;
    }

    return 0;
}

/**
 * @brief Destroy this GL renderbuffer, and the associated DRM buffer-object and DRM framebuffer
 */
static void destroy_drm_rbo(
    struct drm_rbo *rbo
) {
    EGLint egl_error;
    GLenum gl_error;
    int ok;

    eglGetError();
    glGetError();

    glDeleteRenderbuffers(1, &rbo->gl_rbo_id);
    if (gl_error = glGetError()) {
        fprintf(stderr, "[compositor] error destroying OpenGL RBO, glDeleteRenderbuffers: 0x%08X\n", gl_error);
    }

    ok = drmModeRmFB(flutterpi.drm.drmdev->fd, rbo->drm_fb_id);
    if (ok < 0) {
        fprintf(stderr, "[compositor] error removing DRM FB, drmModeRmFB: %s\n", strerror(errno));
    }

    eglDestroyImage(flutterpi.egl.display, rbo->egl_image);
    if (egl_error = eglGetError(), egl_error != EGL_SUCCESS) {
        fprintf(stderr, "[compositor] error destroying EGL image, eglDestroyImage: 0x%08X\n", egl_error);
    }
}

static void rendertarget_gbm_destroy(struct rendertarget *target) {
    free(target);
}

static int rendertarget_gbm_present(
    struct rendertarget *target,
    struct drmdev_atomic_req *atomic_req,
    uint32_t drm_plane_id,
    int offset_x,
    int offset_y,
    int width,
    int height,
    int zpos
) {
    struct rendertarget_gbm *gbm_target;
    struct gbm_bo *next_front_bo;
    uint32_t next_front_fb_id;
    int ok;

    gbm_target = &target->gbm;

    next_front_bo = gbm_surface_lock_front_buffer(gbm_target->gbm_surface);
	next_front_fb_id = gbm_bo_get_drm_fb_id(next_front_bo);

    drmdev_atomic_req_put_plane_property(atomic_req, drm_plane_id, "FB_ID", next_front_fb_id);
    drmdev_atomic_req_put_plane_property(atomic_req, drm_plane_id, "CRTC_ID", target->compositor->drmdev->selected_crtc->crtc->crtc_id);
    drmdev_atomic_req_put_plane_property(atomic_req, drm_plane_id, "SRC_X", 0);
    drmdev_atomic_req_put_plane_property(atomic_req, drm_plane_id, "SRC_Y", 0);
    drmdev_atomic_req_put_plane_property(atomic_req, drm_plane_id, "SRC_W", ((uint16_t) flutterpi.display.width) << 16);
    drmdev_atomic_req_put_plane_property(atomic_req, drm_plane_id, "SRC_H", ((uint16_t) flutterpi.display.height) << 16);
    drmdev_atomic_req_put_plane_property(atomic_req, drm_plane_id, "CRTC_X", 0);
    drmdev_atomic_req_put_plane_property(atomic_req, drm_plane_id, "CRTC_Y", 0);
    drmdev_atomic_req_put_plane_property(atomic_req, drm_plane_id, "CRTC_W", flutterpi.display.width);
    drmdev_atomic_req_put_plane_property(atomic_req, drm_plane_id, "CRTC_H", flutterpi.display.height);
    drmdev_atomic_req_put_plane_property(atomic_req, drm_plane_id, "rotation", DRM_MODE_ROTATE_0);
    drmdev_atomic_req_put_plane_property(atomic_req, drm_plane_id, "zpos", zpos);

    // TODO: move this to the page flip handler.
    // We can only be sure the buffer can be released when the buffer swap
    // ocurred.
    if (gbm_target->current_front_bo != NULL) {
        gbm_surface_release_buffer(gbm_target->gbm_surface, gbm_target->current_front_bo);
    }
	gbm_target->current_front_bo = (struct gbm_bo *) next_front_bo;

    return 0;
}

/**
 * @brief Create a type of rendertarget that is backed by a GBM Surface, used for rendering into the DRM primary plane.
 * 
 * @param[out] out A pointer to the pointer of the created rendertarget.
 * @param[in] compositor The compositor which this rendertarget should be associated with.
 * 
 * @see rendertarget_gbm
 */
static int rendertarget_gbm_new(
    struct rendertarget **out,
    struct compositor *compositor
) {
    struct rendertarget *target;
    int ok;

    target = calloc(1, sizeof *target);
    if (target == NULL) {
        *out = NULL;
        return ENOMEM;
    }

    *target = (struct rendertarget) {
        .is_gbm = true,
        .compositor = compositor,
        .gbm = {
            .gbm_surface = flutterpi.gbm.surface,
            .current_front_bo = NULL
        },
        .gl_fbo_id = 0,
        .destroy = rendertarget_gbm_destroy,
        .present = rendertarget_gbm_present
    };

    *out = target;

    return 0;
}

static void rendertarget_nogbm_destroy(struct rendertarget *target) {
    glDeleteFramebuffers(1, &target->nogbm.gl_fbo_id);
    destroy_drm_rbo(target->nogbm.rbos + 1);
    destroy_drm_rbo(target->nogbm.rbos + 0);
    free(target);
}

static int rendertarget_nogbm_present(
    struct rendertarget *target,
    struct drmdev_atomic_req *req,
    uint32_t drm_plane_id,
    int offset_x,
    int offset_y,
    int width,
    int height,
    int zpos
) {
    struct rendertarget_nogbm *nogbm_target;
    int ok;

    nogbm_target = &target->nogbm;

    nogbm_target->current_front_rbo ^= 1;
    ok = attach_drm_rbo_to_fbo(nogbm_target->gl_fbo_id, nogbm_target->rbos + nogbm_target->current_front_rbo);
    if (ok != 0) return ok;

    drmdev_atomic_req_put_plane_property(req, drm_plane_id, "FB_ID", nogbm_target->rbos[nogbm_target->current_front_rbo ^ 1].drm_fb_id);
    drmdev_atomic_req_put_plane_property(req, drm_plane_id, "CRTC_ID", target->compositor->drmdev->selected_crtc->crtc->crtc_id);
    drmdev_atomic_req_put_plane_property(req, drm_plane_id, "SRC_X", 0);
    drmdev_atomic_req_put_plane_property(req, drm_plane_id, "SRC_Y", 0);
    drmdev_atomic_req_put_plane_property(req, drm_plane_id, "SRC_W", ((uint16_t) flutterpi.display.width) << 16);
    drmdev_atomic_req_put_plane_property(req, drm_plane_id, "SRC_H", ((uint16_t) flutterpi.display.height) << 16);
    drmdev_atomic_req_put_plane_property(req, drm_plane_id, "CRTC_X", 0);
    drmdev_atomic_req_put_plane_property(req, drm_plane_id, "CRTC_Y", 0);
    drmdev_atomic_req_put_plane_property(req, drm_plane_id, "CRTC_W", flutterpi.display.width);
    drmdev_atomic_req_put_plane_property(req, drm_plane_id, "CRTC_H", flutterpi.display.height);
    drmdev_atomic_req_put_plane_property(req, drm_plane_id, "rotation", DRM_MODE_ROTATE_0 | DRM_MODE_REFLECT_Y);
    drmdev_atomic_req_put_plane_property(req, drm_plane_id, "zpos", zpos);

    return 0;
}

/**
 * @brief Create a type of rendertarget that is not backed by a GBM-Surface, used for rendering into DRM overlay planes.
 * 
 * @param[out] out A pointer to the pointer of the created rendertarget.
 * @param[in] compositor The compositor which this rendertarget should be associated with.
 * 
 * @see rendertarget_nogbm
 */
static int rendertarget_nogbm_new(
    struct rendertarget **out,
    struct compositor *compositor
) {
    struct rendertarget *target;
    EGLint egl_error;
    GLenum gl_error;
    int ok;

    target = calloc(1, sizeof *target);
    if (target == NULL) {
        return ENOMEM;
    }

    target->is_gbm = false;
    target->compositor = compositor;
    target->destroy = rendertarget_nogbm_destroy;
    target->present = rendertarget_nogbm_present;

    eglGetError();
    glGetError();

    glGenFramebuffers(1, &target->nogbm.gl_fbo_id);
    if (gl_error = glGetError()) {
        fprintf(stderr, "[compositor] error generating FBOs for flutter backing store, glGenFramebuffers: %d\n", gl_error);
        ok = EINVAL;
        goto fail_free_target;
    }

    ok = create_drm_rbo(
        flutterpi.display.width,
        flutterpi.display.height,
        target->nogbm.rbos + 0
    );
    if (ok != 0) {
        goto fail_delete_fb;
    }

    ok = create_drm_rbo(
        flutterpi.display.width,
        flutterpi.display.height,
        target->nogbm.rbos + 1
    );
    if (ok != 0) {
        goto fail_destroy_drm_rbo_0;
    }

    ok = attach_drm_rbo_to_fbo(target->nogbm.gl_fbo_id, target->nogbm.rbos + target->nogbm.current_front_rbo);
    if (ok != 0) {
        goto fail_destroy_drm_rbo_1;
    }

    target->gl_fbo_id = target->nogbm.gl_fbo_id;

    *out = target;
    return 0;


    fail_destroy_drm_rbo_1:
    destroy_drm_rbo(target->nogbm.rbos + 1);

    fail_destroy_drm_rbo_0:
    destroy_drm_rbo(target->nogbm.rbos + 0);

    fail_delete_fb:
    glDeleteFramebuffers(1, &target->nogbm.gl_fbo_id);

    fail_free_target:
    free(target);
    *out = NULL;
    return ok;
}

/**
 * @brief Called by flutter when the OpenGL FBO of a backing store should be destroyed.
 * Called on an internal engine-managed thread. This is actually called after the engine
 * calls @ref on_collect_backing_store.
 * 
 * @param[in] userdata The pointer to the struct flutterpi_backing_store that should be destroyed.
 */
static void on_destroy_backing_store_gl_fb(void *userdata) {
    struct flutterpi_backing_store *store;
    struct compositor *compositor;
    
    store = userdata;
    compositor = store->target->compositor;

    printf("on_destroy_backing_store_gl_fb(is_gbm: %s, backing_store: %p)\n", store->target->is_gbm ? "true" : "false", store);

    cpset_put_(&compositor->stale_rendertargets, store->target);

    if (store->should_free_on_next_destroy) {
        free(store);
    } else {
        store->should_free_on_next_destroy = true;
    }
}

/**
 * @brief A callback invoked by the engine to release the backing store. The embedder may
 * collect any resources associated with the backing store. Invoked on an internal
 * engine-managed thread. This is actually called before the engine calls @ref on_destroy_backing_store_gl_fb.
 * 
 * @param[in] backing_store The backing store to be collected.
 * @param[in] userdata A pointer to the flutterpi compositor.
 */
static bool on_collect_backing_store(
    const FlutterBackingStore *backing_store,
    void *userdata
) {
    struct flutterpi_backing_store *store;
    struct compositor *compositor;
    
    store = backing_store->user_data;
    compositor = store->target->compositor;

    printf("on_collect_backing_store(is_gbm: %s, backing_store: %p)\n", store->target->is_gbm ? "true" : "false", store);

    cpset_put_(&compositor->stale_rendertargets, store->target);

    if (store->should_free_on_next_destroy) {
        free(store);
    } else {
        store->should_free_on_next_destroy = true;
    }

    return true;
}

/**
 * @brief A callback invoked by the engine to obtain a FlutterBackingStore for a specific FlutterLayer.
 * Called on an internal engine-managed thread.
 * 
 * @param[in] config The dimensions of the backing store to be created, post transform.
 * @param[out] backing_store_out The created backing store.
 * @param[in] userdata A pointer to the flutterpi compositor.
 */
static bool on_create_backing_store(
    const FlutterBackingStoreConfig *config,
    FlutterBackingStore *backing_store_out,
    void *userdata
) {
    struct flutterpi_backing_store *store;
    struct rendertarget *target;
    struct compositor *compositor;
    int ok;

    compositor = userdata;

    store = calloc(1, sizeof *store);
    if (store == NULL) {
        return false;
    }

    printf("on_create_backing_store(%f x %f): %p\n", config->size.width, config->size.height, store);

    // first, try to find a stale GBM rendertarget.
    cpset_lock(&compositor->stale_rendertargets);
    for_each_pointer_in_cpset(&compositor->stale_rendertargets, target) break;
    if (target != NULL) {
        printf("found stale rendertarget.\n");
        cpset_remove_locked(&compositor->stale_rendertargets, target);
    }
    cpset_unlock(&compositor->stale_rendertargets);

    // if we didn't find one, check if we should create one. If not,
    // try to find a stale No-GBM rendertarget. If there is none,
    // create one.
    if (target == NULL) {
        if (compositor->should_create_window_surface_backing_store) {
            // We create 1 "backing store" that is rendering to the DRM_PLANE_PRIMARY
            // plane. That backing store isn't really a backing store at all, it's
            // FBO id is 0, so it's actually rendering to the window surface.

            printf("Creating a GBM rendertarget.\n");

            FlutterEngineTraceEventDurationBegin("rendertarget_gbm_new");
            ok = rendertarget_gbm_new(
                &target,
                compositor
            );
            FlutterEngineTraceEventDurationEnd("rendertarget_gbm_new");

            if (ok != 0) {
                free(store);
                return false;
            }

            compositor->should_create_window_surface_backing_store = false;
        } else {
            printf("Creating a No-GBM rendertarget.\n");

            FlutterEngineTraceEventDurationBegin("rendertarget_nogbm_new");
            ok = rendertarget_nogbm_new(
                &target,
                compositor
            );
            FlutterEngineTraceEventDurationEnd("rendertarget_nogbm_new");

            if (ok != 0) {
                free(store);
                return false;
            }
        }
    }

    store->target = target;
    store->flutter_backing_store = (FlutterBackingStore) {
        .struct_size = backing_store_out->struct_size,
        .type = kFlutterBackingStoreTypeOpenGL,
        .open_gl = {
            .type = kFlutterOpenGLTargetTypeFramebuffer,
            .framebuffer = {
                .target = GL_BGRA8_EXT,
                .name = target->gl_fbo_id,
                .destruction_callback = on_destroy_backing_store_gl_fb,
                .user_data = store
            }
        },
        .user_data = store
    };

    memcpy(backing_store_out, &store->flutter_backing_store, sizeof(FlutterBackingStore));

    return true;
}

/// PRESENT FUNCS
static bool on_present_layers(
    const FlutterLayer **layers,
    size_t layers_count,
    void *userdata
) {
    struct drmdev_atomic_req *req;
    struct view_cb_data *cb_data;
    struct compositor *compositor;
    struct drm_plane *plane;
    uint32_t req_flags;
    int ok;

    compositor = userdata;

    drmdev_new_atomic_req(compositor->drmdev, &req);

    cpset_lock(&compositor->cbs);

    eglMakeCurrent(flutterpi.egl.display, flutterpi.egl.surface, flutterpi.egl.surface, flutterpi.egl.root_context);
    eglSwapBuffers(flutterpi.egl.display, flutterpi.egl.surface);

    req_flags = DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK;
    if (compositor->has_applied_modeset == false) {
        ok = drmdev_atomic_req_put_modeset_props(req, &req_flags);
        if (ok != 0) return false;
        
        compositor->has_applied_modeset = true;
    }
    
    // first, the state machine phase.
    // go through the layers, update
    // all platform views accordingly.
    // unmount, update, mount. in that order
    {
        void *mounted_views_storage[layers_count];
        memset(mounted_views_storage, 0, layers_count * sizeof(void*));
        struct pointer_set mounted_views = PSET_INITIALIZER_STATIC(mounted_views_storage, layers_count);

        void *unmounted_views_storage[layers_count];
        memset(unmounted_views_storage, 0, layers_count * sizeof(void*));
        struct pointer_set unmounted_views = PSET_INITIALIZER_STATIC(unmounted_views_storage, layers_count);

        void *updated_views_storage[layers_count];
        memset(updated_views_storage, 0, layers_count * sizeof(void*));
        struct pointer_set updated_views = PSET_INITIALIZER_STATIC(updated_views_storage, layers_count);
    
        for_each_pointer_in_cpset(&compositor->cbs, cb_data) {
            const FlutterLayer *layer;
            bool is_present = false;
            int zpos;

            for (int i = 0; i < layers_count; i++) {
                if (layers[i]->type == kFlutterLayerContentTypePlatformView &&
                    layers[i]->platform_view->identifier == cb_data->view_id) {
                    is_present = true;
                    layer = layers[i];
                    zpos = i;
                    break;
                }
            }

            if (!is_present && cb_data->was_present_last_frame) {
                pset_put(&unmounted_views, cb_data);
            } else if (is_present && cb_data->was_present_last_frame) {
                if (cb_data->update_view != NULL) {
                    bool did_update_view = false;

                    did_update_view = did_update_view || (zpos != cb_data->last_zpos);
                    did_update_view = did_update_view || memcmp(&cb_data->last_size, &layer->size, sizeof(FlutterSize));
                    did_update_view = did_update_view || memcmp(&cb_data->last_offset, &layer->offset, sizeof(FlutterPoint));
                    did_update_view = did_update_view || (cb_data->last_num_mutations != layer->platform_view->mutations_count);
                    for (int i = 0; (i < layer->platform_view->mutations_count) && !did_update_view; i++) {
                        did_update_view = did_update_view || memcmp(cb_data->last_mutations + i, layer->platform_view->mutations[i], sizeof(FlutterPlatformViewMutation));
                    }

                    if (did_update_view) {
                        pset_put(&updated_views, cb_data);
                    }
                }
            } else if (is_present && !cb_data->was_present_last_frame) {
                pset_put(&mounted_views, cb_data);
            }
        }

        for_each_pointer_in_pset(&unmounted_views, cb_data) {
            if (cb_data->unmount != NULL) {
                ok = cb_data->unmount(
                    cb_data->view_id,
                    req,
                    cb_data->userdata
                );
                if (ok != 0) {
                    fprintf(stderr, "[compositor] Could not unmount platform view. unmount: %s\n", strerror(ok));
                }
            }
        }

        for_each_pointer_in_pset(&updated_views, cb_data) {
            const FlutterLayer *layer;
            int zpos;

            for (int i = 0; i < layers_count; i++) {
                if (layers[i]->type == kFlutterLayerContentTypePlatformView &&
                    layers[i]->platform_view->identifier == cb_data->view_id) {
                    layer = layers[i];
                    zpos = i;
                    break;
                }
            }

            ok = cb_data->update_view(
                cb_data->view_id,
                req,
                layer->platform_view->mutations,
                layer->platform_view->mutations_count,
                (int) round(layer->offset.x),
                (int) round(layer->offset.y),
                (int) round(layer->size.width),
                (int) round(layer->size.height),
                zpos,
                cb_data->userdata
            );
            if (ok != 0) {
                fprintf(stderr, "[compositor] Could not update platform view. update_view: %s\n", strerror(ok));
            }

            cb_data->last_zpos = zpos;
            cb_data->last_size = layer->size;
            cb_data->last_offset = layer->offset;
            cb_data->last_num_mutations = layer->platform_view->mutations_count;
            for (int i = 0; i < layer->platform_view->mutations_count; i++) {
                memcpy(cb_data->last_mutations + i, layer->platform_view->mutations[i], sizeof(FlutterPlatformViewMutation));
            }
        }

        for_each_pointer_in_pset(&mounted_views, cb_data) {
            const FlutterLayer *layer;
            int zpos;

            for (int i = 0; i < layers_count; i++) {
                if (layers[i]->type == kFlutterLayerContentTypePlatformView &&
                    layers[i]->platform_view->identifier == cb_data->view_id) {
                    layer = layers[i];
                    zpos = i;
                    break;
                }
            }

            if (cb_data->mount != NULL) {
                ok = cb_data->mount(
                    layer->platform_view->identifier,
                    req,
                    layer->platform_view->mutations,
                    layer->platform_view->mutations_count,
                    (int) round(layer->offset.x),
                    (int) round(layer->offset.y),
                    (int) round(layer->size.width),
                    (int) round(layer->size.height),
                    zpos,
                    cb_data->userdata
                );
                if (ok != 0) {
                    fprintf(stderr, "[compositor] Could not mount platform view. %s\n", strerror(ok));
                }
            }

            cb_data->last_zpos = zpos;
            cb_data->last_size = layer->size;
            cb_data->last_offset = layer->offset;
            cb_data->last_num_mutations = layer->platform_view->mutations_count;
            for (int i = 0; i < layer->platform_view->mutations_count; i++) {
                memcpy(cb_data->last_mutations + i, layer->platform_view->mutations[i], sizeof(FlutterPlatformViewMutation));
            }
        }
    }

    for (int i = 0; i < layers_count; i++) {
        if (layers[i]->type == kFlutterLayerContentTypeBackingStore) {
            for_each_unreserved_plane_in_atomic_req(req, plane) {
                // choose a plane which has an "intrinsic" zpos that matches
                // the zpos we want the plane to have.
                // (Since planes are buggy and we can't rely on the zpos we explicitly
                // configure the plane to have to be actually applied to the hardware.
                // In short, assigning a different value to the zpos property won't always
                // take effect.)
                if ((i == 0) && (plane->type == DRM_PLANE_TYPE_PRIMARY)) {
                    drmdev_atomic_req_reserve_plane(req, plane);
                    break;
                } else if ((i != 0) && (plane->type == DRM_PLANE_TYPE_OVERLAY)) {
                    drmdev_atomic_req_reserve_plane(req, plane);
                    break;
                }
            }
            if (plane == NULL) {
                fprintf(stderr, "[compositor] Could not find a free primary/overlay DRM plane for presenting the backing store. drmdev_atomic_req_reserve_plane: %s\n", strerror(ok));
                continue;
            }

            struct flutterpi_backing_store *store = layers[i]->backing_store->user_data;
            struct rendertarget *target = store->target;

            ok = target->present(
                target,
                req,
                plane->plane->plane_id,
                0,
                0,
                compositor->drmdev->selected_mode->hdisplay,
                compositor->drmdev->selected_mode->vdisplay,
                plane->type == DRM_PLANE_TYPE_PRIMARY ? 0 : 1
            );
            if (ok != 0) {
                fprintf(stderr, "[compositor] Could not present backing store. rendertarget->present: %s\n", strerror(ok));
            }
        } else if (layers[i]->type == kFlutterLayerContentTypePlatformView) {
            cb_data = get_cbs_for_view_id_locked(layers[i]->platform_view->identifier);

            if ((cb_data != NULL) && (cb_data->present != NULL)) {
                ok = cb_data->present(
                    cb_data->view_id,
                    req,
                    layers[i]->platform_view->mutations,
                    layers[i]->platform_view->mutations_count,
                    (int) round(layers[i]->offset.x),
                    (int) round(layers[i]->offset.y),
                    (int) round(layers[i]->size.width),
                    (int) round(layers[i]->size.height),
                    i,
                    cb_data->userdata
                );
                if (ok != 0) {
                    fprintf(stderr, "[compositor] Could not present platform view. platform_view->present: %s\n", strerror(ok));
                }
            }
        }
    }

    for_each_unreserved_plane_in_atomic_req(req, plane) {
        drmdev_atomic_req_put_plane_property(req, plane->plane->plane_id, "FB", 0);
    }

    eglMakeCurrent(flutterpi.egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    FlutterEngineTraceEventDurationBegin("drmdev_atomic_req_commit");
    drmdev_atomic_req_commit(req, req_flags, NULL);
    FlutterEngineTraceEventDurationEnd("drmdev_atomic_req_commit");

    cpset_unlock(&compositor->cbs);
}

int compositor_on_page_flip(
	uint32_t sec,
	uint32_t usec
) {
    return 0;
}

/// PLATFORM VIEW CALLBACKS
int compositor_set_view_callbacks(
    int64_t view_id,
    platform_view_mount_cb mount,
    platform_view_unmount_cb unmount,
    platform_view_update_view_cb update_view,
    platform_view_present_cb present,
    void *userdata
) {
    struct view_cb_data *entry;

    cpset_lock(&compositor.cbs);

    entry = get_cbs_for_view_id_locked(view_id);

    if (entry == NULL) {
        entry = calloc(1, sizeof(*entry));
        if (!entry) {
            cpset_unlock(&compositor.cbs);
            return ENOMEM;
        }

        cpset_put_locked(&compositor.cbs, entry);
    }

    entry->view_id = view_id;
    entry->mount = mount;
    entry->unmount = unmount;
    entry->update_view = update_view;
    entry->present = present;
    entry->userdata = userdata;

    return cpset_unlock(&compositor.cbs);
}

int compositor_remove_view_callbacks(int64_t view_id) {
    struct view_cb_data *entry;

    cpset_lock(&compositor.cbs);

    entry = get_cbs_for_view_id_locked(view_id);
    if (entry == NULL) {
        return EINVAL;
    }

    cpset_remove_locked(&compositor.cbs, entry);

    return cpset_unlock(&compositor.cbs);
}

/// DRM HARDWARE PLANE RESERVATION

/// COMPOSITOR INITIALIZATION
int compositor_initialize(struct drmdev *drmdev) {
    //struct plane_data *data;
    //const struct drm_plane *plane;

    /*
    cpset_lock(&compositor.planes);

    for_each_plane_in_drmdev(drmdev, plane) {
        data = calloc(1, sizeof (struct plane_data));
        if (data == NULL) {
            for_each_pointer_in_cpset(&compositor.planes, data)
                free(data);
            cpset_unlock(&compositor.planes);
            return ENOMEM;
        }

        for (int i = 0; i < plane->props->count_props; i++) {
            if (strcmp(plane->props_info[i]->name, "type") == 0) {
                uint32_t prop_id = plane->props_info[i]->prop_id;

                for (int i = 0; i < plane->props->count_props; i++) {
                    if (plane->props->props[i] == prop_id) {
                        data->type = plane->props->prop_values[i];
                    }
                }
            }
        }

        data->plane = plane;
        data->is_reserved = false;

        cpset_put(&compositor.planes, data);
    }

    cpset_unlock(&compositor.planes);
    */

    compositor.drmdev = drmdev;

    return 0;
}

int compositor_apply_cursor_skin_for_rotation(int rotation) {
    const struct cursor_icon *cursor;
    int ok;
    
    if (compositor.cursor.is_enabled) {
        cursor = NULL;
        for (int i = 0; i < n_cursors; i++) {
            if (cursors[i].rotation == rotation) {
                cursor = cursors + i;
                break;
            }
        }

        memcpy(compositor.cursor.buffer, cursor->data, compositor.cursor.size);

        ok = drmModeSetCursor2(
            compositor.drmdev->fd,
            compositor.drmdev->selected_crtc->crtc->crtc_id,
            compositor.cursor.gem_bo_handle,
            compositor.cursor.width,
            compositor.cursor.height,
            cursor->hot_x,
            cursor->hot_y
        );
        if (ok < 0) {
            perror("[compositor] Could not set the mouse cursor buffer. drmModeSetCursor");
            return errno;
        }

        return 0;
    }

    return 0;
}

int compositor_set_cursor_enabled(bool enabled) {
    if ((compositor.cursor.is_enabled == false) && (enabled == true)) {
        struct drm_mode_create_dumb create_req;
        struct drm_mode_map_dumb map_req;
        uint32_t drm_fb_id;
        uint32_t *buffer;
        uint64_t cap;
        uint8_t depth;
        int ok;

        ok = drmGetCap(compositor.drmdev->fd, DRM_CAP_DUMB_BUFFER, &cap);
        if (ok < 0) {
            perror("[compositor] Could not query GPU Driver support for dumb buffers. drmGetCap");
            return errno;
        }

        if (cap == 0) {
            fprintf(stderr, "[compositor] Kernel / GPU Driver does not support dumb DRM buffers. Mouse cursor will not be displayed.\n");
            return ENOTSUP;
        }

        ok = drmGetCap(compositor.drmdev->fd, DRM_CAP_DUMB_PREFERRED_DEPTH, &cap);
        if (ok < 0) {
            perror("[compositor] Could not query dumb buffer preferred depth capability. drmGetCap");
        }

        depth = (uint8_t) cap;

        memset(&create_req, 0, sizeof create_req);
        create_req.width = 64;
        create_req.height = 64;
        create_req.bpp = 32;
        create_req.flags = 0;

        ok = ioctl(compositor.drmdev->fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_req);
        if (ok < 0) {
            perror("[compositor] Could not create a dumb buffer for the hardware cursor. ioctl");
            return errno;
        }

        ok = drmModeAddFB(compositor.drmdev->fd, create_req.width, create_req.height, depth, create_req.bpp, create_req.pitch, create_req.handle, &drm_fb_id);
        if (ok < 0) {
            perror("[compositor] Could not make a DRM FB out of the hardware cursor buffer. drmModeAddFB");

            return errno;
        }

        memset(&map_req, 0, sizeof map_req);
        map_req.handle = create_req.handle;

        ok = ioctl(compositor.drmdev->fd, DRM_IOCTL_MODE_MAP_DUMB, &map_req);
        if (ok < 0) {
            perror("[compositor] Could not prepare dumb buffer mmap for uploading the hardware cursor icon. ioctl");
            return errno;
        }

        buffer = mmap(0, create_req.size, PROT_READ | PROT_WRITE, MAP_SHARED, compositor.drmdev->fd, map_req.offset);
        if (buffer == MAP_FAILED) {
            perror("[compositor] Could not mmap dumb buffer for uploading the hardware cursor icon. mmap");
            return errno;
        }

        compositor.cursor.is_enabled = true;
        compositor.cursor.width = create_req.width;
        compositor.cursor.height = create_req.height;
        compositor.cursor.bpp = create_req.bpp;
        compositor.cursor.depth = depth;
        compositor.cursor.gem_bo_handle = create_req.handle;
        compositor.cursor.pitch = create_req.pitch;
        compositor.cursor.size = create_req.size;
        compositor.cursor.drm_fb_id = drm_fb_id;
        compositor.cursor.x = 0;
        compositor.cursor.y = 0;
        compositor.cursor.buffer = buffer;
    } else if ((compositor.cursor.is_enabled == true) && (enabled == false)) {
        struct drm_mode_destroy_dumb destroy_req;

        drmModeSetCursor(
            compositor.drmdev->fd,
            compositor.drmdev->selected_crtc->crtc->crtc_id,
            0, 0, 0
        );

        munmap(compositor.cursor.buffer, compositor.cursor.size);

        drmModeRmFB(compositor.drmdev->fd, compositor.cursor.drm_fb_id);

        memset(&destroy_req, 0, sizeof destroy_req);
        destroy_req.handle = compositor.cursor.gem_bo_handle;

        ioctl(compositor.drmdev->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_req);

        compositor.cursor.is_enabled = false;
        compositor.cursor.width = 0;
        compositor.cursor.height = 0;
        compositor.cursor.bpp = 0;
        compositor.cursor.depth = 0;
        compositor.cursor.gem_bo_handle = 0;
        compositor.cursor.pitch = 0;
        compositor.cursor.size = 0;
        compositor.cursor.drm_fb_id = 0;
        compositor.cursor.x = 0;
        compositor.cursor.y = 0;
        compositor.cursor.buffer = NULL;
    }
}

int compositor_set_cursor_pos(int x, int y) {
    int ok;

    ok = drmModeMoveCursor(compositor.drmdev->fd, compositor.drmdev->selected_crtc->crtc->crtc_id, x, y);
    if (ok < 0) {
        perror("[compositor] Could not move cursor. drmModeMoveCursor");
        return errno;
    }

    compositor.cursor.x = x;
    compositor.cursor.y = y;    

    return 0;
}

const FlutterCompositor flutter_compositor = {
    .struct_size = sizeof(FlutterCompositor),
    .create_backing_store_callback = on_create_backing_store,
    .collect_backing_store_callback = on_collect_backing_store,
    .present_layers_callback = on_present_layers,
    .user_data = &compositor
};