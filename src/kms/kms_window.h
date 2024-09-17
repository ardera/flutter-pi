#ifndef _FLUTTERPI_SRC_KMS_KMS_WINDOW_H
#define _FLUTTERPI_SRC_KMS_KMS_WINDOW_H

#include "pixel_format.h"
#include "window.h"

#include "config.h"

struct tracer;
struct frame_scheduler;
struct gl_renderer;
struct vk_renderer;
struct drmdev;
struct drm_resources;

MUST_CHECK struct window *kms_window_new(
    // clang-format off
    struct tracer *tracer,
    struct frame_scheduler *scheduler,
    enum renderer_type renderer_type,
    struct gl_renderer *gl_renderer,
    struct vk_renderer *vk_renderer,
    bool has_rotation, drm_plane_transform_t rotation,
    bool has_orientation, enum device_orientation orientation,
    bool has_explicit_dimensions, int width_mm, int height_mm,
    bool has_forced_pixel_format, enum pixfmt forced_pixel_format,
    struct drmdev *drmdev,
    struct drm_resources *resources,
    const char *desired_videomode
    // clang-format on
);

#endif  // _FLUTTERPI_SRC_KMS_KMS_WINDOW_H
