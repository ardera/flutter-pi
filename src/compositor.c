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
#include <dylib_deps.h>

struct view_cb_data {
	int64_t view_id;
	platform_view_mount_cb mount;
	platform_view_unmount_cb unmount;
	platform_view_update_view_cb update_view;
	platform_view_present_cb present;
	void *userdata;

	bool was_present_last_frame;
	int64_t last_zpos;
	FlutterSize last_size;
	FlutterPoint last_offset;
	size_t last_num_mutations;
	FlutterPlatformViewMutation last_mutations[16];
};

struct compositor {
    struct drmdev *drmdev;

	struct gbm_surface *gbm_surface;

    /**
     * @brief The EGL interface to for swapping buffers, creating EGL images, etc.
     */
    struct libegl *libegl;

    EGLDisplay display;
	EGLSurface surface;
	EGLContext context;

    /**
     * @brief Information about the EGL Display, like supported extensions / EGL versions.
     */
    struct egl_display_info *display_info;

    /**
     * @brief Flutter engine function for getting the current system time (uses monotonic clock)
     */
    flutter_engine_get_current_time_t get_current_time;

    /*
     * Flutter engine profiling functions 
     */
	flutter_engine_trace_event_duration_begin_t trace_event_begin;
	flutter_engine_trace_event_duration_end_t trace_event_end;
	flutter_engine_trace_event_instant_t trace_event_instant;

    /**
     * @brief Contains a struct for each existing platform view, containing the view id
     * and platform view callbacks.
     * 
     * @see compositor_set_view_callbacks compositor_remove_view_callbacks
     */
    struct concurrent_pointer_set cbs;

    /**
     * @brief Whether the compositor should invoke @ref rendertarget_gbm_new the next time
     * flutter creates a backing store. Otherwise @ref rendertarget_nogbm_new is invoked.
     * 
     * It's only possible to have at most one GBM-Surface backed backing store (== @ref rendertarget_gbm). So the first
     * time @ref on_create_backing_store is invoked, a GBM-Surface backed backing store is returned and after that,
     * only backing stores with @ref rendertarget_nogbm.
     */
    bool should_create_window_surface_backing_store;

    /**
     * @brief Whether the display mode was already applied. (Resolution, Refresh rate, etc)
     * If it wasn't already applied, it will be the first time @ref on_present_layers
     * is invoked.
     */
    bool has_applied_modeset;

    /**
     * @brief A cache of rendertargets that are not currently in use for
     * any flutter layers and can be reused.
     * 
     * Make sure to destroy all stale rendertargets before presentation so all the DRM planes
     * that are reserved by any stale rendertargets get freed.
     */
    struct concurrent_pointer_set stale_rendertargets;

    /**
     * @brief Whether the mouse cursor is currently enabled and visible.
     */

    struct {
        bool is_enabled;
        int cursor_size;
        const struct cursor_icon *current_cursor;
        int current_rotation;
        int hot_x, hot_y;
        int x, y;

        bool has_buffer;
        int buffer_depth;
        int buffer_pitch;
        int buffer_width;
        int buffer_height;
        int buffer_size;
        uint32_t drm_fb_id;
        uint32_t gem_bo_handle;
        uint32_t *buffer;
    } cursor;

    /**
     * If true, @ref on_present_layers will commit blockingly.
     * 
     * It will also schedule a simulated page flip event on the main thread
     * afterwards so the frame queue works.
     * 
     * If false, @ref on_present_layers will commit nonblocking using page flip events,
     * like usual.
     */
    bool do_blocking_atomic_commits;
};


static struct view_cb_data *get_cbs_for_view_id_locked(
	struct compositor *compositor,
	int64_t view_id
) {
	struct view_cb_data *data;
	
	for_each_pointer_in_cpset(&compositor->cbs, data) {
		if (data->view_id == view_id) {
			return data;
		}
	}

	return NULL;
}

static struct view_cb_data *get_cbs_for_view_id(
	struct compositor *compositor,
	int64_t view_id
) {
	struct view_cb_data *data;
	
	cpset_lock(&compositor->cbs);
	data = get_cbs_for_view_id_locked(compositor, view_id);
	cpset_unlock(&compositor->cbs);
	
	return data;
}

/**
 * @brief Destroy all the rendertargets in the stale rendertarget cache.
 */
static void destroy_stale_rendertargets(
	struct compositor *compositor
) {
	struct rendertarget *target;

	cpset_lock(&compositor->stale_rendertargets);

	for_each_pointer_in_cpset(&compositor->stale_rendertargets, target) {
		target->destroy(target);
		cpset_remove_locked(&compositor->stale_rendertargets, target);
		target = NULL;
	}

	cpset_unlock(&compositor->stale_rendertargets);
}

static void destroy_gbm_bo(
	struct gbm_bo *bo,
	void *userdata
) {
	struct drm_fb *fb = userdata;

	if (fb && fb->fb_id)
		kmsdev_destroy_fb(fb->dev, fb->fb_id);
	
	free(fb);
}

/**
 * @brief Get a DRM FB id for this GBM BO, so we can display it.
 */
