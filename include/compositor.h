#ifndef _COMPOSITOR_H
#define _COMPOSITOR_H

#include <stdint.h>

#include <gbm.h>
#include <flutter_embedder.h>

#include <collection.h>
#include <modesetting.h>

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

struct flutterpi_compositor {
    struct drmdev *drmdev;
    struct concurrent_pointer_set cbs;
    struct concurrent_pointer_set planes;
    bool should_create_window_surface_backing_store;
    bool has_applied_modeset;
};

struct window_surface_backing_store {
    struct flutterpi_compositor *compositor;
    struct gbm_surface *gbm_surface;
    struct gbm_bo *current_front_bo;
    uint32_t drm_plane_id;
};

struct drm_rbo {
    EGLImage egl_image;
    GLuint gl_rbo_id;
    uint32_t gem_handle;
    uint32_t gem_stride;
    uint32_t drm_fb_id;
};

struct drm_fb_backing_store {   
    struct flutterpi_compositor *compositor;

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

int compositor_reserve_plane(
    uint32_t *plane_id_out
);

int compositor_free_plane(
    uint32_t plane_id
);

int compositor_initialize(
    struct drmdev *drmdev
);

#endif