#define _GNU_SOURCE
#include <stdio.h>
#include <errno.h>
#include <sys/mman.h>
#include <math.h>

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

FILE_DESCR("compositor")

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

struct compositor compositor = {
	.drmdev = NULL,
	.cbs = CPSET_INITIALIZER(CPSET_DEFAULT_MAX_SIZE),
	.has_applied_modeset = false,
	.should_create_window_surface_backing_store = true,
	.stale_rendertargets = CPSET_INITIALIZER(CPSET_DEFAULT_MAX_SIZE),
	.do_blocking_atomic_commits = true
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

MAYBE_UNUSED static struct view_cb_data *get_cbs_for_view_id(int64_t view_id) {
	struct view_cb_data *data;

	cpset_lock(&compositor.cbs);
	data = get_cbs_for_view_id_locked(view_id);
	cpset_unlock(&compositor.cbs);

	return data;
}

/**
 * @brief Destroy all the rendertargets in the stale rendertarget cache.
 */
MAYBE_UNUSED static int destroy_stale_rendertargets(void) {
	struct rendertarget *target;

	cpset_lock(&compositor.stale_rendertargets);

	for_each_pointer_in_cpset(&compositor.stale_rendertargets, target) {
		target->destroy(target);
		target = NULL;
	}

	cpset_unlock(&compositor.stale_rendertargets);

	return 0;
}

static void destroy_gbm_bo(
	struct gbm_bo *bo,
	void *userdata
) {
	(void) bo;

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
			LOG_DEBUG("drm_fb_get_from_bo: modifiers failed!\n");

		memcpy(handles, (uint32_t [4]){gbm_bo_get_handle(bo).u32,0,0,0}, 16);
		memcpy(strides, (uint32_t [4]){gbm_bo_get_stride(bo),0,0,0}, 16);
		memset(offsets, 0, 16);

		ok = drmModeAddFB2(flutterpi.drm.drmdev->fd, width, height, format, handles, strides, offsets, &fb->fb_id, 0);
	}

	if (ok) {
		LOG_ERROR("drm_fb_get_from_bo: failed to create fb: %s\n", strerror(errno));
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
		LOG_ERROR("error creating DRM EGL Image for flutter backing store, eglCreateDRMImageMESA: %" PRId32 "\n", egl_error);
		return EINVAL;
	}

	flutterpi.egl.exportDRMImageMESA(flutterpi.egl.display, fbo.egl_image, NULL, (EGLint*) &fbo.gem_handle, (EGLint*) &fbo.gem_stride);
	if ((egl_error = eglGetError()) != EGL_SUCCESS) {
		LOG_ERROR("error getting handle & stride for DRM EGL Image, eglExportDRMImageMESA: %d\n", egl_error);
		return EINVAL;
	}

	glGenRenderbuffers(1, &fbo.gl_rbo_id);
	if ((gl_error = glGetError())) {
		LOG_ERROR("error generating renderbuffers for flutter backing store, glGenRenderbuffers: %u\n", gl_error);
		return EINVAL;
	}

	glBindRenderbuffer(GL_RENDERBUFFER, fbo.gl_rbo_id);
	if ((gl_error = glGetError())) {
		LOG_ERROR("error binding renderbuffer, glBindRenderbuffer: %d\n", gl_error);
		return EINVAL;
	}

	flutterpi.gl.EGLImageTargetRenderbufferStorageOES(GL_RENDERBUFFER, fbo.egl_image);
	if ((gl_error = glGetError())) {
		LOG_ERROR("error binding DRM EGL Image to renderbuffer, glEGLImageTargetRenderbufferStorageOES: %u\n", gl_error);
		return EINVAL;
	}

