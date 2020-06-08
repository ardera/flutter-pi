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
    platform_view_present_cb present_cb;
    void *userdata;
};

struct flutterpi_compositor {
    bool should_create_window_surface_backing_store;
    struct concurrent_pointer_set cbs;
} flutterpi_compositor = {
    .should_create_window_surface_backing_store = true,
    .cbs = CPSET_INITIALIZER(CPSET_DEFAULT_MAX_SIZE)
};

static bool should_create_window_surface_backing_store = true;

static void destroy_gbm_bo(struct gbm_bo *bo, void *userdata) {
	struct drm_fb *fb = userdata;

	if (fb && fb->fb_id)
		drmModeRmFB(drm.fd, fb->fb_id);
	
	free(fb);
}

uint32_t gbm_bo_get_drm_fb_id(struct gbm_bo *bo) {
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

	ok = drmModeAddFB2WithModifiers(drm.fd, width, height, format, handles, strides, offsets, modifiers, &fb->fb_id, flags);

	if (ok) {
		if (flags)
			fprintf(stderr, "drm_fb_get_from_bo: modifiers failed!\n");
		
		memcpy(handles, (uint32_t [4]){gbm_bo_get_handle(bo).u32,0,0,0}, 16);
		memcpy(strides, (uint32_t [4]){gbm_bo_get_stride(bo),0,0,0}, 16);
		memset(offsets, 0, 16);

		ok = drmModeAddFB2(drm.fd, width, height, format, handles, strides, offsets, &fb->fb_id, 0);
	}

	if (ok) {
		fprintf(stderr, "drm_fb_get_from_bo: failed to create fb: %s\n", strerror(errno));
		free(fb);
		return 0;
	}

	gbm_bo_set_user_data(bo, fb, destroy_gbm_bo);

	return fb->fb_id;
}

/// Find the next DRM overlay plane that has no FB associated with it
static uint32_t find_next_unused_drm_plane(void) {
    uint32_t result;
    bool has_result;

    has_result = false;

    drmModePlaneRes *plane_res = drmModeGetPlaneResources(drm.fd);
    for (int i = 0; (i < plane_res->count_planes) && !has_result; i++) {
        drmModePlane *plane = drmModeGetPlane(drm.fd, plane_res->planes[i]);
        drmModeObjectProperties *props = drmModeObjectGetProperties(drm.fd, plane->plane_id, DRM_MODE_OBJECT_ANY);

        for (int j = 0; (j < props->count_props) && !has_result; j++) {
            drmModePropertyRes *prop = drmModeGetProperty(drm.fd, props->props[j]);

            if ((strcmp(prop->name, "type") == 0)
                && (props->prop_values[j] == DRM_PLANE_TYPE_OVERLAY)) {
                
                result = plane->plane_id;
                has_result = true;
            }

            drmModeFreeProperty(prop);
        }

        drmModeFreeObjectProperties(props);
        drmModeFreePlane(plane);
    }
    drmModeFreePlaneResources(plane_res);

    return has_result? result : 0;
}

static int set_property_value(
    const uint32_t object_id,
    const uint32_t object_type,
    char name[32],
    uint64_t value
) {
    bool has_result = false;
    int ok;

    drmModeObjectProperties *props = drmModeObjectGetProperties(drm.fd, object_id, object_type);
    if (props == NULL) {
        perror("[compositor] Could not get object properties. drmModeObjectGetProperties");
        return errno;
    }

    for (int i = 0; (i < props->count_props) && (!has_result); i++) {
        drmModePropertyRes *prop = drmModeGetProperty(drm.fd, props->props[i]);

        if (strcmp(prop->name, name) == 0) {
            ok = drmModeObjectSetProperty(drm.fd, object_id, object_type, prop->prop_id, value);
            if (ok == -1) {
                perror("Could not set object property. drmModeObjectSetProperty");
                return errno;
            }

            has_result = true;
        }

        drmModeFreeProperty(prop);
    }

    drmModeFreeObjectProperties(props);

    if (!has_result) {
        fprintf(stderr, "[compositor] Could not find any property with name %32s\n", name);
    }

    return has_result? 0 : ENOENT;
}

static int set_plane_property_value(
    const uint32_t plane_id,
    char name[32],
    uint64_t value
) {
    return set_property_value(
        plane_id,
        DRM_MODE_OBJECT_PLANE,
        name,
        value
    );
}

