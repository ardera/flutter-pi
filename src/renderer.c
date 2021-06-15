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

#define LOG_RENDERER_ERROR(format_str, ...) fprintf(stderr, "[renderer] %s: " format_str, __func__, ##__VA_ARGS__)

#define DEBUG_ASSERT_GL_RENDERER(r) DEBUG_ASSERT((r)->type == kOpenGL && "Expected renderer to be an OpenGL renderer.")
#define DEBUG_ASSERT_SW_RENDERER(r) DEBUG_ASSERT((r)->type == kSoftware && "Expected renderer to be a software renderer.")

#define RENDERER_PRIVATE_GL(renderer) ((struct gl_renderer*) (renderer->private))
#define RENDERER_PRIVATE_SW(renderer) ((struct sw_renderer*) (renderer->private))


struct gl_renderer {
	struct libegl *libegl;
	struct egl_client_info *client_info;
	const struct flutter_renderer_gl_interface *gl_interface;
	struct gbm_surface *gbm_surface;

	EGLDisplay egl_display;
	struct egl_display_info *display_info;
	EGLConfig egl_config;
	EGLSurface egl_surface;

	struct concurrent_pointer_set egl_contexts;
	struct concurrent_queue unused_egl_contexts;

	EGLContext flutter_rendering_context, flutter_resource_context;
	
	struct libgl *libgl;
	char *gl_extensions_override;
};

struct sw_renderer {
	/**
	 * @brief The dispatcher functions. When the FlutterSoftwareRendererConfig is filled
	 * using @ref renderer_fill_flutter_renderer_config, these functions will be filled.
	 * The functions should then call the sw_renderer_flutter_* functions with the correct
	 * renderer instance.
	 * 
	 * This is needed because the both the global flutter config and the flutter renderer config
	 * callbacks share the userdata.
	 */
	const struct flutter_renderer_sw_interface *sw_dispatcher;
};

struct renderer_private;

struct renderer {
	FlutterRendererType type;
	struct renderer_private *private;

	void (*destroy)(struct renderer *renderer);
	void (*fill_flutter_renderer_config)(struct renderer *renderer, FlutterRendererConfig *config);
	int (*flush_rendering)(struct renderer *renderer);
};


void renderer_destroy(struct renderer *renderer) {
	DEBUG_ASSERT(renderer != NULL);
	DEBUG_ASSERT(renderer->destroy != NULL);
	return renderer->destroy(renderer);
}

/**
 * @brief Destroy this GL renderer.
 */
static void gl_renderer_destroy(struct renderer *renderer) {
	struct gl_renderer *private = RENDERER_PRIVATE_GL(renderer);
	
	/// TODO: Implement gl_renderer_destroy
	
	free(private);
	free(renderer);
}

static void sw_renderer_destroy(struct renderer *renderer) {
	struct sw_renderer *private = RENDERER_PRIVATE_SW(renderer);

	/// TODO: Implement sw_renderer_destroy

	free(private);
	free(renderer);
}



/**
 * @brief Create a new EGL context with a random EGL already constructed
 * context as the share context, the internally configured EGL config
 * and OpenGL ES 2 as the API.
 */
static EGLContext create_egl_context(struct gl_renderer *private) {
	/// TODO: Create EGL Context
	EGLContext context;

	cpset_lock(&private->egl_contexts);

	context = EGL_NO_CONTEXT;

	// get any EGL context
	for_each_pointer_in_cpset(&private->egl_contexts, context)
		break;

	if (context == EGL_NO_CONTEXT) {
		cpset_unlock(&private->egl_contexts);
		return EGL_NO_CONTEXT;
	}

	/// NOTE: This depends on the OpenGL ES API being bound with @ref eglBindAPI.
	context = eglCreateContext(
		private->egl_display,
		private->egl_config,
		context,
		(const EGLint [3]) {
			EGL_CONTEXT_CLIENT_VERSION, 2,
			EGL_NONE
		}
	);
	if (context == EGL_NO_CONTEXT) {
		LOG_RENDERER_ERROR("Could not create additional EGL context. eglCreateContext: %s\n", eglGetErrorString());
		cpset_unlock(&private->egl_contexts);
		return EGL_NO_CONTEXT;
	}
	
	cpset_put_locked(&private->egl_contexts, context);
	cpset_unlock(&private->egl_contexts);

	return context;
}