static uint32_t gbm_bo_get_drm_fb_id(struct kmsdev *dev, struct gbm_bo *bo) {
	struct drm_fb *fb;
	uint64_t modifiers[4] = {0};
	uint32_t width, height, format;
	uint32_t strides[4] = {0};
	uint32_t handles[4] = {0};
	uint32_t offsets[4] = {0};
	uint32_t flags = 0;
	int num_planes;
	int ok = -1;
	
	fb = gbm_bo_get_user_data(bo);
	/// TODO: Invalidate the drm_fb in the case that the drmdev changed (although that should never happen)
	// If the buffer object already has some userdata associated with it,
	// it's the drm_fb we allocated.
	if (fb != NULL) {
		return fb->fb_id;
	}

	// If there's no framebuffer for the BO, we need to create one.
	fb = calloc(1, sizeof(struct drm_fb));
	if (fb == NULL) {
		return (uint32_t) -1;
	}

	fb->bo = bo;
	fb->dev = dev;

	width = gbm_bo_get_width(bo);
	height = gbm_bo_get_height(bo);
	format = gbm_bo_get_format(bo);

	modifiers[0] = gbm_bo_get_modifier(bo);
	num_planes = gbm_bo_get_plane_count(bo);

	for (int i = 0; i < num_planes; i++) {
		strides[i] = gbm_bo_get_stride_for_plane(bo, i);
		handles[i] = gbm_bo_get_handle(bo).u32;
		offsets[i] = gbm_bo_get_offset(bo, i);
		modifiers[i] = modifiers[0];
	}

	if (modifiers[0]) {
		flags = DRM_MODE_FB_MODIFIERS;
	}

	ok = kmsdev_add_fb(
		dev,
		width,
		height,
		format,
		handles,
		strides,
		offsets,
		modifiers,
		&fb->fb_id,
		flags
	);
	if (ok != 0) {
		LOG_COMPOSITOR_ERROR("Couldn't add GBM BO as DRM framebuffer. kmsdev_add_fb: %s\n", strerror(ok));
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
	struct drmdev *drmdev,
	struct libegl *libegl,
	EGLDisplay display,
	struct egl_display_info *display_info,
	size_t width,
	size_t height,
	struct drm_rbo *out
) {
	struct drm_rbo fbo;
	EGLBoolean egl_ok;
	EGLint egl_error;
	GLenum gl_error;
	int ok;

	eglGetError();
	glGetError();

	if (!display_info->supports_mesa_drm_image) {
		LOG_COMPOSITOR_ERROR("EGL doesn't support MESA_drm_image. Can't create DRM image backed OpenGL renderbuffer.\n");
		return ENOTSUP;
	}

	fbo.egl_image = libegl->eglCreateDRMImageMESA(display, (const EGLint[]) {
		EGL_WIDTH, width,
		EGL_HEIGHT, height,
		EGL_DRM_BUFFER_FORMAT_MESA, EGL_DRM_BUFFER_FORMAT_ARGB32_MESA,
		EGL_DRM_BUFFER_USE_MESA, EGL_DRM_BUFFER_USE_SCANOUT_MESA,
		EGL_NONE
	});
	if ((fbo.egl_image == EGL_NO_IMAGE) || (egl_error = eglGetError()) != EGL_SUCCESS) {
		LOG_COMPOSITOR_ERROR("Error creating DRM EGL Image for flutter backing store. eglCreateDRMImageMESA: %u\n", egl_error);
		ok = EIO;
		goto fail_return_ok;
	}

	{
		EGLint stride = fbo.gem_stride;
		egl_ok = libegl->eglExportDRMImageMESA(display, fbo.egl_image, NULL, (EGLint*) &fbo.gem_handle, &stride);
		if (egl_ok == false || (egl_error = eglGetError()) != EGL_SUCCESS) {
			LOG_COMPOSITOR_ERROR("Error getting handle & stride for DRM EGL Image. eglExportDRMImageMESA: %u\n", egl_error);
			ok = EIO;
			goto fail_destroy_egl_image;
		}
	}

	glGenRenderbuffers(1, &fbo.gl_rbo_id);
	if ((gl_error = glGetError())) {
		LOG_COMPOSITOR_ERROR("Error generating renderbuffers for flutter backing store. glGenRenderbuffers: %u\n", gl_error);
		ok = EIO;
		goto fail_destroy_egl_image;
	}

	glBindRenderbuffer(GL_RENDERBUFFER, fbo.gl_rbo_id);
	if ((gl_error = glGetError())) {
		LOG_COMPOSITOR_ERROR("Error binding renderbuffer, glBindRenderbuffer: %u\n", gl_error);
		ok = EIO;
		goto fail_delete_rbo;
	}

	/// FIXME: Use libgl here
	//flutterpi.gl.EGLImageTargetRenderbufferStorageOES(GL_RENDERBUFFER, fbo.egl_image);
	if ((gl_error = glGetError())) {
		LOG_COMPOSITOR_ERROR("Error binding DRM EGL Image to renderbuffer, glEGLImageTargetRenderbufferStorageOES: %u\n", gl_error);
		ok = EIO;
		goto fail_unbind_rbo;
	}

	glBindRenderbuffer(GL_RENDERBUFFER, 0);
	
	ok = drmModeAddFB2(
		drmdev->fd,
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
		LOG_COMPOSITOR_ERROR("Could not make DRM fb from EGL Image. drmModeAddFB2: %s", strerror(errno));
		ok = errno;
		goto fail_delete_rbo;
	}

	fbo.drmdev = drmdev;
	fbo.libegl = libegl;
	fbo.display = display;

	*out = fbo;

	return 0;

	fail_unbind_rbo:
	glBindRenderbuffer(GL_RENDERBUFFER, 0);

	fail_delete_rbo:
	glDeleteRenderbuffers(1, &fbo.gl_rbo_id);

	fail_destroy_egl_image:
	libegl->eglDestroyImage(display, fbo.egl_image);

	fail_return_ok:
	return ok;
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
	if ((gl_error = glGetError())) {
		fprintf(stderr, "[compositor] error binding FBO for attaching the renderbuffer, glBindFramebuffer: %d\n", gl_error);
		return EINVAL;
	}

	glBindRenderbuffer(GL_RENDERBUFFER, rbo->gl_rbo_id);
	if ((gl_error = glGetError())) {
		fprintf(stderr, "[compositor] error binding renderbuffer, glBindRenderbuffer: %d\n", gl_error);
		return EINVAL;
	}

	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rbo->gl_rbo_id);
	if ((gl_error = glGetError())) {
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
	EGLBoolean egl_ok;
	EGLint egl_error;
	GLenum gl_error;
	int ok;

	eglGetError();
	glGetError();

	glDeleteRenderbuffers(1, &rbo->gl_rbo_id);
	if ((gl_error = glGetError())) {
		LOG_COMPOSITOR_ERROR("Error destroying OpenGL RBO, glDeleteRenderbuffers: 0x%08X\n", gl_error);
	}

	ok = drmModeRmFB(rbo->drmdev->fd, rbo->drm_fb_id);
	if (ok < 0) {
		LOG_COMPOSITOR_ERROR("Error removing DRM FB, drmModeRmFB: %s\n", strerror(errno));
	}

	egl_ok = eglDestroyImage(rbo->display, rbo->egl_image);
	if ((egl_ok == false) || (egl_error = eglGetError()) != EGL_SUCCESS) {
		LOG_COMPOSITOR_ERROR("Error destroying EGL image, eglDestroyImage: 0x%08X\n", egl_error);
	}
}

static void rendertarget_gbm_destroy(struct rendertarget *target) {
	free(target);
}

static void rendertarget_gbm_on_pageflipped_out(struct kmsdev *dev, void *userdata) {
	struct rendertarget *target;

	target = userdata;

	gbm_surface_release_buffer(target->gbm.gbm_surface, target->gbm.current_front_bo);
}

static int rendertarget_gbm_present(
	struct rendertarget *target,
	struct kms_crtc_info *crtc_info,
	struct kms_presenter *builder,
	double x, double y,
	double width, double height
) {
	struct rendertarget_gbm *gbm_target;
	struct gbm_bo *next_front_bo;
	uint32_t next_front_fb_id;
	bool supported;
	int ok;

	gbm_target = &target->gbm;

	next_front_bo = gbm_surface_lock_front_buffer(gbm_target->gbm_surface);
	/// TODO: put into allocation
	next_front_fb_id = gbm_bo_get_drm_fb_id(kmsdev, next_front_bo);

	ok = kms_presenter_push_fb_layer(
		builder,
		&(const struct kms_fb_layer) {
			.fb_id = next_front_fb_id,
			.src_x = 0, .src_y = 0,
			.src_w = gbm_bo_get_width(next_front_bo) << 16,
			.src_h = gbm_bo_get_height(next_front_bo) << 16,
			.crtc_x = 0, .crtc_y = 0,
			.crtc_x = round(width), .crtc_y = round(height),
			.has_rotation = false,
			.rotation = DRM_MODE_ROTATE_0,
			.on_flipped_in = NULL,
			.on_flipped_out = NULL,
			.userdata = NULL
		}
	);

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
			.gbm_surface = compositor->gbm_surface,
			.current_front_bo = NULL
		},
		.gl_fbo_id = 0,
		.destroy = rendertarget_gbm_destroy,
		.present = rendertarget_gbm_present,
		.present_legacy = NULL
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
	bool supported;
	int ok;

	nogbm_target = &target->nogbm;

	nogbm_target->current_front_rbo ^= 1;
	ok = attach_drm_rbo_to_fbo(nogbm_target->gl_fbo_id, nogbm_target->rbos + nogbm_target->current_front_rbo);
	if (ok != 0) return ok;

	drmdev_atomic_req_put_plane_property(req, drm_plane_id, "FB_ID", nogbm_target->rbos[nogbm_target->current_front_rbo ^ 1].drm_fb_id);
	drmdev_atomic_req_put_plane_property(req, drm_plane_id, "CRTC_ID", req->drmdev->selected_crtc->crtc->crtc_id);
	drmdev_atomic_req_put_plane_property(req, drm_plane_id, "SRC_X", 0);
	drmdev_atomic_req_put_plane_property(req, drm_plane_id, "SRC_Y", 0);
	drmdev_atomic_req_put_plane_property(req, drm_plane_id, "SRC_W", ((uint16_t) req->drmdev->selected_mode->hdisplay) << 16);
	drmdev_atomic_req_put_plane_property(req, drm_plane_id, "SRC_H", ((uint16_t) req->drmdev->selected_mode->vdisplay) << 16);
	drmdev_atomic_req_put_plane_property(req, drm_plane_id, "CRTC_X", 0);
	drmdev_atomic_req_put_plane_property(req, drm_plane_id, "CRTC_Y", 0);
	drmdev_atomic_req_put_plane_property(req, drm_plane_id, "CRTC_W", req->drmdev->selected_mode->hdisplay);
	drmdev_atomic_req_put_plane_property(req, drm_plane_id, "CRTC_H", req->drmdev->selected_mode->vdisplay);
	
	ok = drmdev_plane_supports_setting_rotation_value(req->drmdev, drm_plane_id, DRM_MODE_ROTATE_0 | DRM_MODE_REFLECT_Y, &supported);
	if (ok != 0) return ok;
	
	if (supported) {
		drmdev_atomic_req_put_plane_property(req, drm_plane_id, "rotation", DRM_MODE_ROTATE_0 | DRM_MODE_REFLECT_Y);
	} else {
		static bool printed = false;

		if (!printed) {
			fprintf(stderr,
					"[compositor] GPU does not support reflecting the screen in Y-direction.\n"
					"             This is required for rendering into hardware overlay planes though.\n"
					"             Any UI that is drawn in overlay planes will look upside down.\n"
			);
			printed = true;
		}
	}

	ok = drmdev_plane_supports_setting_zpos_value(req->drmdev, drm_plane_id, zpos, &supported);
	if (ok != 0) return ok;
	
	if (supported) {
		drmdev_atomic_req_put_plane_property(req, drm_plane_id, "zpos", zpos);
	} else {
		static bool printed = false;

		if (!printed) { 
			fprintf(stderr,
					"[compositor] GPU does not supported the desired HW plane order.\n"
					"             Some UI layers may be invisible.\n"
			);
			printed = true;
		}
	}

	return 0;
}

