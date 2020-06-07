#ifndef _COMPOSITOR_H
#define _COMPOSITOR_H

#include <stdint.h>

#include <gbm.h>
#include <flutter_embedder.h>

typedef int (*platform_view_present_cb)(
    int64_t view_id,
    const FlutterPlatformViewMutation **mutations,
    size_t num_mutations,
    int offset_x,
    int offset_y,
    int width,
    int height,
    int zpos,
    void *userdata
);

struct window_surface_backing_store {
    struct gbm_surface *gbm_surface;
    struct gbm_bo *current_front_bo;
};

struct drm_fb_backing_store {
    EGLImage egl_image;
    GLuint gl_fbo_id;
    GLuint gl_rbo_id;
    uint32_t gem_handle;
    uint32_t gem_stride;
    uint32_t drm_fb_id;
    uint32_t drm_plane_id;
    int64_t current_zpos;
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

uint32_t gbm_bo_get_drm_fb_id(struct gbm_bo *bo);

int compositor_set_platform_view_present_cb(int64_t view_id, platform_view_present_cb cb, void *userdata);

#endif