/**
 * @brief Get an unused EGL context, removing it from the unused context queue.
 * If there are no unused contexts, a new one will be created.
 */
static EGLContext get_unused_egl_context(struct gl_renderer *private) {
	struct renderer_egl_context *context;
	int ok;
	
	ok = cqueue_try_dequeue(&private->unused_egl_contexts, &context);
	if (ok == EAGAIN) {
		return create_egl_context(private);
	} else if (ok != 0) {
		return NULL;
	}

	return context;
}

/**
 * @brief Put an EGL context into the unused context queue.
 */
static int put_unused_egl_context(struct gl_renderer *private, EGLContext context) {
	return cqueue_enqueue(&private->unused_egl_contexts, context);
}

__thread struct renderer *renderer_associated_with_current_egl_context = NULL;

int gl_renderer_make_current(struct renderer *renderer, bool surfaceless) {
	struct gl_renderer *private;
	EGLContext context;
	EGLBoolean egl_ok;

	DEBUG_ASSERT_GL_RENDERER(renderer);
	private = RENDERER_PRIVATE_GL(renderer);

	context = get_unused_egl_context(private);
	if (context == NULL) {
		return EINVAL;
	}

	egl_ok = eglMakeCurrent(
		private->egl_display,
		surfaceless ? EGL_NO_SURFACE : private->egl_surface,
		surfaceless ? EGL_NO_SURFACE : private->egl_surface,
		context
	);
	if (egl_ok != EGL_TRUE) {
		LOG_RENDERER_ERROR("Could not make EGL context current. eglMakeCurrent: %s\n", eglGetErrorString());
		return EINVAL;
	}

	renderer_associated_with_current_egl_context = renderer;

	return 0;
}

