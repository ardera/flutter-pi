#ifndef _FLUTTERPI_INCLUDE_RENDERER_RENDERER_SW_H
#define _FLUTTERPI_INCLUDE_RENDERER_RENDERER_SW_H

#include <stdlib.h>

struct sw_renderer;

#define DEBUG_ASSERT_SW_RENDERER(r) DEBUG_ASSERT((r)->type == kSoftware && "Expected renderer to be a software renderer.")
#define RENDERER_PRIVATE_SW(renderer) ((struct sw_renderer*) (renderer->private))

struct flutter_renderer_sw_interface {
    SoftwareSurfacePresentCallback surface_present_callback;
};

struct renderer *sw_renderer_new(
	const struct flutter_renderer_sw_interface *sw_dispatcher
);

bool sw_renderer_flutter_present(
	struct renderer *renderer,
	const void *allocation,
	size_t bytes_per_row,
	size_t height
);

#endif // _FLUTTERPI_INCLUDE_RENDERER_RENDERER_SW_H