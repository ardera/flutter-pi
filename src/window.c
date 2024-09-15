// SPDX-License-Identifier: MIT
/*
 * window object
 *
 * - a window is something where flutter graphics can be presented on.
 *
 * Copyright (c) 2022, Hannes Winkler <hanneswinkler2000@web.de>
 */

#define _GNU_SOURCE
#include "window.h"

#include <errno.h>
#include <stdlib.h>

#include <pthread.h>

#include <flutter_embedder.h>

#include "compositor_ng.h"
#include "cursor.h"
#include "flutter-pi.h"
#include "frame_scheduler.h"
#include "kms/req_builder.h"
#include "kms/resources.h"
#include "render_surface.h"
#include "surface.h"
#include "tracer.h"
#include "user_input.h"
#include "util/collection.h"
#include "util/logging.h"
#include "util/refcounting.h"
#include "window_private.h"

#include "config.h"

#ifdef HAVE_EGL_GLES2
    #include "egl_gbm_render_surface.h"
    #include "gl_renderer.h"
#endif

#ifdef HAVE_VULKAN
    #include "vk_gbm_render_surface.h"
    #include "vk_renderer.h"
#endif

void window_destroy(struct window *window);

DEFINE_STATIC_LOCK_OPS(window, lock)
DEFINE_REF_OPS(window, n_refs)

static void fill_view_matrices(
    drm_plane_transform_t transform,
    int display_width,
    int display_height,
    struct mat3f *display_to_view_transform_out,
    struct mat3f *view_to_display_transform_out
) {
    assert(PLANE_TRANSFORM_IS_ONLY_ROTATION(transform));

    if (transform.rotate_0) {
        *view_to_display_transform_out = MAT3F_TRANSLATION(0, 0);

        *display_to_view_transform_out = MAT3F_TRANSLATION(0, 0);
    } else if (transform.rotate_90) {
        *view_to_display_transform_out = MAT3F_ROTZ(90);
        view_to_display_transform_out->transX = display_width;

        *display_to_view_transform_out = MAT3F_ROTZ(-90);
        display_to_view_transform_out->transY = display_width;
    } else if (transform.rotate_180) {
        *view_to_display_transform_out = MAT3F_ROTZ(180);
        view_to_display_transform_out->transX = display_width;
        view_to_display_transform_out->transY = display_height;

        *display_to_view_transform_out = MAT3F_ROTZ(-180);
        display_to_view_transform_out->transX = display_width;
        display_to_view_transform_out->transY = display_height;
    } else if (transform.rotate_270) {
        *view_to_display_transform_out = MAT3F_ROTZ(270);
        view_to_display_transform_out->transY = display_height;

        *display_to_view_transform_out = MAT3F_ROTZ(-270);
        display_to_view_transform_out->transX = display_height;
    }
}

