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

struct view_geometry {
    struct vec2f view_size, display_size;
    struct mat3f display_to_view_transform;
	struct mat3f view_to_display_transform;
	double device_pixel_ratio;
};

enum renderer_type {
    kOpenGL_RendererType,
    kVulkan_RendererType
};

DECLARE_REF_OPS(window)

/**
 * @brief Creates a new KMS window.
 * 
 * @param tracer 
 * @param scheduler 
 * @param render_surface_interface 
 * @param has_rotation 
 * @param rotation 
 * @param has_orientation 
 * @param orientation 
 * @param has_explicit_dimensions 
 * @param width_mm 
 * @param height_mm 
 * @param has_forced_pixel_format 
 * @param forced_pixel_format 
 * @param drmdev 
 * @param desired_videomode 
 * @return struct window* The new KMS window.
 */
ATTR_MALLOC struct window *kms_window_new(
    // clang-format off
    struct tracer *tracer,
    struct frame_scheduler *scheduler,
    enum renderer_type renderer_type,
    struct gl_renderer *gl_renderer,
    struct vk_renderer *vk_renderer,
    bool has_rotation, drm_plane_transform_t rotation,
    bool has_orientation, enum device_orientation orientation,
    bool has_explicit_dimensions, int width_mm, int height_mm,
    bool has_forced_pixel_format, enum pixfmt forced_pixel_format,
    struct drmdev *drmdev,
    const char *desired_videomode
    // clang-format on
);

/**
 * @brief Push a new flutter composition to the window, outputting a new frame.
 * 
 * @param window The window instance.
 * @param composition The composition that should be presented.
 * @return int Zero if successful, errno-code otherwise.
 */
int window_push_composition(struct window *window, struct fl_layer_composition *composition);

/**
 * @brief Get the current view geometry of this window.
 * 
 * @param window The window instance.
 * @return struct view_geometry 
 */
struct view_geometry window_get_view_geometry(struct window *window);

/**
 * @brief Returns the vertical refresh rate of the chosen mode & display.
 * 
 * @param window The window instance.
 * @return double The refresh rate.
 */
ATTR_PURE double window_get_refresh_rate(struct window *window);

/**
 * @brief Returns the timestamp of the next vblank signal in @param next_vblank_ns_out.
 * 
 * @param window The window instance.
 * @param next_vblank_ns_out Next vblank timestamp will be stored here. Must be non-null.
 * @return int Zero if successful, errno-code otherwise.
 */
int window_get_next_vblank(struct window *window, uint64_t *next_vblank_ns_out);

bool window_has_egl_surface(struct window *window);

EGLSurface window_get_egl_surface(struct window *window);

/**
 * @brief Gets a render surface, used as the backing store for an engine layer.
 * 
 * This only makes sense if there's a single UI (engine) layer. If there's multiple ones, lifetimes become weird.
 * 
 */
struct render_surface *window_get_render_surface(struct window *window, struct vec2f size);

#endif // _FLUTTERPI_INCLUDE_WINDOW_H
