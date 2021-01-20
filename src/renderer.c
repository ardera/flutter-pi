#define _GNU_SOURCE
#include <stdio.h>
#include <assert.h>
#include <dlfcn.h>

#include <drm_fourcc.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <flutter_embedder.h>
#include <flutter-pi.h>

#include <collection.h>
#include <dylib_deps.h>
#include <renderer.h>
#include <modesetting.h>

#ifdef DEBUG
#define DEBUG_ASSERT(r) assert(r)
#else
#define DEBUG_ASSERT(r) (void)
#endif

#define ASSERT_GL_RENDERER(r) DEBUG_ASSERT((r)->type == kOpenGL && "Expected renderer to be an OpenGL renderer.")
#define ASSERT_SW_RENDERER(r) DEBUG_ASSERT((r)->type == kSoftware && "Expected renderer to be a software renderer.")

struct fbdev;

struct renderer {
	FlutterRendererType type;

	/**
	 * @brief The drmdev for rendering (when using opengl renderer) and/or graphics output.
	 */
	struct drmdev *drmdev;

	/**
	 * @brief Optional fbdev for outputting the graphics there
	 * instead of the drmdev.
	 */
	struct fbdev *fbdev;

	union {
		struct {
			struct libegl *libegl;
			struct egl_client_info *client_info;
			const struct flutter_renderer_gl_interface *gl_interface;

			/**
			 * @brief The EGL display for this @ref drmdev.
			 */
			EGLDisplay egl_display;

			struct egl_display_info *display_info;

			char *gl_extensions_override;

			/**
			 * @brief The GBM Surface backing the @ref egl_surface.
			 * We need one regardless of whether we're outputting to @ref drmdev or @ref fbdev.
			 * A gbm_surface has no concept of on-screen or off-screen, it's on-screen
			 * when we decide to present its buffers via KMS and off-screen otherwise.
			 */
			struct gbm_surface *gbm_surface;

			/**
			 * @brief The framebuffer configuration used for creating
			 * EGL surfaces and contexts. Should have the pixel format ARGB8888.
			 */
			EGLConfig egl_config;

			/**
			 * @brief The root EGL surface. Can be single-buffered when we're outputting
			 * to @ref fbdev since we're copying the buffer after present anyway.
			 */
			EGLSurface egl_surface;

			/**
			 * @brief Set of EGL contexts created by this renderer. All EGL contexts are created
			 * the same so any context can be bound by any thread.
			 */
			struct concurrent_pointer_set egl_contexts;

			/**
			 * @brief Queue of EGL contexts available for use on any thread. Clients using this renderer
			 * can also reserve an EGL context so they access it directly, without the renderer
			 * having to lock/unlock this queue to find an unused context each time.
			 */
			struct concurrent_queue unused_egl_contexts;

			/**
			 * @brief Two reserved EGL contexts for flutter rendering and flutter resource uploading.
			 */
			EGLContext flutter_rendering_context, flutter_resource_context;
		} gl;
		struct {
			const struct flutter_renderer_sw_interface *sw_dispatcher;
		} sw;
	};
};

