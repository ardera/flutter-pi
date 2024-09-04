// SPDX-License-Identifier: MIT
/*
 * Compositor NG
 *
 * - newer flutter compositor
 *
 * Copyright (c) 2022, Hannes Winkler <hanneswinkler2000@web.de>
 */

#ifndef _FLUTTERPI_SRC_COMPOSITOR_NG_H
#define _FLUTTERPI_SRC_COMPOSITOR_NG_H

#include <flutter_embedder.h>

#include "cursor.h"
#include "flutter-pi.h"
#include "frame_scheduler.h"
#include "pixel_format.h"
#include "util/collection.h"
#include "util/refcounting.h"

#include "config.h"

#ifdef HAVE_EGL_GLES2
    #include "egl.h"
#endif

struct compositor;


struct fl_layer_props {
    /**
     * @brief True if the presentation quadrangle (the quadrangle on the target window into which the
     * layer should be rendered) is an axis-aligned rectangle. For example, allows us to use a plain
     * hardware overlay layer for this layer.
     *
     * This should always be true for backing stores, but might be false for platform views.
     */
    bool is_aa_rect;

    /**
     * @brief The coords of the axis aligned rectangle if @ref is_aa_rect is true.
     */
    struct aa_rect aa_rect;

    /**
     * @brief The quadrangle on the target window into which the layer should be rendered.
     */
    struct quad quad;

    /**
     * @brief Opacity as a normalized float from 0 (transparent) to 1 (opqaue).
     */
    double opacity;

    /**
     * @brief Rotation of the buffer in degrees clockwise, normalized to a range 0 - 360.
     */
    double rotation;

    /**
     * @brief The number of clip rectangles in the @ref clip_rects array.
     */
    size_t n_clip_rects;

    /**
     * @brief The (possibly rounded) rectangles that the surface should be clipped to.
     */
    struct clip_rect *clip_rects;
};

struct fl_layer {
    struct fl_layer_props props;
    struct surface *surface;
};

struct fl_layer_composition {
    refcount_t n_refs;
    size_t n_layers;
    struct fl_layer layers[];
};

struct drmdev;
struct compositor;
struct frame_scheduler;
struct view_geometry;
struct window;
struct tracer;
struct drm_resources;

typedef void (*compositor_frame_begin_cb_t)(void *userdata, uint64_t vblank_ns, uint64_t next_vblank_ns);

struct fl_display_interface {
    FlutterEngineNotifyDisplayUpdateFnPtr notify_display_update;
    FlutterEngine engine;
};

struct compositor *compositor_new_multiview(
    struct tracer *tracer,
    struct evloop *raster_loop,
    struct udev *udev,
    struct drmdev *drmdev,
    struct drm_resources *resources,
    const struct fl_display_interface *display_interface
);

struct compositor *compositor_new_singleview(
    struct tracer *tracer,
    struct evloop *raster_loop,
    struct window *window,
    const struct fl_display_interface *display_interface
);

void compositor_destroy(struct compositor *compositor);

DECLARE_REF_OPS(compositor)

void compositor_get_view_geometry(struct compositor *compositor, struct view_geometry *view_geometry_out);

ATTR_PURE double compositor_get_refresh_rate(struct compositor *compositor);

int compositor_get_next_vblank(struct compositor *compositor, uint64_t *next_vblank_ns_out);

/**
 * @brief Adds a (non-implicit) view to the compositor, returning the view id.
 */
int64_t compositor_add_view(
    struct compositor *compositor,
    struct window *window
);

/**
 * @brief Removes a view from the compositor.
 */
void compositor_remove_view(
    struct compositor *compositor,
    int64_t view_id
);

/**
 * @brief Sets the implicit view (view with id 0) to the given window.
 */
int compositor_put_implicit_view(
    struct compositor *compositor,
    struct window *window
);

/**
 * @brief Adds a platform view to the compositor, returning the platform view id.
 */
int compositor_add_platform_view(struct compositor *compositor, struct surface *surface);

/**
 * @brief Removes a platform view from the compositor.
 */
void compositor_remove_platform_view(struct compositor *compositor, int64_t view_id);


const FlutterCompositor *compositor_get_flutter_compositor(struct compositor *compositor);

int compositor_request_frame(struct compositor *compositor, compositor_frame_begin_cb_t cb, void *userdata);

#ifdef HAVE_EGL_GLES2
bool compositor_has_egl_surface(struct compositor *compositor);

