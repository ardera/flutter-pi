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
#include <renderer.h>

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

struct frame {
	compositor_frame_begin_callback_t callback;
	void *userdata;
};

struct compositor {
	struct display **displays;
	size_t n_displays;
	struct display *main_display;

#ifdef HAS_GBM
	struct gbm_device *gbm_device;
	struct gbm_surface *gbm_surface;
#endif

	FlutterRendererType renderer_type;
	struct renderer *renderer;

	/*
     * Flutter engine profiling functions 
     */
	struct flutter_tracing_interface tracing;

    /**
     * @brief Contains a struct for each existing platform view, containing the view id
     * and platform view callbacks.
     * 
     * @see compositor_set_view_callbacks compositor_remove_view_callbacks
     */
    struct concurrent_pointer_set cbs;

	/**
     * @brief A cache of rendertargets that are not currently in use for
     * any flutter layers and can be reused.
     * 
     * Make sure to destroy all stale rendertargets before presentation so all the DRM planes
     * that are reserved by any stale rendertargets get freed.
     */
    struct concurrent_pointer_set stale_rendertargets;

	/**
	 * @brief Queue of requested frames that were requested by flutter using the vsync request callback.
	 * 
	 * Once a request becomes the peek of the queue (the first element), it is signalled. (A callback is called,
	 * and that callback will probably return the flutter vsync baton back to the engine using FlutterEngineOnVsync)
	 * The request is not dequeued however. The point at which the request is dequeued depends on whether triple buffering
	 * is enabled or not. If triple buffering is enabled, the request will be dequeued as soon as the last frame has finished
	 * rendering (Note: eglSwapBuffers does not finish rendering). If triple buffering is disabled, the request will be dequeued
	 * as soon as the previous frame has started to scan out.
	 * 
	 * When a request is dequeued, possibly a new frame request becomes the first element and is answered.
	 */
	struct concurrent_queue frame_queue;

    /**
     * @brief Whether the compositor should invoke @ref rendertarget_gbm_new the next time
     * flutter creates a backing store. Otherwise @ref rendertarget_nogbm_new is invoked.
     * 
     * It's only possible to have at most one GBM-Surface backed backing store (== @ref rendertarget_gbm). So the first
     * time @ref on_create_backing_store is invoked, a GBM-Surface backed backing store is returned and after that,
     * only backing stores with @ref rendertarget_nogbm.
     */
    bool should_create_window_surface_backing_store;

	bool use_triple_buffering;