int window_init(
    // clang-format off
    struct window *window,
    struct tracer *tracer,
    struct frame_scheduler *scheduler,
    bool has_rotation, drm_plane_transform_t rotation,
    bool has_orientation, enum device_orientation orientation,
    int width, int height,
    bool has_dimensions, int width_mm, int height_mm,
    double refresh_rate,
    bool has_forced_pixel_format, enum pixfmt forced_pixel_format,
    enum renderer_type renderer_type,
    struct gl_renderer *gl_renderer,
    struct vk_renderer *vk_renderer
    // clang-format on
) {
    enum device_orientation original_orientation;
    float pixel_ratio;

    ASSERT_NOT_NULL(window);
    ASSERT_NOT_NULL(tracer);
    ASSERT_NOT_NULL(scheduler);
    assert(!has_rotation || PLANE_TRANSFORM_IS_ONLY_ROTATION(rotation));
    assert(!has_orientation || ORIENTATION_IS_VALID(orientation));
    assert(!has_dimensions || (width_mm > 0 && height_mm > 0));

#if !defined(HAVE_VULKAN)
    ASSUME(renderer_type != kVulkan_RendererType);
#endif

#if !defined(HAVE_EGL_GLES2)
    ASSUME(renderer_type != kOpenGL_RendererType);
#endif

    // if opengl --> gl_renderer != NULL && vk_renderer == NULL
    assert(renderer_type != kOpenGL_RendererType || (gl_renderer != NULL && vk_renderer == NULL));

    // if vulkan --> vk_renderer != NULL && gl_renderer == NULL
    assert(renderer_type != kVulkan_RendererType || (vk_renderer != NULL && gl_renderer == NULL));

    memset(window, 0, sizeof *window);

    if (has_dimensions == false) {
        LOG_DEBUG(
            "WARNING: display didn't provide valid physical dimensions. The device-pixel ratio will default "
            "to 1.0, which may not be the fitting device-pixel ratio for your display. \n"
            "Use the `-d` commandline parameter to specify the physical dimensions of your display.\n"
        );
        pixel_ratio = 1.0;
    } else {
        pixel_ratio = (10.0f * width) / (width_mm * 38.0f);

        int horizontal_dpi = (int) (width / (width_mm / 25.4));
        int vertical_dpi = (int) (height / (height_mm / 25.4));

        if (horizontal_dpi != vertical_dpi) {
            // See https://github.com/flutter/flutter/issues/71865 for current status of this issue.
            LOG_DEBUG("INFO: display has non-square pixels. Non-square-pixels are not supported by flutter.\n");
        }
    }

    assert(!has_rotation || PLANE_TRANSFORM_IS_ONLY_ROTATION(rotation));

    if (width > height) {
        original_orientation = kLandscapeLeft;
    } else {
        original_orientation = kPortraitUp;
    }

    if (!has_rotation && !has_orientation) {
        rotation = PLANE_TRANSFORM_ROTATE_0;
        orientation = original_orientation;
        has_rotation = true;
        has_orientation = true;
    } else if (!has_orientation) {
        drm_plane_transform_t r = rotation;
        orientation = original_orientation;
        while (r.u64 != PLANE_TRANSFORM_ROTATE_0.u64) {
            orientation = ORIENTATION_ROTATE_CW(orientation);
            r = PLANE_TRANSFORM_ROTATE_CCW(r);
        }
        has_orientation = true;
    } else if (!has_rotation) {
        enum device_orientation o = orientation;
        rotation = PLANE_TRANSFORM_ROTATE_0;
        while (o != original_orientation) {
            rotation = PLANE_TRANSFORM_ROTATE_CW(rotation);
            o = ORIENTATION_ROTATE_CCW(o);
        }
        has_rotation = true;
    } else {
        enum device_orientation o = orientation;
        drm_plane_transform_t r = rotation;
        while (r.u64 != PLANE_TRANSFORM_ROTATE_0.u64) {
            r = PLANE_TRANSFORM_ROTATE_CCW(r);
            o = ORIENTATION_ROTATE_CCW(o);
        }

        if (ORIENTATION_IS_LANDSCAPE(o) && !(width >= height)) {
            LOG_DEBUG(
                "Explicit orientation and rotation given, but orientation is inconsistent with orientation. (display "
                "is more high than wide, but de-rotated orientation is landscape)\n"
            );
        } else if (ORIENTATION_IS_PORTRAIT(o) && !(height >= width)) {
            LOG_DEBUG(
                "Explicit orientation and rotation given, but orientation is inconsistent with orientation. (display "
                "is more wide than high, but de-rotated orientation is portrait)\n"
            );
        }

        original_orientation = o;
    }

    assert(has_orientation && has_rotation);

    fill_view_matrices(rotation, width, height, &window->display_to_view_transform, &window->view_to_display_transform);

    pthread_mutex_init(&window->lock, NULL);
    window->n_refs = REFCOUNT_INIT_1;
    window->tracer = tracer_ref(tracer);
    window->frame_scheduler = frame_scheduler_ref(scheduler);
    window->refresh_rate = refresh_rate;
    window->pixel_ratio = pixel_ratio;
    window->has_dimensions = has_dimensions;
    window->width_mm = width_mm;
    window->height_mm = height_mm;
    window->view_size = rotation.rotate_90 || rotation.rotate_270 ? VEC2F(height, width) : VEC2F(width, height);
    window->display_size = VEC2F(width, height);
    window->rotation = rotation;
    window->orientation = orientation;
    window->original_orientation = original_orientation;
    window->has_forced_pixel_format = has_forced_pixel_format;
    window->forced_pixel_format = forced_pixel_format;
    window->composition = NULL;
    window->renderer_type = kOpenGL_RendererType;
    window->gl_renderer = NULL;
    window->vk_renderer = NULL;
    window->render_surface = NULL;
    window->cursor_enabled = false;
    window->cursor_pos = VEC2F(0, 0);
    if (gl_renderer != NULL) {
#ifdef HAVE_EGL_GLES2
        window->gl_renderer = gl_renderer_ref(gl_renderer);
#else
        UNREACHABLE();
#endif
    }
    if (vk_renderer != NULL) {
#ifdef HAVE_VULKAN
        window->vk_renderer = vk_renderer_ref(vk_renderer);
#else
        UNREACHABLE();
#endif
    } else {
        window->vk_renderer = NULL;
    }
    window->ops.deinit = window_deinit;
    return 0;
}

void window_deinit(struct window *window) {
    // It's possible we're destroying the window before any frame was presented.
    if (window->composition != NULL) {
        fl_layer_composition_unref(window->composition);
    }

    frame_scheduler_unref(window->frame_scheduler);
    tracer_unref(window->tracer);
    pthread_mutex_destroy(&window->lock);
}