	/*
	glGenFramebuffers(1, &fbo.gl_fbo_id);
	if (gl_error = glGetError()) {
		LOG_ERROR("error generating FBOs for flutter backing store, glGenFramebuffers: %d\n", gl_error);
		return EINVAL;
	}

	glBindFramebuffer(GL_FRAMEBUFFER, fbo.gl_fbo_id);
	if (gl_error = glGetError()) {
		LOG_ERROR("error binding FBO for attaching the renderbuffer, glBindFramebuffer: %d\n", gl_error);
		return EINVAL;
	}

	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, fbo.gl_rbo_id);
	if (gl_error = glGetError()) {
		LOG_ERROR("error attaching renderbuffer to FBO, glFramebufferRenderbuffer: %d\n", gl_error);
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
		LOG_ERROR("Could not make DRM fb from EGL Image, drmModeAddFB2: %s", strerror(errno));
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
	GLenum gl_error;

	eglGetError();
	glGetError();

	glBindFramebuffer(GL_FRAMEBUFFER, fbo_id);
	if ((gl_error = glGetError())) {
		LOG_ERROR("error binding FBO for attaching the renderbuffer, glBindFramebuffer: %d\n", gl_error);
		return EINVAL;
	}

	glBindRenderbuffer(GL_RENDERBUFFER, rbo->gl_rbo_id);
	if ((gl_error = glGetError())) {
		LOG_ERROR("error binding renderbuffer, glBindRenderbuffer: %d\n", gl_error);
		return EINVAL;
	}

	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rbo->gl_rbo_id);
	if ((gl_error = glGetError())) {
		LOG_ERROR("error attaching renderbuffer to FBO, glFramebufferRenderbuffer: %d\n", gl_error);
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
	if ((gl_error = glGetError())) {
		LOG_ERROR("error destroying OpenGL RBO, glDeleteRenderbuffers: 0x%08X\n", gl_error);
	}

	ok = drmModeRmFB(flutterpi.drm.drmdev->fd, rbo->drm_fb_id);
	if (ok < 0) {
		LOG_ERROR("error removing DRM FB, drmModeRmFB: %s\n", strerror(errno));
	}

	eglDestroyImage(flutterpi.egl.display, rbo->egl_image);
	if (egl_error = eglGetError(), egl_error != EGL_SUCCESS) {
		LOG_ERROR("error destroying EGL image, eglDestroyImage: 0x%08X\n", egl_error);
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
	bool supported;
	int ok;

	(void)offset_x;
	(void)offset_y;
	(void)width;
	(void)height;

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

	ok = drmdev_plane_supports_setting_rotation_value(atomic_req->drmdev, drm_plane_id, DRM_MODE_ROTATE_0, &supported);
	if (ok != 0) return ok;

	if (supported) {
		drmdev_atomic_req_put_plane_property(atomic_req, drm_plane_id, "rotation", DRM_MODE_ROTATE_0);
	} else {
		static bool printed = false;

		if (!printed) {
			LOG_ERROR(
				"GPU does not support reflecting the screen in Y-direction.\n"
				"  This is required for rendering into hardware overlay planes though.\n"
				"  Any UI that is drawn in overlay planes will look upside down.\n"
			);
			printed = true;
		}
	}

	ok = drmdev_plane_supports_setting_zpos_value(atomic_req->drmdev, drm_plane_id, zpos, &supported);
	if (ok != 0) return ok;

	if (supported) {
		drmdev_atomic_req_put_plane_property(atomic_req, drm_plane_id, "zpos", zpos);
	} else {
		static bool printed = false;

		if (!printed) {
			LOG_ERROR(
				"GPU does not supported the desired HW plane order.\n"
				"  Some UI layers may be invisible.\n"
			);
			printed = true;
		}
	}

	// TODO: move this to the page flip handler.
	// We can only be sure the buffer can be released when the buffer swap
	// ocurred.
	if (gbm_target->current_front_bo != NULL) {
		gbm_surface_release_buffer(gbm_target->gbm_surface, gbm_target->current_front_bo);
	}
	gbm_target->current_front_bo = (struct gbm_bo *) next_front_bo;

	return 0;
}

static int rendertarget_gbm_present_legacy(
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
	struct rendertarget_gbm *gbm_target;
	struct gbm_bo *next_front_bo;
	uint32_t next_front_fb_id;
	bool is_primary;

	(void)offset_x;
	(void)offset_y;
	(void)width;
	(void)height;
	(void)zpos;

	gbm_target = &target->gbm;

	is_primary = drmdev_plane_get_type(drmdev, drm_plane_id) == DRM_PLANE_TYPE_PRIMARY;

	next_front_bo = gbm_surface_lock_front_buffer(gbm_target->gbm_surface);
	next_front_fb_id = gbm_bo_get_drm_fb_id(next_front_bo);

	if (is_primary) {
		if (set_mode) {
			drmdev_legacy_set_mode_and_fb(
				drmdev,
				next_front_fb_id
			);
		} else {
			/*drmdev_legacy_primary_plane_pageflip(
				drmdev,
				next_front_fb_id,
				NULL
			);*/

