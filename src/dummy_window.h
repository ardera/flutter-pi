#ifndef _FLUTTERPI_SRC_DUMMY_WINDOW_H
#define _FLUTTERPI_SRC_DUMMY_WINDOW_H

#include "util/geometry.h"
#include "window.h"

struct tracer;
struct frame_scheduler;
struct gl_renderer;
struct vk_renderer;

MUST_CHECK struct window *dummy_window_new(
    // clang-format off
    struct tracer *tracer,
    struct frame_scheduler *scheduler,
    enum renderer_type renderer_type,
    struct gl_renderer *gl_renderer,
    struct vk_renderer *vk_renderer,
    struct vec2i size,
    bool has_explicit_dimensions, int width_mm, int height_mm,
    double refresh_rate
    // clang-format on
);

#endif  // _FLUTTERPI_SRC_DUMMY_WINDOW_H