struct renderer *gl_renderer_new(
	struct drmdev *drmdev,
	struct libegl *libegl,
	struct egl_client_info *egl_client_info,
	const struct flutter_renderer_gl_interface *gl_interface,
	unsigned int width, unsigned int height
) {
	struct renderer *renderer;
	struct egl_display_info *egl_display_info;
	struct gbm_surface *gbm_surface;
	EGLDisplay egl_display;
	EGLSurface egl_surface;
	EGLContext root_context, rendering_context, resource_context;
	EGLBoolean egl_ok;
	EGLConfig egl_config;
	EGLint egl_error, n_matched, major, minor;
	int ok;

	renderer = malloc(sizeof *renderer);
	if (renderer == NULL) {
		goto fail_return_null;
	}

	ok = cpset_init(&renderer->gl.egl_contexts, CPSET_DEFAULT_MAX_SIZE);
	if (ok != 0) {
		goto fail_free_renderer;
	}

	ok = cqueue_init(&renderer->gl.unused_egl_contexts, sizeof(EGLContext), CQUEUE_DEFAULT_MAX_SIZE);
	if (ok != 0) {
		goto fail_deinit_contexts;
	}

	gbm_surface = gbm_surface_create_with_modifiers(
		drmdev->gbmdev,
		width,
		height,
		DRM_FORMAT_ARGB8888,
		(uint64_t[1]) {DRM_FORMAT_MOD_LINEAR},
		1
	);
	if (gbm_surface == NULL) {
		LOG_RENDERER_ERROR("Couldn't create GBM surface. gbm_surface_create_with_modifiers: %s\n", strerror(errno));
		goto fail_deinit_unused_contexts;
	}
	
	if (libegl->eglGetPlatformDisplay != NULL) {
		egl_display = libegl->eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, drmdev->gbmdev, NULL);
		if (egl_error = eglGetError(), egl_display == EGL_NO_DISPLAY || egl_error != EGL_SUCCESS) {
			LOG_FLUTTERPI_ERROR("Could not get EGL display! eglGetPlatformDisplay: 0x%08X\n", egl_error);
			ok = EIO;
			goto fail_destroy_gbm_surface;
		}
	} else if (egl_client_info->supports_ext_platform_base && egl_client_info->supports_khr_platform_gbm) {
		egl_display = libegl->eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR, drmdev->gbmdev, NULL);
		if (egl_error = eglGetError(), egl_display == EGL_NO_DISPLAY || egl_error != EGL_SUCCESS) {
			LOG_FLUTTERPI_ERROR("Could not get EGL display! eglGetPlatformDisplayEXT: 0x%08X\n", egl_error);
			ok = EIO;
			goto fail_destroy_gbm_surface;
		}
	} else {
		egl_display = eglGetDisplay((void*) drmdev->gbmdev);
		if (egl_error = eglGetError(), egl_display == EGL_NO_DISPLAY || egl_error != EGL_SUCCESS) {
			LOG_FLUTTERPI_ERROR("Could not get EGL display! eglGetDisplay: 0x%08X\n", egl_error);
			ok = EIO;
			goto fail_destroy_gbm_surface;
		}
	}

	egl_ok = eglInitialize(egl_display, &major, &minor);
	if (egl_error = eglGetError(), egl_ok == EGL_FALSE || egl_error != EGL_SUCCESS) {
		LOG_FLUTTERPI_ERROR("Could not initialize EGL display! eglInitialize: 0x%08X\n", egl_error);
		ok = EIO;
		goto fail_destroy_gbm_surface;
	}

	egl_display_info = egl_display_info_new(libegl, major, minor, egl_display);
	if (egl_display_info == NULL) {
		LOG_FLUTTERPI_ERROR("Could not create EGL display info!\n");
		ok = EIO;
		goto fail_terminate_display;
	}

	// We take the first config with ARGB8888. EGL orders all matching configs
	// so the top ones are most "desirable", so we should be fine
	// with just fetching the first config.
	egl_ok = eglChooseConfig(
		egl_display,
		(const EGLint[]) {
			EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
			EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
			EGL_NATIVE_VISUAL_ID, DRM_FORMAT_ARGB8888,
			EGL_NONE
		},
		&egl_config,
		1,
		&n_matched
	);
	if (egl_error = eglGetError(), egl_ok == EGL_FALSE || egl_error != EGL_SUCCESS) {
		LOG_FLUTTERPI_ERROR("Error finding a hardware accelerated EGL framebuffer configuration. eglChooseConfig: 0x%08X\n", egl_error);
		ok = EIO;
		goto fail_destroy_egl_display_info;
	}

	if (n_matched == 0) {
		LOG_FLUTTERPI_ERROR("Couldn't configure a hardware accelerated EGL framebuffer configuration.\n");
		ok = EIO;
		goto fail_destroy_egl_display_info;
	}

	egl_ok = eglBindAPI(EGL_OPENGL_ES_API);
	if (egl_error = eglGetError(), egl_ok == EGL_FALSE || egl_error != EGL_SUCCESS) {
		LOG_FLUTTERPI_ERROR("Failed to bind OpenGL ES API! eglBindAPI: 0x%08X\n", egl_error);
		ok = EIO;
		goto fail_destroy_egl_display_info;
	}
	
	root_context = eglCreateContext(
		egl_display,
		egl_config,
		EGL_NO_CONTEXT,
		(const EGLint[]) {
			EGL_CONTEXT_CLIENT_VERSION, 2,
			EGL_NONE
		}
	);
	if (egl_error = eglGetError(), root_context == EGL_NO_CONTEXT || egl_error != EGL_SUCCESS) {
		LOG_FLUTTERPI_ERROR("Could not create OpenGL ES context. eglCreateContext: 0x%08X\n", egl_error);
		ok = EIO;
		goto fail_destroy_egl_display_info;
	}

	egl_surface = eglCreateWindowSurface(egl_display, egl_config, (EGLNativeWindowType) gbm_surface, NULL);
	if (egl_error = eglGetError(), egl_surface == EGL_NO_SURFACE || egl_error != EGL_SUCCESS) {
		LOG_FLUTTERPI_ERROR("Could not create EGL window surface. eglCreateWindowSurface: 0x%08X\n", egl_error);
		ok = EIO;
		goto fail_destroy_egl_context;
	}

	cpset_put_locked(&renderer->gl.egl_contexts, &root_context);
	cqueue_enqueue_locked(&renderer->gl.unused_egl_contexts, &root_context);

	renderer->type = kOpenGL;
	renderer->drmdev = drmdev;
	renderer->fbdev = NULL;
	renderer->gl.libegl = libegl;
	renderer->gl.client_info = egl_client_info;
	renderer->gl.gl_interface = gl_interface;
	renderer->gl.egl_display = egl_display;
	renderer->gl.display_info = egl_display_info;
	renderer->gl.gl_extensions_override = NULL;
	renderer->gl.gbm_surface = gbm_surface;
	renderer->gl.egl_config = egl_config;
	renderer->gl.egl_surface = egl_surface;
	renderer->gl.flutter_rendering_context = NULL;
	renderer->gl.flutter_resource_context = NULL;

	if (renderer->gl.display_info->supports_14 == false) {
		LOG_RENDERER_ERROR("Flutter-pi requires EGL version 1.4 or newer, which is not supported by your system.\n");
		goto fail_free_renderer;
	}

	return renderer;


	fail_destroy_egl_context:
	eglDestroyContext(egl_display, root_context);

	fail_destroy_egl_display_info:
	egl_display_info_destroy(egl_display_info);

	fail_terminate_display:
	eglTerminate(egl_display);

	fail_destroy_gbm_surface:
	gbm_surface_destroy(gbm_surface);

	fail_deinit_unused_contexts:
	cqueue_deinit(&renderer->gl.unused_egl_contexts);

	fail_deinit_contexts:
	cpset_deinit(&renderer->gl.egl_contexts);

	fail_free_renderer:
	free(renderer);

	fail_return_null:
	return NULL;
}

