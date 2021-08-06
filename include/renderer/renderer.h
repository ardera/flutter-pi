#ifndef _FLUTTERPI_INCLUDE_RENDERER_RENDERER_H
#define _FLUTTERPI_INCLUDE_RENDERER_RENDERER_H

#include <flutter_embedder.h>
#include <collection.h>

#ifdef HAS_GL
#	include <renderer/renderer_gl.h>
#endif // HAS_GL

// We always have software rendering.
#include <renderer/renderer_sw.h>

struct renderer;
struct backing_store;

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
 * @brief Returns true if the @arg renderer is an OpenGL ES renderer.
 */
bool renderer_is_gl(struct renderer *renderer);

/**
 * @brief Returns true if the @arg renderer is a Software renderer.
 */
bool renderer_is_sw(struct renderer *renderer);


#endif // _FLUTTERPI_INCLUDE_RENDERER_RENDERER_H