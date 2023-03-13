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
	.cursor = NULL,
	.cursor_x = 0,
	.cursor_y = 0,
	.do_blocking_atomic_commits = true
};

enum cursor_size {
	k32x32_CursorSize,
	k48x48_CursorSize,
	k64x64_CursorSize,
	k96x96_CursorSize,
	k128x128_CursorSize,

	kMax_CursorSize = k128x128_CursorSize,
	kCount_CursorSize = kMax_CursorSize + 1
};

struct cursor_buffer {
	refcount_t n_refs;

	struct drmdev *drmdev;
	uint32_t gem_handle;
	int drm_fb_id;
	enum pixfmt format;
	int width, height;
	enum cursor_size size;
	int rotation;

	int hot_x, hot_y;
};

static const int pixel_size_for_cursor_size[] = {
	[k32x32_CursorSize] = 32,
	[k48x48_CursorSize] = 48,
	[k64x64_CursorSize] = 64,
	[k96x96_CursorSize] = 96,
	[k128x128_CursorSize] = 128
};

COMPILE_ASSERT(ARRAY_SIZE(pixel_size_for_cursor_size) == kCount_CursorSize);


static enum cursor_size cursor_size_from_pixel_ratio(double device_pixel_ratio) {
	double last_diff = INFINITY;
	enum cursor_size size;

	for (enum cursor_size size_iter = k32x32_CursorSize; size_iter < kCount_CursorSize; size_iter++) {
		double cursor_dpr = (pixel_size_for_cursor_size[size_iter] * 3 * 10.0) / (25.4 * 38);
		double cursor_screen_dpr_diff = device_pixel_ratio - cursor_dpr;
		if ((-last_diff < cursor_screen_dpr_diff) && (cursor_screen_dpr_diff < last_diff)) {
			size = size_iter;
			last_diff = cursor_screen_dpr_diff;
		}
	}

	return size;
}

static struct cursor_buffer *cursor_buffer_new(struct drmdev *drmdev, enum cursor_size size, int rotation) {
	const struct cursor_icon *icon;
	struct cursor_buffer *b;
	uint32_t gem_handle, pitch;
	uint32_t fb_id;
	size_t buffer_size;
	void *map_void;
	int pixel_size, hot_x, hot_y;
	int ok;

	DEBUG_ASSERT_NOT_NULL(drmdev);
	DEBUG_ASSERT(rotation == 0 || rotation == 1 || rotation == 2 || rotation == 3);

	if (!drmdev_supports_dumb_buffers(drmdev)) {
		LOG_ERROR("KMS doesn't support dumb buffers. Can't upload mouse cursor icon.\n");
		return NULL;
	}

	b = malloc(sizeof *b);
	if (b == NULL) {
		return NULL;
	}

	pixel_size = pixel_size_for_cursor_size[size];

	ok = drmdev_create_dumb_buffer(
		drmdev,
		pixel_size, pixel_size, 32,
		&gem_handle,
		&pitch,
		&buffer_size
	);
	if (ok != 0) {
		goto fail_free_b;
	}

	map_void = drmdev_map_dumb_buffer(
		drmdev,
		gem_handle,
		buffer_size
	);
	if (map_void == NULL) {
		goto fail_destroy_dumb_buffer;
	}
	
	icon = cursors + size;
	DEBUG_ASSERT_EQUALS(pixel_size, icon->width);
	DEBUG_ASSERT_EQUALS(pixel_size, icon->height);

	if (rotation == 0) {
		DEBUG_ASSERT_EQUALS(pixel_size * 4, pitch);
		memcpy(map_void, icon->data, buffer_size);
		hot_x = icon->hot_x;
		hot_y = icon->hot_y;
	} else if ((rotation == 1) || (rotation == 2) || (rotation == 3)) {
		uint32_t *map_uint32 = (uint32_t*) map_void;

		for (int y = 0; y < pixel_size; y++) {
			for (int x = 0; x < pixel_size; x++) {
				int buffer_x, buffer_y;
				if (rotation == 1) {
					buffer_x = pixel_size - y - 1;
					buffer_y = x;
				} else if (rotation == 2) {
					buffer_x = pixel_size - y - 1;
					buffer_y = pixel_size - x - 1;
				} else {
					buffer_x = y;
					buffer_y = pixel_size - x - 1;
				}

				int buffer_offset = pitch * buffer_y + 4 * buffer_x;
				int cursor_offset = pixel_size * y + x;

				map_uint32[buffer_offset / 4] = icon->data[cursor_offset];
			}
		}

		if (rotation == 1) {
			hot_x = pixel_size - icon->hot_y - 1;
			hot_y = icon->hot_x;
		} else if (rotation == 2) {
			hot_x = pixel_size - icon->hot_x - 1;
			hot_y = pixel_size - icon->hot_y - 1;
		} else {
			DEBUG_ASSERT(rotation == 3);
			hot_x = icon->hot_y;
			hot_y = pixel_size - icon->hot_x - 1;
		}
	}

