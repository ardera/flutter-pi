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
#include "kms/drmdev.h"
#include "kms/req_builder.h"
#include "kms/resources.h"
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
    float device_pixel_ratio;
};

enum renderer_type { kOpenGL_RendererType, kVulkan_RendererType };

DECLARE_REF_OPS(window)

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

/**
 * @brief Set the cursor (enabled, kind, position) for this window.
 * 
 * @param window The window to set the cursor for.
 * @param has_enabled True if @param enabled contains a valid value, and the cursor should either be enabled to disabled.
 * @param enabled Whether the cursor should be enabled or disabled.
 * @param has_kind True if the cursor kind should be changed.
 * @param kind The kind of cursor to set.
 * @param has_pos True if the cursor position should be changed.
 * @param pos The position of the cursor.
 */
int window_set_cursor(
    // clang-format off
    struct window *window,
    bool has_enabled, bool enabled,
    bool has_kind, enum pointer_kind kind,
    bool has_pos, struct vec2i pos
    // clang-format on
);

struct user_input_event;

/**
 * @brief Transform a normalized device coordinate (0..1) to a view coordinate.
 * 
 * In most cases, this just means the NDC is multiplied by the view size.
 * However, for a rotated display, this might be more complex.
 * 
 * @param window The window to transform for.
 * @param ndc The normalized device coordinate.
 */
struct vec2f window_transform_ndc_to_view(struct window *window, struct vec2f ndc);

/**
 * @brief A score, describing how good an input device matches to an output window.
 * 
 * A negative value means it doesn't match at all, and the window should under no circumstances receive
 * input events from this device.
 */
typedef int input_device_match_score_t;

struct user_input_device;

/**
 * @brief Returns how likely it is that this input device is meant for this window.
 * 
 * Input devices that send absolute positions (e.g. touch-screen, tablet, but not a mouse) always
 * need to be matched to a specific output. This input device <-> output association is unfortunately
 * not always clear, so we need to guess.
 * 
 * When a new input device is connected, the compositor will call this function to see if any window
 * is a good match for this device. The window with the highest score will receive the input events.
 * 
 * @param window The window to check.
 * @param device The input device to check.
 */
input_device_match_score_t window_match_input_device(struct window *window, struct user_input_device *device);

#endif  // _FLUTTERPI_SRC_WINDOW_H
