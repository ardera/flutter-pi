#include <stdio.h>
#include <errno.h>

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

struct view_cb_data {
    int64_t view_id;
    platform_view_mount_cb mount;
    platform_view_unmount_cb unmount;
    platform_view_update_view_cb update_view;
    platform_view_present_cb present;
    void *userdata;

    bool was_present_last_frame;
    FlutterSize last_size;
    FlutterPoint last_offset;
    int last_num_mutations;
    FlutterPlatformViewMutation last_mutations[16];
};

struct plane_reservation_data {
    const struct drm_plane *plane;
    bool is_reserved;
};

struct compositor compositor = {
    .drmdev = NULL,
    .cbs = CPSET_INITIALIZER(CPSET_DEFAULT_MAX_SIZE),
    .planes = CPSET_INITIALIZER(CPSET_DEFAULT_MAX_SIZE),
    .has_applied_modeset = false,
    .should_create_window_surface_backing_store = true,
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

/// GBM BO funcs for the main (window) surface
static void destroy_gbm_bo(
    struct gbm_bo *bo,
    void *userdata
) {
	struct drm_fb *fb = userdata;

	if (fb && fb->fb_id)
		drmModeRmFB(flutterpi.drm.drmdev->fd, fb->fb_id);
	
	free(fb);
}

/// Get a DRM FB id for this GBM BO, so we can display it.
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

/// Create a GL renderbuffer that is backed by a DRM buffer-object and registered as a DRM framebuffer
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

/// Set the color attachment of a GL FBO to this DRM RBO.
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

/// Destroy the OpenGL ES framebuffer-object that is associated with this
/// EGL Window Surface backed backing store
static void destroy_window_surface_backing_store_fb(void *userdata) {
    struct backing_store_metadata *meta = (struct backing_store_metadata*) userdata;

    printf("destroying window surface backing store FBO\n");
}

/// Destroy this GL renderbuffer, and the associated DRM buffer-object and DRM framebuffer
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

/// Destroy the OpenGL ES framebuffer-object that is associated with this
/// DRM FB backed backing store
static void destroy_drm_fb_backing_store_gl_fb(void *userdata) {
    struct backing_store_metadata *meta;
    EGLint egl_error;
    GLenum gl_error;
    int ok;

    meta = (struct backing_store_metadata*) userdata;

    //printf("destroy_drm_fb_backing_store_gl_fb(gl_fbo_id: %u)\n", meta->drm_fb.gl_fbo_id);

    eglGetError();
    glGetError();

    glDeleteFramebuffers(1, &meta->drm_fb.gl_fbo_id);
    if (gl_error = glGetError()) {
        fprintf(stderr, "[compositor] error destroying OpenGL FBO, glDeleteFramebuffers: %d\n", gl_error);
    }

    destroy_drm_rbo(meta->drm_fb.rbos + 0);
    destroy_drm_rbo(meta->drm_fb.rbos + 1);

    free(meta);
}

int compositor_on_page_flip(
	uint32_t sec,
	uint32_t usec
) {
    //x
    return 0;
}

/// CREATE FUNCS
/// Create a flutter backing store that is backed by the
/// EGL window surface (created using libgbm).
/// I.e. create a EGL window surface and set fbo_id to 0.
static int  create_window_surface_backing_store(
    const FlutterBackingStoreConfig *config,
    FlutterBackingStore *backing_store_out,
    struct compositor *compositor
) {
    struct backing_store_metadata *meta;
    int ok;

    // This should really be creating the GBM surface,
    // but we need the GBM surface to be bound as an EGL display earlier.

    meta = calloc(1, sizeof *meta);
    if (meta == NULL) {
        perror("[compositor] Could not allocate metadata for backing store. calloc");
        return ENOMEM;
    }

    ok = compositor_reserve_plane(&meta->window_surface.drm_plane_id);
    if (ok != 0) {
        free(meta);
        fprintf(stderr, "[compositor] Could not find an unused DRM plane for flutter backing store creation. compositor_reserve_plane: %s\n", strerror(ok));
        return ok;
    }

    meta->type = kWindowSurface;
    meta->window_surface.compositor = compositor;
    meta->window_surface.current_front_bo = NULL;
    meta->window_surface.gbm_surface = flutterpi.gbm.surface;

    backing_store_out->type = kFlutterBackingStoreTypeOpenGL;
    backing_store_out->open_gl.type = kFlutterOpenGLTargetTypeFramebuffer;
    backing_store_out->open_gl.framebuffer.target = GL_BGRA8_EXT;
    backing_store_out->open_gl.framebuffer.name = 0;
    backing_store_out->open_gl.framebuffer.destruction_callback = destroy_window_surface_backing_store_fb;
    backing_store_out->open_gl.framebuffer.user_data = meta;
    backing_store_out->user_data = meta;

    return 0;
}

static int  create_drm_fb_backing_store(
    const FlutterBackingStoreConfig *config,
    FlutterBackingStore *backing_store_out,
    struct compositor *compositor
) {
    struct backing_store_metadata *meta;
    struct drm_fb_backing_store *inner;
    uint32_t plane_id;
    EGLint egl_error;
    GLenum gl_error;
    int ok;

    meta = calloc(1, sizeof *meta);
    if (meta == NULL) {
        perror("[compositor] Could not allocate backing store metadata, calloc");
        return ENOMEM;
    }

    meta->type = kDrmFb;

    inner = &meta->drm_fb;
    inner->compositor = compositor;

    ok = compositor_reserve_plane(&inner->drm_plane_id);
    if (ok != 0) {
        free(meta);
        fprintf(stderr, "[compositor] Could not find an unused DRM plane for flutter backing store creation. compositor_reserve_plane: %s\n", strerror(ok));
        return ok;
    }

    eglGetError();
    glGetError();

    glGenFramebuffers(1, &inner->gl_fbo_id);
    if (gl_error = glGetError()) {
        fprintf(stderr, "[compositor] error generating FBOs for flutter backing store, glGenFramebuffers: %d\n", gl_error);
        compositor_free_plane(inner->drm_plane_id);
        free(meta);
        return EINVAL;
    }

    ok = create_drm_rbo(
        flutterpi.display.width,
        flutterpi.display.height,
        inner->rbos + 0
    );
    if (ok != 0) {
        glDeleteFramebuffers(1, &inner->gl_fbo_id);
        compositor_free_plane(inner->drm_plane_id);
        free(meta);
        return ok;
    }

    ok = create_drm_rbo(
        flutterpi.display.width,
        flutterpi.display.height,
        inner->rbos + 1
    );
    if (ok != 0) {
        destroy_drm_rbo(inner->rbos + 0);
        glDeleteFramebuffers(1, &inner->gl_fbo_id);
        compositor_free_plane(inner->drm_plane_id);
        free(meta);
        return ok;
    }

    ok = attach_drm_rbo_to_fbo(inner->gl_fbo_id, inner->rbos + inner->current_front_rbo);
    if (ok != 0) {
        destroy_drm_rbo(inner->rbos + 1);
        destroy_drm_rbo(inner->rbos + 0);
        glDeleteFramebuffers(1, &inner->gl_fbo_id);
        compositor_free_plane(inner->drm_plane_id);
        free(meta);
        return ok;
    }

    backing_store_out->type = kFlutterBackingStoreTypeOpenGL;
    backing_store_out->open_gl.type = kFlutterOpenGLTargetTypeFramebuffer;
    backing_store_out->open_gl.framebuffer.target = GL_BGRA8_EXT;
    backing_store_out->open_gl.framebuffer.name = inner->gl_fbo_id;
    backing_store_out->open_gl.framebuffer.destruction_callback = destroy_drm_fb_backing_store_gl_fb;
    backing_store_out->open_gl.framebuffer.user_data = meta;
    backing_store_out->user_data = meta;

    return 0;
}

static bool create_backing_store(
    const FlutterBackingStoreConfig *config,
    FlutterBackingStore *backing_store_out,
    void *user_data
) {
    int ok;

    printf("create_backing_store\n");

    if (compositor.should_create_window_surface_backing_store) {
        // We create 1 "backing store" that is rendering to the DRM_PLANE_PRIMARY
        // plane. That backing store isn't really a backing store at all, it's
        // FBO id is 0, so it's actually rendering to the window surface.
        ok = create_window_surface_backing_store(
            config,
            backing_store_out,
            user_data
        );

        if (ok != 0) {
            return false;
        }

        compositor.should_create_window_surface_backing_store = false;
    } else {
        // After the primary plane backing store was created,
        // we only create overlay plane backing stores. I.e.
        // backing stores, which have a FBO, that have a
        // color-attached RBO, that has a DRM EGLImage as the storage,
        // which in turn has a DRM FB associated with it.

        FlutterEngineTraceEventDurationBegin("create_drm_fb_backing_store");
        ok = create_drm_fb_backing_store(
            config,
            backing_store_out,
            user_data
        );
        FlutterEngineTraceEventDurationEnd("create_drm_fb_backing_store");

        if (ok != 0) {
            return false;
        }
    }

    return true;
}

/// COLLECT FUNCS
static int  collect_window_surface_backing_store(
    const FlutterBackingStore *store,
    struct backing_store_metadata *meta
) {
    struct window_surface_backing_store *inner = &meta->window_surface;

    printf("collect_window_surface_backing_store\n");

    compositor_free_plane(inner->drm_plane_id);

    compositor.should_create_window_surface_backing_store = true;

    return 0;
}

static int  collect_drm_fb_backing_store(
    const FlutterBackingStore *store,
    struct backing_store_metadata *meta
) {
    struct drm_fb_backing_store *inner = &meta->drm_fb;

    //printf("collect_drm_fb_backing_store(gl_fbo_id: %u)\n", inner->gl_fbo_id);
    
    // makes sense that the FlutterBackingStore collect callback is called before the
    // FlutterBackingStore OpenGL framebuffer destroy callback. Thanks flutter. (/s)
    // free(inner);

    compositor_free_plane(inner->drm_plane_id);

    return 0;
}

static bool collect_backing_store(
    const FlutterBackingStore *renderer,
    void *user_data
) {
    int ok;

    if (renderer->type == kFlutterBackingStoreTypeOpenGL &&
        renderer->open_gl.type == kFlutterOpenGLTargetTypeFramebuffer) {
        struct backing_store_metadata *meta = renderer->open_gl.framebuffer.user_data;

        if (meta->type == kWindowSurface) {
            ok = collect_window_surface_backing_store(renderer, meta);
            if (ok != 0) {
                return false;
            }

            compositor.should_create_window_surface_backing_store = true;
        } else if (meta->type == kDrmFb) {
            ok = collect_drm_fb_backing_store(renderer, meta);
            if (ok != 0) {
                return false;
            }
        } else {
            fprintf(stderr, "[compositor] Unsupported flutter backing store backend: %d\n", meta->type);
            return false;
        }
    } else {
        fprintf(stderr, "[compositor] Unsupported flutter backing store type\n");
        return false;
    }

    return true;
}

/// PRESENT FUNCS
static int present_window_surface_backing_store(
    struct window_surface_backing_store *backing_store,
    struct drmdev_atomic_req *atomic_req,
    int offset_x,
    int offset_y,
    int width,
    int height,
    int zpos
) {
    struct gbm_bo *next_front_bo;
    uint32_t next_front_fb_id;
    int ok;
	
    next_front_bo = gbm_surface_lock_front_buffer(backing_store->gbm_surface);
	next_front_fb_id = gbm_bo_get_drm_fb_id(next_front_bo);

    drmdev_atomic_req_put_plane_property(atomic_req, backing_store->drm_plane_id, "FB_ID", next_front_fb_id);
    drmdev_atomic_req_put_plane_property(atomic_req, backing_store->drm_plane_id, "CRTC_ID", backing_store->compositor->drmdev->selected_crtc->crtc->crtc_id);
    drmdev_atomic_req_put_plane_property(atomic_req, backing_store->drm_plane_id, "SRC_X", 0);
    drmdev_atomic_req_put_plane_property(atomic_req, backing_store->drm_plane_id, "SRC_Y", 0);
    drmdev_atomic_req_put_plane_property(atomic_req, backing_store->drm_plane_id, "SRC_W", ((uint16_t) flutterpi.display.width) << 16);
    drmdev_atomic_req_put_plane_property(atomic_req, backing_store->drm_plane_id, "SRC_H", ((uint16_t) flutterpi.display.height) << 16);
    drmdev_atomic_req_put_plane_property(atomic_req, backing_store->drm_plane_id, "CRTC_X", 0);
    drmdev_atomic_req_put_plane_property(atomic_req, backing_store->drm_plane_id, "CRTC_Y", 0);
    drmdev_atomic_req_put_plane_property(atomic_req, backing_store->drm_plane_id, "CRTC_W", flutterpi.display.width);
    drmdev_atomic_req_put_plane_property(atomic_req, backing_store->drm_plane_id, "CRTC_H", flutterpi.display.height);
    drmdev_atomic_req_put_plane_property(atomic_req, backing_store->drm_plane_id, "zpos", zpos);

    // TODO: move this to the page flip handler.
    // We can only be sure the buffer can be released when the buffer swap
    // ocurred.
    if (backing_store->current_front_bo != NULL) {
        gbm_surface_release_buffer(backing_store->gbm_surface, backing_store->current_front_bo);
    }
	backing_store->current_front_bo = (struct gbm_bo *) next_front_bo;

    return 0;
}

static int present_drm_fb_backing_store(
    struct drm_fb_backing_store *backing_store,
    struct drmdev_atomic_req *req,
    int offset_x,
    int offset_y,
    int width,
    int height,
    int zpos
) {
    int ok;

    backing_store->current_front_rbo ^= 1;
    ok = attach_drm_rbo_to_fbo(backing_store->gl_fbo_id, backing_store->rbos + backing_store->current_front_rbo);
    if (ok != 0) return ok;

    // present the back buffer
    /*
    ok = drmModeSetPlane(
        drm.fd,
        backing_store->drm_plane_id,
        drm.crtc_id,
        backing_store->rbos[backing_store->current_front_rbo ^ 1].drm_fb_id,
        0,
        offset_x, offset_y, width, height,
        0, 0, ((uint16_t) width) << 16, ((uint16_t) height) << 16
    );
    if (ok == -1) {
        perror("[compositor] Could not update overlay plane for presenting a DRM FB backed backing store. drmModeSetPlane");
        return errno;
    }
    */

    drmdev_atomic_req_put_plane_property(req, backing_store->drm_plane_id, "FB_ID", backing_store->rbos[backing_store->current_front_rbo ^ 1].drm_fb_id);
    drmdev_atomic_req_put_plane_property(req, backing_store->drm_plane_id, "CRTC_ID", backing_store->compositor->drmdev->selected_crtc->crtc->crtc_id);
    drmdev_atomic_req_put_plane_property(req, backing_store->drm_plane_id, "SRC_X", 0);
    drmdev_atomic_req_put_plane_property(req, backing_store->drm_plane_id, "SRC_Y", 0);
    drmdev_atomic_req_put_plane_property(req, backing_store->drm_plane_id, "SRC_W", ((uint16_t) flutterpi.display.width) << 16);
    drmdev_atomic_req_put_plane_property(req, backing_store->drm_plane_id, "SRC_H", ((uint16_t) flutterpi.display.height) << 16);
    drmdev_atomic_req_put_plane_property(req, backing_store->drm_plane_id, "CRTC_X", 0);
    drmdev_atomic_req_put_plane_property(req, backing_store->drm_plane_id, "CRTC_Y", 0);
    drmdev_atomic_req_put_plane_property(req, backing_store->drm_plane_id, "CRTC_W", flutterpi.display.width);
    drmdev_atomic_req_put_plane_property(req, backing_store->drm_plane_id, "CRTC_H", flutterpi.display.height);
    drmdev_atomic_req_put_plane_property(req, backing_store->drm_plane_id, "rotation", DRM_MODE_ROTATE_0 | DRM_MODE_REFLECT_Y);
    drmdev_atomic_req_put_plane_property(req, backing_store->drm_plane_id, "zpos", zpos);

    return 0;
}

static int present_platform_view(
    int64_t view_id,
    struct drmdev_atomic_req *req,
    const FlutterPlatformViewMutation **mutations,
    size_t num_mutations,
    int offset_x,
    int offset_y,
    int width,
    int height,
    int zpos
) {
    struct view_cb_data *cbs;

    cbs = get_cbs_for_view_id(view_id);
    if (cbs == NULL) {
        return EINVAL;
    }

    if (cbs->was_present_last_frame == false) {
        cbs->mount(
            view_id,
            req,
            mutations,
            num_mutations,
            offset_x,
            offset_y,
            width,
            height,
            zpos,
            cbs->userdata
        );
        cbs->was_present_last_frame = true;
    }

    if (cbs->present != NULL) {
        return cbs->present(
            view_id,
            req,
            mutations,
            num_mutations,
            offset_x,
            offset_y,
            width,
            height,
            zpos,
            cbs->userdata
        );
    } else {
        return 0;
    }
}

static bool present_layers_callback(
    const FlutterLayer **layers,
    size_t layers_count,
    void *user_data
) {
    struct plane_reservation_data *data;
    struct compositor *compositor;
    struct drmdev_atomic_req *req;
    struct view_cb_data *cb_data;
    uint32_t req_flags;
	int ok;

    compositor = user_data;

    printf("present_layers_callback\n");

    /*
    printf("[compositor] present_layers_callback(\n"
           "  layers_count: %lu,\n"
           "  layers = {\n",
           layers_count);
    for (int i = 0; i < layers_count; i++) {
        printf(
            "    [%d] = {\n"
            "      type: %s,\n"
            "      offset: {x: %f, y: %f},\n"
            "      size: %f x %f,\n",
            i,
            layers[i]->type == kFlutterLayerContentTypeBackingStore ? "backing store" : "platform view",
            layers[i]->offset.x,
            layers[i]->offset.y,
            layers[i]->size.width,
            layers[i]->size.height
        );

        if (layers[i]->type == kFlutterLayerContentTypeBackingStore) {
            struct backing_store_metadata *meta = layers[i]->backing_store->user_data;

            printf("      %s\n", meta->type == kWindowSurface ? "window surface" : "drm fb");
        } else if (layers[i]->type == kFlutterLayerContentTypePlatformView) {
            printf(
                "      platform_view: {\n"
                "        identifier: %lld\n"
                "        mutations: {\n",
                layers[i]->platform_view->identifier
            );
            for (int j = 0; j < layers[i]->platform_view->mutations_count; j++) {
                const FlutterPlatformViewMutation *mut = layers[i]->platform_view->mutations[j];

                printf(
                    "          [%d] = {\n"
                    "            type: %s,\n",
                    j,
                    mut->type == kFlutterPlatformViewMutationTypeOpacity ? "opacity" :
                    mut->type == kFlutterPlatformViewMutationTypeClipRect ? "clip rect" :
                    mut->type == kFlutterPlatformViewMutationTypeClipRoundedRect ? "clip rounded rect" :
                    mut->type == kFlutterPlatformViewMutationTypeTransformation ? "transformation" :
                    "(?)"
                );

                if (mut->type == kFlutterPlatformViewMutationTypeOpacity) {
                    printf(
                        "            opacity: %f\n",
                        mut->opacity
                    );
                } else if (mut->type == kFlutterPlatformViewMutationTypeClipRect) {
                    printf(
                        "            clip_rect: {bottom: %f, left: %f, right: %f, top: %f}\n",
                        mut->clip_rect.bottom,
                        mut->clip_rect.left,
                        mut->clip_rect.right,
                        mut->clip_rect.top
                    );
                } else if (mut->type == kFlutterPlatformViewMutationTypeClipRoundedRect) {
                    printf(
                        "            clip_rounded_rect: {\n"
                        "              lower_left_corner_radius: %f,\n"
                        "              lower_right_corner_radius: %f,\n"
                        "              upper_left_corner_radius: %f,\n"
                        "              upper_right_corner_radius: %f,\n"
                        "              rect: {bottom: %f, left: %f, right: %f, top: %f}\n"
                        "            }\n",
                        mut->clip_rounded_rect.lower_left_corner_radius,
                        mut->clip_rounded_rect.lower_right_corner_radius,
                        mut->clip_rounded_rect.upper_left_corner_radius,
                        mut->clip_rounded_rect.upper_right_corner_radius,
                        mut->clip_rounded_rect.rect.bottom,
                        mut->clip_rounded_rect.rect.left,
                        mut->clip_rounded_rect.rect.right,
                        mut->clip_rounded_rect.rect.top
                    );
                } else if (mut->type == kFlutterPlatformViewMutationTypeTransformation) {
                    printf(
                        "            transformation\n"
                    );
                }

                printf("          },\n");
            }
            printf("      }\n");
        }
        printf("    },\n");
    }
    printf("  }\n)\n");
    */

    FlutterEngineTraceEventDurationBegin("present");

    // flush GL
    FlutterEngineTraceEventDurationBegin("eglSwapBuffers");
    eglMakeCurrent(flutterpi.egl.display, flutterpi.egl.surface, flutterpi.egl.surface, flutterpi.egl.root_context);
    eglSwapBuffers(flutterpi.egl.display, flutterpi.egl.surface);
    FlutterEngineTraceEventDurationEnd("eglSwapBuffers");

    FlutterEngineTraceEventDurationBegin("drmdev_new_atomic_req");
    drmdev_new_atomic_req(compositor->drmdev, &req);
    FlutterEngineTraceEventDurationEnd("drmdev_new_atomic_req");

    // if we haven't yet set the display mode, set one
    req_flags = DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK;
    if (compositor->has_applied_modeset == false) {
        ok = drmdev_atomic_req_put_modeset_props(req, &req_flags);
        if (ok != 0) return false;
        
        compositor->has_applied_modeset = true;
    }

    // unmount non-present platform views
    FlutterEngineTraceEventDurationBegin("unmount non-present platform views");
    for_each_pointer_in_cpset(&compositor->cbs, cb_data) {
        bool is_present = false;
        for (int i = 0; i < layers_count; i++) {
            if (layers[i]->type == kFlutterLayerContentTypePlatformView &&
                layers[i]->platform_view->identifier == cb_data->view_id) {
                is_present = true;
                break;
            }
        }

        if (!is_present && cb_data->was_present_last_frame) {
            if (cb_data->unmount != NULL) {
                ok = cb_data->unmount(
                    cb_data->view_id,
                    req,
                    cb_data->userdata
                );
                if (ok != 0) {
                    fprintf(stderr, "[compositor] Could not unmount platform view. %s\n", strerror(ok));
                }
            }
        }
    }
    FlutterEngineTraceEventDurationEnd("unmount non-present platform views");

    // present all layers, invoke the mount/update_view/present callbacks of platform views
    for (int i = 0; i < layers_count; i++) {
        const FlutterLayer *layer = layers[i];

        if (layer->type == kFlutterLayerContentTypeBackingStore) {
            const FlutterBackingStore *backing_store = layer->backing_store;
            struct backing_store_metadata *meta = backing_store->user_data;

            if (meta->type == kWindowSurface) {
                FlutterEngineTraceEventDurationBegin("present_window_surface_backing_store");
                ok = present_window_surface_backing_store(
                    &meta->window_surface,
                    req,
                    (int) layer->offset.x,
                    (int) layer->offset.y,
                    (int) layer->size.width,
                    (int) layer->size.height,
                    0
                );
                FlutterEngineTraceEventDurationEnd("present_window_surface_backing_store");
            } else if (meta->type == kDrmFb) {
                FlutterEngineTraceEventDurationBegin("present_drm_fb_backing_store");
                ok = present_drm_fb_backing_store(
                    &meta->drm_fb,
                    req,
                    (int) layer->offset.x,
                    (int) layer->offset.y,
                    (int) layer->size.width,
                    (int) layer->size.height,
                    1
                );
                FlutterEngineTraceEventDurationEnd("present_drm_fb_backing_store");
            }
            
        } else if (layer->type == kFlutterLayerContentTypePlatformView) {
            cb_data = get_cbs_for_view_id(layer->platform_view->identifier);
            
            if (cb_data != NULL) {
                if (cb_data->was_present_last_frame == false) {
                    if (cb_data->mount != NULL) {
                        FlutterEngineTraceEventDurationBegin("mount platform view");
                        ok = cb_data->mount(
                            layer->platform_view->identifier,
                            req,
                            layer->platform_view->mutations,
                            layer->platform_view->mutations_count,
                            (int) round(layer->offset.x),
                            (int) round(layer->offset.y),
                            (int) round(layer->size.width),
                            (int) round(layer->size.height),
                            0,
                            cb_data->userdata
                        );
                        FlutterEngineTraceEventDurationEnd("mount platform view");
                        if (ok != 0) {
                            fprintf(stderr, "[compositor] Could not mount platform view. %s\n", strerror(ok));
                        }
                    }
                } else {
                    bool did_update_view = false;
                    
                    did_update_view = did_update_view || memcmp(&cb_data->last_size, &layer->size, sizeof(FlutterSize));
                    did_update_view = did_update_view || memcmp(&cb_data->last_offset, &layer->offset, sizeof(FlutterPoint));
                    did_update_view = did_update_view || (cb_data->last_num_mutations != layer->platform_view->mutations_count);
                    for (int i = 0; (i < layer->platform_view->mutations_count) && !did_update_view; i++) {
                        did_update_view = did_update_view || memcmp(cb_data->last_mutations + i, layer->platform_view->mutations[i], sizeof(FlutterPlatformViewMutation));
                    }

                    if (did_update_view) {
                        if (cb_data->update_view != NULL) {
                            FlutterEngineTraceEventDurationBegin("update platform view");
                            ok = cb_data->update_view(
                                cb_data->view_id,
                                req,
                                layer->platform_view->mutations,
                                layer->platform_view->mutations_count,
                                (int) round(layer->offset.x),
                                (int) round(layer->offset.y),
                                (int) round(layer->size.width),
                                (int) round(layer->size.height),
                                0,
                                cb_data->userdata
                            );
                            FlutterEngineTraceEventDurationEnd("update platform view");
                            if (ok != 0) {
                                fprintf(stderr, "[compositor] Could not update platform views' view. %s\n", strerror(ok));
                            }
                        }
                    }
                }

                if (cb_data->present) {
                    FlutterEngineTraceEventDurationBegin("present platform view");
                    ok = cb_data->present(
                        layer->platform_view->identifier,
                        req,
                        layer->platform_view->mutations,
                        layer->platform_view->mutations_count,
                        (int) round(layer->offset.x),
                        (int) round(layer->offset.y),
                        (int) round(layer->size.width),
                        (int) round(layer->size.height),
                        0,
                        cb_data->userdata
                    );
                    FlutterEngineTraceEventDurationEnd("present platform view");
                    if (ok != 0) {
                        fprintf(stderr, "[compositor] Could not present platform view. %s\n", strerror(ok));
                    }
                }

                cb_data->was_present_last_frame = true;
                cb_data->last_size = layer->size;
                cb_data->last_offset = layer->offset;
                cb_data->last_num_mutations = layer->platform_view->mutations_count;
                for (int i = 0; i < layer->platform_view->mutations_count; i++) {
                    memcpy(cb_data->last_mutations + i, layer->platform_view->mutations[i], sizeof(FlutterPlatformViewMutation));
                }
            }
        } else {
            fprintf(stderr, "[compositor] Unsupported flutter layer type: %d\n", layer->type);
        }
    }
    
    eglMakeCurrent(flutterpi.egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    // all unused planes will be set inactive
    for_each_pointer_in_cpset(&compositor->planes, data) {
        if (data->is_reserved == false) {
            drmdev_atomic_req_put_plane_property(req, data->plane->plane->plane_id, "FB_ID", 0);
        }
    }

    FlutterEngineTraceEventDurationBegin("drmdev_atomic_req_commit");
    drmdev_atomic_req_commit(req, req_flags, NULL);
    FlutterEngineTraceEventDurationEnd("drmdev_atomic_req_commit");

    drmdev_destroy_atomic_req(req);

    FlutterEngineTraceEventDurationEnd("present");

    return true;
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
int compositor_reserve_plane(uint32_t *plane_id_out) {
    struct plane_reservation_data *data;

    cpset_lock(&compositor.planes);

    for_each_pointer_in_cpset(&compositor.planes, data) {
        if (data->is_reserved == false) {
            data->is_reserved = true;
            cpset_unlock(&compositor.planes);

            *plane_id_out = data->plane->plane->plane_id;
            return 0;
        }
    }

    cpset_unlock(&compositor.planes);
    
    *plane_id_out = 0;
    return EBUSY;
}

int compositor_free_plane(uint32_t plane_id) {
    struct plane_reservation_data *data;

    cpset_lock(&compositor.planes);

    for_each_pointer_in_cpset(&compositor.planes, data) {
        if (data->plane->plane->plane_id == plane_id) {
            data->is_reserved = false;
            cpset_unlock(&compositor.planes);
            return 0;
        }
    }

    cpset_unlock(&compositor.planes);
    return EINVAL;
}

/// COMPOSITOR INITIALIZATION
int compositor_initialize(struct drmdev *drmdev) {
    struct plane_reservation_data *data;
    const struct drm_plane *plane;

    cpset_lock(&compositor.planes);

    for_each_plane_in_drmdev(drmdev, plane) {
        
        data = calloc(1, sizeof (struct plane_reservation_data));
        if (data == NULL) {
            for_each_pointer_in_cpset(&compositor.planes, data)
                free(data);
            cpset_unlock(&compositor.planes);
            return ENOMEM;
        }

        data->plane = plane;
        data->is_reserved = false;

        cpset_put_locked(&compositor.planes, data);
    }

    cpset_unlock(&compositor.planes);

    compositor.drmdev = drmdev;

    return 0;
}


const FlutterCompositor flutter_compositor = {
    .struct_size = sizeof(FlutterCompositor),
    .create_backing_store_callback = create_backing_store,
    .collect_backing_store_callback = collect_backing_store,
    .present_layers_callback = present_layers_callback,
    .user_data = &compositor
};