void window_destroy(struct window *window) {
    ASSERT_NOT_NULL(window);
    ASSERT_NOT_NULL(window->ops.deinit);

    window->ops.deinit(window);
    free(window);
}

int window_push_composition(struct window *window, struct fl_layer_composition *composition) {
    ASSERT_NOT_NULL(window);
    ASSERT_NOT_NULL(composition);
    ASSERT_NOT_NULL(window->ops.push_composition);
    return window->ops.push_composition(window, composition);
}

struct view_geometry window_get_view_geometry(struct window *window) {
    ASSERT_NOT_NULL(window);

    window_lock(window);
    struct view_geometry geometry = {
        .view_size = window->view_size,
        .display_size = window->display_size,
        .display_to_view_transform = window->display_to_view_transform,
        .view_to_display_transform = window->view_to_display_transform,
        .device_pixel_ratio = window->pixel_ratio,
    };
    window_unlock(window);

    return geometry;
}

double window_get_refresh_rate(struct window *window) {
    ASSERT_NOT_NULL(window);

    return window->refresh_rate;
}

int window_get_next_vblank(struct window *window, uint64_t *next_vblank_ns_out) {
    ASSERT_NOT_NULL(window);
    ASSERT_NOT_NULL(next_vblank_ns_out);
    (void) window;
    (void) next_vblank_ns_out;

    /// TODO: Implement
    UNIMPLEMENTED();

    return 0;
}

#ifdef HAVE_EGL_GLES2
bool window_has_egl_surface(struct window *window) {
    ASSERT_NOT_NULL(window);
    ASSERT_NOT_NULL(window->ops.has_egl_surface);
    return window->ops.has_egl_surface(window);
}

EGLSurface window_get_egl_surface(struct window *window) {
    ASSERT_NOT_NULL(window);
    ASSERT_NOT_NULL(window->ops.get_egl_surface);
    return window->ops.get_egl_surface(window);
}
#endif

/// TODO: Once we enable the backing store cache, we can actually sanely manage lifetimes and
///       rename this to window_create_render_surface.
struct render_surface *window_get_render_surface(struct window *window, struct vec2i size) {
    ASSERT_NOT_NULL(window);
    ASSERT_NOT_NULL(window->ops.get_render_surface);
    return window->ops.get_render_surface(window, size);
}

bool window_is_cursor_enabled(struct window *window) {
    bool enabled;

    ASSERT_NOT_NULL(window);

    window_lock(window);
    enabled = window->cursor_enabled;
    window_unlock(window);

    return enabled;
}

int window_set_cursor(
    // clang-format off
    struct window *window,
    bool has_enabled, bool enabled,
    bool has_kind, enum pointer_kind kind,
    bool has_pos, struct vec2i pos
    // clang-format on
) {
    int ok;

    ASSERT_NOT_NULL(window);
    ASSERT_NOT_NULL(window->ops.set_cursor_locked);

    window_lock(window);

    ok = window->ops.set_cursor_locked(window, has_enabled, enabled, has_kind, kind, has_pos, pos);

    window_unlock(window);

    return ok;
}

struct vec2f window_transform_ndc_to_view(struct window *window, struct vec2f ndc) {
    ASSERT_NOT_NULL(window);

    struct vec2f view_pos;

    window_lock(window);
    view_pos = transform_point(window->ndc_to_view_transform, ndc);
    window_unlock(window);

    return view_pos;
}

