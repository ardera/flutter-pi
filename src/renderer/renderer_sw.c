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
#include <pixel_format.h>
#include <renderer/renderer.h>
#include <renderer/renderer_private.h>
#include <renderer/renderer_sw.h>

#define LOG_RERROR(format_str, ...) fprintf(stderr, "[software renderer] %s: " format_str, __func__, ##__VA_ARGS__)

#define DEBUG_ASSERT_SW_RENDERER(r) DEBUG_ASSERT((r)->type == kSoftware && "Expected renderer to be a software renderer.")
#define RENDERER_PRIVATE_SW(renderer) ((struct sw_renderer*) (renderer->private))

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

static void sw_renderer_destroy(struct renderer *renderer) {
	struct sw_renderer *private = RENDERER_PRIVATE_SW(renderer);

	/// TODO: Implement sw_renderer_destroy

	free(private);
	free(renderer);
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

static int sw_renderer_flush_rendering(struct renderer *renderer) {
	(void) renderer;
	return 0;
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
	renderer->is_gl = false;
	renderer->is_sw = true;
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
