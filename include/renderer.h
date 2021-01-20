#ifndef RENDERER_H_
#define RENDERER_H_

#define LOG_RENDERER_ERROR(...) fprintf(stderr, "[flutter-pi renderer] " __VA_ARGS__)

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
	struct drmdev *drmdev,
	struct libegl *libegl,
	struct egl_client_info *egl_client_info,
	const struct flutter_renderer_gl_interface *gl_interface,
	unsigned int width, unsigned int height
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

bool gl_renderer_flutter_make_renderering_context_current(struct renderer *renderer);

bool gl_renderer_flutter_make_resource_context_current(struct renderer *renderer);

bool gl_renderer_flutter_clear_current(struct renderer *renderer);

uint32_t gl_renderer_flutter_get_fbo(struct renderer *renderer);

FlutterTransformation gl_renderer_flutter_get_surface_transformation(struct renderer *renderer);

void *gl_renderer_flutter_resolve_gl_proc(struct renderer *renderer, const char *name);

uint32_t gl_renderer_flutter_get_fbo_with_info(struct renderer *renderer, const FlutterFrameInfo *info);

bool gl_renderer_flutter_present_with_info(struct renderer *renderer, const FlutterPresentInfo *info);

struct renderer *sw_renderer_new(
	struct drmdev *drmdev,
	struct flutter_renderer_sw_interface *sw_dispatcher
);

bool sw_renderer_flutter_present(
	struct renderer *renderer,
	void *allocation,
	size_t bytes_per_row,
	size_t height
);

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
int renderer_fill_flutter_renderer_config(
	struct renderer *renderer,
	FlutterRendererConfig *config
);

void renderer_destroy(struct renderer *renderer);

#endif RENDERER_H_