	drmdev_unmap_dumb_buffer(drmdev, map_void, size);

	fb_id = drmdev_add_fb(
		drmdev,
		pixel_size,
		pixel_size,
		kARGB8888_FpiPixelFormat,
		gem_handle,
		pitch,
		0,
		true,
		DRM_FORMAT_MOD_LINEAR
	);
	if (fb_id == 0) {
		LOG_ERROR("Couldn't add mouse cursor buffer as KMS framebuffer.\n");
		goto fail_destroy_dumb_buffer;
	}

	b->n_refs = REFCOUNT_INIT_1;
	b->drmdev = drmdev_ref(drmdev);
	b->gem_handle = gem_handle;
	b->drm_fb_id = fb_id;
	b->format = kARGB8888_FpiPixelFormat;
	b->width = pixel_size;
	b->height = pixel_size;
	b->size = size;
	b->rotation = rotation;
	b->hot_x = hot_x;
	b->hot_y = hot_y;
	return b;

	fail_destroy_dumb_buffer:
	drmdev_destroy_dumb_buffer(drmdev, gem_handle);

	fail_free_b:
	free(b);
	return NULL;
}

static void cursor_buffer_destroy(struct cursor_buffer *buffer) {
	drmdev_rm_fb(buffer->drmdev, buffer->drm_fb_id);
	drmdev_destroy_dumb_buffer(buffer->drmdev, buffer->gem_handle);
	drmdev_unref(buffer->drmdev);
	free(buffer);
}

DEFINE_STATIC_REF_OPS(cursor_buffer, n_refs)

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

	release_data->surface = gbm_target->gbm_surface;
	release_data->bo = next_front_bo;

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
			.prefer_cursor = false,
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

MAYBE_UNUSED static int execute_simulate_page_flip_event(void *userdata) {
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

	// add cursor infos
	if (compositor->cursor != NULL) {
		ok = kms_req_builder_push_fb_layer(
			builder,
			&(const struct kms_fb_layer) {
				.drm_fb_id = compositor->cursor->drm_fb_id,
				.format = compositor->cursor->format,
				.has_modifier = true,
				.modifier = DRM_FORMAT_MOD_LINEAR,
				.src_x = 0,
				.src_y = 0,
				.src_w = ((uint16_t) compositor->cursor->width) << 16,
				.src_h = ((uint16_t) compositor->cursor->height) << 16,
				.dst_x = compositor->cursor_x - compositor->cursor->hot_x,
				.dst_y = compositor->cursor_y - compositor->cursor->hot_y,
				.dst_w = compositor->cursor->width,
				.dst_h = compositor->cursor->height,
				.has_rotation = false,
				.rotation = PLANE_TRANSFORM_NONE,
				.has_in_fence_fd = false,
				.in_fence_fd = 0,
				.prefer_cursor = true,
			},
			cursor_buffer_unref_void,
			NULL,
			compositor->cursor
		);
		if (ok != 0) {
			LOG_ERROR("Couldn't present cursor.\n");
		} else {
			cursor_buffer_ref(compositor->cursor);
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

		// Disabled because vsync callback is broken.
		// struct simulated_page_flip_event_data *data = malloc(sizeof(struct simulated_page_flip_event_data));
		// if (data == NULL) {
		//     goto fail_unref_req;
		// }
		// data->sec = vblank_ns / 1000000000llu;
		// data->usec = (vblank_ns % 1000000000llu) / 1000;
		// flutterpi_post_platform_task(execute_simulate_page_flip_event, data);
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

int compositor_apply_cursor_state(
	bool is_enabled,
	int rotation,
	double device_pixel_ratio
) {
	if (is_enabled == true) {
		// find the best fitting cursor icon.
		enum cursor_size size = cursor_size_from_pixel_ratio(device_pixel_ratio);

		if (compositor.cursor != NULL && (compositor.cursor->rotation != rotation || compositor.cursor->size != size)) {
			cursor_buffer_unref(compositor.cursor);
			compositor.cursor = NULL;
		}

		if (compositor.cursor == NULL) {
			compositor.cursor = cursor_buffer_new(compositor.drmdev, size, rotation);
		}
	} else if ((is_enabled == false) && (compositor.cursor != NULL)) {
		cursor_buffer_unref(compositor.cursor);
		compositor.cursor = NULL;
	}
	return 0;
}

int compositor_set_cursor_pos(int x, int y) {
	compositor.cursor_x = x;
	compositor.cursor_y = y;
	return 0;
}

const FlutterCompositor flutter_compositor = {
	.struct_size = sizeof(FlutterCompositor),
	.create_backing_store_callback = on_create_backing_store,
	.collect_backing_store_callback = on_collect_backing_store,
	.present_layers_callback = on_present_layers,
	.user_data = &compositor
};