/**
 * @brief Destroy this GL renderer.
 */
static int gl_renderer_destroy(struct renderer *renderer) {
	/// TODO: Implement gl_renderer_destroy

	return 0;
}

/**
 * @brief Create a new EGL context with a random EGL already constructed
 * context as the share context, the internally configured EGL config
 * and OpenGL ES 2 as the API.
 */
static EGLContext create_egl_context(struct renderer *renderer) {
	/// TODO: Create EGL Context
	EGLContext context;

	cpset_lock(&renderer->gl.egl_contexts);

	context = EGL_NO_CONTEXT;

	// get any EGL context
	for_each_pointer_in_cpset(&renderer->gl.egl_contexts, context)
		break;

	if ((context == EGL_NO_CONTEXT) || (context == NULL)) {
		cpset_unlock(&renderer->gl.egl_contexts);
		return EGL_NO_CONTEXT;
	}

	/// NOTE: This depends on the OpenGL ES API being bound with @ref eglBindAPI.
	context = eglCreateContext(
		renderer->gl.egl_display,
		renderer->gl.egl_config,
		context,
		(const EGLint [3]) {
			EGL_CONTEXT_CLIENT_VERSION, 2,
			EGL_NONE
		}
	);
	if (context == EGL_NO_CONTEXT) {
		cpset_unlock(&renderer->gl.egl_contexts);
		return EGL_NO_CONTEXT;
	}
	
	cpset_put_locked(&renderer->gl.egl_contexts, context);
	cpset_unlock(&renderer->gl.egl_contexts);

	return context;
}

/**
 * @brief Get an unused EGL context, removing it from the unused context queue.
 * If there are no unused contexts, a new one will be created.
 */
static EGLContext get_unused_egl_context(struct renderer *renderer) {
	struct renderer_egl_context *context;
	int ok;
	
	ok = cqueue_try_dequeue(renderer, &context);
	if (ok == EAGAIN) {
		return create_egl_context(renderer);
	} else {
		return NULL;
	}

	return context;
}

/**
 * @brief Put an EGL context into the unused context queue.
 */
