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

struct compositor;

struct point {
    double x, y;
};

#define POINT(_x, _y) ((struct point) {.x = _x, .y = _y})

struct quad {
    struct point top_left, top_right, bottom_left, bottom_right;
};

#define QUAD(_top_left, _top_right, _bottom_left, _bottom_right) ((struct quad) {.top_left = _top_left, .top_right = _top_right, .bottom_left = _bottom_left, .bottom_right = _bottom_right})
#define QUAD_FROM_COORDS(_x1, _y1, _x2, _y2, _x3, _y3, _x4, _y4) QUAD(POINT(_x1, _y1), POINT(_x2, _y2), POINT(_x3, _y3), POINT(_x4, _y4))

struct aa_rect {
    struct point offset, size;
};

#define AA_RECT(_offset, _size) ((struct aa_rect) {.offset = offset, .size = size})
#define AA_RECT_FROM_COORDS(offset_x, offset_y, width, height) ((struct aa_rect) {.offset = POINT(offset_x, offset_y), .size = POINT(width, height)})

ATTR_CONST static inline struct aa_rect get_aa_bounding_rect(const struct quad _rect) {
    double l = min(min(min(_rect.top_left.x, _rect.top_right.x), _rect.bottom_left.x), _rect.bottom_right.x);
	double r = max(max(max(_rect.top_left.x, _rect.top_right.x), _rect.bottom_left.x), _rect.bottom_right.x);
	double t = min(min(min(_rect.top_left.y, _rect.top_right.y), _rect.bottom_left.y), _rect.bottom_right.y);
	double b = max(max(max(_rect.top_left.y, _rect.top_right.y), _rect.bottom_left.y), _rect.bottom_right.y);
    return AA_RECT_FROM_COORDS(l, t, r - l, b - t);
}

ATTR_CONST static inline struct quad get_quad(const struct aa_rect rect) {
    return (struct quad) {
        .top_left = rect.offset,
        .top_right.x = rect.offset.x + rect.size.x,
        .top_right.y = rect.offset.y,
        .bottom_left.x = rect.offset.x,
        .bottom_left.y = rect.offset.y + rect.size.y,
        .bottom_right.x = rect.offset.x + rect.size.x,
        .bottom_right.y = rect.offset.y + rect.size.y
    };
}

ATTR_CONST static inline struct point transform_point(const FlutterTransformation transform, const struct point point) {
    return POINT(
        transform.scaleX*point.x + transform.skewX*point.y + transform.transX, 
        transform.skewY*point.x + transform.scaleY*point.y + transform.transY
    );
}

ATTR_CONST static inline struct quad transform_quad(const FlutterTransformation transform, const struct quad rect) {
    return QUAD(
        transform_point(transform, rect.top_left),
        transform_point(transform, rect.top_right),
        transform_point(transform, rect.bottom_left),
        transform_point(transform, rect.bottom_right)
    );
}

ATTR_CONST static inline struct quad transform_aa_rect(const FlutterTransformation transform, const struct aa_rect rect) {
    return transform_quad(transform, get_quad(rect));
}

ATTR_CONST static inline struct point point_swap_xy(const struct point point) {
    return POINT(point.y, point.x);
}

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

struct view_geometry {
    struct point view_size, display_size;
    FlutterTransformation display_to_view_transform;
	FlutterTransformation view_to_display_transform;
	double device_pixel_ratio;
};

struct clip_rect {
    struct quad rect;
    bool is_aa;

    struct aa_rect aa_rect;
    
    bool is_rounded;
    struct point upper_left_corner_radius;
    struct point upper_right_corner_radius;
    struct point lower_right_corner_radius;
    struct point lower_left_corner_radius;
};

/// TODO: Remove
enum fl_layer_type {
    kBackingStore_FlLayerType,
    kPlatformView_FlLayerType
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

enum present_mode {
    kDoubleBufferedVsync_PresentMode,
    kTripleBufferedVsync_PresentMode
};

struct drmdev;
struct compositor;

typedef void (*compositor_frame_begin_cb_t)(void *userdata, uint64_t vblank_ns, uint64_t next_vblank_ns);

ATTR_MALLOC struct compositor *compositor_new(
    struct drmdev *drmdev,
    struct tracer *tracer,
    struct gl_renderer *renderer,
    bool has_rotation, drm_plane_transform_t rotation,
    bool has_orientation, enum device_orientation orientation,
    bool has_explicit_dimensions, int width_mm, int height_mm,
    EGLConfig egl_config,
    bool has_forced_pixel_format,
    enum pixfmt forced_pixel_format,
    bool use_frame_requests,
    enum present_mode present_mode
);

ATTR_MALLOC struct compositor *compositor_new_vulkan(
    struct drmdev *drmdev,
    struct tracer *tracer,
    struct vk_renderer *renderer,
    bool has_rotation,
    drm_plane_transform_t rotation,
    bool has_orientation,
    enum device_orientation orientation,
    bool has_explicit_dimensions,
    int width_mm,
    int height_mm,
    bool has_forced_pixel_format,
    enum pixfmt forced_pixel_format,
    bool use_frame_requests,
    enum present_mode present_mode
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

#endif // _FLUTTERPI_INCLUDE_COMPOSITOR_NG_H