int gl_renderer_clear_current(struct renderer *renderer) {
	struct gl_renderer *private;
	EGLContext context;
	EGLBoolean egl_ok;
	int ok;
	
	DEBUG_ASSERT_GL_RENDERER(renderer);
	private = RENDERER_PRIVATE_GL(renderer);

	context = private->libegl->eglGetCurrentContext();
	if (context == EGL_NO_CONTEXT) {
		LOG_RENDERER_ERROR("in gl_renderer_clear_current: No EGL context is current, so none can be cleared.\n");
		return EINVAL;
	}

	egl_ok = eglMakeCurrent(private->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	if (egl_ok != EGL_TRUE) {
		LOG_RENDERER_ERROR("Could not clear the current EGL context. eglMakeCurrent");
		return EIO;
	}

	renderer_associated_with_current_egl_context = NULL;

	ok = put_unused_egl_context(private, context);
	if (ok != 0) {
		LOG_RENDERER_ERROR("Could not mark the cleared EGL context as unused. put_unused_egl_context: %s\n", strerror(ok));
		return ok;
	}

	return 0;
}

EGLContext gl_renderer_reserve_context(struct renderer *renderer) {
	struct gl_renderer *private;

	DEBUG_ASSERT_GL_RENDERER(renderer);

	private = RENDERER_PRIVATE_GL(renderer);
	return get_unused_egl_context(private);
}

int gl_renderer_release_context(struct renderer *renderer, EGLContext context) {
	struct gl_renderer *private;

	DEBUG_ASSERT_GL_RENDERER(renderer);

	private = RENDERER_PRIVATE_GL(renderer);
	return put_unused_egl_context(private, context);
}

int gl_renderer_reserved_make_current(struct renderer *renderer, EGLContext context, bool surfaceless) {
	struct gl_renderer *private;
	EGLBoolean egl_ok;

	DEBUG_ASSERT_GL_RENDERER(renderer);

	private = RENDERER_PRIVATE_GL(renderer);

	egl_ok = eglMakeCurrent(
		private->egl_display,
		surfaceless ? EGL_NO_SURFACE : private->egl_surface,
		surfaceless ? EGL_NO_SURFACE : private->egl_surface,
		context
	);
	if (egl_ok != EGL_TRUE) {
		LOG_RENDERER_ERROR("Could not make EGL context current. eglMakeCurrent: %s\n", eglGetErrorString());
		return EINVAL;
	}

	return 0;
}

int gl_renderer_reserved_clear_current(struct renderer *renderer) {
	struct gl_renderer *private;
	EGLBoolean egl_ok;
	
	DEBUG_ASSERT_GL_RENDERER(renderer);

	private = RENDERER_PRIVATE_GL(renderer);

	egl_ok = eglMakeCurrent(private->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	if (egl_ok != EGL_TRUE) {
		LOG_RENDERER_ERROR("Could not clear the current EGL context. eglMakeCurrent: %s\n", eglGetErrorString());
		return EIO;
	}

	return 0;
}

int gl_renderer_create_scanout_fbo(
	struct renderer *renderer,
	int width, int height,
	EGLImage *image_out,
	uint32_t *handle_out,
	uint32_t *stride_out,
	GLuint *rbo_out,
	GLuint *fbo_out
) {
	struct gl_renderer *private;
	EGLBoolean egl_ok;
	EGLImage image;
	uint32_t handle, stride;
	GLenum gl_error;
	GLuint fbo, rbo;
	int ok;

	DEBUG_ASSERT_GL_RENDERER(renderer);
	DEBUG_ASSERT(image_out != NULL);
	DEBUG_ASSERT(handle_out != NULL);
	DEBUG_ASSERT(eglGetCurrentDisplay() != EGL_NO_DISPLAY);
	DEBUG_ASSERT(eglGetCurrentContext() != EGL_NO_CONTEXT);

	private = RENDERER_PRIVATE_GL(renderer);

	eglGetError();
	glGetError();

	if (!private->display_info->supports_mesa_drm_image) {
		LOG_RENDERER_ERROR("EGL doesn't support MESA_drm_image. Can't create DRM image backed OpenGL renderbuffer.\n");
		ok = ENOTSUP;
		goto fail_return_ok;
	}

	image = private->libegl->eglCreateDRMImageMESA(private->egl_display, (const EGLint[]) {
		EGL_WIDTH, width,
		EGL_HEIGHT, height,
		EGL_DRM_BUFFER_FORMAT_MESA, EGL_DRM_BUFFER_FORMAT_ARGB32_MESA,
		EGL_DRM_BUFFER_USE_MESA, EGL_DRM_BUFFER_USE_SCANOUT_MESA,
		EGL_NONE
	});
	if (image == EGL_NO_IMAGE) {
		LOG_RENDERER_ERROR("Error creating DRM EGL Image for flutter backing store. eglCreateDRMImageMESA: %s\n", eglGetErrorString());
		ok = EIO;
		goto fail_return_ok;
	}

	egl_ok = private->libegl->eglExportDRMImageMESA(private->egl_display, image, NULL, (EGLint*) &handle, (EGLint*) &stride);
	if (egl_ok == false) {
		LOG_RENDERER_ERROR("Error getting handle & stride for DRM EGL Image. eglExportDRMImageMESA: %s\n", eglGetErrorString());
		ok = EIO;
		goto fail_destroy_egl_image;
	}

	glGenRenderbuffers(1, &rbo);
	if ((gl_error = glGetError())) {
		LOG_RENDERER_ERROR("Error generating renderbuffers for flutter backing store. glGenRenderbuffers: %u\n", gl_error);
		ok = EIO;
		goto fail_destroy_egl_image;
	}

	glBindRenderbuffer(GL_RENDERBUFFER, rbo);
	if ((gl_error = glGetError())) {
		LOG_RENDERER_ERROR("Error binding renderbuffer. glBindRenderbuffer: %u\n", gl_error);
		ok = EIO;
		goto fail_delete_rbo;
	}

	/// FIXME: Use libgl here
	eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES");
	//flutterpi.gl.EGLImageTargetRenderbufferStorageOES(GL_RENDERBUFFER, fbo.egl_image);
	if ((gl_error = glGetError())) {
		LOG_RENDERER_ERROR("Error binding DRM EGL Image to renderbuffer, glEGLImageTargetRenderbufferStorageOES: %u\n", gl_error);
		ok = EIO;
		goto fail_delete_rbo;
	}

	glGenFramebuffers(1, &fbo);
	if ((gl_error = glGetError())) {
		LOG_RENDERER_ERROR("Error generating OpenGL framebuffer. glGenFramebuffers: %u\n", gl_error);
		ok = EIO;
		goto fail_delete_rbo;
	}

	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	if ((gl_error = glGetError())) {
		LOG_RENDERER_ERROR("Error binding OpenGL framebuffer. glBindFramebuffer: %u\n", gl_error);
		ok = EIO;
		goto fail_delete_fbo;
	}

	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rbo);
	if ((gl_error = glGetError())) {
		LOG_RENDERER_ERROR("Error attaching renderbuffer to framebuffer. glFramebufferRenderbuffer: %u\n", gl_error);
		ok = EIO;
		goto fail_delete_fbo;
	}

	*image_out = image;
	*handle_out = handle;
	*stride_out = stride;
	*rbo_out = rbo;
	*fbo_out = fbo;

	return 0;


	fail_delete_fbo:
	glDeleteFramebuffers(1, &fbo);

	fail_delete_rbo:
	glDeleteRenderbuffers(1, &rbo);

	fail_destroy_egl_image:
	private->libegl->eglDestroyImage(private->egl_display, image);

	fail_return_ok:
	return ok;
}

void gl_renderer_destroy_scanout_fbo(
	struct renderer *renderer,
	EGLImage image,
	GLuint rbo,
	GLuint fbo
) {
	struct gl_renderer *private;

	DEBUG_ASSERT_GL_RENDERER(renderer);
	DEBUG_ASSERT(image != EGL_NO_IMAGE);
	DEBUG_ASSERT(eglGetCurrentDisplay() != EGL_NO_DISPLAY);
	DEBUG_ASSERT(eglGetCurrentContext() != EGL_NO_CONTEXT);

	private = RENDERER_PRIVATE_GL(renderer);

	glDeleteFramebuffers(1, &fbo);
	glDeleteRenderbuffers(1, &rbo);
	private->libegl->eglDestroyImage(private->egl_display, image);
}

bool gl_renderer_flutter_make_rendering_context_current(struct renderer *renderer) {
	struct gl_renderer *private;
	EGLContext context;
	int ok;

	DEBUG_ASSERT_GL_RENDERER(renderer);
	private = RENDERER_PRIVATE_GL(renderer);
	context = private->flutter_rendering_context;

	if (context == EGL_NO_CONTEXT) {
		context = get_unused_egl_context(private);
		if (context == EGL_NO_CONTEXT) {
			return false;
		}

		private->flutter_rendering_context = context;
	}

	ok = gl_renderer_reserved_make_current(renderer, context, false);
	if (ok != 0) {
		put_unused_egl_context(private, context);
		return false;
	}

	return true;
}

bool gl_renderer_flutter_make_resource_context_current(struct renderer *renderer) {
	struct gl_renderer *private;
	EGLContext context;
	int ok;

	DEBUG_ASSERT_GL_RENDERER(renderer);
	private = RENDERER_PRIVATE_GL(renderer);
	context = private->flutter_resource_context;

	if (context == EGL_NO_CONTEXT) {
		context = get_unused_egl_context(private);
		if (context == EGL_NO_CONTEXT) {
			return false;
		}

		private->flutter_resource_context = context;
	}

	ok = gl_renderer_reserved_make_current(renderer, context, true);
	if (ok != 0) {
		put_unused_egl_context(private, context);
		return false;
	}

	return true;
}

bool gl_renderer_flutter_clear_current(struct renderer *renderer) {
	return gl_renderer_reserved_clear_current(renderer) == 0;
}

bool gl_renderer_flutter_present(struct renderer *renderer) {
	(void) renderer;

	printf("gl_renderer_flutter_present\n");

	return true;
}

uint32_t gl_renderer_flutter_get_fbo(struct renderer *renderer) {
	(void) renderer;
	DEBUG_ASSERT_GL_RENDERER(renderer);
	return 0;
}

FlutterTransformation gl_renderer_flutter_get_surface_transformation(struct renderer *renderer) {
	/// TODO: Implement surface transformation
	(void) renderer;
	return FLUTTER_ROTZ_TRANSFORMATION(0);
}

static const GLubyte *hacked_gl_get_string(GLenum name) {
	struct gl_renderer *private;
	struct renderer *renderer = renderer_associated_with_current_egl_context;

	DEBUG_ASSERT(renderer != NULL);
	DEBUG_ASSERT_GL_RENDERER(renderer);
	private = RENDERER_PRIVATE_GL(renderer);

	if (name == GL_EXTENSIONS) {
		return (GLubyte *) private->gl_extensions_override;
	} else {
		return glGetString(name);
	}
}

void *gl_renderer_flutter_resolve_gl_proc(struct renderer *renderer, const char *name) {
	struct gl_renderer *private;
	void *address;

	DEBUG_ASSERT_GL_RENDERER(renderer);
	private = RENDERER_PRIVATE_GL(renderer);

	/*  
	 * The mesa V3D driver reports some OpenGL ES extensions as supported and working
	 * even though they aren't. hacked_glGetString is a workaround for this, which will
	 * cut out the non-working extensions from the list of supported extensions.
	 */

	if (name == NULL) {
		return NULL;
	} else if (private->gl_extensions_override && (strcmp(name, "glGetString") == 0)) {
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
	(void) renderer;
	(void) info;
	DEBUG_ASSERT_GL_RENDERER(renderer);
	return 0;
}

bool gl_renderer_flutter_present_with_info(struct renderer *renderer, const FlutterPresentInfo *info) {
	(void) renderer;
	(void) info;
	DEBUG_ASSERT_GL_RENDERER(renderer);
	return true;
}


bool sw_renderer_flutter_present(
	struct renderer *renderer,
	const void *allocation,
	size_t bytes_per_row,
	size_t height
) {
	(void) renderer;
	(void) allocation;
	(void) bytes_per_row;
	(void) height;

	printf("sw_renderer_flutter_present\n");

	return true;
}

int renderer_flush_rendering(struct renderer *renderer) {
	DEBUG_ASSERT(renderer != NULL);
	DEBUG_ASSERT(renderer->flush_rendering != NULL);
	return renderer->flush_rendering(renderer);
}

static int gl_renderer_flush_rendering(struct renderer *renderer) {
	struct gl_renderer *private;
	EGLBoolean egl_ok;

	DEBUG_ASSERT_GL_RENDERER(renderer);
	private = RENDERER_PRIVATE_GL(renderer);

	egl_ok = eglSwapBuffers(private->egl_display, private->egl_surface);
	if (egl_ok == EGL_FALSE) {
		LOG_RENDERER_ERROR("Couldn't flush rendering. eglSwapBuffers: %s\n", eglGetErrorString());
		return EIO;
	}

	return 0;
}

static int sw_renderer_flush_rendering(struct renderer *renderer) {
	(void) renderer;
	return 0;
}


void renderer_fill_flutter_renderer_config(struct renderer *renderer, FlutterRendererConfig *config) {
	DEBUG_ASSERT(renderer != NULL);
	DEBUG_ASSERT(config != NULL);
	DEBUG_ASSERT(renderer->fill_flutter_renderer_config != NULL);
	return renderer->fill_flutter_renderer_config(renderer, config);
}

static void sw_renderer_fill_flutter_renderer_config(struct renderer *renderer, FlutterRendererConfig *config) {
	struct sw_renderer *private;

	(void) renderer;

	DEBUG_ASSERT_SW_RENDERER(renderer);
	private = RENDERER_PRIVATE_SW(renderer);

	config->type = kSoftware;
	config->software = (FlutterSoftwareRendererConfig) {
		.struct_size = sizeof(FlutterSoftwareRendererConfig),
		.surface_present_callback = private->sw_dispatcher->surface_present_callback
	};
}

static void gl_renderer_fill_flutter_renderer_config(struct renderer *renderer, FlutterRendererConfig *config) {
	struct gl_renderer *private;

	DEBUG_ASSERT_GL_RENDERER(renderer);
	private = RENDERER_PRIVATE_GL(renderer);

	config->type = kOpenGL;
	config->open_gl = (FlutterOpenGLRendererConfig) {
		.struct_size = sizeof(FlutterOpenGLRendererConfig),
		.make_current = private->gl_interface->make_current,
		.clear_current = private->gl_interface->clear_current,
		.make_resource_current = private->gl_interface->make_resource_current,
		.present = private->gl_interface->present,
		.fbo_callback = private->gl_interface->fbo_callback,
		.fbo_reset_after_present = false,
		.surface_transformation = private->gl_interface->surface_transformation,
		.gl_proc_resolver = private->gl_interface->gl_proc_resolver,
		.gl_external_texture_frame_callback = private->gl_interface->gl_external_texture_frame_callback,
		.fbo_with_frame_info_callback = NULL,
		.present_with_info = NULL
	};
}

static EGLConfig get_matching_config(EGLDisplay display, enum pixfmt format) {
	EGLConfig *configs;
	EGLint egl_ok, n_matched, attrib_value;

	static const EGLint attributes[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_NONE
	};

	egl_ok = eglChooseConfig(
		display,
		attributes,
		NULL, 0,
		&n_matched
	);
	if (egl_ok == EGL_FALSE) {
		LOG_RENDERER_ERROR("Could not query number of EGL configs: eglChooseConfig: %s\n", eglGetErrorString());
		return EGL_NO_CONFIG_KHR;
	}

	configs = alloca(sizeof(EGLConfig) * n_matched);

	egl_ok = eglChooseConfig(
		display,
		attributes,
		configs, n_matched,
		&n_matched
	);
	if (egl_ok == EGL_FALSE) {
		LOG_RENDERER_ERROR("Could not query EGL configs: eglChooseConfig: %s\n", eglGetErrorString());
		return EGL_NO_CONFIG_KHR;
	}

	for (int i = 0; i < n_matched; i++) {
		egl_ok = eglGetConfigAttrib(
			display,
			configs[i],
			EGL_NATIVE_VISUAL_ID,
			&attrib_value
		);
		if (egl_ok == EGL_FALSE) {
			LOG_RENDERER_ERROR("Could not query EGL config attribute: eglGetConfigAttrib: %s\n", eglGetErrorString());
			return EGL_NO_CONFIG_KHR;
		}

		if (*((uint32_t*) &attrib_value) == get_pixfmt_info(format)->gbm_format) {
			return configs[i];
		}
	}

	LOG_RENDERER_ERROR("Could not find EGL framebuffer config for pixel format %s.\n", get_pixfmt_info(format)->name);
	return EGL_NO_CONFIG_KHR;
}

struct renderer *gl_renderer_new(
	struct gbm_device *gbmdev,
	struct libegl *libegl,
	struct egl_client_info *egl_client_info,
	struct libgl *libgl,
	const struct flutter_renderer_gl_interface *gl_interface,
	enum pixfmt format,
	int w, int h
) {
	struct gl_renderer *private;
	struct renderer *renderer;
	struct egl_display_info *egl_display_info;
	struct gbm_surface *gbm_surface;
	EGLDisplay egl_display;
	EGLSurface egl_surface;
	EGLContext root_context;
	EGLBoolean egl_ok;
	EGLConfig egl_config;
	EGLint major, minor;
	int ok;

	renderer = malloc(sizeof *renderer);
	if (renderer == NULL) {
		goto fail_return_null;
	}

	private = malloc(sizeof *private);
	if (private == NULL) {
		goto fail_free_renderer;
	}

	ok = cpset_init(&private->egl_contexts, CPSET_DEFAULT_MAX_SIZE);
	if (ok != 0) {
		goto fail_free_private;
	}

	ok = cqueue_init(&private->unused_egl_contexts, sizeof(EGLContext), CQUEUE_DEFAULT_MAX_SIZE);
	if (ok != 0) {
		goto fail_deinit_contexts;
	}

	gbm_surface = gbm_surface_create_with_modifiers(
		gbmdev,
		w, h,
		get_pixfmt_info(format)->gbm_format,
		(uint64_t[1]) {DRM_FORMAT_MOD_LINEAR},
		1
	);
	if (gbm_surface == NULL) {
		LOG_RENDERER_ERROR("Couldn't create GBM surface. gbm_surface_create_with_modifiers: %s\n", strerror(errno));
		goto fail_deinit_unused_contexts;
	}
	
	if (libegl->eglGetPlatformDisplay != NULL) {
		egl_display = libegl->eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbmdev, NULL);
		if (egl_display == EGL_NO_DISPLAY) {
			LOG_RENDERER_ERROR("Could not get EGL display! eglGetPlatformDisplay: %s\n", eglGetErrorString());
			goto fail_destroy_gbm_surface;
		}
	} else if (egl_client_info->supports_ext_platform_base && egl_client_info->supports_khr_platform_gbm) {
		egl_display = libegl->eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR, gbmdev, NULL);
		if (egl_display == EGL_NO_DISPLAY) {
			LOG_RENDERER_ERROR("Could not get EGL display! eglGetPlatformDisplayEXT: %s\n", eglGetErrorString());
			goto fail_destroy_gbm_surface;
		}
	} else {
		egl_display = eglGetDisplay((void*) gbmdev);
		if (egl_display == EGL_NO_DISPLAY) {
			LOG_RENDERER_ERROR("Could not get EGL display! eglGetDisplay: %s\n", eglGetErrorString());
			goto fail_destroy_gbm_surface;
		}
	}

	egl_ok = eglInitialize(egl_display, &major, &minor);
	if (egl_ok == EGL_FALSE) {
		LOG_RENDERER_ERROR("Could not initialize EGL display! eglInitialize: %s\n", eglGetErrorString());
		goto fail_destroy_gbm_surface;
	}

	egl_display_info = egl_display_info_new(libegl, major, minor, egl_display);
	if (egl_display_info == NULL) {
		LOG_RENDERER_ERROR("Could not create EGL display info!\n");
		goto fail_terminate_display;
	}

	egl_config = get_matching_config(egl_display, format);
	if (egl_config == EGL_NO_CONFIG_KHR) {
		goto fail_destroy_egl_display_info;
	}

	egl_ok = eglBindAPI(EGL_OPENGL_ES_API);
	if (egl_ok == EGL_FALSE) {
		LOG_RENDERER_ERROR("Failed to bind OpenGL ES API! eglBindAPI: %s\n", eglGetErrorString());
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
	if (root_context == EGL_NO_CONTEXT) {
		LOG_RENDERER_ERROR("Could not create OpenGL ES context. eglCreateContext: %s\n", eglGetErrorString());
		goto fail_destroy_egl_display_info;
	}

	egl_surface = eglCreateWindowSurface(egl_display, egl_config, (EGLNativeWindowType) gbm_surface, NULL);
	if (egl_surface == EGL_NO_SURFACE) {
		LOG_RENDERER_ERROR("Could not create EGL window surface. eglCreateWindowSurface: %s\n", eglGetErrorString());
		goto fail_destroy_egl_context;
	}

	cpset_put_locked(&private->egl_contexts, root_context);
	cqueue_enqueue_locked(&private->unused_egl_contexts, &root_context);

	private->libegl = libegl;
	private->client_info = egl_client_info;
	private->gl_interface = gl_interface;
	private->egl_display = egl_display;
	private->display_info = egl_display_info;
	private->gl_extensions_override = NULL;
	private->gbm_surface = gbm_surface;
	private->egl_config = egl_config;
	private->egl_surface = egl_surface;
	private->flutter_rendering_context = EGL_NO_CONTEXT;
	private->flutter_resource_context = EGL_NO_CONTEXT;
	private->libgl = libgl;

	renderer->type = kOpenGL;
	renderer->private = (struct renderer_private *) private;
	renderer->destroy = gl_renderer_destroy;
	renderer->fill_flutter_renderer_config = gl_renderer_fill_flutter_renderer_config;
	renderer->flush_rendering = gl_renderer_flush_rendering;

	if (private->display_info->supports_14 == false) {
		LOG_RENDERER_ERROR("Flutter-pi requires EGL version 1.4 or newer, which is not supported by your system.\n");
		goto fail_destroy_egl_context;
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
	cqueue_deinit(&private->unused_egl_contexts);

	fail_deinit_contexts:
	cpset_deinit(&private->egl_contexts);

	fail_free_private:
	free(private);

	fail_free_renderer:
	free(renderer);

	fail_return_null:
	return NULL;
}

struct gbm_surface *gl_renderer_get_main_gbm_surface(
	struct renderer *renderer
) {
	struct gl_renderer *private;

	DEBUG_ASSERT_GL_RENDERER(renderer);
	private = RENDERER_PRIVATE_GL(renderer);

	return private->gbm_surface;
}

struct renderer *sw_renderer_new(
	const struct flutter_renderer_sw_interface *sw_dispatcher
) {
	struct sw_renderer *private;
	struct renderer *renderer;

	renderer = malloc(sizeof *renderer);
	if (renderer == NULL) {
		goto fail_return_null;
	}

	private = malloc(sizeof *private);
	if (private == NULL) {
		goto fail_free_renderer;
	}

	private->sw_dispatcher = sw_dispatcher;
	renderer->type = kSoftware;
	renderer->private = (struct renderer_private *) private;
	renderer->destroy = sw_renderer_destroy;
	renderer->fill_flutter_renderer_config = sw_renderer_fill_flutter_renderer_config;
	renderer->flush_rendering = sw_renderer_flush_rendering;

	return renderer;


	fail_free_renderer:
	free(renderer);

	fail_return_null:
	return NULL;
}
