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
		drmModeRmFB(drmdev_get_fd(flutterpi.drm.drmdev), fb->fb_id);

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

	ok = drmModeAddFB2WithModifiers(drmdev_get_fd(flutterpi.drm.drmdev), width, height, format, handles, strides, offsets, modifiers, &fb->fb_id, flags);

	if (ok) {
		if (flags)
			LOG_DEBUG("drm_fb_get_from_bo: modifiers failed!\n");

		memcpy(handles, (uint32_t [4]){gbm_bo_get_handle(bo).u32,0,0,0}, 16);
		memcpy(strides, (uint32_t [4]){gbm_bo_get_stride(bo),0,0,0}, 16);
		memset(offsets, 0, 16);

		ok = drmModeAddFB2(drmdev_get_fd(flutterpi.drm.drmdev), width, height, format, handles, strides, offsets, &fb->fb_id, 0);
	}

	if (ok) {
		LOG_ERROR("drm_fb_get_from_bo: failed to create fb: %s\n", strerror(errno));
		free(fb);
		return 0;
	}

	gbm_bo_set_user_data(bo, fb, destroy_gbm_bo);

	return fb->fb_id;
}

static void rendertarget_gbm_destroy(struct rendertarget *target) {
	free(target);
}

struct rendertarget_release_data {
	struct gbm_surface *surface;
	struct gbm_bo *bo;
};

static void on_release_gbm_rendertarget_fb(void *userdata) {
	struct rendertarget_release_data *data;

	DEBUG_ASSERT_NOT_NULL(userdata);
	data = userdata;

	gbm_surface_release_buffer(data->surface, data->bo);
	free(data);
}

