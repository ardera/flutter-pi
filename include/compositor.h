#ifndef _COMPOSITOR_H
#define _COMPOSITOR_H

#include <stdint.h>

#include <gbm.h>
#include <flutter_embedder.h>
#include <event_loop.h>

#include <collection.h>
#include <modesetting.h>

#define LOG_COMPOSITOR_ERROR(format_str, ...) fprintf(stderr, "[compositor] %s: " format_str, __func__, ##__VA_ARGS__)

typedef int (*platform_view_mount_cb)(
    int64_t view_id,
    struct presenter *presenter,
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
    struct presenter *presenter,
    void *userdata
);

typedef int (*platform_view_update_view_cb)(
    int64_t view_id,
    struct presenter *presenter,
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
    struct presenter *presenter,
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
typedef void (*compositor_frame_begin_callback_t)(uint64_t vblank_nanos, uint64_t next_vblank_nanos, void *userdata);

struct compositor;

struct drm_fbo {
    EGLImage egl_image;
    GLuint gl_rbo_id;
    GLuint gl_fbo_id;
    uint32_t gem_handle;
    uint32_t gem_stride;
    uint32_t drm_fb_id;
};

struct drm_fb {
    struct kmsdev *dev;
    struct gbm_bo *bo;
    uint32_t fb_id;
};

struct gl_rendertarget_gbm {
    struct gbm_surface *gbm_surface;
    //struct gbm_bo *current_front_bo;
};

/**
 * @brief No-GBM Rendertarget.
 * A type of rendertarget that is not backed by a GBM-Surface, used for rendering into DRM overlay planes.
 */
struct gl_rendertarget_nogbm {
    struct kmsdev *kmsdev;
    struct renderer *renderer;
    struct drm_fbo fbo;
};

struct gl_rendertarget {
    bool is_gbm;

    union {
        struct gl_rendertarget_gbm gbm;
        struct gl_rendertarget_nogbm nogbm;
    };

    /**
     * @brief This used for adding this rendertarget to the compositors rendertarget cache
     * after it was disposed.
     */
    struct compositor *compositor;

    GLuint gl_fbo_id;

    void (*destroy)(struct gl_rendertarget *target);
    int (*present)(
        struct gl_rendertarget *target,
        struct presenter *presenter,
        int x, int y,
        int w, int h
    );
};

struct flutterpi_backing_store {
    struct gl_rendertarget *target;
    FlutterBackingStore flutter_backing_store;
    bool should_free_on_next_destroy;
};

enum graphics_output_type {
    kGraphicsOutputTypeKmsdev,
    kGraphicsOutputTypeFbdev
};

struct graphics_output {
    enum graphics_output_type type;
    union {
        struct kmsdev *kmsdev;
        struct fbdev *fbdev;
    };
};

struct flutter_tracing_interface {
    flutter_engine_get_current_time_t get_current_time;
	flutter_engine_trace_event_duration_begin_t trace_event_begin;
	flutter_engine_trace_event_duration_end_t trace_event_end;
	flutter_engine_trace_event_instant_t trace_event_instant;
};

/**
 * @brief Create a new compositor, basically 
 */
struct compositor *compositor_new(
    const struct graphics_output *output,
    FlutterRendererType renderer_type,
    struct renderer *renderer,
    const struct flutter_tracing_interface *tracing_interface,
    struct event_loop *evloop
);

void compositor_set_tracing_interface(
    struct compositor *compositor,
    const struct flutter_tracing_interface *tracing_interface
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
void compositor_destroy(struct compositor *compositor);

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

int compositor_request_frame(
	struct compositor *compositor,
	compositor_frame_begin_callback_t callback,
	void *userdata
);
int compositor_set_cursor_state(
	struct compositor *compositor,
	bool has_is_enabled,
	bool is_enabled,
	bool has_rotation,
	int rotation,
	bool has_device_pixel_ratio,
	double device_pixel_ratio
);

int compositor_set_cursor_pos(struct compositor *compositor, int x, int y);

#endif