static int rendertarget_nogbm_present_legacy(
	struct rendertarget *target,
	struct drmdev *drmdev,
	uint32_t drm_plane_id,
	int offset_x,
	int offset_y,
	int width,
	int height,
	int zpos,
	bool set_mode
) {
	struct rendertarget_nogbm *nogbm_target;
	uint32_t fb_id;
	bool supported, is_primary;
	int ok;

	nogbm_target = &target->nogbm;

	is_primary = drmdev_plane_get_type(drmdev, drm_plane_id) == DRM_PLANE_TYPE_PRIMARY;

	nogbm_target->current_front_rbo ^= 1;
	ok = attach_drm_rbo_to_fbo(nogbm_target->gl_fbo_id, nogbm_target->rbos + nogbm_target->current_front_rbo);
	if (ok != 0) return ok;

	fb_id = nogbm_target->rbos[nogbm_target->current_front_rbo ^ 1].drm_fb_id;

	if (is_primary) {
		if (set_mode) {
			drmdev_legacy_set_mode_and_fb(
				drmdev,
				fb_id
			);
		} else {
			drmdev_legacy_primary_plane_pageflip(
				drmdev,
				fb_id,
				NULL
			);
		}
	} else {
		drmdev_legacy_overlay_plane_pageflip(
			drmdev,
			drm_plane_id,
			fb_id,
			0,
			0,
			drmdev->selected_mode->hdisplay,
			drmdev->selected_mode->vdisplay,
			0,
			0,
			((uint16_t) drmdev->selected_mode->hdisplay) << 16,
			((uint16_t) drmdev->selected_mode->vdisplay) << 16
		);
	}
	
	ok = drmdev_plane_supports_setting_rotation_value(drmdev, drm_plane_id, DRM_MODE_ROTATE_0 | DRM_MODE_REFLECT_Y, &supported);
	if (ok != 0) return ok;
	
	if (supported) {
		drmdev_legacy_set_plane_property(drmdev, drm_plane_id, "rotation", DRM_MODE_ROTATE_0 | DRM_MODE_REFLECT_Y);
	} else {
		static bool printed = false;

		if (!printed) {
			fprintf(stderr,
					"[compositor] GPU does not support reflecting the screen in Y-direction.\n"
					"             This is required for rendering into hardware overlay planes though.\n"
					"             Any UI that is drawn in overlay planes will look upside down.\n"
			);
			printed = true;
		}
	}

	ok = drmdev_plane_supports_setting_zpos_value(drmdev, drm_plane_id, zpos, &supported);
	if (ok != 0) return ok;
	
	if (supported) {
		drmdev_legacy_set_plane_property(drmdev, drm_plane_id, "zpos", zpos);
	} else {
		static bool printed = false;

		if (!printed) { 
			fprintf(stderr,
					"[compositor] GPU does not supported the desired HW plane order.\n"
					"             Some UI layers may be invisible.\n"
			);
			printed = true;
		}
	}

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
	target->present_legacy = rendertarget_nogbm_present_legacy;

	eglGetError();
	glGetError();

	glGenFramebuffers(1, &target->nogbm.gl_fbo_id);
	if ((gl_error = glGetError())) {
		fprintf(stderr, "[compositor] error generating FBOs for flutter backing store, glGenFramebuffers: %d\n", gl_error);
		ok = EINVAL;
		goto fail_free_target;
	}

	ok = create_drm_rbo(
		compositor->drmdev,
		compositor->libegl,
		compositor->display,
		compositor->display_info,
		compositor->drmdev->selected_mode->hdisplay,
		compositor->drmdev->selected_mode->vdisplay,
		target->nogbm.rbos + 0
	);
	if (ok != 0) {
		goto fail_delete_fb;
	}

	ok = create_drm_rbo(
		compositor->drmdev,
		compositor->libegl,
		compositor->display,
		compositor->display_info,
		compositor->drmdev->selected_mode->hdisplay,
		compositor->drmdev->selected_mode->vdisplay,
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

	cpset_put(&compositor->stale_rendertargets, store->target);

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

	cpset_put(&compositor->stale_rendertargets, store->target);

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

	// first, try to find a stale GBM rendertarget.
	cpset_lock(&compositor->stale_rendertargets);
	for_each_pointer_in_cpset(&compositor->stale_rendertargets, target) break;
	if (target != NULL) {
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

			ok = rendertarget_gbm_new(
				&target,
				compositor
			);

			if (ok != 0) {
				free(store);
				return false;
			}

			compositor->should_create_window_surface_backing_store = false;
		} else {
			ok = rendertarget_nogbm_new(
				&target,
				compositor
			);

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

struct simulated_page_flip_event_data {
	unsigned int sec;
	unsigned int usec;
};

extern void on_pageflip_event(
	int fd,
	unsigned int frame,
	unsigned int sec,
	unsigned int usec,
	void *userdata
);

static int execute_simulate_page_flip_event(void *userdata) {
	struct simulated_page_flip_event_data *data;

	data = userdata;

	/// TODO: Reimplement simulate page flip
	//on_pageflip_event(flutterpi.drm.drmdev->fd, 0, data->sec, data->usec, NULL);

	free(data);

	return 0;
}

struct compositor *compositor_new(
	struct drmdev *drmdev,
	struct gbm_surface *gbm_surface,
	struct libegl *libegl,
	EGLDisplay display,
	EGLSurface surface,
	EGLContext context,
	struct egl_display_info *display_info,
	flutter_engine_get_current_time_t get_current_time,
	flutter_engine_trace_event_duration_begin_t trace_event_begin,
	flutter_engine_trace_event_duration_end_t trace_event_end,
	flutter_engine_trace_event_instant_t trace_event_instant
) {
	struct compositor *compositor;
	int ok;

	if (drmdev == NULL) {
		LOG_COMPOSITOR_ERROR("In compositor_new: drmdev can't be NULL.\n");
		return NULL;
	}
	if (gbm_surface == NULL) {
		LOG_COMPOSITOR_ERROR("In compositor_new: gbm_surface can't be NULL.\n");
		return NULL;
	}
	if (libegl == NULL) {
		LOG_COMPOSITOR_ERROR("In compositor_new: libegl can't be NULL.\n");
		return NULL;
	}
	if (display == EGL_NO_DISPLAY) {
		LOG_COMPOSITOR_ERROR("In compositor_new: display can't be EGL_NO_DISPLAY.\n");
		return NULL;
	}
	if (surface == EGL_NO_SURFACE) {
		LOG_COMPOSITOR_ERROR("In compositor_new: surface can't be EGL_NO_SURFACE.\n");
		return NULL;
	}
	if (context == EGL_NO_CONTEXT) {
		LOG_COMPOSITOR_ERROR("In compositor_new: context can't be EGL_NO_CONTEXT.\n");
		return NULL;
	}
	if (display_info == NULL) {
		LOG_COMPOSITOR_ERROR("In compositor_new: display_info can't be NULL.\n");
		return NULL;
	}
	
	compositor = malloc(sizeof *compositor);
	if (compositor == NULL) {
		LOG_COMPOSITOR_ERROR("In compositor_new: Out of memory\n");
		return NULL;
	}

	ok = cpset_init(&compositor->cbs, CPSET_DEFAULT_MAX_SIZE);
	if (ok != 0) {
		goto fail_free_compositor;
	}

	ok = cpset_init(&compositor->stale_rendertargets, CPSET_DEFAULT_MAX_SIZE);
	if (ok != 0) {
		goto fail_deinit_cbs;
	}

	compositor->drmdev = drmdev;
	compositor->gbm_surface = gbm_surface;
	compositor->libegl = libegl;
	compositor->display = display;
	compositor->surface = surface;
	compositor->context = context;
	compositor->display_info = display_info;
	compositor->get_current_time = get_current_time;
	compositor->trace_event_begin = trace_event_begin;
	compositor->trace_event_end = trace_event_end;
	compositor->trace_event_instant = trace_event_instant;
	compositor->should_create_window_surface_backing_store = true;
	compositor->has_applied_modeset = false;
	compositor->cursor.is_enabled = false;
	compositor->cursor.cursor_size = 0;
	compositor->cursor.current_cursor = NULL;
	compositor->cursor.current_rotation = 0;
	compositor->cursor.hot_x = 0;
	compositor->cursor.hot_y = 0;
	compositor->cursor.x = 0;
	compositor->cursor.y = 0;
	compositor->cursor.has_buffer = false;
	compositor->cursor.buffer_depth = 0;
	compositor->cursor.buffer_pitch = 0;
	compositor->cursor.buffer_width = 0;
	compositor->cursor.buffer_height = 0;
	compositor->cursor.buffer_size = 0;
	compositor->cursor.drm_fb_id = (uint32_t) -1;
	compositor->cursor.gem_bo_handle = (uint32_t) -1;
	compositor->cursor.buffer = NULL;
	compositor->do_blocking_atomic_commits = false;

	return compositor;


	fail_deinit_cbs:
	cpset_deinit(&compositor->cbs);

	fail_free_compositor:
	free(compositor);

	return NULL;
}

void compositor_set_engine_callbacks(
    struct compositor *compositor,
    flutter_engine_get_current_time_t get_current_time,
	flutter_engine_trace_event_duration_begin_t trace_event_begin,
	flutter_engine_trace_event_duration_end_t trace_event_end,
	flutter_engine_trace_event_instant_t trace_event_instant
) {
	if (get_current_time == NULL) {
		LOG_COMPOSITOR_ERROR("In compositor_new: get_current_time can't be NULL.");
		return;
	}
	if (trace_event_begin == NULL) {
		LOG_COMPOSITOR_ERROR("In compositor_new: trace_event_begin can't be NULL.");
		return;
	}
	if (trace_event_end == NULL) {
		LOG_COMPOSITOR_ERROR("In compositor_new: trace_event_end can't be NULL.");
		return;
	}
	if (trace_event_instant == NULL) {
		LOG_COMPOSITOR_ERROR("In compositor_new: trace_event_instant can't be NULL.");
		return;
	}

	compositor->get_current_time = get_current_time;
	compositor->trace_event_begin = trace_event_begin;
	compositor->trace_event_end = trace_event_end;
	compositor->trace_event_instant = trace_event_instant;
}

void compositor_destroy(
	struct compositor *compositor
) {
	
}

static bool on_present_layers(
	const FlutterLayer **layers,
	size_t layers_count,
	void *userdata
) {
	struct drmdev_atomic_req *req;
	struct view_cb_data *cb_data;
	struct pointer_set planes;
	struct compositor *compositor;
	struct drm_plane *plane;
	struct drmdev *drmdev;
	EGLDisplay stored_display;
	EGLSurface stored_read_surface;
	EGLSurface stored_write_surface;
	EGLContext stored_context;
	EGLBoolean egl_ok;
	uint32_t req_flags;
	EGLenum egl_error;
	void *planes_storage[32] = {0};
	bool legacy_rendertarget_set_mode = false;
	bool schedule_fake_page_flip_event;
	bool use_atomic_modesetting;
	int ok;

	/// TODO: proper error handling

	compositor = userdata;
	drmdev = compositor->drmdev;
	schedule_fake_page_flip_event = compositor->do_blocking_atomic_commits;
	use_atomic_modesetting = drmdev->supports_atomic_modesetting;

	req = NULL;
	if (use_atomic_modesetting) {
		// create a new atomic request
		ok = drmdev_new_atomic_req(compositor->drmdev, &req);
		if (ok != 0) {
			return false;
		}
	} else {
		// create a new pointer set so we can track what planes are in use / free to use
		// (since we don't have drmdev_atomic_req_reserve_plane)
		planes = PSET_INITIALIZER_STATIC(planes_storage, 32);
		for_each_plane_in_drmdev(drmdev, plane) {
			if (plane->plane->possible_crtcs & drmdev->selected_crtc->bitmask) {
				ok = pset_put(&planes, plane);
				if (ok != 0) {
					return false;
				}
			}
		}
	}

	cpset_lock(&compositor->cbs);

	// store the previously the currently bound display, surface & context
	stored_display = eglGetCurrentDisplay();
	stored_read_surface = eglGetCurrentSurface(EGL_READ);
	stored_write_surface = eglGetCurrentSurface(EGL_DRAW);
	stored_context = eglGetCurrentContext();

	// make our own display, surface & context current
	egl_ok = eglMakeCurrent(compositor->display, compositor->surface, compositor->surface, compositor->context);
	if ((egl_ok == false) || (egl_error = eglGetError()) != EGL_SUCCESS) {
		LOG_COMPOSITOR_ERROR("Could not make the presenting EGL surface & context current. eglMakeCurrent: %d\n", egl_error);
		return false;
	}

	// swap buffers / wait for rendering to complete
	egl_ok = eglSwapBuffers(compositor->display, compositor->surface);
	if ((egl_ok == false) || (egl_error = eglGetError()) != EGL_SUCCESS) {
		LOG_COMPOSITOR_ERROR("Could not wait for GPU to finish rendering. eglSwapBuffers: %d\n", egl_error);
		return false;
	}

	req_flags =  0 /* DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK*/;

	// if we haven't applied the display mode,
	// we need to do that on the first frame
	// we also try to set the zpos value of the cursor plane to the highest possible value,
	// so it's always in front
	if (compositor->has_applied_modeset == false) {
		if (use_atomic_modesetting) {
			ok = drmdev_atomic_req_put_modeset_props(req, &req_flags);
			if (ok != 0) return false;
		} else {
			legacy_rendertarget_set_mode = true;
			schedule_fake_page_flip_event = true;
		}

		bool moved_cursor_to_front = false;

		for_each_unreserved_plane_in_atomic_req(req, plane) {
			if (plane->type == DRM_PLANE_TYPE_CURSOR) {
				int64_t max_zpos;
				bool supported;

				// Find out the max zpos value for the cursor plane
				ok = drmdev_plane_get_max_zpos_value(drmdev, plane->plane->plane_id, &max_zpos);
				if (ok != 0) {
					LOG_COMPOSITOR_ERROR("Couldn't get max zpos for mouse cursor. drmdev_plane_get_max_zpos_value: %s\n", strerror(ok));
					continue;
				}
				
				// Find out if we can actually set it (which may not be supported if the zpos property is immutable)
				ok = drmdev_plane_supports_setting_zpos_value(drmdev, plane->plane->plane_id, max_zpos, &supported);
				if (ok != 0) {
					LOG_COMPOSITOR_ERROR("Failed to check if updating the mouse cursor zpos is supported. drmdev_plane_supports_setting_zpos_value: %s\n", strerror(ok));
					continue;
				}

				// If we can set it, do that
				if (supported) {
					if (use_atomic_modesetting) {
						ok = drmdev_atomic_req_put_plane_property(req, plane->plane->plane_id, "zpos", max_zpos);
						if (ok != 0) {
							LOG_COMPOSITOR_ERROR("Couldn't move mouse cursor to front. Mouse cursor may be invisible. drmdev_atomic_req_put_plane_property: %s\n", strerror(ok));
							continue;
						}
					} else {
						ok = drmdev_legacy_set_plane_property(drmdev, plane->plane->plane_id, "zpos", max_zpos);
						if (ok != 0) {
							LOG_COMPOSITOR_ERROR("Couldn't move mouse cursor to front. Mouse cursor may be invisible. drmdev_atomic_req_put_plane_property: %s\n", strerror(ok));
							continue;
						}
					}

					moved_cursor_to_front = true;
					break;
				} else {
					LOG_COMPOSITOR_ERROR("Moving mouse cursor to front is not supported. Mouse cursor may be invisible.\n");
					continue;
				}
			}
		}
		
		if (moved_cursor_to_front == false) {
			LOG_COMPOSITOR_ERROR("Couldn't move cursor to front. Mouse cursor may be invisible.\n");
		}

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

			for (size_t i = 0; i < layers_count; i++) {
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
					for (size_t i = 0; (i < layer->platform_view->mutations_count) && !did_update_view; i++) {
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

			for (size_t i = 0; i < layers_count; i++) {
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
			for (size_t i = 0; i < layer->platform_view->mutations_count; i++) {
				memcpy(cb_data->last_mutations + i, layer->platform_view->mutations[i], sizeof(FlutterPlatformViewMutation));
			}
		}

		for_each_pointer_in_pset(&mounted_views, cb_data) {
			const FlutterLayer *layer;
			int zpos;

			for (size_t i = 0; i < layers_count; i++) {
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
			for (size_t i = 0; i < layer->platform_view->mutations_count; i++) {
				memcpy(cb_data->last_mutations + i, layer->platform_view->mutations[i], sizeof(FlutterPlatformViewMutation));
			}
		}
	}
	
	int64_t min_zpos;
	
	// find out the minimum zpos value we can set.
	// technically, the zpos ranges could be different for each plane
	// but I think there's no driver doing that.
	for_each_pointer_in_pset(&planes, plane) {
		if (plane->type == DRM_PLANE_TYPE_PRIMARY) {
			ok = drmdev_plane_get_min_zpos_value(drmdev, plane->plane->plane_id, &min_zpos);
			
			if (ok != 0) {
				min_zpos = 0;
			}
			break;
		}
	}

	for (size_t i = 0; i < layers_count; i++) {
		if (layers[i]->type == kFlutterLayerContentTypeBackingStore) {
			for_each_pointer_in_pset(&planes, plane) {
				// choose a plane which has an "intrinsic" zpos that matches
				// the zpos we want the plane to have.
				// (Since planes are buggy and we can't rely on the zpos we explicitly
				// configure the plane to have to be actually applied to the hardware.
				// In short, assigning a different value to the zpos property won't always
				// take effect.)
				if ((i == 0) && (plane->type == DRM_PLANE_TYPE_PRIMARY)) {
					break;
				} else if ((i != 0) && (plane->type == DRM_PLANE_TYPE_OVERLAY)) {
					break;
				}
			}
			if (plane != NULL) {
				if (use_atomic_modesetting) {
					drmdev_atomic_req_reserve_plane(req, plane);
				} else {
					pset_remove(&planes, plane);
				}
			} else {
				LOG_COMPOSITOR_ERROR("Could not find a free primary/overlay DRM plane for presenting the backing store. drmdev_atomic_req_reserve_plane: %s\n", strerror(ok));
				continue;
			}

			struct flutterpi_backing_store *store = layers[i]->backing_store->user_data;
			struct rendertarget *target = store->target;

			if (use_atomic_modesetting) {
				ok = target->present(
					target,
					req,
					plane->plane->plane_id,
					0,
					0,
					compositor->drmdev->selected_mode->hdisplay,
					compositor->drmdev->selected_mode->vdisplay,
					i + min_zpos
				);
				if (ok != 0) {
					LOG_COMPOSITOR_ERROR("Could not present backing store. rendertarget->present: %s\n", strerror(ok));
				}
			} else {
				ok = target->present_legacy(
					target,
					drmdev,
					plane->plane->plane_id,
					0,
					0,
					compositor->drmdev->selected_mode->hdisplay,
					compositor->drmdev->selected_mode->vdisplay,
					i + min_zpos,
					legacy_rendertarget_set_mode && (plane->type == DRM_PLANE_TYPE_PRIMARY)
				);
				if (ok != 0) {
					LOG_COMPOSITOR_ERROR("Could not present backing store. rendertarget->present_legacy: %s\n", strerror(ok));
				}
			}
		} else if (layers[i]->type == kFlutterLayerContentTypePlatformView) {
			cb_data = get_cbs_for_view_id_locked(compositor, layers[i]->platform_view->identifier);

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
					i + min_zpos,
					cb_data->userdata
				);
				if (ok != 0) {
					LOG_COMPOSITOR_ERROR("Could not present platform view. platform_view->present: %s\n", strerror(ok));
				}
			}
		}
	}

	// Fill in blank FB_ID and CRTC_ID for unused planes.
	if (use_atomic_modesetting) {
		for_each_unreserved_plane_in_atomic_req(req, plane) {
			if ((plane->type == DRM_PLANE_TYPE_PRIMARY) || (plane->type == DRM_PLANE_TYPE_OVERLAY)) {
				drmdev_atomic_req_put_plane_property(req, plane->plane->plane_id, "FB_ID", 0);
				drmdev_atomic_req_put_plane_property(req, plane->plane->plane_id, "CRTC_ID", 0);
			}
		}
	}

	// Restore the previously bound display, surface and context
	egl_ok = eglMakeCurrent(stored_display, stored_read_surface, stored_write_surface, stored_context);
	if ((egl_ok == false) || (egl_error = eglGetError()) != EGL_SUCCESS) {
		LOG_COMPOSITOR_ERROR("Could not make the previously active EGL surface and context current. eglMakeCurrent: %d\n", egl_error);
		return false;
	}

	// If we use atomic modesetting, first, try to commit non-blockingly with a page flip event sent to the drmdev fd.
	// If that fails with EBUSY, use blocking commits
	/// TODO: Use syncobj's for synchronization
	if (use_atomic_modesetting) {
		do_commit:
		if (compositor->do_blocking_atomic_commits) {
			req_flags &= ~(DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT);
		} else {
			req_flags |= DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT;
		}
		
		ok = drmdev_atomic_req_commit(req, req_flags, NULL);
		if ((compositor->do_blocking_atomic_commits == false) && (ok == EBUSY)) {
			printf("[compositor] Non-blocking drmModeAtomicCommit failed with EBUSY.\n"
				"             Future drmModeAtomicCommits will be executed blockingly.\n"
				"             This may have have an impact on performance.\n");

			compositor->do_blocking_atomic_commits = true;
			schedule_fake_page_flip_event = true;
			goto do_commit;
		} else if (ok != 0) {
			fprintf(stderr, "[compositor] Could not present frame. drmModeAtomicCommit: %s\n", strerror(ok));
			drmdev_destroy_atomic_req(req);
			cpset_unlock(&compositor->cbs);
			return false;
		}

		drmdev_destroy_atomic_req(req);	
	}

	// If we use blocking atomic commits, we need to schedule a fake page flip event so flutters FlutterEngineOnVsync callback is called.
	if (schedule_fake_page_flip_event) {
		uint64_t time = compositor->get_current_time();

		struct simulated_page_flip_event_data *data = malloc(sizeof(struct simulated_page_flip_event_data));
		if (data == NULL) {
			return false;
		}

		data->sec = time / 1000000000llu;
		data->usec = (time % 1000000000llu) / 1000;

		/// TODO: Implement again
		// flutterpi_post_platform_task(NULL, execute_simulate_page_flip_event, data);
	}

	cpset_unlock(&compositor->cbs);

	return true;
}

int compositor_on_page_flip(
	struct compositor *compositor,
	uint32_t sec,
	uint32_t usec
) {
	return 0;
}

int compositor_put_view_callbacks(
	struct compositor *compositor,
	int64_t view_id,
	platform_view_mount_cb mount,
	platform_view_unmount_cb unmount,
	platform_view_update_view_cb update_view,
	platform_view_present_cb present,
	void *userdata
) {
	struct view_cb_data *entry;

	cpset_lock(&compositor->cbs);

	entry = get_cbs_for_view_id_locked(compositor, view_id);

	if (entry == NULL) {
		entry = calloc(1, sizeof(*entry));
		if (!entry) {
			cpset_unlock(&compositor->cbs);
			return ENOMEM;
		}

		cpset_put_locked(&compositor->cbs, entry);
	}

	entry->view_id = view_id;
	entry->mount = mount;
	entry->unmount = unmount;
	entry->update_view = update_view;
	entry->present = present;
	entry->userdata = userdata;

	cpset_unlock(&compositor->cbs);

	return 0;
}

int compositor_remove_view_callbacks(struct compositor *compositor, int64_t view_id) {
	struct view_cb_data *entry;

	cpset_lock(&compositor->cbs);

	entry = get_cbs_for_view_id_locked(compositor, view_id);
	if (entry == NULL) {
		return EINVAL;
	}

	cpset_remove_locked(&compositor->cbs, entry);

	free(entry);

	cpset_unlock(&compositor->cbs);

	return 0;
}


static void destroy_cursor_buffer(struct compositor *compositor) {
	struct drm_mode_destroy_dumb destroy_req;

	munmap(compositor->cursor.buffer, compositor->cursor.buffer_size);

	drmModeRmFB(compositor->drmdev->fd, compositor->cursor.drm_fb_id);

	memset(&destroy_req, 0, sizeof destroy_req);
	destroy_req.handle = compositor->cursor.gem_bo_handle;

	ioctl(compositor->drmdev->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_req);

	compositor->cursor.has_buffer = false;
	compositor->cursor.buffer_depth = 0;
	compositor->cursor.gem_bo_handle = 0;
	compositor->cursor.buffer_pitch = 0;
	compositor->cursor.buffer_width = 0;
	compositor->cursor.buffer_height = 0;
	compositor->cursor.buffer_size = 0;
	compositor->cursor.drm_fb_id = 0;
	compositor->cursor.buffer = NULL;
}

static int create_cursor_buffer(struct compositor *compositor, int width, int height, int bpp) {
	struct drm_mode_create_dumb create_req;
	struct drm_mode_map_dumb map_req;
	uint32_t drm_fb_id;
	uint32_t *buffer;
	uint64_t cap;
	uint8_t depth;
	int ok;

	ok = drmGetCap(compositor->drmdev->fd, DRM_CAP_DUMB_BUFFER, &cap);
	if (ok < 0) {
		ok = errno;
		perror("[compositor] Could not query GPU Driver support for dumb buffers. drmGetCap");
		goto fail_return_ok;
	}

	if (cap == 0) {
		fprintf(stderr, "[compositor] Kernel / GPU Driver does not support dumb DRM buffers. Mouse cursor will not be displayed.\n");
		ok = ENOTSUP;
		goto fail_return_ok;
	}

	ok = drmGetCap(compositor->drmdev->fd, DRM_CAP_DUMB_PREFERRED_DEPTH, &cap);
	if (ok < 0) {
		ok = errno;
		perror("[compositor] Could not query dumb buffer preferred depth capability. drmGetCap");
		goto fail_return_ok;
	}

	depth = (uint8_t) cap;

	if (depth != 32) {
		fprintf(stderr, "[compositor] Preferred framebuffer depth for hardware cursor is not supported by flutter-pi.\n");
	}

	memset(&create_req, 0, sizeof create_req);
	create_req.width = width;
	create_req.height = height;
	create_req.bpp = bpp;
	create_req.flags = 0;

	ok = ioctl(compositor->drmdev->fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_req);
	if (ok < 0) {
		ok = errno;
		perror("[compositor] Could not create a dumb buffer for the hardware cursor. ioctl");
		goto fail_return_ok;
	}

	ok = drmModeAddFB(compositor->drmdev->fd, create_req.width, create_req.height, 32, create_req.bpp, create_req.pitch, create_req.handle, &drm_fb_id);
	if (ok < 0) {
		ok = errno;
		perror("[compositor] Could not make a DRM FB out of the hardware cursor buffer. drmModeAddFB");
		goto fail_destroy_dumb_buffer;
	}

	memset(&map_req, 0, sizeof map_req);
	map_req.handle = create_req.handle;

	ok = ioctl(compositor->drmdev->fd, DRM_IOCTL_MODE_MAP_DUMB, &map_req);
	if (ok < 0) {
		ok = errno;
		perror("[compositor] Could not prepare dumb buffer mmap for uploading the hardware cursor icon. ioctl");
		goto fail_rm_drm_fb;
	}

	buffer = mmap(0, create_req.size, PROT_READ | PROT_WRITE, MAP_SHARED, compositor->drmdev->fd, map_req.offset);
	if (buffer == MAP_FAILED) {
		ok = errno;
		perror("[compositor] Could not mmap dumb buffer for uploading the hardware cursor icon. mmap");
		goto fail_rm_drm_fb;
	}

	compositor->cursor.has_buffer = true;
	compositor->cursor.buffer_depth = depth;
	compositor->cursor.gem_bo_handle = create_req.handle;
	compositor->cursor.buffer_pitch = create_req.pitch;
	compositor->cursor.buffer_width = width;
	compositor->cursor.buffer_height = height;
	compositor->cursor.buffer_size = create_req.size;
	compositor->cursor.drm_fb_id = drm_fb_id;
	compositor->cursor.buffer = buffer;
	
	return 0;


	fail_rm_drm_fb:
	drmModeRmFB(compositor->drmdev->fd, drm_fb_id);

	fail_destroy_dumb_buffer: ;
	struct drm_mode_destroy_dumb destroy_req;
	memset(&destroy_req, 0, sizeof destroy_req);
	destroy_req.handle = create_req.handle;
	ioctl(compositor->drmdev->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_req);

	fail_return_ok:
	return ok;
}

int compositor_apply_cursor_state(
	struct compositor *compositor,
	bool is_enabled,
	int rotation,
	double device_pixel_ratio
) {
	const struct cursor_icon *cursor;
	int ok;

	if (is_enabled == true) {
		// find the best fitting cursor icon.
		{
			double last_diff = INFINITY;

			cursor = NULL;
			for (int i = 0; i < n_cursors; i++) {
				double cursor_dpr = (cursors[i].width * 3 * 10.0) / (25.4 * 38);
				double cursor_screen_dpr_diff = device_pixel_ratio - cursor_dpr;
				if ((cursor_screen_dpr_diff >= 0) && (cursor_screen_dpr_diff < last_diff)) {
					cursor = cursors + i;
				}
			}
		}

		// destroy the old cursor buffer, if necessary
		if (compositor->cursor.has_buffer && (compositor->cursor.buffer_width != cursor->width)) {
			destroy_cursor_buffer(compositor);
		}

		// create a new cursor buffer, if necessary
		if (compositor->cursor.has_buffer == false) {
			ok = create_cursor_buffer(compositor, cursor->width, cursor->width, 32);
			if (ok != 0) {
				LOG_COMPOSITOR_ERROR("Couldn't create new mouse cursor buffer. create_cursor_buffer: %s", strerror(ok));
				
				// disable HW cursor
				compositor->cursor.cursor_size = 0;
				compositor->cursor.current_cursor = NULL;
				compositor->cursor.current_rotation = 0;
				compositor->cursor.hot_x = 0;
				compositor->cursor.hot_y = 0;
				compositor->cursor.x = 0;
				compositor->cursor.y = 0;
				compositor->cursor.is_enabled = false;

				return ok;
			}
		}

		if ((compositor->cursor.is_enabled == false) || (compositor->cursor.current_rotation != rotation) || (compositor->cursor.current_cursor != cursor)) {
			int rotated_hot_x, rotated_hot_y;
			
			if (rotation == 0) {
				memcpy(compositor->cursor.buffer, cursor->data, compositor->cursor.buffer_size);
				rotated_hot_x = cursor->hot_x;
				rotated_hot_y = cursor->hot_y;
			} else if ((rotation == 90) || (rotation == 180) || (rotation == 270)) {
				for (int y = 0; y < cursor->width; y++) {
					for (int x = 0; x < cursor->width; x++) {
						int buffer_x, buffer_y;
						if (rotation == 90) {
							buffer_x = cursor->width - y - 1;
							buffer_y = x;
						} else if (rotation == 180) {
							buffer_x = cursor->width - y - 1;
							buffer_y = cursor->width - x - 1;
						} else {
							buffer_x = y;
							buffer_y = cursor->width - x - 1;
						}
						
						int buffer_offset = compositor->cursor.buffer_pitch * buffer_y + (compositor->cursor.buffer_depth / 8) * buffer_x;
						int cursor_offset = cursor->width * y + x;
						
						compositor->cursor.buffer[buffer_offset / 4] = cursor->data[cursor_offset];
					}
				}

				if (rotation == 90) {
					rotated_hot_x = cursor->width - cursor->hot_y - 1;
					rotated_hot_y = cursor->hot_x;
				} else if (rotation == 180) {
					rotated_hot_x = cursor->width - cursor->hot_x - 1;
					rotated_hot_y = cursor->width - cursor->hot_y - 1;
				} else if (rotation == 270) {
					rotated_hot_x = cursor->hot_y;
					rotated_hot_y = cursor->width - cursor->hot_x - 1;
				}
			} else {
				return EINVAL;
			}

			compositor->cursor.current_rotation = rotation;
			compositor->cursor.current_cursor = cursor;
			compositor->cursor.cursor_size = cursor->width;
			compositor->cursor.hot_x = rotated_hot_x;
			compositor->cursor.hot_y = rotated_hot_y;
			compositor->cursor.is_enabled = true;

			ok = drmModeSetCursor2(
				compositor->drmdev->fd,
				compositor->drmdev->selected_crtc->crtc->crtc_id,
				compositor->cursor.gem_bo_handle,
				compositor->cursor.cursor_size,
				compositor->cursor.cursor_size,
				rotated_hot_x,
				rotated_hot_y
			);
			if (ok < 0) {
				perror("[compositor] Could not set the mouse cursor buffer. drmModeSetCursor");
				return errno;
			}

			ok = drmModeMoveCursor(
				compositor->drmdev->fd,
				compositor->drmdev->selected_crtc->crtc->crtc_id,
				compositor->cursor.x - compositor->cursor.hot_x,
				compositor->cursor.y - compositor->cursor.hot_y
			);
			if (ok < 0) {
				perror("[compositor] Could not move cursor. drmModeMoveCursor");
				return errno;
			}
		}
		
		return 0;
	} else if ((is_enabled == false) && (compositor->cursor.is_enabled == true)) {
		drmModeSetCursor(
			compositor->drmdev->fd,
			compositor->drmdev->selected_crtc->crtc->crtc_id,
			0, 0, 0
		);

		destroy_cursor_buffer(compositor);

		compositor->cursor.cursor_size = 0;
		compositor->cursor.current_cursor = NULL;
		compositor->cursor.current_rotation = 0;
		compositor->cursor.hot_x = 0;
		compositor->cursor.hot_y = 0;
		compositor->cursor.x = 0;
		compositor->cursor.y = 0;
		compositor->cursor.is_enabled = false;
	}

	return 0;
}

int compositor_set_cursor_pos(struct compositor *compositor, int x, int y) {
	int ok;

	if (compositor->cursor.is_enabled == false) {
		return EINVAL;
	}

	ok = drmModeMoveCursor(compositor->drmdev->fd, compositor->drmdev->selected_crtc->crtc->crtc_id, x - compositor->cursor.hot_x, y - compositor->cursor.hot_y);
	if (ok < 0) {
		LOG_COMPOSITOR_ERROR("Could not move cursor. drmModeMoveCursor: %s", strerror(errno));
		return errno;
	}

	compositor->cursor.x = x;
	compositor->cursor.y = y;    

	return 0;
}

void compositor_fill_flutter_compositor(struct compositor *compositor, FlutterCompositor *flutter_compositor) {
	flutter_compositor->struct_size = sizeof(FlutterCompositor);
	flutter_compositor->create_backing_store_callback = on_create_backing_store;
	flutter_compositor->collect_backing_store_callback = on_collect_backing_store;
	flutter_compositor->present_layers_callback = on_present_layers;
	flutter_compositor->user_data = compositor;
}