EGLSurface compositor_get_egl_surface(struct compositor *compositor);
#endif

int compositor_get_event_fd(struct compositor *compositor);

int compositor_on_event_fd_ready(struct compositor *compositor);

void compositor_set_cursor(
    struct compositor *compositor,
    bool has_enabled,
    bool enabled,
    bool has_kind,
    enum pointer_kind kind,
    bool has_delta,
    struct vec2f delta
);

enum connector_type {
    CONNECTOR_TYPE_VGA,
    CONNECTOR_TYPE_DVI,
    CONNECTOR_TYPE_LVDS,
    CONNECTOR_TYPE_DISPLAY_PORT,
    CONNECTOR_TYPE_HDMI,
    CONNECTOR_TYPE_TV,
    CONNECTOR_TYPE_EDP,
    CONNECTOR_TYPE_DSI,
    CONNECTOR_TYPE_DPI,
    CONNECTOR_TYPE_OTHER,
};

struct connector {
    /**
     * @brief The ID of the connector.
     * 
     * e.g. `HDMI-A-1`, `DP-1`, `LVDS-1`, etc.
     * 
     * This string will only live till the end of the iteration, so make sure to copy it if you need it later.
     */
    const char *id;

    /**
     * @brief The type of the connector.
     */
    enum connector_type type;

    /**
     * @brief The name of the connector type, if @ref type is @ref CONNECTOR_TYPE_OTHER.
     * 
     * e.g. `Virtual`, `Composite`, etc.
     * 
     * This string will only live till the end of the iteration, so make sure to copy it if you need it later.
     */
    const char *other_type_name;
};

/**
 * @brief Callback that will be called on each iteration of
 * @ref compositor_for_each_connector.
 *
 * Should return true if looping should continue. False if iterating should be
 * stopped.
 *
 * @param display The current iteration value.
 * @param userdata Userdata that was passed to @ref compositor_for_each_connector.
 */
typedef bool (*connector_callback_t)(const struct connector *connector, void *userdata);

/**
 * @brief Iterates over every present connector.
 *
 * See @ref connector_callback_t for documentation on the callback.
 */
void compositor_for_each_connector(
    struct compositor *compositor,
    connector_callback_t callback,
    void *userdata
);

struct display {
    /**
     * @brief The ID of the display, as reported to flutter.
     */
    uint64_t fl_display_id;

    /**
     * @brief The refresh rate of the display.
     */
    double refresh_rate;

    /**
     * @brief The width of the display in the selected mode, in physical pixels.
     */
    size_t width;

    /**
     * @brief The height of the display in the selected mode, in physical pixels.
     */
    size_t height;

    /**
     * @brief The device pixel ratio of the display, in the selected mode.
     */
    double device_pixel_ratio;

    /**
     * @brief The identifier of the connector this display is connected to.
     * 
     * This string will only live till the end of the iteration, so make sure to copy it if you need it later.
     */
    const char *connector_id;
};

/**
 * @brief Callback that will be called on each iteration of
 * @ref compositor_for_each_display.
 *
 * Should return true if looping should continue. False if iterating should be
 * stopped.
 *
 * @param display The current iteration value.
 * @param userdata Userdata that was passed to @ref compositor_for_each_display.
 */
typedef bool (*display_callback_t)(const struct display *display, void *userdata);

/**
 * @brief Iterates over every connected display.
 *
 * See @ref display_callback_t for documentation on the callback.
 */
void compositor_for_each_display(
    struct compositor *compositor,
    display_callback_t callback,
    void *userdata
);


struct display_setup {
    size_t n_connectors;
    struct connector *connectors;

    size_t n_displays;
    struct display *displays;
};

/**
 * @brief Gets a value notifier for the displays & connectors attached to the compositor.
 * 
 * The value is a @ref struct display_setup.
 */
struct notifier *compositor_get_display_setup_notifier(struct compositor *compositor);


struct fl_layer_composition;

struct fl_layer_composition *fl_layer_composition_new(size_t n_layers);
void fl_layer_composition_destroy(struct fl_layer_composition *composition);
DECLARE_REF_OPS(fl_layer_composition)

size_t fl_layer_composition_get_n_layers(struct fl_layer_composition *composition);
struct fl_layer *fl_layer_composition_peek_layer(struct fl_layer_composition *composition, int layer);

#endif  // _FLUTTERPI_SRC_COMPOSITOR_NG_H
