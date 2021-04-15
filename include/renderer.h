#ifndef RENDERER_H_
#define RENDERER_H_

#include <gbm.h>
#include <modesetting.h>

typedef void (*present_complete_callback_t)(void *userdata);

typedef void (*frame_start_callback_t)(void *userdata);

struct renderer;

struct flutter_renderer_gl_interface {
    BoolCallback make_current;
    BoolCallback clear_current;
    BoolCallback present;
    UIntCallback fbo_callback;
    BoolCallback make_resource_current;
    TransformationCallback surface_transformation;
    ProcResolver gl_proc_resolver;
    TextureFrameCallback gl_external_texture_frame_callback;
    UIntFrameInfoCallback fbo_with_frame_info_callback;
    BoolPresentInfoCallback present_with_info;
};

struct flutter_renderer_sw_interface {
    SoftwareSurfacePresentCallback surface_present_callback;
};

struct renderer *gl_renderer_new(
	struct gbm_device *gbmdev,
	struct libegl *libegl,
	struct egl_client_info *egl_client_info,
	struct libgl *libgl,
	const struct flutter_renderer_gl_interface *gl_interface,
	uint32_t format,
	int w, int h
);

/**
 * @brief Create a new software renderer.
 * @param kmsdev optional. Used for allocating the 
 */
struct renderer *sw_renderer_new(
	struct kmsdev *kmsdev,
	struct flutter_renderer_sw_interface *sw_dispatcher
);

/**
 * @brief Destroy this renderer, freeing all allocated resources.
 */
void renderer_destroy(struct renderer *renderer);

/**
 * @brief Fill @ref config with the dispatcher functions in the @ref flutter_renderer_gl_interface given to
 * @ref gl_renderer_new or the @ref flutter_sw_interface @ref sw_renderer_new depending
 * on whether OpenGL or software rendering is used.
 * All fields in @ref gl_interface and @ref sw_dispatcher should be populated.
 * Sometimes there are multiple ways the config can be filled with the interface functions
 * In that case @ref renderer_fill_flutter_renderer_config will choose a way that it best supports internally.
 * 
 * The functions in @ref gl_interface and @ref sw_dispatcher should just call their renderer
 * equivalents with the renderer instance. For example, `gl_interface->make_current` should call
 * `gl_renderer_make_flutter_rendering_context_current(renderer)`.
 */
void renderer_fill_flutter_renderer_config(
	struct renderer *renderer,
	FlutterRendererConfig *config
);

/**
 * @brief Make a random EGL context current. This will internally call
 * @ref get_unused_egl_context. If @ref surfaceless is false,
 * the EGL surface will be made current too. (Otherwise the context is
 * bound without a surface.)
 */
int gl_renderer_make_current(struct renderer *renderer, bool surfaceless);

/**
 * @brief Clear the EGL context current on this thread, putting it
 * back into the unused context queue.
 */
int gl_renderer_clear_current(struct renderer *renderer);

/**
 * @brief Get an EGL context using @ref get_unused_egl_context and don't make it current.
 */
EGLContext gl_renderer_reserve_context(struct renderer *renderer);

/**
 * @brief Put an EGL context into the unused context queue.
 */
int gl_renderer_release_context(struct renderer *renderer, EGLContext context);

/**
 * @brief Make a specific EGL context current (without fetching a random one from
 * the unused context queue)
 */
int gl_renderer_reserved_make_current(struct renderer *renderer, EGLContext context, bool surfaceless);

/**
 * @brief Clear the current EGL context without putting it back into the
 * unused context queue.
 */
int gl_renderer_reserved_clear_current(struct renderer *renderer);

int gl_renderer_create_scanout_fbo(
	struct renderer *renderer,
	int width, int height,
	EGLImage *image_out,
	uint32_t *handle_out,
	uint32_t *stride_out,
	GLuint *rbo_out,
	GLuint *fbo_out
);

int gl_renderer_destroy_scanout_fbo(
	struct renderer *renderer,
	EGLImage image,
	uint32_t handle,
	GLuint rbo,
	GLuint fbo
);

bool gl_renderer_flutter_make_rendering_context_current(struct renderer *renderer);

bool gl_renderer_flutter_make_resource_context_current(struct renderer *renderer);

bool gl_renderer_flutter_clear_current(struct renderer *renderer);

bool gl_renderer_flutter_present(struct renderer *renderer);

uint32_t gl_renderer_flutter_get_fbo(struct renderer *renderer);

FlutterTransformation gl_renderer_flutter_get_surface_transformation(struct renderer *renderer);

void *gl_renderer_flutter_resolve_gl_proc(struct renderer *renderer, const char *name);

uint32_t gl_renderer_flutter_get_fbo_with_info(struct renderer *renderer, const FlutterFrameInfo *info);

bool gl_renderer_flutter_present_with_info(struct renderer *renderer, const FlutterPresentInfo *info);

bool sw_renderer_flutter_present(
	struct renderer *renderer,
	const void *allocation,
	size_t bytes_per_row,
	size_t height
);


static const char *streglerr(EGLint egl_error) {
	static const int egl_errors_offset = EGL_SUCCESS;
	static const char *egl_errors[] = {
		"EGL_SUCCESS",
		"EGL_NOT_INITIALIZED",
		"EGL_BAD_ACCESS",
		"EGL_BAD_ALLOC",
		"EGL_BAD_ATTRIBUTE",
		"EGL_BAD_CONFIG",
		"EGL_BAD_CONTEXT",
		"EGL_BAD_CURRENT_SURFACE",
		"EGL_BAD_DISPLAY",
		"EGL_BAD_MATCH",
		"EGL_BAD_NATIVE_PIXMAP",
		"EGL_BAD_NATIVE_WINDOW",
		"EGL_BAD_PARAMETER",
		"EGL_BAD_SURFACE",
		"EGL_CONTEXT_LOSS"
	};

	if (egl_error < egl_errors_offset) {
		return NULL;
	} else if ((unsigned) egl_error >= (egl_errors_offset + (sizeof(egl_errors) / sizeof(*egl_errors)))) {
		return NULL;
	} else {
		return egl_errors[egl_error - egl_errors_offset];
	}
};

static inline const char *eglGetErrorString(void) {
	return streglerr(eglGetError());
}

#endif