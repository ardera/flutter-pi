#ifndef _COMPOSITOR_H
#define _COMPOSITOR_H

#include <stdint.h>

#include <gbm.h>
#include <flutter_embedder.h>

#include <collection.h>
#include <modesetting.h>

struct platform_view_params;

typedef int (*platform_view_mount_cb)(
    int64_t view_id,
    struct drmdev_atomic_req *req,
    const struct platform_view_params *params,
    int zpos,
    void *userdata  
);

typedef int (*platform_view_unmount_cb)(
    int64_t view_id,
    struct drmdev_atomic_req *req,
    void *userdata
);

typedef int (*platform_view_update_view_cb)(
    int64_t view_id,
    struct drmdev_atomic_req *req,
    const struct platform_view_params *params,
    int zpos,
    void *userdata
);

typedef int (*platform_view_present_cb)(
    int64_t view_id,
    struct drmdev_atomic_req *req,
    const struct platform_view_params *params,
    int zpos,
    void *userdata
);

struct point {
    double x, y;
};

struct quad {
    struct point top_left, top_right, bottom_left, bottom_right;
};

struct aa_rect {
    struct point offset, size;
};

static inline struct aa_rect get_aa_bounding_rect(const struct quad _rect) {
    double l = min(min(min(_rect.top_left.x, _rect.top_right.x), _rect.bottom_left.x), _rect.bottom_right.x);
	double r = max(max(max(_rect.top_left.x, _rect.top_right.x), _rect.bottom_left.x), _rect.bottom_right.x);
	double t = min(min(min(_rect.top_left.y, _rect.top_right.y), _rect.bottom_left.y), _rect.bottom_right.y);
	double b = max(max(max(_rect.top_left.y, _rect.top_right.y), _rect.bottom_left.y), _rect.bottom_right.y);

    return (struct aa_rect) {
        .offset.x = l,
        .offset.y = t,
        .size.x = r - l,
        .size.y = b - t
    };
}

