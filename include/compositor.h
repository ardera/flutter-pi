#ifndef _COMPOSITOR_H
#define _COMPOSITOR_H

#include <stdint.h>

#include <gbm.h>
#include <flutter_embedder.h>

#include <collection.h>
#include <modesetting.h>

#define LOG_COMPOSITOR_ERROR(...) fprintf(stderr, "[compositor]" __VA_ARGS__)

typedef int (*platform_view_mount_cb)(
    int64_t view_id,
    struct drmdev_atomic_req *req,
    const FlutterPlatformViewMutation **mutations,
    size_t num_mutations,
    int offset_x,
    int offset_y,
    int width,
    int height,
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
    const FlutterPlatformViewMutation **mutations,
    size_t num_mutations,
    int offset_x,
    int offset_y,
    int width,
    int height,
    int zpos,
    void *userdata
);

typedef int (*platform_view_present_cb)(
    int64_t view_id,
    struct drmdev_atomic_req *req,
    const FlutterPlatformViewMutation **mutations,
    size_t num_mutations,
    int offset_x,
    int offset_y,
    int width,
    int height,
    int zpos,
    void *userdata
);

typedef uint64_t (*flutter_engine_get_current_time_t)();
typedef void (*flutter_engine_trace_event_duration_begin_t)(const char* name);
typedef void (*flutter_engine_trace_event_duration_end_t)(const char* name);
typedef void (*flutter_engine_trace_event_instant_t)(const char* name);

struct compositor;

/*
struct window_surface_backing_store {
    struct compositor *compositor;
    struct gbm_surface *gbm_surface;
    struct gbm_bo *current_front_bo;
    uint32_t drm_plane_id;
};
*/

struct drm_rbo {
    struct drmdev *drmdev;
	struct libegl *libegl;
    EGLDisplay display;
    EGLImage egl_image;
    GLuint gl_rbo_id;
    uint32_t gem_handle;
    uint32_t gem_stride;
    uint32_t drm_fb_id;
};

struct drm_fb {
    struct gbm_bo *bo;
    uint32_t fb_id;
    struct drmdev *drmdev;
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

/**
 * @brief Create a new compositor. One compositor instance uses one drmdev exclusively.
 */
struct compositor *compositor_new(
	struct drmdev *drmdev,
	struct gbm_surface *gbm_surface,
	struct libegl *libegl,
	EGLDisplay display,
	EGLSurface surface,
	EGLContext context,
	struct egl_display_info *display_info,
	flutter_engine_get_current_time_t get_current_time,
	flutter_engine_trace_event_duration_begin_t trace_event_begin,
	flutter_engine_trace_event_duration_end_t trace_event_end,
	flutter_engine_trace_event_instant_t trace_event_instant
);

/**
 * @brief Update the engine callbacks for this compositor.
 */
void compositor_set_engine_callbacks(
    struct compositor *compositor,
    flutter_engine_get_current_time_t get_current_time,
	flutter_engine_trace_event_duration_begin_t trace_event_begin,
	flutter_engine_trace_event_duration_end_t trace_event_end,
	flutter_engine_trace_event_instant_t trace_event_instant
);

/**
 * @brief Fill the given flutter compositor with the callbacks and userdata for this compositor.
 */
void compositor_fill_flutter_compositor(
    struct compositor *compositor,
    FlutterCompositor *flutter_compositor
);

/**
 * @brief Destroy the compositor, freeing all allocated resources.
 * 
 * The engine should be stopped before this method is called. It'd cause errors
 * to call any of the functions in the FlutterCompositor interface after this function has been called. 
 */
void compositor_destroy(
	struct compositor *compositor
);

int compositor_on_page_flip(
    struct compositor *compositor,
	uint32_t sec,
	uint32_t usec
);

int compositor_put_view_callbacks(
    struct compositor *compositor,
    int64_t view_id,
    platform_view_mount_cb mount,
    platform_view_unmount_cb unmount,
    platform_view_update_view_cb update_view,
    platform_view_present_cb present,
    void *userdata
);

int compositor_remove_view_callbacks(
    struct compositor *compositor,
    int64_t view_id
);

int compositor_apply_cursor_state(
	struct compositor *compositor,
	bool is_enabled,
	int rotation,
	double device_pixel_ratio
);

int compositor_set_cursor_pos(
    struct compositor *compositor,
    int x,
    int y
);


#endif