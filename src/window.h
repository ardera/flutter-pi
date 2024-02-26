// SPDX-License-Identifier: MIT
/*
 * Window
 *
 * - a window is a 2D rect inside a real, physical display where flutter content can be displayed, and input events can be routed.
 *
 * Copyright (c) 2022, Hannes Winkler <hanneswinkler2000@web.de>
 */

#ifndef _FLUTTERPI_SRC_WINDOW_H
#define _FLUTTERPI_SRC_WINDOW_H

#include "compositor_ng.h"
#include "modesetting.h"
#include "pixel_format.h"
#include "util/refcounting.h"

#include "config.h"

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

enum renderer_type { kOpenGL_RendererType, kVulkan_RendererType };

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
struct window *kms_window_new(
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
 * Creates a new dummy window.
 *
 * @param tracer The tracer object.
 * @param scheduler The frame scheduler object.
 * @param renderer_type The type of renderer.
 * @param gl_renderer The GL renderer object.
 * @param vk_renderer The Vulkan renderer object.
 * @param size The size of the window.
 * @param has_explicit_dimensions Indicates if the window has explicit dimensions.
 * @param width_mm The width of the window in millimeters.
 * @param height_mm The height of the window in millimeters.
 * @param refresh_rate The refresh rate of the window.
 * @return A pointer to the newly created window.
 */
MUST_CHECK struct window *dummy_window_new(
    struct tracer *tracer,
    struct frame_scheduler *scheduler,
    enum renderer_type renderer_type,
    struct gl_renderer *gl_renderer,
    struct vk_renderer *vk_renderer,
    struct vec2i size,
    bool has_explicit_dimensions,
    int width_mm,
    int height_mm,
    double refresh_rate
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

#ifdef HAVE_EGL_GLES2
bool window_has_egl_surface(struct window *window);

EGLSurface window_get_egl_surface(struct window *window);
#endif

/**
 * @brief Gets a render surface, used as the backing store for an engine layer.
 *
 * This only makes sense if there's a single UI (engine) layer. If there's multiple ones, lifetimes become weird.
 *
 */
struct render_surface *window_get_render_surface(struct window *window, struct vec2i size);

bool window_is_cursor_enabled(struct window *window);

int window_set_cursor(
    // clang-format off
    struct window *window,
    bool has_enabled, bool enabled,
    bool has_kind, enum pointer_kind kind,
    bool has_pos, struct vec2i pos
    // clang-format on
);

#endif  // _FLUTTERPI_SRC_WINDOW_H