void window_on_input(
    struct window *window,
    size_t n_events,
    const struct user_input_event *events,
    const struct fl_pointer_event_interface *pointer_event_interface,
    int64_t view_id
) {
    (void) window;
    (void) n_events;
    (void) events;

    size_t n_fl_events = 0;
    FlutterPointerEvent fl_events[64];

    for (size_t i = 0; i < n_events; i++) {
        const struct user_input_event *event = events + i;

        if (n_fl_events >= ARRAY_SIZE(fl_events)) {
            if (pointer_event_interface != NULL) {
                pointer_event_interface->send_pointer_event(pointer_event_interface->engine, fl_events, n_fl_events);
            }
            n_fl_events = 0;
        }

        FlutterPointerEvent *fl_event = fl_events + n_fl_events;

        memset(fl_event, 0, sizeof *fl_event);
        fl_event->struct_size = sizeof *fl_event;
        fl_event->phase = kCancel;
        fl_event->timestamp = event->timestamp;
        fl_event->x = 0.0;
        fl_event->y = 0.0;
        fl_event->device = event->global_slot_id;
        fl_event->signal_kind = kFlutterPointerSignalKindNone;
        fl_event->scroll_delta_x = 0.0;
        fl_event->scroll_delta_y = 0.0;
        fl_event->device_kind = kFlutterPointerDeviceKindTouch;
        fl_event->buttons = 0;
        fl_event->pan_x = 0.0;
        fl_event->pan_y = 0.0;
        fl_event->scale = 0.0;
        fl_event->rotation = 0.0;
        fl_event->view_id = view_id;
        switch (event->type) {
            case USER_INPUT_DEVICE_ADDED:
            case USER_INPUT_DEVICE_REMOVED: goto skip;
            case USER_INPUT_SLOT_ADDED:
            case USER_INPUT_SLOT_REMOVED: {
                fl_event->phase = event->type == USER_INPUT_SLOT_ADDED ? kAdd : kRemove;
                fl_event->device = event->global_slot_id;
                if (event->slot_type == USER_INPUT_SLOT_POINTER) {
                    fl_event->device_kind = kFlutterPointerDeviceKindMouse;
                } else if (event->slot_type == USER_INPUT_SLOT_TOUCH) {
                    fl_event->device_kind = kFlutterPointerDeviceKindTouch;
                } else if (event->slot_type == USER_INPUT_SLOT_TABLET_TOOL) {
                    fl_event->device_kind = kFlutterPointerDeviceKindStylus;
                }
                break;
            }
            case USER_INPUT_POINTER: {
                mutex_lock(&window->lock);

                if (event->pointer.is_absolute) {
                    window->cursor_pos.x = CLAMP(event->pointer.position_ndc.x * window->view_size.x, 0.0, window->view_size.x);
                    window->cursor_pos.y = CLAMP(event->pointer.position_ndc.y * window->view_size.y, 0.0, window->view_size.y);
                } else {
                    window->cursor_pos.x = CLAMP(window->cursor_pos.x + event->pointer.delta.x, 0.0, window->view_size.x);
                    window->cursor_pos.y = CLAMP(window->cursor_pos.y + event->pointer.delta.y, 0.0, window->view_size.y);
                }

                fl_event->x = window->cursor_pos.x;
                fl_event->y = window->cursor_pos.y;

                mutex_unlock(&window->lock);

                if (event->pointer.changed_buttons & kFlutterPointerButtonMousePrimary) {
                    if (event->pointer.buttons & kFlutterPointerButtonMousePrimary) {
                        fl_event->phase = kDown;
                    } else {
                        fl_event->phase = kUp;
                    }
                } else {
                    if (event->pointer.buttons & kFlutterPointerButtonMousePrimary) {
                        fl_event->phase = kMove;
                    } else {
                        fl_event->phase = kHover;
                    }
                }

                if (event->pointer.scroll_delta.x != 0.0 || event->pointer.scroll_delta.y != 0.0) {
                    fl_event->signal_kind = kFlutterPointerSignalKindScroll;
                    fl_event->scroll_delta_x = event->pointer.scroll_delta.x;
                    fl_event->scroll_delta_y = event->pointer.scroll_delta.y;
                }

                fl_event->device_kind = kFlutterPointerDeviceKindMouse;
                fl_event->buttons = event->pointer.buttons;
                break;
            }
            case USER_INPUT_TOUCH: {
                if (event->touch.down_changed) {
                    fl_event->phase = event->touch.down ? kDown : kUp;
                } else {
                    fl_event->phase = kMove;
                }

                fl_event->device_kind = kFlutterPointerDeviceKindTouch;
                fl_event->x = event->touch.position_ndc.x * window->view_size.x;
                fl_event->y = event->touch.position_ndc.y * window->view_size.y;
                break;
            }
            case USER_INPUT_TABLET_TOOL: {
                if (event->tablet.tip_changed) {
                    fl_event->phase = event->tablet.tip ? kDown : kUp;
                } else {
                    fl_event->phase = event->tablet.tip ? kMove : kHover;
                }
                fl_event->device_kind = kFlutterPointerDeviceKindStylus;
                fl_event->x = event->tablet.position_ndc.x * window->view_size.x;
                fl_event->y = event->tablet.position_ndc.y * window->view_size.y;
                break;
            }
            default: break;
        }

        n_fl_events++;

skip:
        continue;
    }

    if (pointer_event_interface != NULL && n_fl_events > 0) {
        pointer_event_interface->send_pointer_event(pointer_event_interface->engine, fl_events, n_fl_events);
    }
}

input_device_match_score_t window_match_input_device(struct window *window, struct user_input_device *device) {
    ASSERT_NOT_NULL(window);
    ASSERT_NOT_NULL(device);

    if (window->ops.match_input_device == NULL) {
        return -1;
    } else {
        return window->ops.match_input_device(window, device);
    }
}
