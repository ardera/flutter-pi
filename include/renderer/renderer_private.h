#ifndef _FLUTTERPI_INCLUDE_RENDERER_RENDERER_PRIVATE_H
#define _FLUTTERPI_INCLUDE_RENDERER_RENDERER_PRIVATE_H

#include <flutter_embedder.h>

struct renderer_private;

struct renderer {
	struct renderer_private *private;
	
	bool is_gl;
	bool is_sw;

	void (*destroy)(struct renderer *renderer);
	void (*fill_flutter_renderer_config)(struct renderer *renderer, FlutterRendererConfig *config);
	int (*flush_rendering)(struct renderer *renderer);
};

#endif // _FLUTTERPI_INCLUDE_RENDERER_RENDERER_PRIVATE_H