static int get_property_value(
    const uint32_t object_id,
    const uint32_t object_type,
    char name[32],
    uint64_t *value_out
) {
    bool has_result = false;

    drmModeObjectProperties *props = drmModeObjectGetProperties(drm.fd, object_id, object_type);
    if (props == NULL) {
        perror("[compositor] Could not get object properties. drmModeObjectGetProperties");
        return errno;
    }

    for (int i = 0; (i < props->count_props) && (!has_result); i++) {
        drmModePropertyRes *prop = drmModeGetProperty(drm.fd, props->props[i]);

        if (strcmp(prop->name, name) == 0) {
            *value_out = props->prop_values[i];
            has_result = true;
        }

        drmModeFreeProperty(prop);
    }

    drmModeFreeObjectProperties(props);

    if (!has_result) {
        fprintf(stderr, "[compositor] Could not find any property with name %32s\n", name);
    }

    return has_result? 0 : ENOENT;
}

static int get_plane_property_value(
    const uint32_t plane_id,
    char name[32],
    uint64_t *value_out
) {
    return get_property_value(
        plane_id,
        DRM_MODE_OBJECT_ANY,
        name,
        value_out
    );
}

/// Destroy the OpenGL ES framebuffer-object that is associated with this
/// EGL Window Surface backed backing store
static void destroy_window_surface_backing_store_fb(void *userdata) {
    struct backing_store_metadata *meta = (struct backing_store_metadata*) userdata;


}

/// Destroy the OpenGL ES framebuffer-object that is associated with this
/// DRM FB backed backing store
static void destroy_drm_fb_backing_store_gl_fb(void *userdata) {
    struct backing_store_metadata *meta = (struct backing_store_metadata*) userdata;
    

}


