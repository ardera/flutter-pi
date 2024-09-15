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
    FlutterEngineSendWindowMetricsEventFnPtr send_window_metrics_event;
    FlutterEngine engine;
};

struct fl_pointer_event_interface {
    FlutterEngineSendPointerEventFnPtr send_pointer_event;
    FlutterEngine engine;
};

struct user_input;

MUST_CHECK struct compositor *compositor_new_multiview(
    struct tracer *tracer,
    struct evloop *raster_loop,
    struct udev *udev,
    struct drmdev *drmdev,
    struct drm_resources *resources,
    struct user_input *input
);

struct compositor *
compositor_new_singleview(struct tracer *tracer, struct evloop *raster_loop, struct window *window, struct user_input *input);

void compositor_destroy(struct compositor *compositor);

DECLARE_REF_OPS(compositor)

/**
 * @brief Sets the callback & flutter engine that flutter displays will be registered to.
 * 
 * This will immediately call the notify_display_update callback with the current connected displays,
 * and call the send_window_metrics_event callback for all views.
 * 
 * @param compositor The compositor to set the display interface for.
 * @param display_interface The display interface to set. NULL is not allowed. The struct is copied.
 */
void compositor_set_fl_display_interface(struct compositor *compositor, const struct fl_display_interface *display_interface);

/**
 * @brief Sets the callback & flutter engine that pointer events will be sent to.
 * 
 * @param compositor The compositor to set the pointer event interface for.
 * @param pointer_event_interface The pointer event interface to set. NULL is not allowed. The struct is copied.
 */
void compositor_set_fl_pointer_event_interface(
    struct compositor *compositor,
    const struct fl_pointer_event_interface *pointer_event_interface
);

void compositor_get_view_geometry(struct compositor *compositor, struct view_geometry *view_geometry_out);

ATTR_PURE double compositor_get_refresh_rate(struct compositor *compositor);

int compositor_get_next_vblank(struct compositor *compositor, uint64_t *next_vblank_ns_out);

/**
 * @brief Adds a (non-implicit) view to the compositor, returning the view id.
 */
int64_t compositor_add_view(struct compositor *compositor, struct window *window);

/**
 * @brief Removes a view from the compositor.
 */
void compositor_remove_view(struct compositor *compositor, int64_t view_id);

/**
 * @brief Gets the implicit view of the compositor.
 * 
 * Some flutter APIs still assume a single view, so this is a way to get a view to use for those.
 * 
 * @attention Since the view setup of the compositor can change at any time, and views
 * might be removed and destroyed at any time, this function increases the refcount
 * of the view before returning. The caller must call @ref window_unref on the view
 * to avoid a memory leak.
 * 
 * @return The implicit view of the compositor.
 */
MUST_CHECK struct window *compositor_ref_implicit_view(struct compositor *compositor);

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

struct display_setup;
struct connector;
struct display;

DECLARE_REF_OPS(display_setup)

/**
 * @brief Gets the number of connectors present in this display setup.
 */
size_t display_setup_get_n_connectors(struct display_setup *s);

/**
 * @brief Gets the connector at the given index.
 */
const struct connector *display_setup_get_connector(struct display_setup *s, size_t i);

/**
 * @brief Gets the name of the connector. Can be useful for identification.
 * 
 * e.g. `HDMI-A-1`, `DP-1`, `LVDS-1`, etc.
 */
const char *connector_get_name(const struct connector *connector);

/**
 * @brief Gets the type of the connector.
 */
enum connector_type connector_get_type(const struct connector *connector);

/**
 * @brief Gets the name of the type of the connector.
 * 
 * @attention This can be NULL if the type is unrecognized.
 */
const char *connector_get_type_name(const struct connector *connector);

/**
 * @brief Checks if the connector has a display attached.
 */
bool connector_has_display(const struct connector *connector);

/**
 * @brief Gets the display attached to the connector.
 * 
 * @attention This can be NULL if the connector has no display attached.
 */
const struct display *connector_get_display(const struct connector *connector);

/**
 * @brief Gets the ID of the display as reported to flutter.
 */
size_t display_get_fl_display_id(const struct display *display);

/**
 * @brief Gets the refresh rate of the display in the current mode.
 */
double display_get_refresh_rate(const struct display *display);

/**
 * @brief Gets the width of the display in the current mode, in physical pixels.
 */
struct vec2i display_get_size(const struct display *display);

/**
 * @brief Gets the physical size of the display in millimeters.
 */
struct vec2i display_get_physical_size(const struct display *display);

/**
 * @brief Gets the device pixel ratio of the display in the current mode.
 */
double display_get_device_pixel_ratio(const struct display *display);

/**
 * @brief Gets the ID of the connector.
 */
const char *display_get_connector_id(const struct display *display);

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