static int rendertarget_gbm_present(
	struct rendertarget *target,
	struct kms_req_builder *builder
) {
	struct rendertarget_release_data *release_data;
	struct rendertarget_gbm *gbm_target;
	struct gbm_bo *next_front_bo;
	uint32_t next_front_fb_id;
	int ok;

	gbm_target = &target->gbm;

	release_data = malloc(sizeof *release_data);
	if (release_data == NULL) {
		return ENOMEM;
	}

	next_front_bo = gbm_surface_lock_front_buffer(gbm_target->gbm_surface);
	if (next_front_bo == NULL) {
		LOG_ERROR("Couldn't lock front buffer.\n");
		ok = EIO;
		goto fail_free_release_data;
	}

	next_front_fb_id = gbm_bo_get_drm_fb_id(next_front_bo);

	ok = kms_req_builder_push_fb_layer(
		builder,
		&(const struct kms_fb_layer) {
			.drm_fb_id = next_front_fb_id,
			.format = flutterpi.gbm.format,
			.has_modifier = flutterpi.gbm.modifier != DRM_FORMAT_MOD_NONE,
			.modifier = flutterpi.gbm.modifier,
			.src_x = 0,
			.src_y = 0,
			.src_w = ((uint16_t) flutterpi.display.width) << 16,
			.src_h = ((uint16_t) flutterpi.display.height) << 16,
			.dst_x = 0,
			.dst_y = 0,
			.dst_w = flutterpi.display.width,
			.dst_h = flutterpi.display.height,
			.has_rotation = false,
			.rotation = PLANE_TRANSFORM_NONE,
			.has_in_fence_fd = false,
			.in_fence_fd = 0,
		},
		on_release_gbm_rendertarget_fb,
		NULL,
		release_data
	);
	if (ok != 0) {
		LOG_ERROR("Couldn't add fb layer.\n");
		goto fail_release_next_front_buffer;
	}

	return 0;

	fail_release_next_front_buffer:
	gbm_surface_release_buffer(gbm_target->gbm_surface, next_front_bo);

	fail_free_release_data:
	free(release_data);

	return ok;
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
	};

	*out = target;

	return 0;
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

	

	struct quad quad = transform_aa_rect(
		FLUTTER_TRANSFORM_AS_MAT3F(display_to_view_transform),
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
			quad = transform_quad(FLUTTER_TRANSFORM_AS_MAT3F(mutations[i]->transformation), quad);

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

static void on_scanout(struct drmdev *drmdev, uint64_t vblank_ns, void *userdata) {
	struct compositor *compositor;

	DEBUG_ASSERT_NOT_NULL(drmdev);
	DEBUG_ASSERT_NOT_NULL(userdata);
	compositor = userdata;

	(void) drmdev;
	(void) vblank_ns;
	(void) compositor;

	// disabled because vsync is broken
	/*
	on_pageflip_event(
		drmdev_get_fd(drmdev),
		0,
		vblank_ns / 1000000000ull,
		(vblank_ns % 1000000000ull) / 1000,
		NULL
	);
	*/
}

/// PRESENT FUNCS
static bool on_present_layers(
	const FlutterLayer **layers,
	size_t layers_count,
	void *userdata
) {
	struct kms_req_builder *builder;
	struct view_cb_data *cb_data;
	struct compositor *compositor;
	EGLBoolean egl_ok;
	int ok;

	// TODO: proper error handling

	compositor = userdata;

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

	builder = drmdev_create_request_builder(compositor->drmdev, flutterpi.drm.selected_crtc_id);
	if (builder == NULL) {
		LOG_ERROR("Could not create KMS request builder.\n");
		return EINVAL;
	}

	if (compositor->has_applied_modeset == false) {
		ok = kms_req_builder_set_mode(builder, flutterpi.drm.selected_mode);
		if (ok != 0) {
			LOG_ERROR("Could not apply videomode.\n");
			goto fail_destroy_builder;
		}

		ok = kms_req_builder_set_connector(builder, flutterpi.drm.selected_connector_id);
		if (ok != 0) {
			LOG_ERROR("Could not apply output connector.\n");
			goto fail_destroy_builder;
		}

		compositor->has_applied_modeset = true;
	}

	EGLDisplay stored_display = eglGetCurrentDisplay();
	EGLSurface stored_read_surface = eglGetCurrentSurface(EGL_READ);
	EGLSurface stored_write_surface = eglGetCurrentSurface(EGL_DRAW);
	EGLContext stored_context = eglGetCurrentContext();

	if (stored_display != flutterpi.egl.display ||
		stored_read_surface != flutterpi.egl.surface ||
		stored_write_surface != flutterpi.egl.surface ||
		stored_context == EGL_NO_CONTEXT
	) {
		egl_ok = eglMakeCurrent(flutterpi.egl.display, flutterpi.egl.surface, flutterpi.egl.surface, flutterpi.egl.root_context);
		if (egl_ok != EGL_TRUE) {
			LOG_ERROR("Could not make EGL context current.\n");
			goto fail_destroy_builder;
		}
	}

	egl_ok = eglSwapBuffers(flutterpi.egl.display, flutterpi.egl.surface);
	if (egl_ok != EGL_TRUE) {
		LOG_ERROR("Could not flush EGL rendering. eglSwapBuffers: 0x%08X\n", eglGetError());
		goto fail_restore_context;
	}

	cpset_lock(&compositor->cbs);

	for (int i = 0; i < layers_count; i++) {
		if (layers[i]->type == kFlutterLayerContentTypeBackingStore) {
			struct flutterpi_backing_store *store = layers[i]->backing_store->user_data;
			struct rendertarget *target = store->target;
			
			ok = target->present(target, builder);
			if (ok != 0) {
				LOG_ERROR("Could not present backing store. rendertarget->present: %s\n", strerror(ok));
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
					builder,
					&params,
					cb_data->userdata
				);
				if (ok != 0) {
					LOG_ERROR("Could not present platform view. platform_view->present: %s\n", strerror(ok));
				}
			}
		}
	}

	cpset_unlock(&compositor->cbs);

	if (stored_display != flutterpi.egl.display ||
		stored_read_surface != flutterpi.egl.surface ||
		stored_write_surface != flutterpi.egl.surface ||
		stored_context == EGL_NO_CONTEXT
	) {
		ok = eglMakeCurrent(stored_display, stored_read_surface, stored_write_surface, stored_context);
		if (ok != EGL_TRUE) {
			LOG_ERROR("Could not restore EGL context.\n");
			goto fail_destroy_builder;
		}
	}

	struct kms_req *req = kms_req_builder_build(builder);
	if (req == NULL) {
		LOG_ERROR("Could not build atomic request.\n");
		goto fail_destroy_builder;
	}

	kms_req_builder_unref(builder);
	builder = NULL;

	if (!compositor->do_blocking_atomic_commits) {
		ok = kms_req_commit_nonblocking(req, on_scanout, compositor, NULL);
		if (ok == EBUSY) {
			LOG_ERROR(
				"Non-blocking kms_req_commit_nonblocking failed with EBUSY.\n"
				"  Future commits will be executed blockingly.\n"
				"  This may have have an impact on performance.\n"
			);
			compositor->do_blocking_atomic_commits = true;
		} else if (ok != 0) {
			LOG_ERROR("Could not present frame. kms_req_commit_nonblocking: %s\n", strerror(ok));
			goto fail_unref_req;
		}
	}

	if (compositor->do_blocking_atomic_commits) {
		uint64_t vblank_ns = 0;

		ok = kms_req_commit_blocking(req, &vblank_ns);
		if (ok != 0) {
			LOG_ERROR("Could not present frame. kms_req_commit_blocking: %s\n", strerror(ok));
			goto fail_unref_req;
		}

		struct simulated_page_flip_event_data *data = malloc(sizeof(struct simulated_page_flip_event_data));
		if (data == NULL) {
			goto fail_unref_req;
		}

		data->sec = vblank_ns / 1000000000llu;
		data->usec = (vblank_ns % 1000000000llu) / 1000;

		flutterpi_post_platform_task(execute_simulate_page_flip_event, data);
	}

	kms_req_unref(req);
	req = NULL;
	return true;


	fail_unref_req:
	kms_req_unref(req);
	goto fail_return_false;

	fail_restore_context:
	if (stored_display != flutterpi.egl.display ||
		stored_read_surface != flutterpi.egl.surface ||
		stored_write_surface != flutterpi.egl.surface ||
		stored_context == EGL_NO_CONTEXT
	) {
		eglMakeCurrent(stored_display, stored_read_surface, stored_write_surface, stored_context);
	}

	fail_destroy_builder:
	kms_req_builder_destroy(builder);

	fail_return_false:
	return false;
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

	drmdev_rm_fb(compositor.drmdev, compositor.cursor.drm_fb_id);

	memset(&destroy_req, 0, sizeof destroy_req);
	destroy_req.handle = compositor.cursor.gem_bo_handle;

	ioctl(drmdev_get_fd(compositor.drmdev), DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_req);

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

	ok = drmGetCap(drmdev_get_fd(compositor.drmdev), DRM_CAP_DUMB_BUFFER, &cap);
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

	ok = drmGetCap(drmdev_get_fd(compositor.drmdev), DRM_CAP_DUMB_PREFERRED_DEPTH, &cap);
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

	ok = ioctl(drmdev_get_fd(compositor.drmdev), DRM_IOCTL_MODE_CREATE_DUMB, &create_req);
	if (ok < 0) {
		ok = errno;
		LOG_ERROR("Could not create a dumb buffer for the hardware cursor. ioctl: %s\n", strerror(errno));
		goto fail_return_ok;
	}

	ok = drmdev_add_fb(
		compositor.drmdev,
		create_req.width, create_req.height,
		kARGB8888_FpiPixelFormat,
		create_req.handle,
		create_req.pitch,
		0,
		false,
		0
	);
	if (ok < 0) {
		ok = errno;
		LOG_ERROR("Could not make a DRM FB out of the hardware cursor buffer. drmModeAddFB: %s\n", strerror(errno));
		goto fail_destroy_dumb_buffer;
	}

	drm_fb_id = ok,

	memset(&map_req, 0, sizeof map_req);
	map_req.handle = create_req.handle;

	ok = ioctl(drmdev_get_fd(compositor.drmdev), DRM_IOCTL_MODE_MAP_DUMB, &map_req);
	if (ok < 0) {
		ok = errno;
		LOG_ERROR("Could not prepare dumb buffer mmap for uploading the hardware cursor icon. ioctl: %s\n", strerror(errno));
		goto fail_rm_drm_fb;
	}

	buffer = mmap(0, create_req.size, PROT_READ | PROT_WRITE, MAP_SHARED, drmdev_get_fd(compositor.drmdev), map_req.offset);
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
	drmModeRmFB(drmdev_get_fd(compositor.drmdev), drm_fb_id);

	fail_destroy_dumb_buffer: ;
	struct drm_mode_destroy_dumb destroy_req;
	memset(&destroy_req, 0, sizeof destroy_req);
	destroy_req.handle = create_req.handle;
	ioctl(drmdev_get_fd(compositor.drmdev), DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_req);

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
				drmdev_get_fd(compositor.drmdev),
				flutterpi.drm.selected_crtc_id,
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
				drmdev_get_fd(compositor.drmdev),
				flutterpi.drm.selected_crtc_id,
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
			drmdev_get_fd(compositor.drmdev),
			flutterpi.drm.selected_crtc_id,
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

	ok = drmModeMoveCursor(drmdev_get_fd(compositor.drmdev), flutterpi.drm.selected_crtc_id, x - compositor.cursor.hot_x, y - compositor.cursor.hot_y);
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