/// Create a flutter backing store that is backed by the
/// EGL window surface (created using libgbm).
/// I.e. create a EGL window surface and set fbo_id to 0.
static int  create_window_surface_backing_store(
    const FlutterBackingStoreConfig *config,
    FlutterBackingStore *backing_store_out,
    void *user_data
) {
    struct backing_store_metadata *meta;

    meta = malloc(sizeof(*meta));
    if (meta == NULL) {
        perror("[compositor] Could not allocate metadata for backing store. malloc");
        return ENOMEM;
    }
    memset(meta, 0, sizeof(*meta));

    meta->type = kWindowSurface;
    meta->window_surface.current_front_bo = drm.current_bo;
    meta->window_surface.gbm_surface = gbm.surface;

    backing_store_out->type = kFlutterBackingStoreTypeOpenGL;
    backing_store_out->open_gl.type = kFlutterOpenGLTargetTypeFramebuffer;
    backing_store_out->open_gl.framebuffer.target = GL_BGRA8_EXT;
    backing_store_out->open_gl.framebuffer.name = 0;
    backing_store_out->open_gl.framebuffer.destruction_callback = destroy_window_surface_backing_store_fb;
    backing_store_out->open_gl.framebuffer.user_data = meta;
    backing_store_out->user_data = meta;

    return 0;
}

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

    fbo.egl_image = egl.createDRMImageMESA(egl.display, (const EGLint[]) {
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

    egl.exportDRMImageMESA(egl.display, fbo.egl_image, NULL, &fbo.gem_handle, &fbo.gem_stride);
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

    gl.EGLImageTargetRenderbufferStorageOES(GL_RENDERBUFFER, fbo.egl_image);
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
        drm.fd,
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

/// Create a flutter backing store that 
static int  create_drm_fb_backing_store(
    const FlutterBackingStoreConfig *config,
    FlutterBackingStore *backing_store_out,
    void *user_data
) {
    struct backing_store_metadata *meta;
    struct drm_fb_backing_store *inner;
    uint32_t plane_id;
    EGLint egl_error;
    GLenum gl_error;
    int ok;

    plane_id = find_next_unused_drm_plane();
    if (!plane_id) {
        fprintf(stderr, "[compositor] Could not find an unused DRM overlay plane for flutter backing store creation.\n");
        return false;
    }

    meta = malloc(sizeof(struct backing_store_metadata));
    if (meta == NULL) {
        perror("[compositor] Could not allocate backing store metadata, malloc");
        return false;
    }
    
    memset(meta, 0, sizeof(*meta));

    meta->type = kDrmFb;
    meta->drm_fb.drm_plane_id = plane_id;
    inner = &meta->drm_fb;

    eglGetError();
    glGetError();

    glGenFramebuffers(1, &inner->gl_fbo_id);
    if (gl_error = glGetError()) {
        fprintf(stderr, "[compositor] error generating FBOs for flutter backing store, glGenFramebuffers: %d\n", gl_error);
        return EINVAL;
    }

    ok = create_drm_rbo(
        config->size.width,
        config->size.height,
        inner->rbos + 0
    );
    if (ok != 0) return ok;

    ok = create_drm_rbo(
        config->size.width,
        config->size.height,
        inner->rbos + 1
    );
    if (ok != 0) return ok;

    ok = attach_drm_rbo_to_fbo(inner->gl_fbo_id, inner->rbos + inner->current_front_rbo);
    if (ok != 0) return ok;

    ok = get_plane_property_value(inner->drm_plane_id, "zpos", &inner->current_zpos);
    if (ok != 0) {
        return false;
    }

    // Reflect Y because GL draws to its buffers that upside-down.
    ok = set_plane_property_value(inner->drm_plane_id, "rotation", DRM_MODE_ROTATE_0 | DRM_MODE_REFLECT_Y);
    if (ok == -1) {
        perror("[compositor] Could not set rotation & reflection of hardware plane. drmModeObjectSetProperty");
        return false;
    }

    // We don't scan out anything yet. Just attach the FB to this plane to reserve it.
    // Compositing details (offset, size, zpos) are set in the present
    // procedure.
    ok = drmModeSetPlane(
        drm.fd,
        inner->drm_plane_id,
        drm.crtc_id,
        inner->rbos[1 ^ inner->current_front_rbo].drm_fb_id,
        0,
        0, 0, 0, 0,
        0, 0, ((uint32_t) config->size.width) << 16, ((uint32_t) config->size.height) << 16
    );
    if (ok == -1) {
        perror("[compositor] Could not attach DRM framebuffer to hardware plane. drmModeSetPlane");
        return false;
    }

    get_plane_property_value(inner->drm_plane_id, "zpos", (uint64_t*) &inner->current_zpos);

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

    if (should_create_window_surface_backing_store) {
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

        should_create_window_surface_backing_store = false;
    } else {
        // After the primary plane backing store was created,
        // we only create overlay plane backing stores. I.e.
        // backing stores, which have a FBO, that have a
        // color-attached RBO, that has a DRM EGLImage as the storage,
        // which in turn has a DRM FB associated with it.
        ok = create_drm_fb_backing_store(
            config,
            backing_store_out,
            user_data
        );

        if (ok != 0) {
            return false;
        }
    }

    return true;
}

static int  collect_window_surface_backing_store(
    const FlutterBackingStore *store,
    struct backing_store_metadata *meta
) {
    struct window_surface_backing_store *inner = &meta->window_surface;
    return 0;
}

static int  collect_drm_fb_backing_store(
    const FlutterBackingStore *store,
    struct backing_store_metadata *meta
) {
    struct drm_fb_backing_store *inner = &meta->drm_fb;
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

            should_create_window_surface_backing_store = true;
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


static int present_window_surface_backing_store(
    struct window_surface_backing_store *backing_store,
    int offset_x,
    int offset_y,
    int width,
    int height
) {
    struct gbm_bo *next_front_bo;
    uint32_t next_front_fb_id;
    int ok;
	
    next_front_bo = gbm_surface_lock_front_buffer(backing_store->gbm_surface);
	next_front_fb_id = gbm_bo_get_drm_fb_id(next_front_bo);

	// workaround for #38
	if (!drm.disable_vsync) {
		ok = drmModePageFlip(drm.fd, drm.crtc_id, next_front_fb_id, DRM_MODE_PAGE_FLIP_EVENT, backing_store->current_front_bo);
		if (ok) {
			perror("failed to queue page flip");
			return false;
		}
	} else {
		ok = drmModeSetCrtc(drm.fd, drm.crtc_id, next_front_fb_id, 0, 0, &drm.connector_id, 1, drm.mode);
		if (ok == -1) {
			perror("failed swap buffers");
			return false;
		}
	}

    // TODO: move this to the page flip handler.
    // We can only be sure the buffer can be released when the buffer swap
    // ocurred.
	gbm_surface_release_buffer(backing_store->gbm_surface, backing_store->current_front_bo);
	backing_store->current_front_bo = (struct gbm_bo *) next_front_bo;

    return 0;
}

static int present_drm_fb_backing_store(
    struct drm_fb_backing_store *backing_store,
    int offset_x,
    int offset_y,
    int width,
    int height,
    int zpos
) {
    int ok;

    printf("Presenting drm fb backing store\n");
    
    if (zpos != backing_store->current_zpos) {
        ok = set_plane_property_value(backing_store->drm_plane_id, "zpos", zpos);
        if (ok != 0) {
            perror("[compositor] Could not update zpos of hardware layer. drmModeObjectSetProperty");
            return errno;
        }
    }

    backing_store->current_front_rbo ^= 1;
    printf("Attaching rbo with id %u\n", backing_store->rbos[backing_store->current_front_rbo].gl_rbo_id);
    ok = attach_drm_rbo_to_fbo(backing_store->gl_fbo_id, backing_store->rbos + backing_store->current_front_rbo);
    if (ok != 0) return ok;

    // present the back buffer
    printf("Presenting rbo with id %u\n", backing_store->rbos[backing_store->current_front_rbo ^ 1].gl_rbo_id);
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
    
    return 0;
}

static struct view_cb_data *get_data_for_view_id(int64_t view_id) {
    struct view_cb_data *data;
    
    cpset_lock(&flutterpi_compositor.cbs);

    for_each_pointer_in_cpset(&flutterpi_compositor.cbs, data) {
        if (data->view_id == view_id) {
            cpset_unlock(&flutterpi_compositor.cbs);
            return data;
        }
    }

    cpset_unlock(&flutterpi_compositor.cbs);
    return NULL;
}

static int present_platform_view(
    int64_t view_id,
    const FlutterPlatformViewMutation **mutations,
    size_t num_mutations,
    int offset_x,
    int offset_y,
    int width,
    int height,
    int zpos
) {
    struct view_cb_data *data;

    data = get_data_for_view_id(view_id);
    if (data == NULL) {
        return EINVAL;
    }

    return data->present_cb(
        view_id,
        mutations,
        num_mutations,
        offset_x,
        offset_y,
        width,
        height,
        zpos,
        data->userdata
    );
}

static bool present_layers_callback(
    const FlutterLayer **layers,
    size_t layers_count,
    void *user_data
) {
    int window_surface_index;
	int ok;

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

    eglMakeCurrent(egl.display, egl.surface, egl.surface, egl.root_context);
    eglSwapBuffers(egl.display, egl.surface);

    /// find the index of the window surface.
    /// the window surface's zpos can't change, so we need to
    /// normalize all other backing stores' zpos around the
    /// window surfaces zpos.
    for (int i = 0; i < layers_count; i++) {
        if (layers[i]->type == kFlutterLayerContentTypeBackingStore
            && layers[i]->backing_store->type == kFlutterBackingStoreTypeOpenGL
            && layers[i]->backing_store->open_gl.type == kFlutterOpenGLTargetTypeFramebuffer
            && ((struct backing_store_metadata *) layers[i]->backing_store->user_data)->type == kWindowSurface) {
            
            window_surface_index = i;

            break;
        }
    }

    for (int i = 0; i < layers_count; i++) {
        const FlutterLayer *layer = layers[i];

        if (layer->type == kFlutterLayerContentTypeBackingStore) {
            const FlutterBackingStore *backing_store = layer->backing_store;
            struct backing_store_metadata *meta = backing_store->user_data;

            if (meta->type == kWindowSurface) {
                ok = present_window_surface_backing_store(
                    &meta->window_surface,
                    (int) layer->offset.x,
                    (int) layer->offset.y,
                    (int) layer->size.width,
                    (int) layer->size.height
                );
            } else if (meta->type == kDrmFb) {
                ok = present_drm_fb_backing_store(
                    &meta->drm_fb,
                    (int) layer->offset.x,
                    (int) layer->offset.y,
                    (int) layer->size.width,
                    (int) layer->size.height,
                    i - window_surface_index
                );
            }
        } else if (layer->type == kFlutterLayerContentTypePlatformView) {
            ok = present_platform_view(
                layer->platform_view->identifier,
                layer->platform_view->mutations,
                layer->platform_view->mutations_count,
                (int) round(layer->offset.x),
                (int) round(layer->offset.y),
                (int) round(layer->size.width),
                (int) round(layer->size.height),
                i - window_surface_index
            );
        } else {
            fprintf(stderr, "[compositor] Unsupported flutter layer type: %d\n", layer->type);
        }
    }

    eglMakeCurrent(egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    FlutterEngineTraceEventDurationEnd("present");

    return true;
}

int compositor_set_platform_view_present_cb(int64_t view_id, platform_view_present_cb cb, void *userdata) {
    struct view_cb_data *entry;

    cpset_lock(&flutterpi_compositor.cbs);

    for_each_pointer_in_cpset(&flutterpi_compositor.cbs, entry) {
        if (entry->view_id == view_id) {
            break;
        }
    }

    if (entry && !cb) {
        cpset_remove_locked(&flutterpi_compositor.cbs, entry);
    } else if (!entry && cb) {
        entry = calloc(1, sizeof(*entry));
        if (!entry) {
            cpset_unlock(&flutterpi_compositor.cbs);
            return ENOMEM;
        }

        entry->view_id = view_id;
        entry->present_cb = cb;
        entry->userdata = userdata;

        cpset_put_locked(&flutterpi_compositor.cbs, entry);
    }

    return cpset_unlock(&flutterpi_compositor.cbs);
}

const FlutterCompositor flutter_compositor = {
    .struct_size = sizeof(FlutterCompositor),
    .create_backing_store_callback = create_backing_store,
    .collect_backing_store_callback = collect_backing_store,
    .present_layers_callback = present_layers_callback,
    .user_data = &flutterpi_compositor
};