	struct {
		struct kms_cursor *cursor;
		bool is_enabled;
		int rotation;
		double device_pixel_ratio;
		int x, y;
	} cursor;
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

/*
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
*/

/**
 * @brief Destroy all the rendertargets in the stale rendertarget cache.
 */
/*
static void destroy_stale_rendertargets(
	struct compositor *compositor
) {
	struct gl_rendertarget *target;

	cpset_lock(&compositor->stale_rendertargets);

	for_each_pointer_in_cpset(&compositor->stale_rendertargets, target) {
		target->destroy(target);
		cpset_remove_locked(&compositor->stale_rendertargets, target);
		target = NULL;
	}

	cpset_unlock(&compositor->stale_rendertargets);
}
*/

static inline int cache_gl_rendertarget(struct compositor *compositor, struct gl_rendertarget *rendertarget) {
	return cpset_put(&compositor->stale_rendertargets, rendertarget);
}

static struct kms_cursor *load_cursor_image(
	struct kmsdev *dev,
	int rotation,
	double device_pixel_ratio
) {
	const struct cursor_icon *cursor;

	DEBUG_ASSERT(dev != NULL);
	DEBUG_ASSERT((rotation == 0) || (rotation == 90) || (rotation == 180) || (rotation == 270));

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

	if (rotation == 0) {
		return kmsdev_load_cursor(dev, cursor->width, cursor->height, GBM_FORMAT_ARGB8888, cursor->hot_x, cursor->hot_y, (const uint8_t*) cursor->data);
	} else if ((rotation == 90) || (rotation == 180) || (rotation == 270)) {
		uint32_t rotated_data[cursor->width * cursor->height];
		int rotated_hot_x, rotated_hot_y;

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
				
				int buffer_offset = cursor->width * buffer_y + buffer_x;
				int cursor_offset = cursor->width * y + x;
				
				rotated_data[buffer_offset] = cursor->data[cursor_offset];
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

		return kmsdev_load_cursor(dev, cursor->width, cursor->height, GBM_FORMAT_ARGB8888, rotated_hot_x, rotated_hot_y, (const uint8_t*) rotated_data);
	}

	return 0;
}


/*
static void destroy_gbm_bo(
	struct gbm_bo *bo,
	void *userdata
) {
	struct drm_fb *fb = userdata;

	(void) bo;

	DEBUG_ASSERT(fb != NULL);

	if (fb && fb->fb_id)
		kmsdev_destroy_fb(fb->dev, fb->fb_id);
	
	free(fb);
}
*/

/**
 * @brief Create a GL framebuffer object that is backed by a GEM BO (that can be scanned out)
 * and registered as a DRM framebuffer
 */
static int create_drm_fbo(
	struct renderer *renderer,
	struct display *display,
	int w, int h,
	struct drm_fbo *out
) {
	struct drm_fbo fbo;
	int ok;

	ok = kmsdev_add_fb_planar(
		kmsdev,
		w, h,
		DRM_FORMAT_ARGB8888,
		fbo.gem_handle,
		fbo.gem_stride,
		0,
		DRM_FORMAT_MOD_LINEAR,
		&fbo.drm_fb_id,
		0
	);
	if (ok != 0) {
		gl_renderer_destroy_scanout_fbo(renderer, fbo.egl_image, fbo.gl_rbo_id, fbo.gl_fbo_id);
		return ok;
	}

	*out = fbo;
	return 0;
}

static void destroy_drm_fbo(
	struct kmsdev *dev,
	struct renderer *renderer,
	struct drm_fbo *fbo
) {
	kmsdev_destroy_fb(dev, fbo->drm_fb_id);
	gl_renderer_destroy_scanout_fbo(renderer, fbo->egl_image, fbo->gl_rbo_id, fbo->gl_fbo_id);
}


static void rendertarget_gbm_destroy(struct gl_rendertarget *target) {
	free(target);
}

static void rendertarget_gbm_on_release_gbm_bo(struct gbm_bo *bo, void *userdata) {
	struct gbm_surface *surface = userdata;

	DEBUG_ASSERT(surface != NULL);

	gbm_surface_release_buffer(surface, bo);
}

static int rendertarget_gbm_present(
	struct gl_rendertarget *target,
	struct presenter *builder,
	int x, int y,
	int w, int h
) {
	struct gl_rendertarget_gbm *gbm_target;
	struct gbm_bo *bo;
	int ok;

	gbm_target = &target->gbm;

	bo = gbm_surface_lock_front_buffer(gbm_target->gbm_surface);
	if (bo == NULL) {
		ok = errno;
		LOG_COMPOSITOR_ERROR("Couldn't lock front buffer. gbm_surface_lock_front_buffer: %s\n", strerror(errno));
		return ok;
	}

	ok = presenter_push_gbm_bo_layer(
		builder,
		&(const struct gbm_bo_layer) {
			.bo = bo,
			.src_x = x << 16, .src_y = y << 16,
			.src_w = w << 16, .src_h = h << 16,
			.crtc_x = x, .crtc_y = y,
			.crtc_w = w, .crtc_h = h,
			.has_rotation = false,
			.on_release = rendertarget_gbm_on_release_gbm_bo,
			.userdata = gbm_target->gbm_surface
		}
	);
	if (ok != 0) {
		gbm_surface_release_buffer(gbm_target->gbm_surface, bo);
		return ok;
	}

	//gbm_target->current_front_bo = (struct gbm_bo *) bo;

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
	struct gl_rendertarget **out,
	struct gbm_surface *surface
) {
	struct gl_rendertarget *target;

	target = calloc(1, sizeof *target);
	if (target == NULL) {
		*out = NULL;
		return ENOMEM;
	}

	*target = (struct gl_rendertarget) {
		.is_gbm = true,
		.gbm = {
			.gbm_surface = surface,
			//.current_front_bo = NULL
		},
		.gl_fbo_id = 0,
		.destroy = rendertarget_gbm_destroy,
		.present = rendertarget_gbm_present
	};

	*out = target;

	return 0;
}

static void rendertarget_nogbm_destroy(struct gl_rendertarget *target) {
	DEBUG_ASSERT(eglGetCurrentDisplay() != NULL);
	DEBUG_ASSERT(eglGetCurrentContext() != NULL);

	destroy_drm_fbo(target->nogbm.kmsdev, target->nogbm.renderer, &target->nogbm.fbo);
	free(target);
}

static int rendertarget_nogbm_present(
	struct gl_rendertarget *target,
	struct presenter *presenter,
	int x, int y,
	int w, int h
) {
	return presenter_push_drm_fb_layer(
		presenter,
		&(const struct drm_fb_layer) {
			.fb_id = target->nogbm.fbo.drm_fb_id,
			.src_x = x << 16, .src_y = y << 16,
			.src_w = w << 16, .src_h = h << 16,
			.crtc_x = x, .crtc_y = y,
			.crtc_w = w, .crtc_h = h,
			.has_rotation = true,
			.rotation = DRM_MODE_REFLECT_Y,
			.on_release = NULL,
			.userdata = NULL
		}
	);
}

static void rendertarget_nogbm_on_destroy_display_buffer(struct display *display, const struct display_buffer_backend *backend, void *userdata) {

}

/**
 * @brief Create a type of rendertarget that is not backed by a GBM-Surface, used for rendering into DRM overlay planes.
 * 
 * @param[in] renderer The renderer to be used for creating the target.
 * @param[in] display The display to use for 
 * 
 * @see rendertarget_nogbm
 */
static struct gl_rendertarget *rendertarget_nogbm_new(
	struct renderer *renderer,
	struct display *display,
	int w, int h,
	enum pixfmt format
) {
	struct gl_rendertarget *target;
	struct display_buffer *buffer;
	int ok;

	DEBUG_ASSERT(format == kARGB8888);

	target = malloc(sizeof *target);
	if (target == NULL) {
		goto fail_return_null;
	}

	ok = gl_renderer_create_scanout_fbo(
		renderer,
		w, h,
		&target->nogbm.egl_image,
		&target->nogbm.gem_handle,
		&target->nogbm.gem_stride,
		&target->nogbm.gl_rbo_id,
		&target->nogbm.gl_fbo_id
	);
	if (ok != 0) {
		goto fail_free_target;
	}

	buffer = display_import_buffer(
		display,
		&(struct display_buffer_backend) {
			.type = kDisplayBufferTypeGemBo,
			.gem_bo = {
				.gem_bo_handle = target->nogbm.gem_handle,
				.stride = target->nogbm.gem_stride,
				.width = w,
				.height = h,
				.format = format
			}
		},
		rendertarget_nogbm_on_destroy_display_buffer,
		target
	);
	if (buffer == NULL) {
		goto fail_destroy_scanout_fbo;
	}

	
	target->is_gbm = false;
	target->nogbm.buffer = buffer;
	target->nogbm.renderer = renderer;
	target->gl_fbo_id = target->nogbm.gl_fbo_id;
	target->destroy = rendertarget_nogbm_destroy;
	target->present = rendertarget_nogbm_present;
	return target;

	
	fail_destroy_scanout_fbo:
	gl_renderer_destroy_scanout_fbo(renderer, target->nogbm.egl_image, target->nogbm.gl_rbo_id, target->nogbm.gl_fbo_id);

	fail_free_target:
	free(target);

	fail_return_null:
	return NULL;
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
	struct gl_rendertarget *target;
	struct compositor *compositor;
	
	store = userdata;
	target = store->target;
	compositor = target->compositor;

	DEBUG_ASSERT(store != NULL);
	DEBUG_ASSERT(target != NULL);
	DEBUG_ASSERT(compositor != NULL);

	// cache this rendertarget so the compositor can reuse it later
	cache_gl_rendertarget(compositor, target);

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
static bool on_collect_backing_store(const FlutterBackingStore *backing_store, void *userdata) {
	struct flutterpi_backing_store *store;
	struct gl_rendertarget *target;
	struct compositor *compositor;

	(void) userdata;
	
	store = backing_store->user_data;
	target = store->target;
	compositor = target->compositor;

	DEBUG_ASSERT(store != NULL);
	DEBUG_ASSERT(target != NULL);
	DEBUG_ASSERT(compositor != NULL);

	// cache this rendertarget so the compositor can reuse it later
	cache_gl_rendertarget(compositor, target);

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
	struct gl_rendertarget *target;
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
				compositor->gbm_surface
			);
			if (ok != 0) {
				free(store);
				return false;
			}

			compositor->should_create_window_surface_backing_store = false;
		} else {
			ok = rendertarget_nogbm_new(
				&target,
				compositor->kmsdev,
				compositor->renderer,
				round(config->size.width),
				round(config->size.height)
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

/**
 * @brief Create a renderer that is compatible with all these displays.
 * 
 * Will create a GBM/EGL-backend OpenGL ES renderer if:
 *   - all displays support presenting GBM BOs (currently only KMS, but fbdev can be modified to present GBM BOs as well)
 *     (including foreign GBM BOs (from another driver))
 *   - EGL supports GBM as a platform
 * 
 * If there's no display that has a GBM device, we'll instead try to open a render-only DRM device node
 * and use that as our GBM device. If that fails, fallback to software rendering.
 */
struct compositor *compositor_new(
	struct display **displays,
	size_t n_displays,
	struct libegl *libegl,
	struct egl_client_info *client_info,
	struct libgl *libgl,
	struct event_loop *evloop,
	const struct flutter_renderer_gl_interface *gl_interface,
	const struct flutter_renderer_sw_interface *sw_interface,
	const struct flutter_tracing_interface *tracing_interface,
) {
	struct compositor *compositor;
	struct gbm_device *gbm_device;
	struct renderer *renderer;
	uint32_t format, *p_supported_formats;
	size_t n_supported_formats;
	bool all_displays_support_gbm;
	int width, height;
	int ok;

	DEBUG_ASSERT(displays != NULL);
	DEBUG_ASSERT(n_displays > 0);
	DEBUG_ASSERT(libegl != NULL);
	DEBUG_ASSERT(client_info != NULL);
	DEBUG_ASSERT(libgl != NULL);
	DEBUG_ASSERT(gl_interface != NULL);
	DEBUG_ASSERT(sw_interface != NULL);
	DEBUG_ASSERT(evloop != NULL);

	// first, create a renderer that supports all our video outputs.
	gbm_device = NULL;
	all_displays_support_gbm = true;
	for (unsigned int i = 0; i < n_displays; i++) {
		if (display_supports_gbm(displays[i]) && (gbm_device == NULL)) {
			gbm_device = display_get_gbm_device(displays[i]);
		}

		if (!display_supports_importing_buffer_type(displays[i], kDisplayBufferTypeGbmBo)) {
			all_displays_support_gbm = false;
			break;
		}
	}

	// try to open a render-only gbm device and use that as our renderer
	if (gbm_device == NULL) {
		/// TODO: Implement
		DEBUG_ASSERT(false && "unimplemented");
	}

	// we'll just say displays[0] is our main display and we'll use
	// that displays dimensions for the root GL surface
	display_get_size(displays[0], &width, &height);
	display_get_supported_formats(displays[0], &p_supported_formats, &n_supported_formats);

	compositor = malloc(sizeof *compositor);
	if (compositor == NULL) {
		goto fail_return_null;
	}

	ok = cpset_init(&compositor->cbs, CPSET_DEFAULT_MAX_SIZE);
	if (ok != 0) {
		goto fail_free_compositor;
	}

	ok = cpset_init(&compositor->stale_rendertargets, CPSET_DEFAULT_MAX_SIZE);
	if (ok != 0) {
		goto fail_deinit_cbs;
	}

	ok = cpset_init(&compositor->frame_queue, CPSET_DEFAULT_MAX_SIZE);
	if (ok != 0) {
		goto fail_deinit_stale_rendertargets;
	}

	renderer = NULL;
	if (all_displays_support_gbm && (gbm_device != NULL)) {
		renderer = gl_renderer_new(
			gbm_device,
			libegl,
			client_info,
			libgl,
			gl_interface,
			p_supported_formats[0],
			width, height
		);
	} else {
		renderer = sw_renderer_new(sw_interface);
	}

	if (renderer == NULL) {
		goto fail_deinit_frame_queue;
	}

	compositor->displays = displays;
	compositor->n_displays = n_displays;
	compositor->main_display = displays[0];
	compositor->renderer_type = renderer_type;
	compositor->renderer = renderer;
	if (tracing_interface == NULL) {
		memset(&compositor->tracing, 0, sizeof(struct flutter_tracing_interface));
	} else {
		compositor->tracing = *tracing_interface;
	}
	compositor->should_create_window_surface_backing_store = true;

	return compositor;


	fail_deinit_frame_queue:
	cpset_deinit(&compositor->frame_queue);

	fail_deinit_stale_rendertargets:
	cpset_deinit(&compositor->stale_rendertargets);

	fail_deinit_cbs:
	cpset_deinit(&compositor->cbs);

	fail_free_compositor:
	free(compositor);

	fail_return_null:
	return NULL;
}

/************************
 * COMPOSITOR INTERFACE *
 ************************/
struct compositor *compositor_new(
    struct display **displays,
	size_t n_displays,
	FlutterRendererType renderer_type,
    struct renderer *renderer,
    const struct flutter_tracing_interface *tracing_interface,
    struct event_loop *evloop
) {
	struct compositor *compositor;
	int ok;

	(void) evloop;

	DEBUG_ASSERT(displays != NULL);
	DEBUG_ASSERT(n_displays > 0);
	DEBUG_ASSERT((renderer_type == kOpenGL) && "only OpenGL rendering is supported right now.");
	DEBUG_ASSERT(renderer != NULL);
	DEBUG_ASSERT(evloop != NULL);

	compositor = malloc(sizeof *compositor);
	if (compositor == NULL) {
		goto fail_return_null;
	}

	ok = cpset_init(&compositor->cbs, CPSET_DEFAULT_MAX_SIZE);
	if (ok != 0) {
		goto fail_free_compositor;
	}

	ok = cpset_init(&compositor->stale_rendertargets, CPSET_DEFAULT_MAX_SIZE);
	if (ok != 0) {
		goto fail_deinit_cbs;
	}

	ok = cpset_init(&compositor->frame_queue, CPSET_DEFAULT_MAX_SIZE);
	if (ok != 0) {
		goto fail_deinit_stale_rendertargets;
	}

	compositor->renderer_type = renderer_type;
	compositor->renderer = renderer;
	if (tracing_interface == NULL) {
		memset(&compositor->tracing, 0, sizeof(struct flutter_tracing_interface));
	} else {
		compositor->tracing = *tracing_interface;
	}
	compositor->should_create_window_surface_backing_store = true;

	return compositor;


	fail_deinit_stale_rendertargets:
	cpset_deinit(&compositor->stale_rendertargets);

	fail_deinit_cbs:
	cpset_deinit(&compositor->cbs);

	fail_free_compositor:
	free(compositor);

	fail_return_null:
	return NULL;
}

struct compositor *compositor_new(
	struct display **displays,
	size_t n_displays,
	FlutterRendererType renderer_type,
	struct renderer *renderer,
	struct event_loop *evloop,
	const struct flutter_tracing_interface *tracing_interface,
) {
	struct compositor *compositor;
	int ok;

	DEBUG_ASSERT(displays != NULL);
	DEBUG_ASSERT(n_displays > 0);
	DEBUG_ASSERT(renderer != NULL);
	DEBUG_ASSERT(evloop != NULL);
	
	compositor = malloc(sizeof *compositor);
	if (compositor == NULL) {
		goto fail_return_null;
	}

	ok = cpset_init(&compositor->cbs, CPSET_DEFAULT_MAX_SIZE);
	if (ok != 0) {
		goto fail_free_compositor;
	}

	ok = cpset_init(&compositor->stale_rendertargets, CPSET_DEFAULT_MAX_SIZE);
	if (ok != 0) {
		goto fail_deinit_cbs;
	}

	compositor->

	return compositor;

	fail_deinit_cbs:
	cpset_deinit(&compositor->cbs);

	fail_free_compositor:
	free(compositor);

	fail_return_null:
	return NULL;
}

void compositor_destroy(struct compositor *compositor) {
	cpset_deinit(&compositor->stale_rendertargets);
	cpset_deinit(&compositor->cbs);
	free(compositor);
}

void compositor_set_tracing_interface(
    struct compositor *compositor,
    const struct flutter_tracing_interface *tracing_interface
) {
	DEBUG_ASSERT(compositor != NULL);
	if (tracing_interface == NULL) {
		memset(&compositor->tracing, 0, sizeof(struct flutter_tracing_interface));
	} else {
		compositor->tracing = *tracing_interface;
	}
}

static int respond_to_frame_request(struct compositor *compositor, uint64_t vblank_ns) {
	struct frame presented_frame, *peek;
	int ok;

	cqueue_lock(&compositor->frame_queue);
	
	ok = cqueue_try_dequeue_locked(&compositor->frame_queue, &presented_frame);
	if (ok != 0) {
		LOG_COMPOSITOR_ERROR("Could not dequeue completed frame from frame queue: %s\n", strerror(ok));
		goto fail_unlock_frame_queue;
	}

	ok = cqueue_peek_locked(&compositor->frame_queue, (void**) &peek);
	if (ok == EAGAIN) {
		// no frame queued after the one that was completed right now.
		// do nothing here.
	} else if (ok != 0) {
		LOG_COMPOSITOR_ERROR("Could not get frame queue peek. cqueue_peek_locked: %s\n", strerror(ok));
		goto fail_unlock_frame_queue;
	} else {
		presented_frame.callback(vblank_ns, vblank_ns + compositor->refresh_rate, presented_frame.userdata);
	}

	cqueue_unlock(&compositor->frame_queue);

	return 0;

	fail_unlock_frame_queue:
	cqueue_unlock(&compositor->frame_queue);

	return ok;
}

static int signal_presenting_complete(struct compositor *compositor) {
	if (compositor->use_triple_buffering) {
		return respond_to_frame_request(compositor, get_monotonic_time());
	}
	return 0;
}

static int signal_vblank(struct compositor *compositor, uint64_t vblank_nanos) {
	if (!compositor->use_triple_buffering) {
		return respond_to_frame_request(compositor, vblank_nanos);
	}
	return 0;
}


/*
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

	req_flags =  0 //DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK;

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
			struct gl_rendertarget *target = store->target;

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
*/

static void on_scanout(
	int crtc_index,
	unsigned int tv_sec,
	unsigned int tv_usec,
	void *userdata
) {
	struct compositor *compositor = userdata;
	(void) crtc_index;
	DEBUG_ASSERT(compositor != NULL);
	signal_vblank(compositor, tv_sec*1000000000ull + tv_usec*1000ull);
}

static bool on_present_layers_sw(
	const FlutterLayer **layers,
	size_t n_layers,
	void *userdata
) {
	struct compositor *compositor;
	struct presenter *presenter;
	int ok;

	DEBUG_ASSERT(userdata != NULL);

	compositor = userdata;

	presenter = display_create_presenter(compositor->main_display);
	if (presenter == NULL) {
		return false;
	}

	(void) layers;
	(void) n_layers;

	ok = presenter_flush(presenter);
	if (ok != 0) {
		presenter_destroy(presenter);
		return false;
	}

	presenter_destroy(presenter);

	ok = signal_presenting_complete(compositor);
	return ok == 0;
}

static bool on_present_layers_gl(
	const FlutterLayer **layers,
	size_t n_layers,
	void *userdata
) {
	struct compositor *compositor = userdata;
	struct presenter *presenter;
	EGLDisplay display;
	EGLSurface write_surface;
	EGLBoolean egl_ok;
	int ok;

	DEBUG_ASSERT(userdata != NULL);

	compositor = userdata;

	presenter = create_presenter(compositor);
	if (presenter == NULL) {
		goto fail_return_false;
	}

	display = eglGetCurrentDisplay();
	DEBUG_ASSERT(display != EGL_NO_DISPLAY);
	DEBUG_ASSERT(eglGetCurrentContext() != EGL_NO_CONTEXT);

	write_surface = eglGetCurrentSurface(EGL_DRAW);
	if (write_surface == EGL_NO_SURFACE) {
		LOG_COMPOSITOR_ERROR("Could get current write surface. eglGetCurrentSurface: %s\n", eglGetErrorString());
		goto fail_destroy_presenter;
	}

	egl_ok = eglSwapBuffers(display, write_surface);
	if (egl_ok != EGL_TRUE) {
		LOG_MODESETTING_ERROR("Couldn't flush rendering. eglSwapBuffers: %s\n", streglerr(eglGetError()));
		return false;
	}

	for (unsigned int i = 0; i < n_layers; i++) {
		if (layers[i]->type == kFlutterLayerContentTypeBackingStore) {
			struct flutterpi_backing_store *store = layers[i]->backing_store->user_data;
			struct gl_rendertarget *target = store->target;

			DEBUG_ASSERT(store != NULL);
			DEBUG_ASSERT(target != NULL);
			DEBUG_ASSERT(target->present != NULL);

			ok = target->present(
				target,
				presenter,
				round(layers[i]->offset.x), round(layers[i]->offset.y),
				round(layers[i]->size.width), round(layers[i]->size.height)
			);
			if (ok != 0) {
				goto fail_destroy_presenter;
			}
		} else {
			/// TODO: Implement
			DEBUG_ASSERT(false && "not implemented");
		}
	}

	presenter_set_scanout_callback(presenter, on_scanout, compositor);

	ok = presenter_flush(presenter);
	if (ok != 0) {
		goto fail_destroy_presenter;
	}

	presenter_destroy(presenter);

	signal_presenting_complete(compositor);

	return true;

	fail_destroy_presenter:
	presenter_destroy(presenter);

	fail_return_false:
	return false;
}

int compositor_request_frame(
	struct compositor *compositor,
	compositor_frame_begin_callback_t callback,
	void *userdata
) {
	struct frame *peek;
	int ok;

	DEBUG_ASSERT(compositor != NULL);
	DEBUG_ASSERT(callback != NULL);

	cqueue_lock(&compositor->frame_queue);

	ok = cqueue_peek_locked(&compositor->frame_queue, (void**) &peek);
	if ((ok == 0) || (ok == EAGAIN)) {
		bool reply_instantly = (ok == EAGAIN);

		ok = cqueue_try_enqueue_locked(&compositor->frame_queue, &(struct frame) {
			.callback = callback,
			.userdata = userdata
		});
		if (ok != 0) {
			LOG_COMPOSITOR_ERROR("Could not enqueue frame request. cqueue_try_enqueue_locked: %s\n", strerror(ok));
			goto fail_unlock_frame_queue;
		}

		if (reply_instantly) {	
			uint64_t current_time = get_monotonic_time();
			callback(current_time, current_time + compositor->refresh_rate, userdata);
		}
	} else if (ok != 0) {
		LOG_COMPOSITOR_ERROR("Could not get peek of frame queue. cqueue_peek_locked: %s\n", strerror(ok));
		goto fail_unlock_frame_queue;
	}

	cqueue_unlock(&compositor->frame_queue);
	return 0;
	
	fail_unlock_frame_queue:
	cqueue_unlock(&compositor->frame_queue);
	return ok;
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


int compositor_set_cursor_state(
	struct compositor *compositor,
	bool has_is_enabled,
	bool is_enabled,
	bool has_rotation,
	int rotation,
	bool has_device_pixel_ratio,
	double device_pixel_ratio
) {
	struct kms_cursor *new;
	bool should_be_enabled;
	int ok;

	DEBUG_ASSERT(compositor != NULL);
	DEBUG_ASSERT((rotation == 0) || (rotation == 90) || (rotation == 180) || (rotation == 270));
	
	should_be_enabled = has_is_enabled ? is_enabled : compositor->cursor.is_enabled;

	if (compositor->output_type != kGraphicsOutputTypeKmsdev) {
		LOG_COMPOSITOR_ERROR("Cursors are only supported for KMS right now.\n");
		return EOPNOTSUPP;
	}

	// if anything changed about the cursor specs, load it again
	if ((compositor->cursor.cursor == NULL) ||
		(has_rotation && (rotation != compositor->cursor.rotation)) ||
		(has_device_pixel_ratio && (device_pixel_ratio != compositor->cursor.device_pixel_ratio)))
	{
		new = load_cursor_image(compositor->kmsdev, rotation, device_pixel_ratio);
		if (new == NULL) {
			return EINVAL;
		}
	} else {
		new = NULL;
	}

	// update the cursor used by KMS, pass NULL as the cursor if it should be disabled.
	ok = kmsdev_set_cursor(compositor->kmsdev, compositor->crtc_index, should_be_enabled ? (new ?: compositor->cursor.cursor) : NULL);
	if (ok != 0) {
		if (new != NULL) kmsdev_dispose_cursor(compositor->kmsdev, new);
		return ok;
	}

	// optionally free the old cursor
	if (new != NULL) {
		if (compositor->cursor.cursor != NULL) {
			kmsdev_dispose_cursor(compositor->kmsdev, compositor->cursor.cursor);
		}
		compositor->cursor.cursor = new;
	}

	return 0;
}

int compositor_set_cursor_pos(struct compositor *compositor, int x, int y) {
	int ok;

	DEBUG_ASSERT(compositor->output_type == kGraphicsOutputTypeKmsdev);

	if (compositor->cursor.is_enabled) {
		ok = kmsdev_move_cursor(compositor->kmsdev, compositor->crtc_index, x, y);
		if (ok != 0) {
			return ok;
		}

		compositor->cursor.x = x;
		compositor->cursor.y = y;
	}

	return 0;
}

void compositor_fill_flutter_compositor(struct compositor *compositor, FlutterCompositor *flutter_compositor) {
	flutter_compositor->struct_size = sizeof(FlutterCompositor);
	flutter_compositor->create_backing_store_callback = on_create_backing_store;
	flutter_compositor->collect_backing_store_callback = on_collect_backing_store;
	if (compositor->renderer_type == kSoftware) {
		flutter_compositor->present_layers_callback = on_present_layers_sw;
	} else if (compositor->renderer_type == kOpenGL) {
		flutter_compositor->present_layers_callback = on_present_layers_gl;
	}
	flutter_compositor->user_data = compositor;
}

void compositor_fill_flutter_renderer_config(struct compositor *compositor, FlutterRendererConfig *config) {
	DEBUG_ASSERT(compositor != NULL);
	DEBUG_ASSERT(config != NULL);
	return renderer_fill_flutter_renderer_config(compositor->renderer, config);
}