			drmdev_legacy_overlay_plane_pageflip(
				drmdev,
				drm_plane_id,
				next_front_fb_id,
				0,
				0,
				flutterpi.display.width,
				flutterpi.display.height,
				0,
				0,
				((uint16_t) flutterpi.display.width) << 16,
				((uint16_t) flutterpi.display.height) << 16
			);
		}
	} else {
		drmdev_legacy_overlay_plane_pageflip(
			drmdev,
			drm_plane_id,
			next_front_fb_id,
			0,
			0,
			flutterpi.display.width,
			flutterpi.display.height,
			0,
			0,
			((uint16_t) flutterpi.display.width) << 16,
			((uint16_t) flutterpi.display.height) << 16
		);
	}

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
		.present = rendertarget_gbm_present,
		.present_legacy = rendertarget_gbm_present_legacy
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

	(void)offset_x;
	(void)offset_y;
	(void)width;
	(void)height;

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

	ok = drmdev_plane_supports_setting_rotation_value(req->drmdev, drm_plane_id, DRM_MODE_ROTATE_0 | DRM_MODE_REFLECT_Y, &supported);
	if (ok != 0) return ok;

	if (supported) {
		drmdev_atomic_req_put_plane_property(req, drm_plane_id, "rotation", DRM_MODE_ROTATE_0 | DRM_MODE_REFLECT_Y);
	} else {
		static bool printed = false;

		if (!printed) {
			LOG_ERROR(
				"GPU does not support reflecting the screen in Y-direction.\n"
				"  This is required for rendering into hardware overlay planes though.\n"
				"  Any UI that is drawn in overlay planes will look upside down.\n"
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
			LOG_ERROR(
				"GPU does not supported the desired HW plane order.\n"
				"  Some UI layers may be invisible.\n"
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

	(void)offset_x;
	(void)offset_y;
	(void)width;
	(void)height;

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
			flutterpi.display.width,
			flutterpi.display.height,
			0,
			0,
			((uint16_t) flutterpi.display.width) << 16,
			((uint16_t) flutterpi.display.height) << 16
		);
	}

	ok = drmdev_plane_supports_setting_rotation_value(drmdev, drm_plane_id, DRM_MODE_ROTATE_0 | DRM_MODE_REFLECT_Y, &supported);
	if (ok != 0) return ok;

	if (supported) {
		drmdev_legacy_set_plane_property(drmdev, drm_plane_id, "rotation", DRM_MODE_ROTATE_0 | DRM_MODE_REFLECT_Y);
	} else {
		static bool printed = false;

		if (!printed) {
			LOG_ERROR(
				"GPU does not support reflecting the screen in Y-direction.\n"
				"  This is required for rendering into hardware overlay planes though.\n"
				"  Any UI that is drawn in overlay planes will look upside down.\n"
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
			LOG_ERROR(
				"GPU does not supported the desired HW plane order.\n"
				"  Some UI layers may be invisible.\n"
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
		LOG_ERROR("error generating FBOs for flutter backing store, glGenFramebuffers: %d\n", gl_error);
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

	(void) userdata;

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

	(void) config;
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

	// disabled because vsync is broken
	// on_pageflip_event(flutterpi.drm.drmdev->fd, 0, data->sec, data->usec, NULL);

	free(data);

	return 0;
}

static void fill_platform_view_params(
	struct platform_view_params *params_out,
	const FlutterPoint *offset,
	const FlutterSize *size,
	const FlutterPlatformViewMutation **mutations,
	size_t n_mutations,
	const FlutterTransformation *display_to_view_transform,
	const FlutterTransformation *view_to_display_transform,
	double device_pixel_ratio
) {
	(void) view_to_display_transform;

	/**
	 * inversion for
	 * ```
	 * const auto transformed_layer_bounds =
     *     root_surface_transformation_.mapRect(layer_bounds);
	 * ```
	 */

	struct quad quad = apply_transform_to_aa_rect(
		*display_to_view_transform,
		(struct aa_rect) {
			.offset.x = offset->x,
			.offset.y = offset->y,
			.size.x = size->width,
			.size.y = size->height
		}
	);

	struct aa_rect rect = get_aa_bounding_rect(quad);

	/**
	 * inversion for
	 * ```
	 * const auto layer_bounds =
     *     SkRect::MakeXYWH(params.finalBoundingRect().x(),
     *                      params.finalBoundingRect().y(),
     *                      params.sizePoints().width() * device_pixel_ratio_,
     *                      params.sizePoints().height() * device_pixel_ratio_
     *     );
	 * ```
	 */

	rect.size.x /= device_pixel_ratio;
	rect.size.y /= device_pixel_ratio;

	// okay, now we have the params.finalBoundingRect().x() in aa_back_transformed.x and
	// params.finalBoundingRect().y() in aa_back_transformed.y.
	// those are flutter view coordinates, so we still need to transform them to display coordinates.

	// However, there are also calculated as a side-product of calculating the size of the quadrangle.
	// So we'll avoid calculating them for now. Calculation of the size may fail when the offset
	// given to `SceneBuilder.addPlatformView` (https://api.flutter.dev/flutter/dart-ui/SceneBuilder/addPlatformView.html)
	// is not zero. (Don't really know what to do in that case)

	rect.offset.x = 0;
	rect.offset.y = 0;
	quad = get_quad(rect);

	double rotation = 0, opacity = 1;
	for (int i = n_mutations - 1; i >= 0; i--) {
        if (mutations[i]->type == kFlutterPlatformViewMutationTypeTransformation) {
			apply_transform_to_quad(mutations[i]->transformation, &quad);

			double rotz = atan2(mutations[i]->transformation.skewX, mutations[i]->transformation.scaleX) * 180.0 / M_PI;
            if (rotz < 0) {
                rotz += 360;
            }

            rotation += rotz;
        } else if (mutations[i]->type == kFlutterPlatformViewMutationTypeOpacity) {
			opacity *= mutations[i]->opacity;
		}
    }

	rotation = fmod(rotation, 360.0);

	params_out->rect = quad;
	params_out->opacity = 0;
	params_out->rotation = rotation;
	params_out->clip_rects = NULL;
	params_out->n_clip_rects = 0;
}

/// PRESENT FUNCS
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
	uint32_t req_flags;
	void *planes_storage[32] = {0};
	bool legacy_rendertarget_set_mode = false;
	bool schedule_fake_page_flip_event;
	bool use_atomic_modesetting;
	int ok;

	// TODO: proper error handling

	compositor = userdata;
	drmdev = compositor->drmdev;
	schedule_fake_page_flip_event = compositor->do_blocking_atomic_commits;
	use_atomic_modesetting = drmdev->supports_atomic_modesetting;

#ifdef DUMP_ENGINE_LAYERS
	LOG_DEBUG("layers:\n");
	for (int i = 0; i < layers_count; i++) {
		if (layers[i]->type == kFlutterLayerContentTypeBackingStore) {
			LOG_DEBUG("  backing store (offset: %f, %f. size: %f, %f)\n", layers[i]->offset.x, layers[i]->offset.y, layers[i]->size.width, layers[i]->size.height);
		} else {
			DEBUG_ASSERT(layers[i]->type == kFlutterLayerContentTypePlatformView);

			LOG_DEBUG("  platform view (id: %"PRId64", offset: %f, %f, size: %f, %f) mutations:\n", layers[i]->platform_view->identifier, layers[i]->offset.x, layers[i]->offset.y, layers[i]->size.width, layers[i]->size.height);
			for (size_t j = 0; j < layers[i]->platform_view->mutations_count; j++) {
				const FlutterPlatformViewMutation *mut = layers[i]->platform_view->mutations[j];
				switch (mut->type) {
					case kFlutterPlatformViewMutationTypeOpacity:
						LOG_DEBUG("    opacity %f\n", mut->opacity);
						break;
					case kFlutterPlatformViewMutationTypeClipRect:
						LOG_DEBUG("    clip rect (ltrb: %f, %f, %f, %f)\n", mut->clip_rect.left, mut->clip_rect.top, mut->clip_rect.right, mut->clip_rect.bottom);
						break;
					case kFlutterPlatformViewMutationTypeClipRoundedRect:
						LOG_DEBUG(
							"    clip rounded rect (ltrb: %f, %f, %f, %f, corner radii ul, ur, br, bl: %f, %f, %f, %f)\n",
							mut->clip_rounded_rect.rect.left, mut->clip_rounded_rect.rect.top, mut->clip_rounded_rect.rect.right, mut->clip_rounded_rect.rect.bottom,
							mut->clip_rounded_rect.upper_left_corner_radius,
							mut->clip_rounded_rect.upper_right_corner_radius,
							mut->clip_rounded_rect.lower_right_corner_radius,
							mut->clip_rounded_rect.lower_left_corner_radius
						);
						break;
					case kFlutterPlatformViewMutationTypeTransformation:
						LOG_DEBUG(
							"    transform (matrix: %f %f %f; %f %f %f; %f %f %f)\n",
							mut->transformation.scaleX,
							mut->transformation.skewX,
							mut->transformation.transX,
							mut->transformation.skewY,
							mut->transformation.scaleY,
							mut->transformation.transY,
							mut->transformation.pers0,
							mut->transformation.pers1,
							mut->transformation.pers2
						);
						break;
					default:
						DEBUG_ASSERT_MSG(0, "invalid platform view mutation type\n");
						break;
				}
			}
		}
	}
#endif

	req = NULL;
	if (use_atomic_modesetting) {
		ok = drmdev_new_atomic_req(compositor->drmdev, &req);
		if (ok != 0) {
			return false;
		}
	} else {
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

	EGLDisplay stored_display = eglGetCurrentDisplay();
	EGLSurface stored_read_surface = eglGetCurrentSurface(EGL_READ);
	EGLSurface stored_write_surface = eglGetCurrentSurface(EGL_DRAW);
	EGLContext stored_context = eglGetCurrentContext();

	eglMakeCurrent(flutterpi.egl.display, flutterpi.egl.surface, flutterpi.egl.surface, flutterpi.egl.root_context);
	eglSwapBuffers(flutterpi.egl.display, flutterpi.egl.surface);

	req_flags =  0 /* DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK*/;
	if (compositor->has_applied_modeset == false) {
		if (use_atomic_modesetting) {
			ok = drmdev_atomic_req_put_modeset_props(req, &req_flags);
			if (ok != 0) {
				return false;
			}
		} else {
			legacy_rendertarget_set_mode = true;
			schedule_fake_page_flip_event = true;
		}

		if (use_atomic_modesetting) {
			for_each_unreserved_plane_in_atomic_req(req, plane) {
				if (plane->type == DRM_PLANE_TYPE_CURSOR) {
					// make sure the cursor is in front of everything
					int64_t max_zpos;
					bool supported;

					ok = drmdev_plane_get_max_zpos_value(req->drmdev, plane->plane->plane_id, &max_zpos);
					if (ok != 0) {
						LOG_ERROR("Could not move cursor to front. Mouse cursor may be invisible. drmdev_plane_get_max_zpos_value: %s\n", strerror(ok));
						continue;
					}

					ok = drmdev_plane_supports_setting_zpos_value(req->drmdev, plane->plane->plane_id, max_zpos, &supported);
					if (ok != 0) {
						LOG_ERROR("Could not move cursor to front. Mouse cursor may be invisible. drmdev_plane_supports_setting_zpos_value: %s\n", strerror(ok));
						continue;
					}

					if (supported) {
						drmdev_atomic_req_put_plane_property(req, plane->plane->plane_id, "zpos", max_zpos);
					} else {
						LOG_ERROR("Could not move cursor to front. Mouse cursor may be invisible. drmdev_plane_supports_setting_zpos_value: %s\n", strerror(ok));
						continue;
					}
				}
			}
		} else {
			for_each_pointer_in_pset(&planes, plane) {
				if (plane->type == DRM_PLANE_TYPE_CURSOR) {
					// make sure the cursor is in front of everything
					int64_t max_zpos;
					bool supported;

					ok = drmdev_plane_get_max_zpos_value(drmdev, plane->plane->plane_id, &max_zpos);
					if (ok != 0) {
						LOG_ERROR("Could not move cursor to front. Mouse cursor may be invisible. drmdev_plane_get_max_zpos_value: %s\n", strerror(ok));
						continue;
					}

					ok = drmdev_plane_supports_setting_zpos_value(drmdev, plane->plane->plane_id, max_zpos, &supported);
					if (ok != 0) {
						LOG_ERROR("Could not move cursor to front. Mouse cursor may be invisible. drmdev_plane_supports_setting_zpos_value: %s\n", strerror(ok));
						continue;
					}

					if (supported) {
						drmdev_legacy_set_plane_property(drmdev, plane->plane->plane_id, "zpos", max_zpos);
					} else {
						LOG_ERROR("Could not move cursor to front. Mouse cursor may be invisible. drmdev_plane_supports_setting_zpos_value: %s\n", strerror(ok));
						continue;
					}
				}
			}
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
			const FlutterLayer *layer = NULL;
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

			DEBUG_ASSERT_NOT_NULL(layer);

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
					LOG_ERROR("Could not unmount platform view. unmount: %s\n", strerror(ok));
				}

				cb_data->was_present_last_frame = false;
			}
		}

		for_each_pointer_in_pset(&updated_views, cb_data) {
			const FlutterLayer *layer = NULL;
			int zpos = 0;

			for (int i = 0; i < layers_count; i++) {
				if (layers[i]->type == kFlutterLayerContentTypePlatformView &&
					layers[i]->platform_view->identifier == cb_data->view_id) {
					layer = layers[i];
					zpos = i;
					break;
				}
			}

			DEBUG_ASSERT_NOT_NULL(layer);

			struct platform_view_params params;
			fill_platform_view_params(
				&params,
				&layer->offset,
				&layer->size,
				layer->platform_view->mutations,
				layer->platform_view->mutations_count,
				&flutterpi.view.display_to_view_transform,
				&flutterpi.view.view_to_display_transform,
				flutterpi.display.pixel_ratio
			);

			ok = cb_data->update_view(
				cb_data->view_id,
				req,
				&params,
				zpos,
				cb_data->userdata
			);
			if (ok != 0) {
				LOG_ERROR("Could not update platform view. update_view: %s\n", strerror(ok));
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
			const FlutterLayer *layer = NULL;
			int zpos = 0;

			for (int i = 0; i < layers_count; i++) {
				if (layers[i]->type == kFlutterLayerContentTypePlatformView &&
					layers[i]->platform_view->identifier == cb_data->view_id) {
					layer = layers[i];
					zpos = i;
					break;
				}
			}

			DEBUG_ASSERT_NOT_NULL(layer);

			struct platform_view_params params;
			fill_platform_view_params(
				&params,
				&layer->offset,
				&layer->size,
				layer->platform_view->mutations,
				layer->platform_view->mutations_count,
				&flutterpi.view.display_to_view_transform,
				&flutterpi.view.view_to_display_transform,
				flutterpi.display.pixel_ratio
			);

			if (cb_data->mount != NULL) {
				ok = cb_data->mount(
					layer->platform_view->identifier,
					req,
					&params,
					zpos,
					cb_data->userdata
				);
				if (ok != 0) {
					LOG_ERROR("Could not mount platform view. %s\n", strerror(ok));
				}
			}

			cb_data->was_present_last_frame = true;
			cb_data->last_zpos = zpos;
			cb_data->last_size = layer->size;
			cb_data->last_offset = layer->offset;
			cb_data->last_num_mutations = layer->platform_view->mutations_count;
			for (int i = 0; i < layer->platform_view->mutations_count; i++) {
				memcpy(cb_data->last_mutations + i, layer->platform_view->mutations[i], sizeof(FlutterPlatformViewMutation));
			}
		}
	}

	int64_t min_zpos;
	if (use_atomic_modesetting) {
		for_each_unreserved_plane_in_atomic_req(req, plane) {
			if (plane->type == DRM_PLANE_TYPE_PRIMARY) {
				ok = drmdev_plane_get_min_zpos_value(req->drmdev, plane->plane->plane_id, &min_zpos);
				if (ok != 0) {
					min_zpos = 0;
				}
				break;
			}
		}
	} else {
		for_each_pointer_in_pset(&planes, plane) {
			if (plane->type == DRM_PLANE_TYPE_PRIMARY) {
				ok = drmdev_plane_get_min_zpos_value(drmdev, plane->plane->plane_id, &min_zpos);
				if (ok != 0) {
					min_zpos = 0;
				}
				break;
			}
		}
	}

	for (int i = 0; i < layers_count; i++) {
		if (layers[i]->type == kFlutterLayerContentTypeBackingStore) {
			if (use_atomic_modesetting) {
				for_each_unreserved_plane_in_atomic_req(req, plane) {
					// choose a plane which has an "intrinsic" zpos that matches
					// the zpos we want the plane to have.
					// (Since planes are buggy and we can't rely on the zpos we explicitly
					// configure the plane to have to be actually applied to the hardware.
					// In short, assigning a different value to the zpos property won't always
					// take effect.)
					if ((i == 0) && (plane->type == DRM_PLANE_TYPE_PRIMARY)) {
						ok = drmdev_atomic_req_reserve_plane(req, plane);
						break;
					} else if ((i != 0) && (plane->type == DRM_PLANE_TYPE_OVERLAY)) {
						ok = drmdev_atomic_req_reserve_plane(req, plane);
						break;
					}
				}
			} else {
				for_each_pointer_in_pset(&planes, plane) {
					if ((i == 0) && (plane->type == DRM_PLANE_TYPE_PRIMARY)) {
						break;
					} else if ((i != 0) && (plane->type == DRM_PLANE_TYPE_OVERLAY)) {
						break;
					}
				}
				if (plane != NULL) {
					pset_remove(&planes, plane);
				}
			}
			if (plane == NULL) {
				LOG_ERROR("Could not find a free primary/overlay DRM plane for presenting the backing store. drmdev_atomic_req_reserve_plane: %s\n", strerror(ok));
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
					LOG_ERROR("Could not present backing store. rendertarget->present: %s\n", strerror(ok));
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
			}
		} else {
			DEBUG_ASSERT(layers[i]->type == kFlutterLayerContentTypePlatformView);
			cb_data = get_cbs_for_view_id_locked(layers[i]->platform_view->identifier);

			if ((cb_data != NULL) && (cb_data->present != NULL)) {
				struct platform_view_params params;
				fill_platform_view_params(
					&params,
					&layers[i]->offset,
					&layers[i]->size,
					layers[i]->platform_view->mutations,
					layers[i]->platform_view->mutations_count,
					&flutterpi.view.display_to_view_transform,
					&flutterpi.view.view_to_display_transform,
					flutterpi.display.pixel_ratio
				);

				ok = cb_data->present(
					cb_data->view_id,
					req,
					&params,
					i + min_zpos,
					cb_data->userdata
				);
				if (ok != 0) {
					LOG_ERROR("Could not present platform view. platform_view->present: %s\n", strerror(ok));
				}
			}
		}
	}

	if (use_atomic_modesetting) {
		for_each_unreserved_plane_in_atomic_req(req, plane) {
			if ((plane->type == DRM_PLANE_TYPE_PRIMARY) || (plane->type == DRM_PLANE_TYPE_OVERLAY)) {
				drmdev_atomic_req_put_plane_property(req, plane->plane->plane_id, "FB_ID", 0);
				drmdev_atomic_req_put_plane_property(req, plane->plane->plane_id, "CRTC_ID", 0);
			}
		}
	}

	eglMakeCurrent(stored_display, stored_read_surface, stored_write_surface, stored_context);

	if (use_atomic_modesetting) {
		do_commit:
		if (compositor->do_blocking_atomic_commits) {
			req_flags &= ~(DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT);
		} else {
			req_flags |= DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT;
		}

		ok = drmdev_atomic_req_commit(req, req_flags, NULL);
		if ((compositor->do_blocking_atomic_commits == false) && (ok == EBUSY)) {
			LOG_ERROR(
				"Non-blocking drmModeAtomicCommit failed with EBUSY.\n"
				"  Future drmModeAtomicCommits will be executed blockingly.\n"
				"  This may have have an impact on performance.\n"
			);

			compositor->do_blocking_atomic_commits = true;
			schedule_fake_page_flip_event = true;
			goto do_commit;
		} else if (ok != 0) {
			LOG_ERROR("Could not present frame. drmModeAtomicCommit: %s\n", strerror(ok));
			drmdev_destroy_atomic_req(req);
			cpset_unlock(&compositor->cbs);
			return false;
		}

		drmdev_destroy_atomic_req(req);
	}

	cpset_unlock(&compositor->cbs);

	if (schedule_fake_page_flip_event) {
		uint64_t time = flutterpi.flutter.libflutter_engine.FlutterEngineGetCurrentTime();

		struct simulated_page_flip_event_data *data = malloc(sizeof(struct simulated_page_flip_event_data));
		if (data == NULL) {
			return false;
		}

		data->sec = time / 1000000000llu;
		data->usec = (time % 1000000000llu) / 1000;

		flutterpi_post_platform_task(execute_simulate_page_flip_event, data);
	}

	return true;
}

int compositor_on_page_flip(
	uint32_t sec,
	uint32_t usec
) {
	(void) sec;
	(void) usec;
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
		cpset_unlock(&compositor.cbs);
		return EINVAL;
	}

	cpset_remove_locked(&compositor.cbs, entry);
	free(entry);
	cpset_unlock(&compositor.cbs);
	return 0;
}

/// COMPOSITOR INITIALIZATION
int compositor_initialize(struct drmdev *drmdev) {
	compositor.drmdev = drmdev;
	return 0;
}

static void destroy_cursor_buffer(void) {
	struct drm_mode_destroy_dumb destroy_req;

	munmap(compositor.cursor.buffer, compositor.cursor.buffer_size);

	drmModeRmFB(compositor.drmdev->fd, compositor.cursor.drm_fb_id);

	memset(&destroy_req, 0, sizeof destroy_req);
	destroy_req.handle = compositor.cursor.gem_bo_handle;

	ioctl(compositor.drmdev->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_req);

	compositor.cursor.has_buffer = false;
	compositor.cursor.buffer_depth = 0;
	compositor.cursor.gem_bo_handle = 0;
	compositor.cursor.buffer_pitch = 0;
	compositor.cursor.buffer_width = 0;
	compositor.cursor.buffer_height = 0;
	compositor.cursor.buffer_size = 0;
	compositor.cursor.drm_fb_id = 0;
	compositor.cursor.buffer = NULL;
}

static int create_cursor_buffer(int width, int height, int bpp) {
	struct drm_mode_create_dumb create_req;
	struct drm_mode_map_dumb map_req;
	uint32_t drm_fb_id;
	uint32_t *buffer;
	uint64_t cap;
	uint8_t depth;
	int ok;

	ok = drmGetCap(compositor.drmdev->fd, DRM_CAP_DUMB_BUFFER, &cap);
	if (ok < 0) {
		ok = errno;
		LOG_ERROR("Could not query GPU Driver support for dumb buffers. drmGetCap: %s\n", strerror(errno));
		goto fail_return_ok;
	}

	if (cap == 0) {
		LOG_ERROR("Kernel / GPU Driver does not support dumb DRM buffers. Mouse cursor will not be displayed.\n");
		ok = ENOTSUP;
		goto fail_return_ok;
	}

	ok = drmGetCap(compositor.drmdev->fd, DRM_CAP_DUMB_PREFERRED_DEPTH, &cap);
	if (ok < 0) {
		ok = errno;
		LOG_ERROR("Could not query dumb buffer preferred depth capability. drmGetCap: %s\n", strerror(errno));
		goto fail_return_ok;
	}

	depth = (uint8_t) cap;

	if (depth != 32) {
		LOG_ERROR("Preferred framebuffer depth for hardware cursor is not supported by flutter-pi.\n");
	}

	memset(&create_req, 0, sizeof create_req);
	create_req.width = width;
	create_req.height = height;
	create_req.bpp = bpp;
	create_req.flags = 0;

	ok = ioctl(compositor.drmdev->fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_req);
	if (ok < 0) {
		ok = errno;
		LOG_ERROR("Could not create a dumb buffer for the hardware cursor. ioctl: %s\n", strerror(errno));
		goto fail_return_ok;
	}

	ok = drmModeAddFB(compositor.drmdev->fd, create_req.width, create_req.height, 32, create_req.bpp, create_req.pitch, create_req.handle, &drm_fb_id);
	if (ok < 0) {
		ok = errno;
		LOG_ERROR("Could not make a DRM FB out of the hardware cursor buffer. drmModeAddFB: %s\n", strerror(errno));
		goto fail_destroy_dumb_buffer;
	}

	memset(&map_req, 0, sizeof map_req);
	map_req.handle = create_req.handle;

	ok = ioctl(compositor.drmdev->fd, DRM_IOCTL_MODE_MAP_DUMB, &map_req);
	if (ok < 0) {
		ok = errno;
		LOG_ERROR("Could not prepare dumb buffer mmap for uploading the hardware cursor icon. ioctl: %s\n", strerror(errno));
		goto fail_rm_drm_fb;
	}

	buffer = mmap(0, create_req.size, PROT_READ | PROT_WRITE, MAP_SHARED, compositor.drmdev->fd, map_req.offset);
	if (buffer == MAP_FAILED) {
		ok = errno;
		LOG_ERROR("Could not mmap dumb buffer for uploading the hardware cursor icon. mmap: %s\n", strerror(errno));
		goto fail_rm_drm_fb;
	}

	compositor.cursor.has_buffer = true;
	compositor.cursor.buffer_depth = depth;
	compositor.cursor.gem_bo_handle = create_req.handle;
	compositor.cursor.buffer_pitch = create_req.pitch;
	compositor.cursor.buffer_width = width;
	compositor.cursor.buffer_height = height;
	compositor.cursor.buffer_size = create_req.size;
	compositor.cursor.drm_fb_id = drm_fb_id;
	compositor.cursor.buffer = buffer;

	return 0;


	fail_rm_drm_fb:
	drmModeRmFB(compositor.drmdev->fd, drm_fb_id);

	fail_destroy_dumb_buffer: ;
	struct drm_mode_destroy_dumb destroy_req;
	memset(&destroy_req, 0, sizeof destroy_req);
	destroy_req.handle = create_req.handle;
	ioctl(compositor.drmdev->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_req);

	fail_return_ok:
	return ok;
}

int compositor_apply_cursor_state(
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
		if (compositor.cursor.has_buffer && (compositor.cursor.buffer_width != cursor->width)) {
			destroy_cursor_buffer();
		}

		// create a new cursor buffer, if necessary
		if (compositor.cursor.has_buffer == false) {
			create_cursor_buffer(cursor->width, cursor->width, 32);
		}

		if ((compositor.cursor.is_enabled == false) || (compositor.cursor.current_rotation != rotation) || (compositor.cursor.current_cursor != cursor)) {
			int rotated_hot_x, rotated_hot_y;

			if (rotation == 0) {
				memcpy(compositor.cursor.buffer, cursor->data, compositor.cursor.buffer_size);
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

						int buffer_offset = compositor.cursor.buffer_pitch * buffer_y + (compositor.cursor.buffer_depth / 8) * buffer_x;
						int cursor_offset = cursor->width * y + x;

						compositor.cursor.buffer[buffer_offset / 4] = cursor->data[cursor_offset];
					}
				}

				if (rotation == 90) {
					rotated_hot_x = cursor->width - cursor->hot_y - 1;
					rotated_hot_y = cursor->hot_x;
				} else if (rotation == 180) {
					rotated_hot_x = cursor->width - cursor->hot_x - 1;
					rotated_hot_y = cursor->width - cursor->hot_y - 1;
				} else {
					DEBUG_ASSERT(rotation == 270);
					rotated_hot_x = cursor->hot_y;
					rotated_hot_y = cursor->width - cursor->hot_x - 1;
				}
			} else {
				return EINVAL;
			}

			ok = drmModeSetCursor2(
				compositor.drmdev->fd,
				compositor.drmdev->selected_crtc->crtc->crtc_id,
				compositor.cursor.gem_bo_handle,
				compositor.cursor.cursor_size,
				compositor.cursor.cursor_size,
				rotated_hot_x,
				rotated_hot_y
			);
			if (ok < 0) {
				if (errno == ENXIO) {
					LOG_ERROR("Could not configure cursor. Hardware cursor is not supported by device.\n");
				} else {
					LOG_ERROR("Could not set the mouse cursor buffer. drmModeSetCursor: %s\n", strerror(errno));
				}
				return errno;
			}

			ok = drmModeMoveCursor(
				compositor.drmdev->fd,
				compositor.drmdev->selected_crtc->crtc->crtc_id,
				compositor.cursor.x - compositor.cursor.hot_x,
				compositor.cursor.y - compositor.cursor.hot_y
			);
			if (ok < 0) {
				LOG_ERROR("Could not move cursor. drmModeMoveCursor: %s\n", strerror(errno));
				return errno;
			}

			compositor.cursor.current_rotation = rotation;
			compositor.cursor.current_cursor = cursor;
			compositor.cursor.cursor_size = cursor->width;
			compositor.cursor.hot_x = rotated_hot_x;
			compositor.cursor.hot_y = rotated_hot_y;
			compositor.cursor.is_enabled = true;
		}

		return 0;
	} else if ((is_enabled == false) && (compositor.cursor.is_enabled == true)) {
		drmModeSetCursor(
			compositor.drmdev->fd,
			compositor.drmdev->selected_crtc->crtc->crtc_id,
			0, 0, 0
		);

		destroy_cursor_buffer();

		compositor.cursor.cursor_size = 0;
		compositor.cursor.current_cursor = NULL;
		compositor.cursor.current_rotation = 0;
		compositor.cursor.hot_x = 0;
		compositor.cursor.hot_y = 0;
		compositor.cursor.x = 0;
		compositor.cursor.y = 0;
		compositor.cursor.is_enabled = false;

		return 0;
	}

	return 0;
}

int compositor_set_cursor_pos(int x, int y) {
	int ok;

	if (compositor.cursor.is_enabled == false) {
		return 0;
	}

	ok = drmModeMoveCursor(compositor.drmdev->fd, compositor.drmdev->selected_crtc->crtc->crtc_id, x - compositor.cursor.hot_x, y - compositor.cursor.hot_y);
	if (ok < 0) {
		LOG_ERROR("Could not move cursor. drmModeMoveCursor: %s", strerror(errno));
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
