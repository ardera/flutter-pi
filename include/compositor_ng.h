// SPDX-License-Identifier: MIT
/*
 * Compositor NG
 *
 * - newer flutter compositor
 *
 * Copyright (c) 2022, Hannes Winkler <hanneswinkler2000@web.de>
 */


#ifndef _FLUTTERPI_INCLUDE_COMPOSITOR_NG_H
#define _FLUTTERPI_INCLUDE_COMPOSITOR_NG_H

#include <flutter_embedder.h>
#include <collection.h>
#include <pixel_format.h>
#include <modesetting.h>
#include <flutter-pi.h>
#include <egl.h>
#include <frame_scheduler.h>

struct compositor;

struct drm_connector_config {
    uint32_t connector_type;
    uint32_t connector_type_id;
    
    bool disable, primary;
    
    bool has_mode_size;
    int mode_width, mode_height;
    
    bool has_mode_refreshrate;
    int mode_refreshrate_n, mode_refreshrate_d;

    bool has_framebuffer_size;
    int framebuffer_width, framebuffer_height;

    bool has_physical_dimensions;
    int physical_width_mm, physical_height_mm;
};

struct drm_device_config {
    bool has_path;
    const char *path;

    size_t n_connector_configs;
    struct drm_connector_config *connector_configs;    
};

struct fbdev_device_config {
    const char *path;

    bool has_physical_dimensions;
    int physical_width_mm, physical_height_mm;
};

struct device_config {
    bool is_drm, is_fbdev;
    union {
        struct drm_device_config drm_config;
        struct fbdev_device_config fbdev_config;
    };
};

struct compositor_config {
    bool has_use_hardware_cursor, use_hardware_cursor;

    bool has_forced_pixel_format;
    enum pixfmt forced_pixel_format;

    size_t n_device_configs;
    struct device_config *device_configs;
};

struct clip_rect {
    struct quad rect;
    bool is_aa;

    struct aa_rect aa_rect;
    
    bool is_rounded;
    struct vec2f upper_left_corner_radius;
    struct vec2f upper_right_corner_radius;
    struct vec2f lower_right_corner_radius;
    struct vec2f lower_left_corner_radius;
};

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

typedef void (*compositor_frame_begin_cb_t)(void *userdata, uint64_t vblank_ns, uint64_t next_vblank_ns);

ATTR_MALLOC struct compositor *compositor_new(
    struct tracer *tracer,
    struct window *main_window
);

void compositor_destroy(struct compositor *compositor);

DECLARE_REF_OPS(compositor)

void compositor_get_view_geometry(struct compositor *compositor, struct view_geometry *view_geometry_out);

ATTR_PURE double compositor_get_refresh_rate(struct compositor *compositor);

int compositor_get_next_vblank(struct compositor *compositor, uint64_t *next_vblank_ns_out);

int compositor_set_platform_view(struct compositor *compositor, int64_t id, struct surface *surface);

struct surface *compositor_get_view_by_id_locked(struct compositor *compositor, int64_t view_id);

const FlutterCompositor *compositor_get_flutter_compositor(struct compositor *compositor);

int compositor_request_frame(struct compositor *compositor, compositor_frame_begin_cb_t cb, void *userdata);

bool compositor_has_egl_surface(struct compositor *compositor);

EGLSurface compositor_get_egl_surface(struct compositor *compositor);

int compositor_get_event_fd(struct compositor *compositor);

int compositor_on_event_fd_ready(struct compositor *compositor);

ATTR_PURE EGLConfig egl_choose_config_with_pixel_format(EGLDisplay egl_display, const EGLint *config_attribs, enum pixfmt pixel_format);


struct fl_layer_composition;

struct fl_layer_composition *fl_layer_composition_new(size_t n_layers);
void fl_layer_composition_destroy(struct fl_layer_composition *composition);
DECLARE_REF_OPS(fl_layer_composition)

size_t fl_layer_composition_get_n_layers(struct fl_layer_composition *composition);
struct fl_layer *fl_layer_composition_peek_layer(struct fl_layer_composition *composition, int layer);

#endif // _FLUTTERPI_INCLUDE_COMPOSITOR_NG_H