static inline struct quad get_quad(const struct aa_rect rect) {
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

static inline void apply_transform_to_point(const FlutterTransformation transform, struct point *point) {
    apply_flutter_transformation(transform, &point->x, &point->y);
}

static inline void apply_transform_to_quad(const FlutterTransformation transform, struct quad *rect) {
	apply_transform_to_point(transform, &rect->top_left);
	apply_transform_to_point(transform, &rect->top_right);
	apply_transform_to_point(transform, &rect->bottom_left);
	apply_transform_to_point(transform, &rect->bottom_right);
}

static inline struct quad apply_transform_to_aa_rect(const FlutterTransformation transform, const struct aa_rect rect) {
    struct quad _quad = get_quad(rect);
    apply_transform_to_quad(transform, &_quad);
    return _quad;
}


struct platform_view_params {
    struct quad rect;
    double rotation;
    struct clip_rect *clip_rects;
    size_t n_clip_rects;
    double opacity;
};

struct compositor {
    struct drmdev *drmdev;

    /**
     * @brief Contains a struct for each existing platform view, containing the view id
     * and platform view callbacks.
     * 
     * @see compositor_set_view_callbacks compositor_remove_view_callbacks
     */
    struct concurrent_pointer_set cbs;

    /**
     * @brief Whether the compositor should invoke @ref rendertarget_gbm_new the next time
     * flutter creates a backing store. Otherwise @ref rendertarget_nogbm_new is invoked.
     * 
     * It's only possible to have at most one GBM-Surface backed backing store (== @ref rendertarget_gbm). So the first
     * time @ref on_create_backing_store is invoked, a GBM-Surface backed backing store is returned and after that,
     * only backing stores with @ref rendertarget_nogbm.
     */
    bool should_create_window_surface_backing_store;

    /**
     * @brief Whether the display mode was already applied. (Resolution, Refresh rate, etc)
     * If it wasn't already applied, it will be the first time @ref on_present_layers
     * is invoked.
     */
    bool has_applied_modeset;

    FlutterCompositor flutter_compositor;

    /**
     * @brief A cache of rendertargets that are not currently in use for
     * any flutter layers and can be reused.
     * 
     * Make sure to destroy all stale rendertargets before presentation so all the DRM planes
     * that are reserved by any stale rendertargets get freed.
     */
    struct concurrent_pointer_set stale_rendertargets;

    /**
     * @brief Whether the mouse cursor is currently enabled and visible.
     */

    struct {
        bool is_enabled;
        int cursor_size;
        const struct cursor_icon *current_cursor;
        int current_rotation;
        int hot_x, hot_y;
        int x, y;

        bool has_buffer;
        int buffer_depth;
        int buffer_pitch;
        int buffer_width;
        int buffer_height;
        int buffer_size;
        uint32_t drm_fb_id;
        uint32_t gem_bo_handle;
        uint32_t *buffer;
    } cursor;

    /**
     * If true, @ref on_present_layers will commit blockingly.
     * 
     * It will also schedule a simulated page flip event on the main thread
     * afterwards so the frame queue works.
     * 
     * If false, @ref on_present_layers will commit nonblocking using page flip events,
     * like usual.
     */
    bool do_blocking_atomic_commits;
};

/*
struct window_surface_backing_store {
    struct compositor *compositor;
    struct gbm_surface *gbm_surface;
    struct gbm_bo *current_front_bo;
    uint32_t drm_plane_id;
};
*/

struct drm_rbo {
    EGLImage egl_image;
    GLuint gl_rbo_id;
    uint32_t gem_handle;
    uint32_t gem_stride;
    uint32_t drm_fb_id;
};

struct drm_fb {
    struct gbm_bo *bo;
    uint32_t fb_id;
};

/*
struct drm_fb_backing_store {   
    struct compositor *compositor;

    GLuint gl_fbo_id;
    struct drm_rbo rbos[2];
    
    // The front FB is the one GL is rendering to right now, similiar
    // to libgbm.
    int current_front_rbo;
    
    uint32_t drm_plane_id;
};

enum backing_store_type {
    kWindowSurface,
    kDrmFb
};

struct backing_store_metadata {
    enum backing_store_type type;
    union {
        struct window_surface_backing_store window_surface;
        struct drm_fb_backing_store drm_fb;
    };
};
*/

struct rendertarget_gbm {
    struct gbm_surface *gbm_surface;
    struct gbm_bo *current_front_bo;
};

/**
 * @brief No-GBM Rendertarget.
 * A type of rendertarget that is not backed by a GBM-Surface, used for rendering into DRM overlay planes.
 */
struct rendertarget_nogbm {
    GLuint gl_fbo_id;
    struct drm_rbo rbos[2];
    
    /**
     * @brief The index of the @ref drm_rbo in the @ref rendertarget_nogbm::rbos array that
     * OpenGL is currently rendering into.
     */
    int current_front_rbo;
};

struct rendertarget {
    bool is_gbm;

    struct compositor *compositor;

    union {
        struct rendertarget_gbm gbm;
        struct rendertarget_nogbm nogbm;
    };

    GLuint gl_fbo_id;

    void (*destroy)(struct rendertarget *target);
    int (*present)(
        struct rendertarget *target,
        struct drmdev_atomic_req *atomic_req,
        uint32_t drm_plane_id,
        int offset_x,
        int offset_y,
        int width,
        int height,
        int zpos
    );
    int (*present_legacy)(
        struct rendertarget *target,
        struct drmdev *drmdev,
        uint32_t drm_plane_id,
        int offset_x,
        int offset_y,
        int width,
        int height,
        int zpos,
        bool set_mode
    );
};

struct flutterpi_backing_store {
    struct rendertarget *target;
    FlutterBackingStore flutter_backing_store;
    bool should_free_on_next_destroy;
};

extern const FlutterCompositor flutter_compositor;

int compositor_on_page_flip(
	uint32_t sec,
	uint32_t usec
);

int compositor_set_view_callbacks(
    int64_t view_id,
    platform_view_mount_cb mount,
    platform_view_unmount_cb unmount,
    platform_view_update_view_cb update_view,
    platform_view_present_cb present,
    void *userdata
);

int compositor_remove_view_callbacks(
    int64_t view_id
);

int compositor_apply_cursor_state(
    bool is_enabled,
    int rotation,
    double device_pixel_ratio
);

int compositor_set_cursor_pos(int x, int y);

int compositor_initialize(
    struct drmdev *drmdev
);


#endif