static int put_unused_egl_context(struct renderer *renderer, EGLContext context) {
	return cqueue_enqueue(&renderer->gl.unused_egl_contexts, context);
}

__thread struct renderer *renderer_associated_with_current_egl_context = NULL;

int gl_renderer_make_current(struct renderer *renderer, bool surfaceless) {
	EGLContext context;
	EGLBoolean egl_ok;

	assert(renderer->type == kOpenGL);

	context = get_unused_egl_context(renderer);
	if (context == NULL) {
		return EINVAL;
	}

	egl_ok = eglMakeCurrent(
		renderer->gl.egl_display,
		surfaceless ? EGL_NO_SURFACE : renderer->gl.egl_surface,
		surfaceless ? EGL_NO_SURFACE : renderer->gl.egl_surface,
		context
	);
	if (egl_ok != EGL_TRUE) {
		LOG_RENDERER_ERROR("Could not make EGL context current.\n");
		return EINVAL;
	}

	renderer_associated_with_current_egl_context = renderer;

	return 0;
}

int gl_renderer_clear_current(struct renderer *renderer) {
	EGLContext context;
	EGLBoolean egl_ok;
	int ok;
	
	assert(renderer->type == kOpenGL);

	context = renderer->gl.libegl->eglGetCurrentContext();
	if (context == EGL_NO_CONTEXT) {
		LOG_RENDERER_ERROR("in gl_renderer_clear_current: No EGL context is current, so none can be cleared.\n");
		return EINVAL;
	}

	egl_ok = eglMakeCurrent(renderer->gl.egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	if (egl_ok != EGL_TRUE) {
		LOG_RENDERER_ERROR("Could not clear the current EGL context. eglMakeCurrent");
		return EIO;
	}

	renderer_associated_with_current_egl_context = NULL;

	ok = put_unused_egl_context(renderer, context);
	if (ok != 0) {
		LOG_RENDERER_ERROR("Could not mark the cleared EGL context as unused. put_unused_egl_context: %s\n", strerror(ok));
		return ok;
	}

	return 0;
}

EGLContext gl_renderer_reserve_context(struct renderer *renderer) {
	return get_unused_egl_context(renderer);
}

int gl_renderer_release_context(struct renderer *renderer, EGLContext context) {
	return put_unused_egl_context(renderer, context);
}

int gl_renderer_reserved_make_current(struct renderer *renderer, EGLContext context, bool surfaceless) {
	EGLBoolean egl_ok;

	assert(renderer->type == kOpenGL);

	egl_ok = eglMakeCurrent(
		renderer->gl.egl_display,
		surfaceless ? EGL_NO_SURFACE : renderer->gl.egl_surface,
		surfaceless ? EGL_NO_SURFACE : renderer->gl.egl_surface,
		context
	);
	if (egl_ok != EGL_TRUE) {
		LOG_RENDERER_ERROR("Could not make EGL context current.\n");
		return EINVAL;
	}

	return 0;
}

int gl_renderer_reserved_clear_current(struct renderer *renderer) {
	EGLContext context;
	EGLBoolean egl_ok;
	int ok;
	
	assert(renderer->type == kOpenGL);

	egl_ok = eglMakeCurrent(renderer->gl.egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	if (egl_ok != EGL_TRUE) {
		LOG_RENDERER_ERROR("Could not clear the current EGL context. eglMakeCurrent");
		return EIO;
	}

	return 0;
}


bool gl_renderer_flutter_make_renderering_context_current(struct renderer *renderer) {
	EGLContext context;
	int ok;

	context = renderer->gl.flutter_rendering_context;

	if (context == EGL_NO_CONTEXT) {
		context = get_unused_egl_context(renderer);
		if (context == EGL_NO_CONTEXT) {
			return EIO;
		}

		renderer->gl.flutter_rendering_context = context;
	}

	ok = gl_renderer_reserved_make_current(renderer, context, false);
	if (ok != 0) {
		put_unused_egl_context(renderer, context);
		return ok;
	}

	return 0;
}

bool gl_renderer_flutter_make_resource_context_current(struct renderer *renderer) {
	EGLContext context;
	int ok;

	context = renderer->gl.flutter_resource_context;

	if (context == EGL_NO_CONTEXT) {
		context = get_unused_egl_context(renderer);
		if (context == EGL_NO_CONTEXT) {
			return EIO;
		}

		renderer->gl.flutter_resource_context = context;
	}

	ok = gl_renderer_reserved_make_current(renderer, context, false);
	if (ok != 0) {
		put_unused_egl_context(renderer, context);
		return ok;
	}

	return 0;
}

bool gl_renderer_flutter_clear_current(struct renderer *renderer) {
	return gl_renderer_reserved_clear_current(renderer);
}

uint32_t gl_renderer_flutter_get_fbo(struct renderer *renderer) {
	return 0;
}

FlutterTransformation gl_renderer_flutter_get_surface_transformation(struct renderer *renderer) {
	/// TODO: Implement surface transformation
	return FLUTTER_ROTZ_TRANSFORMATION(0);
}

static const GLubyte *hacked_gl_get_string(GLenum name) {
	struct renderer *renderer = renderer_associated_with_current_egl_context;

	if (name == GL_EXTENSIONS) {
		return (GLubyte *) renderer->gl.gl_extensions_override;
	} else {
		return glGetString(name);
	}
}

void *gl_renderer_flutter_resolve_gl_proc(struct renderer *renderer, const char *name) {
	void *address;

	/*  
	 * The mesa V3D driver reports some OpenGL ES extensions as supported and working
	 * even though they aren't. hacked_glGetString is a workaround for this, which will
	 * cut out the non-working extensions from the list of supported extensions.
	 */

	if (name == NULL) {
		return NULL;
	} else if (renderer->gl.gl_extensions_override && (strcmp(name, "glGetString") == 0)) {
		return hacked_gl_get_string;
	} else {
		address = dlsym(RTLD_DEFAULT, name);
		if (address == NULL) {
			address = eglGetProcAddress(name);
		}

		if (address == NULL) {
			LOG_RENDERER_ERROR("Could not find the GL proc with name \"%s\".\n", name);
		}

		return address;
	}
}

uint32_t gl_renderer_flutter_get_fbo_with_info(struct renderer *renderer, const FlutterFrameInfo *info) {
	return 0;
}

bool gl_renderer_flutter_present_with_info(struct renderer *renderer, const FlutterPresentInfo *info) {
	return true;
}


struct renderer *sw_renderer_new(
	struct drmdev *drmdev,
	struct flutter_renderer_sw_interface *sw_dispatcher
) {
	struct renderer *renderer;

	renderer = malloc(sizeof *renderer);
	if (renderer == NULL) {
		return NULL;
	}

	renderer->type = kSoftware;

	return renderer;


	fail_free_renderer:
	free(renderer);

	fail_return_null:
	return NULL;
}

bool sw_renderer_present(
	struct renderer *renderer,
	void *allocation,
	size_t bytes_per_row,
	size_t height
) {
	return true;
}

static int sw_renderer_destroy(struct renderer *renderer) {
	return 0;
}

int renderer_fill_flutter_renderer_config(
	struct renderer *renderer,
	FlutterRendererConfig *config
) {
	struct flutter_renderer_gl_interface *gl_interface;

	gl_interface = renderer->gl.gl_interface;
	config->type = renderer->type;

	if (config->type == kOpenGL) {
		config->open_gl = (FlutterOpenGLRendererConfig) {
			.struct_size = sizeof(FlutterOpenGLRendererConfig),
			.make_current = gl_interface->make_current,
			.clear_current = gl_interface->clear_current,
			.make_resource_current = gl_interface->make_resource_current,
			.present = gl_interface->present,
			.fbo_callback = gl_interface->fbo_callback,
			.make_resource_current = gl_interface->make_resource_current,
			.fbo_reset_after_present = false,
			.surface_transformation = gl_interface->surface_transformation,
			.gl_proc_resolver = gl_interface->gl_proc_resolver,
			.gl_external_texture_frame_callback = gl_interface->gl_external_texture_frame_callback,
			.fbo_with_frame_info_callback = NULL,
			.present_with_info = NULL
		};
	} else if (config->type = kSoftware) {
		config->software = (FlutterSoftwareRendererConfig) {
			.struct_size = sizeof(FlutterSoftwareRendererConfig),
			.surface_present_callback = renderer->sw.sw_dispatcher->surface_present_callback
		};
	}
}

void renderer_destroy(struct renderer *renderer) {
	if (renderer->type == kOpenGL) {
		gl_renderer_destroy(renderer);
	} else {
		sw_renderer_destroy(renderer);
	}
	free(renderer);
}

#undef ASSERT_GL_RENDERER
#undef ASSERT_SW_RENDERER
