// SPDX-License-Identifier: MIT
/*
 * Window
 *
 * - a window is a 2D rect inside a real, physical display where flutter content can be displayed, and input events can be routed.
 *
 * Copyright (c) 2022, Hannes Winkler <hanneswinkler2000@web.de>
 */


#ifndef _FLUTTERPI_INCLUDE_WINDOW_H
#define _FLUTTERPI_INCLUDE_WINDOW_H

#include <pixel_format.h>
#include <modesetting.h>
#include <compositor_ng.h>

struct surface;
struct window;
struct tracer;
struct frame_scheduler;
struct fl_layer_composition;

struct render_surface_interface {
    struct surface *(*create_render_surface)(
        void *userdata,
        int width, int height,
        bool has_forced_pixel_format, enum pixfmt forced_pixel_format
    );
    void (*destroy)(void *userdata);
    void *userdata;
};

struct view_geometry {
    struct point view_size, display_size;
    FlutterTransformation display_to_view_transform;
	FlutterTransformation view_to_display_transform;
	double device_pixel_ratio;
};

DECLARE_REF_OPS(window)

struct window *kms_window_new(
    // clang-format off
    struct tracer *tracer,
    struct frame_scheduler *waiter,
    const struct render_surface_interface *render_surface_interface,
    bool has_rotation, drm_plane_transform_t rotation,
    bool has_orientation, enum device_orientation orientation,
    bool has_explicit_dimensions, int width_mm, int height_mm,
    bool has_forced_pixel_format, enum pixfmt forced_pixel_format,
    struct drmdev *drmdev
    // clang-format on
);

int window_push_composition(struct window *window, struct fl_layer_composition *composition);

struct view_geometry window_get_view_geometry(struct window *window);

double window_get_refresh_rate(struct window *window);

int window_get_next_vblank(struct window *window, uint64_t *next_vblank_ns_out);

bool window_has_egl_surface(struct window *window);

EGLSurface window_get_egl_surface(struct window *window);

struct render_surface *window_create_render_surface(struct window *window, struct point size);

#endif // _FLUTTERPI_INCLUDE_WINDOW_H
