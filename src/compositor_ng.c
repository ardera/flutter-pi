// SPDX-License-Identifier: MIT
/*
 * compositor-ng
 *
 * - a reimplementation of the flutter compositor
 * - takes flutter layers as input, composits them into multiple hw planes, outputs them to the modesetting interface
 *
 * Copyright (c) 2022, Hannes Winkler <hanneswinkler2000@web.de>
 */

#include <inttypes.h>
#include <math.h>
#include <stdlib.h>

#include <pthread.h>
#include <semaphore.h>

#ifdef HAS_GBM
#    include <gbm.h>
#endif
#include <flutter_embedder.h>
#include <systemd/sd-event.h>

#include <backing_store.h>
#include <collection.h>
#include <compositor_ng.h>
#include <egl.h>
#include <flutter-pi.h>
#include <gbm_surface_backing_store.h>
#include <vk_gbm_backing_store.h>
#include <gl_renderer.h>
#include <vk_renderer.h>
#include <modesetting.h>
#include <notifier_listener.h>
#include <pixel_format.h>
#include <surface.h>
#include <tracer.h>

FILE_DESCR("compositor-ng")


/**
 * @brief A nicer, ref-counted version of the FlutterLayer's passed by the engine to the present layer callback.
 *
 * Differences to the FlutterLayer's passed to the present layer callback:
 *  - for platform views:
 *    - struct platform_view* object as the platform view instead of int64_t view id
 *    - position is given as a quadrilateral or axis-aligned rectangle instead of a bunch of (broken) transforms
 *    - same for clip rects
 *    - opacity and rotation as individual numbers
 *  - for backing stores:
 *    - struct backing_store* object instead of FlutterBackingStore
 *    - offset & size as as a struct aa_rect
 *  - refcounted
 */

static struct fl_layer_composition *fl_layer_composition_new(size_t n_layers) {
    struct fl_layer_composition *composition;
    struct fl_layer *layers;

    composition = malloc((sizeof *composition) + (n_layers * sizeof *layers));
    if (composition == NULL) {
        return NULL;
    }

    composition->n_refs = REFCOUNT_INIT_1;
    composition->n_layers = n_layers;
    return composition;
}

static size_t fl_layer_composition_get_n_layers(struct fl_layer_composition *composition) {
    DEBUG_ASSERT_NOT_NULL(composition);
    return composition->n_layers;
}

static struct fl_layer *fl_layer_composition_peek_layer(struct fl_layer_composition *composition, int layer) {
    DEBUG_ASSERT_NOT_NULL(composition);
    DEBUG_ASSERT_NOT_NULL(composition->layers);
    DEBUG_ASSERT(layer >= 0 && layer < composition->n_layers);
    return composition->layers + layer;
}

static void fl_layer_composition_destroy(struct fl_layer_composition *composition) {
    DEBUG_ASSERT_NOT_NULL(composition);

    for (int i = 0; i < composition->n_layers; i++) {
        surface_unref(composition->layers[i].surface);
        if (composition->layers[i].props.clip_rects != NULL) {
            free(composition->layers[i].props.clip_rects);
        }
    }

    free(composition);
}

DEFINE_STATIC_REF_OPS(fl_layer_composition, n_refs)

struct frame_req {
    compositor_frame_begin_cb_t cb;
    void *userdata;
#ifdef DEBUG
    bool signaled;
#endif
};

/**
 * @brief A single display / screen / window that flutter-pi can display flutter contents on. For example, this
 * would be a drmdev with a specific CRTC, mode and connector, or a single fbdev.
 *
 */
struct window {
    struct compositor *compositor;

    pthread_mutex_t lock;

    /// Event tracing interface.
    struct tracer *tracer;

    /// DRM device for showing the output.
    struct drmdev *drmdev;

    /// GBM Device for allocating the graphics buffers.
    struct gbm_device *gbm_device;

    /// The last presented composition.
    struct fl_layer_composition *composition;

    /// @brief The main backing store.
    /// (Due to the flutter embedder API architecture, we always need to have
    /// a primary surface, other backing stores can only be framebuffers.)
    struct backing_store *backing_store;

    /// @brief The EGL/GL compatible backing store if this is a normal egl/gl window.
    struct gbm_surface_backing_store *egl_backing_store;

    /// @brief The vulkan compatible backing store if this is a vulkan window.
    struct vk_gbm_backing_store *vk_backing_store;

    /// The frame request queue.
    /// Normally, flutter would request a frame using the flutter engine vsync callback
    /// supplied by the embedder, and the embedder would queue it and respond to it when
    /// the engine can start rendering. However, since that callback is broken
    /// (sometimes multiple frames are requested but engine won't start rendering),
    /// instead we'll request and wait for frame begin in the window_push_composition function.
    /// That way we make sure we don't accidentally present two frames per vblank period.
    ///
    /// @ref use_frame_requests is true when flutter engine requests frames (so it's always false
    /// right now), false when we should request them ourselves.
    struct queue frame_req_queue;

    /// The selected connector, encoder, crtc and mode that we should present the flutter graphics on.
    struct drm_connector *selected_connector;
    struct drm_encoder *selected_encoder;
    struct drm_crtc *selected_crtc;
    drmModeModeInfo *selected_mode;

    /// Refresh rate of the selected mode.
    double refresh_rate;

    /// Flutter device pixel ratio (in the horizontal axis)
    /// Number of physical pixels per logical pixel.
    /// There are always 38 logical pixels per cm, or 96 per inch.
    /// This is roughly equivalent to DPI / 100.
    /// A device pixel ratio of 1.0 is roughly a dpi of 96, which is the most common
    /// dpi for full-hd desktop displays.
    /// To calculate this, the physical dimensions of the display are required.
    /// If there are no physical dimensions this will default to 1.0.
    double pixel_ratio;

    /// Whether we have physical screen dimensions and @ref width_mm and @ref height_mm
    /// contain usable values.
    bool has_dimensions;

    /// Width, height of the screen in millimeters.
    int width_mm, height_mm;

    /// The rotation we should apply to the flutter layers to present them on screen.
    drm_plane_transform_t rotation;

    /// The current device orientation and the original (startup) device orientation.
    /// @ref original_orientation is kLandscapeLeft for displays that are more wide than high,
    /// and kPortraitUp for displays that are more high than wide.
    ///
    /// @ref orientation should always equal to rotating @ref original_orientation clock-wise by the
    /// angle in the @ref rotation field.
    enum device_orientation orientation, original_orientation;

    /// @brief Matrix for transforming display coordinates to view coordinates.
    /// For example for transforming pointer events (which are in the display coordinate space)
    /// to flutter coordinates.
    /// Useful if for example flutter has specified a custom device orientation (for example kPortraitDown),
    /// in that case we of course also need to transform the touch coords.
    FlutterTransformation display_to_view_transform;

    /// @brief Matrix for transforming view coordinates to display coordinates.
    /// Can be used as a root surface transform, for fitting the flutter view into
    /// the desired display frame.
    /// Useful if for example flutter has specified a custom device orientation (for example kPortraitDown),
    /// because we need to rotate the flutter view in that case.
    FlutterTransformation view_to_display_transform;

    /// @brief True if flutter will request frames before attempting to render something.
    /// False if the window should sync to vblank internally.
    bool use_frame_requests;

    /// @brief True if we should set the mode on the next KMS req.
    bool set_mode;

    /// @brief True if we should set @ref set_mode to false on the next commit.
    bool set_set_mode;

    /// @brief True if we should use a specific pixel format.
    bool has_forced_pixel_format;

    /// @brief The forced pixel format if @ref has_forced_pixel_format is true.
    enum pixfmt forced_pixel_format;

    /// @brief The EGLConfig to use for creating any EGL surfaces.
    /// Can be EGL_NO_CONFIG_KHR if backing stores should automatically select one.
    EGLConfig egl_config;

    /// @brief How many buffers to use and how we should schedule frames.
    /// Doesn't change after initialization.
    enum present_mode present_mode;

    /// @brief If we're using triple buffering, this is the frame we're going to commit
    /// next, once we can commit a frame again. (I.e., once the previous frame is
    /// being scanned out)
    /// Can be NULL if we don't have a frame we can commit next yet.
    struct kms_req *next_frame;

    /// @brief If using triple buffering, this is true if the previous frame is already
    /// being shown on screen and the next frame can be immediately queued / committed to
    /// be shown on screen.
    /// If this is false, @ref next_frame should be set to the next frame and @ref window_on_pageflip
    /// will commit the next frame.
    bool present_immediately;

    /// @brief The EGL/OpenGL renderer used to create any GL backing stores.
    struct gl_renderer *renderer;

    /// @brief The vulkan renderer if this is a vulkan window.
    struct vk_renderer *vk_renderer;
};

static void fill_view_matrices(
    drm_plane_transform_t transform,
    int display_width,
    int display_height,
    FlutterTransformation *display_to_view_transform_out,
    FlutterTransformation *view_to_display_transform_out
) {
    DEBUG_ASSERT(PLANE_TRANSFORM_IS_ONLY_ROTATION(transform));

    if (transform.rotate_0) {
        *view_to_display_transform_out = FLUTTER_TRANSLATION_TRANSFORMATION(0, 0);

        *display_to_view_transform_out = FLUTTER_TRANSLATION_TRANSFORMATION(0, 0);
    } else if (transform.rotate_90) {
        *view_to_display_transform_out = FLUTTER_ROTZ_TRANSFORMATION(90);
        view_to_display_transform_out->transX = display_width;

        *display_to_view_transform_out = FLUTTER_ROTZ_TRANSFORMATION(-90);
        display_to_view_transform_out->transY = display_width;
    } else if (transform.rotate_180) {
        *view_to_display_transform_out = FLUTTER_ROTZ_TRANSFORMATION(180);
        view_to_display_transform_out->transX = display_width;
        view_to_display_transform_out->transY = display_height;

        *display_to_view_transform_out = FLUTTER_ROTZ_TRANSFORMATION(-180);
        display_to_view_transform_out->transX = display_width;
        display_to_view_transform_out->transY = display_height;
    } else if (transform.rotate_270) {
        *view_to_display_transform_out = FLUTTER_ROTZ_TRANSFORMATION(270);
        view_to_display_transform_out->transY = display_height;

        *display_to_view_transform_out = FLUTTER_ROTZ_TRANSFORMATION(-270);
        display_to_view_transform_out->transX = display_height;
    }
}

static int select_mode(
    struct drmdev *drmdev,
    struct drm_connector **connector_out,
    struct drm_encoder **encoder_out,
    struct drm_crtc **crtc_out,
    drmModeModeInfo **mode_out
) {
    struct drm_connector *connector;
    struct drm_encoder *encoder;
    struct drm_crtc *crtc;
    drmModeModeInfo *mode, *mode_iter;

    // find any connected connector
    for_each_connector_in_drmdev(drmdev, connector) {
        if (connector->variable_state.connection_state == kConnected_DrmConnectionState) {
            break;
        }
    }

    if (connector == NULL) {
        LOG_ERROR("Could not find a connected connector!\n");
        return EINVAL;
    }

    // Find the preferred mode (GPU drivers _should_ always supply a preferred mode, but of course, they don't)
    // Alternatively, find the mode with the highest width*height. If there are multiple modes with the same w*h,
    // prefer higher refresh rates. After that, prefer progressive scanout modes.
    mode = NULL;
    for_each_mode_in_connector(connector, mode_iter) {
        if (mode_iter->type & DRM_MODE_TYPE_PREFERRED) {
            mode = mode_iter;
            break;
        } else if (mode == NULL) {
            mode = mode_iter;
        } else {
            int area = mode_iter->hdisplay * mode_iter->vdisplay;
            int old_area = mode->hdisplay * mode->vdisplay;

            if ((area > old_area) || ((area == old_area) && (mode_iter->vrefresh > mode->vrefresh)) ||
                ((area == old_area) && (mode_iter->vrefresh == mode->vrefresh) &&
                 ((mode->flags & DRM_MODE_FLAG_INTERLACE) == 0))) {
                mode = mode_iter;
            }
        }
    }

    if (mode == NULL) {
        LOG_ERROR("Could not find a preferred output mode!\n");
        return EINVAL;
    }

    // Find the encoder that's linked to the connector right now
    for_each_encoder_in_drmdev(drmdev, encoder) {
        if (encoder->encoder->encoder_id == connector->committed_state.encoder_id) {
            break;
        }
    }

    // Otherwise use use any encoder that the connector supports linking to
    if (encoder == NULL) {
        for (int i = 0; i < connector->n_encoders; i++, encoder = NULL) {
            for_each_encoder_in_drmdev(drmdev, encoder) {
                if (encoder->encoder->encoder_id == connector->encoders[i]) {
                    break;
                }
            }

            if (encoder->encoder->possible_crtcs) {
                // only use this encoder if there's a crtc we can use with it
                break;
            }
        }
    }

    if (encoder == NULL) {
        LOG_ERROR("Could not find a suitable DRM encoder.\n");
        return EINVAL;
    }

    // Find the CRTC that's currently linked to this encoder
    for_each_crtc_in_drmdev(drmdev, crtc) {
        if (crtc->id == encoder->encoder->crtc_id) {
            break;
        }
    }

    // Otherwise use any CRTC that this encoder supports linking to
    if (crtc == NULL) {
        for_each_crtc_in_drmdev(drmdev, crtc) {
            if (encoder->encoder->possible_crtcs & crtc->bitmask) {
                // find a CRTC that is possible to use with this encoder
                break;
            }
        }
    }

    if (crtc == NULL) {
        LOG_ERROR("Could not find a suitable DRM CRTC.\n");
        return EINVAL;
    }

    *connector_out = connector;
    *encoder_out = encoder;
    *crtc_out = crtc;
    *mode_out = mode;
    return 0;
}

static int window_init(
    struct window *window,
    struct compositor *compositor,
    struct tracer *tracer,
    struct drmdev *drmdev,
    struct gl_renderer *renderer,
    bool has_rotation,
    drm_plane_transform_t rotation,
    bool has_orientation,
    enum device_orientation orientation,
    bool has_explicit_dimensions,
    int width_mm,
    int height_mm,
    bool use_frame_requests,
    bool has_forced_pixel_format,
    enum pixfmt forced_pixel_format,
    EGLConfig egl_config,
    enum present_mode present_mode
) {
    enum device_orientation original_orientation;
    struct drm_connector *selected_connector;
    struct drm_encoder *selected_encoder;
    struct drm_crtc *selected_crtc;
    drmModeModeInfo *selected_mode;
    double pixel_ratio;
    bool has_dimensions;
    int ok;

    DEBUG_ASSERT_NOT_NULL(window);
    DEBUG_ASSERT_NOT_NULL(compositor);
    DEBUG_ASSERT_NOT_NULL(tracer);
    DEBUG_ASSERT_NOT_NULL(drmdev);
    DEBUG_ASSERT_NOT_NULL(renderer);
    DEBUG_ASSERT(!has_rotation || PLANE_TRANSFORM_IS_ONLY_ROTATION(rotation));
    DEBUG_ASSERT(!has_orientation || ORIENTATION_IS_VALID(orientation));
    DEBUG_ASSERT(!has_explicit_dimensions || (width_mm > 0 && height_mm > 0));
    //DEBUG_ASSERT_EQUALS_MSG(present_mode, kDoubleBufferedVsync_PresentMode, "Only double buffered vsync supported right now.");

    ok = queue_init(&window->frame_req_queue, sizeof(struct frame_req), QUEUE_DEFAULT_MAX_SIZE);
    if (ok != 0) {
        return ENOMEM;
    }

    ok = select_mode(drmdev, &selected_connector, &selected_encoder, &selected_crtc, &selected_mode);
    if (ok != 0) {
        goto fail_deinit_queue;
    }

    if (has_explicit_dimensions) {
        has_dimensions = true;
    } else if (selected_connector->variable_state.width_mm % 10 || selected_connector->variable_state.height_mm % 10) {
        has_dimensions = true;
        width_mm = selected_connector->variable_state.width_mm;
        height_mm = selected_connector->variable_state.height_mm;
    } else if (selected_connector->type == DRM_MODE_CONNECTOR_DSI
        && selected_connector->variable_state.width_mm == 0
        && selected_connector->variable_state.height_mm == 0) {
        has_dimensions = true;
        width_mm = 155;
        height_mm = 86;
    } else {
        has_dimensions = false;
    }

    if (has_dimensions == false) {
        LOG_DEBUG(
            "WARNING: display didn't provide valid physical dimensions. The device-pixel ratio will default "
            "to 1.0, which may not be the fitting device-pixel ratio for your display. \n"
            "Use the `-d` commandline parameter to specify the physical dimensions of your display.\n"
        );
        pixel_ratio = 1.0;
    } else {
        pixel_ratio = (10.0 * selected_mode->hdisplay) / (width_mm * 38.0);

        int horizontal_dpi = (int) (selected_mode->hdisplay / (width_mm / 25.4));
        int vertical_dpi = (int) (selected_mode->vdisplay / (height_mm / 25.4));

        if (horizontal_dpi != vertical_dpi) {
            // See https://github.com/flutter/flutter/issues/71865 for current status of this issue.
            LOG_DEBUG("INFO: display has non-square pixels. Non-square-pixels are not supported by flutter.\n");
        }
    }

    DEBUG_ASSERT(!has_rotation || PLANE_TRANSFORM_IS_ONLY_ROTATION(rotation));

    if (selected_mode->hdisplay > selected_mode->vdisplay) {
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

        if (ORIENTATION_IS_LANDSCAPE(o) && !(selected_mode->hdisplay >= selected_mode->vdisplay)) {
            LOG_DEBUG(
                "Explicit orientation and rotation given, but orientation is inconsistent with orientation. (display "
                "is more high than wide, but de-rotated orientation is landscape)\n"
            );
        } else if (ORIENTATION_IS_PORTRAIT(o) && !(selected_mode->vdisplay >= selected_mode->hdisplay)) {
            LOG_DEBUG(
                "Explicit orientation and rotation given, but orientation is inconsistent with orientation. (display "
                "is more wide than high, but de-rotated orientation is portrait)\n"
            );
        }

        original_orientation = o;
    }

    DEBUG_ASSERT(has_orientation && has_rotation);

    fill_view_matrices(
        rotation,
        selected_mode->hdisplay,
        selected_mode->vdisplay,
        &window->display_to_view_transform,
        &window->view_to_display_transform
    );

    LOG_DEBUG_UNPREFIXED(
        "===================================\n"
        "display mode:\n"
        "  resolution: %" PRIu16 " x %" PRIu16
        "\n"
        "  refresh rate: %fHz\n"
        "  physical size: %dmm x %dmm\n"
        "  flutter device pixel ratio: %f\n"
        "===================================\n",
        selected_mode->hdisplay,
        selected_mode->vdisplay,
        mode_get_vrefresh(selected_mode),
        width_mm,
        height_mm,
        pixel_ratio
    );

    window->compositor = compositor;
    pthread_mutex_init(&window->lock, NULL);
    window->tracer = tracer_ref(tracer);
    window->drmdev = drmdev_ref(drmdev);
    window->gbm_device = drmdev_get_gbm_device(drmdev);
    window->composition = NULL;
    window->backing_store = NULL;
    window->egl_backing_store = NULL;
    window->vk_backing_store = NULL;
    window->selected_connector = selected_connector;
    window->selected_crtc = selected_crtc;
    window->selected_encoder = selected_encoder;
    window->selected_mode = selected_mode;
    window->refresh_rate = mode_get_vrefresh(selected_mode);
    window->has_dimensions = has_dimensions;
    window->width_mm = width_mm;
    window->height_mm = height_mm;
    window->pixel_ratio = pixel_ratio;
    window->use_frame_requests = use_frame_requests;
    window->rotation = rotation;
    window->orientation = orientation;
    window->original_orientation = original_orientation;
    window->set_mode = true;
    window->set_set_mode = true; /* doesn't really matter */
    window->has_forced_pixel_format = has_forced_pixel_format;
    window->forced_pixel_format = forced_pixel_format;
    window->egl_config = egl_config;
    window->present_mode = present_mode;
    window->present_immediately = true;
    window->next_frame = NULL;
    window->renderer = gl_renderer_ref(renderer);
    window->vk_renderer = NULL;
    window->vk_backing_store = NULL;
    return 0;

fail_deinit_queue:
    queue_deinit(&window->frame_req_queue);
    return ok;
}

static int window_init_vulkan(
    struct window *window,
    struct compositor *compositor,
    struct tracer *tracer,
    struct drmdev *drmdev,
    struct vk_renderer *renderer,
    bool has_rotation, drm_plane_transform_t rotation,
    bool has_orientation, enum device_orientation orientation,
    bool has_explicit_dimensions, int width_mm, int height_mm,
    bool use_frame_requests,
    bool has_forced_pixel_format,
    enum pixfmt forced_pixel_format,
    enum present_mode present_mode
) {
    enum device_orientation original_orientation;
    struct drm_connector *selected_connector;
    struct drm_encoder *selected_encoder;
    struct drm_crtc *selected_crtc;
    drmModeModeInfo *selected_mode;
    double pixel_ratio;
    bool has_dimensions;
    int ok;

    DEBUG_ASSERT_NOT_NULL(window);
    DEBUG_ASSERT_NOT_NULL(compositor);
    DEBUG_ASSERT_NOT_NULL(tracer);
    DEBUG_ASSERT_NOT_NULL(drmdev);
    DEBUG_ASSERT_NOT_NULL(renderer);
    DEBUG_ASSERT(!has_rotation || PLANE_TRANSFORM_IS_ONLY_ROTATION(rotation));
    DEBUG_ASSERT(!has_orientation || ORIENTATION_IS_VALID(orientation));
    DEBUG_ASSERT(!has_explicit_dimensions || (width_mm > 0 && height_mm > 0));
    //DEBUG_ASSERT_EQUALS_MSG(present_mode, kDoubleBufferedVsync_PresentMode, "Only double buffered vsync supported right now.");

    ok = queue_init(&window->frame_req_queue, sizeof(struct frame_req), QUEUE_DEFAULT_MAX_SIZE);
    if (ok != 0) {
        return ENOMEM;
    }

    ok = select_mode(drmdev, &selected_connector, &selected_encoder, &selected_crtc, &selected_mode);
    if (ok != 0) {
        goto fail_deinit_queue;
    }

    if (has_explicit_dimensions) {
        has_dimensions = true;
    } else if (selected_connector->variable_state.width_mm % 10 || selected_connector->variable_state.height_mm % 10) {
        has_dimensions = true;
        width_mm = selected_connector->variable_state.width_mm;
        height_mm = selected_connector->variable_state.height_mm;
    } else if (selected_connector->type == DRM_MODE_CONNECTOR_DSI
        && selected_connector->variable_state.width_mm == 0
        && selected_connector->variable_state.height_mm == 0) {
        has_dimensions = true;
        width_mm = 155;
        height_mm = 86;
    } else {
        has_dimensions = false;
    }

    if (has_dimensions == false) {
        LOG_DEBUG(
            "WARNING: display didn't provide valid physical dimensions. The device-pixel ratio will default "
            "to 1.0, which may not be the fitting device-pixel ratio for your display. \n"
            "Use the `-d` commandline parameter to specify the physical dimensions of your display.\n"
        );
        pixel_ratio = 1.0;
    } else {
        pixel_ratio = (10.0 * selected_mode->hdisplay) / (width_mm * 38.0);

        int horizontal_dpi = (int) (selected_mode->hdisplay / (width_mm / 25.4));
        int vertical_dpi = (int) (selected_mode->vdisplay / (height_mm / 25.4));

        if (horizontal_dpi != vertical_dpi) {
            // See https://github.com/flutter/flutter/issues/71865 for current status of this issue.
            LOG_DEBUG("INFO: display has non-square pixels. Non-square-pixels are not supported by flutter.\n");
        }
    }

    DEBUG_ASSERT(!has_rotation || PLANE_TRANSFORM_IS_ONLY_ROTATION(rotation));

    if (selected_mode->hdisplay > selected_mode->vdisplay) {
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

        if (ORIENTATION_IS_LANDSCAPE(o) && !(selected_mode->hdisplay >= selected_mode->vdisplay)) {
            LOG_DEBUG(
                "Explicit orientation and rotation given, but orientation is inconsistent with orientation. (display "
                "is more high than wide, but de-rotated orientation is landscape)\n"
            );
        } else if (ORIENTATION_IS_PORTRAIT(o) && !(selected_mode->vdisplay >= selected_mode->hdisplay)) {
            LOG_DEBUG(
                "Explicit orientation and rotation given, but orientation is inconsistent with orientation. (display "
                "is more wide than high, but de-rotated orientation is portrait)\n"
            );
        }

        original_orientation = o;
    }

    DEBUG_ASSERT(has_orientation && has_rotation);

    fill_view_matrices(
        rotation,
        selected_mode->hdisplay,
        selected_mode->vdisplay,
        &window->display_to_view_transform,
        &window->view_to_display_transform
    );

    LOG_DEBUG_UNPREFIXED(
        "===================================\n"
        "display mode:\n"
        "  resolution: %" PRIu16 " x %" PRIu16
        "\n"
        "  refresh rate: %fHz\n"
        "  physical size: %dmm x %dmm\n"
        "  flutter device pixel ratio: %f\n"
        "===================================\n",
        selected_mode->hdisplay,
        selected_mode->vdisplay,
        mode_get_vrefresh(selected_mode),
        width_mm,
        height_mm,
        pixel_ratio
    );

    window->compositor = compositor;
    pthread_mutex_init(&window->lock, NULL);
    window->tracer = tracer_ref(tracer);
    window->drmdev = drmdev_ref(drmdev);
    window->gbm_device = drmdev_get_gbm_device(drmdev);
    window->composition = NULL;
    window->backing_store = NULL;
    window->egl_backing_store = NULL;
    window->vk_backing_store = NULL;
    window->selected_connector = selected_connector;
    window->selected_crtc = selected_crtc;
    window->selected_encoder = selected_encoder;
    window->selected_mode = selected_mode;
    window->refresh_rate = mode_get_vrefresh(selected_mode);
    window->has_dimensions = has_dimensions;
    window->width_mm = width_mm;
    window->height_mm = height_mm;
    window->pixel_ratio = pixel_ratio;
    window->use_frame_requests = use_frame_requests;
    window->rotation = rotation;
    window->orientation = orientation;
    window->original_orientation = original_orientation;
    window->set_mode = true;
    window->set_set_mode = true; /* doesn't really matter */
    window->has_forced_pixel_format = true;
    window->forced_pixel_format = has_forced_pixel_format ? forced_pixel_format : kARGB8888;
    window->egl_config = EGL_NO_CONFIG_KHR;
    window->present_mode = present_mode;
    window->present_immediately = true;
    window->next_frame = NULL;
    window->renderer = NULL;
    window->vk_renderer = vk_renderer_ref(renderer);
    return 0;

    fail_deinit_queue:
    queue_deinit(&window->frame_req_queue);
    return ok;
}

MAYBE_UNUSED static void window_deinit(struct window *window) {
    struct kms_req_builder *builder;
    struct kms_req *req;
    int ok;

    builder = drmdev_create_request_builder(window->drmdev, window->selected_crtc->id);
    DEBUG_ASSERT_NOT_NULL(builder);

    ok = kms_req_builder_unset_mode(builder);
    DEBUG_ASSERT_EQUALS(ok, 0);

    req = kms_req_builder_build(builder);
    DEBUG_ASSERT_NOT_NULL(req);

    kms_req_builder_unref(builder);

    ok = kms_req_commit(req, true);
    DEBUG_ASSERT_EQUALS(ok, 0);
    (void) ok;

    kms_req_unref(req);
    drmdev_unref(window->drmdev);
    queue_deinit(&window->frame_req_queue);
    tracer_unref(window->tracer);
}

static struct window *window_ref(struct window *w) {
    DEBUG_ASSERT_NOT_NULL(w);
    compositor_ref(w->compositor);
    return w;
}

static void window_unref(struct window *w) {
    DEBUG_ASSERT_NOT_NULL(w);
    compositor_unref(w->compositor);
}

MAYBE_UNUSED static void window_unrefp(struct window **w) {
    DEBUG_ASSERT_NOT_NULL(w);
    window_unref(*w);
    *w = NULL;
}

DEFINE_STATIC_LOCK_OPS(window, lock)

static void window_get_view_geometry(struct window *window, struct view_geometry *view_geometry_out) {
    struct point display_size;

    display_size = POINT(window->selected_mode->hdisplay, window->selected_mode->vdisplay);

    *view_geometry_out = (struct view_geometry){
        .view_size = (window->rotation.rotate_90 || window->rotation.rotate_270)
            ? point_swap_xy(display_size)
            : display_size,
       .display_size = display_size,
       .display_to_view_transform = window->display_to_view_transform,
       .view_to_display_transform = window->view_to_display_transform,
       .device_pixel_ratio = window->pixel_ratio,
    };
}

ATTR_PURE static double window_get_refresh_rate(struct window *window) {
    DEBUG_ASSERT_NOT_NULL(window);
    return window->refresh_rate;
}

static int window_get_next_vblank(struct window *window, uint64_t *next_vblank_ns_out) {
    uint64_t last_vblank;
    int ok;

    DEBUG_ASSERT_NOT_NULL(window);
    DEBUG_ASSERT_NOT_NULL(next_vblank_ns_out);

    ok = drmdev_get_last_vblank(window->drmdev, window->selected_crtc->id, &last_vblank);
    if (ok != 0) {
        return ok;
    }

    *next_vblank_ns_out = last_vblank + (uint64_t) (1000000000.0 / window->refresh_rate);
    return 0;
}

static int window_on_pageflip(struct window *window, uint64_t vblank_ns, uint64_t next_vblank_ns);

static void on_scanout(struct drmdev *drmdev, uint64_t vblank_ns, void *userdata) {
    struct window *window;
    int ok;

    DEBUG_ASSERT_NOT_NULL(drmdev);
    DEBUG_ASSERT_NOT_NULL(userdata);
    (void) drmdev;
    window = userdata;

    ok = window_on_pageflip(
        window,
        vblank_ns,
        vblank_ns + (uint64_t) (1000000000.0 / mode_get_vrefresh(window->selected_mode))
    );
    if (ok != 0) {
        LOG_ERROR("Error handling pageflip event. window_on_pageflip: %s\n", strerror(ok));
    }

    window_unref(window);
}

static int window_request_frame_and_wait_for_begin(struct window *window);

static int window_on_rendering_complete(struct window *window);

static int window_push_composition(struct window *window, struct fl_layer_composition *composition) {
    struct kms_req_builder *builder;
    struct kms_req *req;
    int ok;

    DEBUG_ASSERT_NOT_NULL(window);
    DEBUG_ASSERT_NOT_NULL(composition);

    // If flutter won't request frames (because the vsync callback is broken),
    // we'll wait here for the previous frame to be presented / rendered.
    // Otherwise the surface_swap_buffers at the bottom might allocate an
    // additional buffer and we'll potentially use more buffers than we're
    // trying to use.
    if (!window->use_frame_requests) {
        TRACER_BEGIN(window->tracer, "window_request_frame_and_wait_for_begin");
        ok = window_request_frame_and_wait_for_begin(window);
        TRACER_END(window->tracer, "window_request_frame_and_wait_for_begin");
        if (ok != 0) {
            LOG_ERROR("Could not wait for frame begin.\n");
            return ok;
        }
    }

    window_lock(window);

    /// TODO: If we don't have new revisions, we don't need to scanout anything.

    /// TODO: Should we do this at the end of the function?
    fl_layer_composition_swap_ptrs(&window->composition, composition);

    builder = drmdev_create_request_builder(window->drmdev, window->selected_crtc->id);
    if (builder == NULL) {
        ok = ENOMEM;
        goto fail_unlock;
    }

    // We only set the mode once, at the first atomic request.
    if (window->set_mode) {
        ok = kms_req_builder_set_connector(builder, window->selected_connector->id);
        if (ok != 0) {
            LOG_ERROR("Couldn't select connector.\n");
            goto fail_unref_builder;
        }

        ok = kms_req_builder_set_mode(builder, window->selected_mode);
        if (ok != 0) {
            LOG_ERROR("Couldn't apply output mode.\n");
            goto fail_unref_builder;
        }

        window->set_set_mode = true;
    }

    for (size_t i = 0; i < fl_layer_composition_get_n_layers(composition); i++) {
        struct fl_layer *layer = fl_layer_composition_peek_layer(composition, i);
        (void) layer;

        ok = surface_present_kms(layer->surface, &layer->props, builder);
        if (ok != 0) {
            LOG_ERROR("Couldn't present flutter layer on screen. surface_present_kms: %s\n", strerror(ok));
            goto fail_release_layers;
        }
    }

    /// TODO: Fix this
    /// make kms_req_builder keep a ref on the buffers
    /// delete the release callbacks
    ok = kms_req_builder_add_scanout_callback(builder, on_scanout, window_ref(window));
    if (ok != 0) {
        LOG_ERROR("Couldn't register scanout callback.\n");
        goto fail_unref_window;
    }

    req = kms_req_builder_build(builder);
    if (req == NULL) {
        goto fail_unref_window;
    }

    kms_req_builder_unref(builder);
    builder = NULL;

    if (window->present_mode == kDoubleBufferedVsync_PresentMode) {
        TRACER_BEGIN(window->tracer, "kms_req_builder_commit");
        ok = kms_req_commit(req, /* blocking: */ false);
        TRACER_END(window->tracer, "kms_req_builder_commit");

        if (ok != 0) {
            LOG_ERROR("Could not commit frame request.\n");
            goto fail_unref_window2;
        }

        if (window->set_set_mode) {
            window->set_mode = false;
            window->set_set_mode = false;
        }
    } else {
        DEBUG_ASSERT_EQUALS(window->present_mode, kTripleBufferedVsync_PresentMode);

        if (window->present_immediately) {
            TRACER_BEGIN(window->tracer, "kms_req_builder_commit");
            ok = kms_req_commit(req, /* blocking: */ false);
            TRACER_END(window->tracer, "kms_req_builder_commit");

            if (ok != 0) {
                LOG_ERROR("Could not commit frame request.\n");
                goto fail_unref_window2;
            }

            if (window->set_set_mode) {
                window->set_mode = false;
                window->set_set_mode = false;
            }

            window->present_immediately = false;
        } else {
            if (window->next_frame != NULL) {
                /// FIXME: Call the release callbacks when the kms_req is destroyed, not when it's unrefed.
                /// Not sure this here will lead to the release callbacks being called multiple times.
                kms_req_call_release_callbacks(window->next_frame);
                kms_req_unref(window->next_frame);
            }

            window->next_frame = kms_req_ref(req);
            window->set_set_mode = window->set_mode;
        }
    }

    window_unlock(window);

    // KMS Req is committed now and drmdev keeps a ref
    // on it internally, so we don't need to keep this one.
    kms_req_unref(req);

    window_on_rendering_complete(window);

    return 0;


fail_unref_window2:
    window_unref(window);
    kms_req_call_release_callbacks(req);
    kms_req_unref(req);
    goto fail_unlock;

fail_unref_window:
    window_unref(window);

fail_release_layers:
    // like above, kms_req_builder_unref won't call the layer release callbacks.
    kms_req_builder_call_release_callbacks(builder);

fail_unref_builder:
    kms_req_builder_unref(builder);

fail_unlock:
    window_unlock(window);
    return ok;
}

static struct backing_store *window_create_backing_store(struct window *window, struct point size) {
    struct backing_store *store;

    DEBUG_ASSERT_NOT_NULL(window);

    /// TODO: Make pixel format configurable or automatically select one
    /// TODO: Only create one real GBM Surface backing store, otherwise create
    ///       custom GBM BO backing stores
    window_lock(window);

    if (window->egl_backing_store == NULL && window->renderer != NULL) {
        struct gbm_surface_backing_store *egl_store;

        egl_store = gbm_surface_backing_store_new_with_egl_config(
            window->tracer,
            size,
            window->gbm_device,
            window->renderer,
            window->has_forced_pixel_format ? window->forced_pixel_format : kARGB8888,
            window->egl_config
        );
        if (egl_store == NULL) {
            window_unlock(window);
            return NULL;
        }

        window->egl_backing_store = CAST_GBM_SURFACE_BACKING_STORE_UNCHECKED(surface_ref(CAST_SURFACE_UNCHECKED(egl_store)));
        window->backing_store = CAST_BACKING_STORE_UNCHECKED(surface_ref(CAST_SURFACE_UNCHECKED(egl_store)));
        store = CAST_BACKING_STORE_UNCHECKED(egl_store);
    } else if (window->vk_backing_store == NULL && window->vk_renderer != NULL) {
        struct vk_gbm_backing_store *vk_store;

        vk_store = vk_gbm_backing_store_new(
            window->tracer,
            size,
            window->gbm_device,
            window->vk_renderer,
            window->forced_pixel_format
        );
        if (vk_store == NULL) {
            window_unlock(window);
            return NULL;
        }

        window->vk_backing_store = CAST_VK_GBM_BACKING_STORE_UNCHECKED(surface_ref(CAST_SURFACE_UNCHECKED(vk_store)));
        window->backing_store = CAST_BACKING_STORE_UNCHECKED(surface_ref(CAST_SURFACE_UNCHECKED(vk_store)));
        store = CAST_BACKING_STORE_UNCHECKED(vk_store);
    } else {
        DEBUG_ASSERT((window->egl_backing_store || window->vk_backing_store) && window->backing_store);
        store = CAST_BACKING_STORE_UNCHECKED(surface_ref(CAST_SURFACE_UNCHECKED(window->backing_store)));
    }

    window_unlock(window);

    return CAST_BACKING_STORE_UNCHECKED(store);
}

static bool window_has_egl_surface(struct window *window) {
    bool result;

    DEBUG_ASSERT_NOT_NULL(window);

    window_lock(window);
    result = window->egl_backing_store != NULL;
    window_unlock(window);

    return result;
}

static EGLSurface window_get_egl_surface(struct window *window) {
    struct gbm_surface_backing_store *store;

    DEBUG_ASSERT_NOT_NULL(window);

    window_lock(window);

    if (window->egl_backing_store == NULL) {
        if (window->renderer == NULL) {
            LOG_DEBUG("EGL Surface was requested but there's not EGL/GL renderer configured.\n");
            window_unlock(window);
            return EGL_NO_SURFACE;
        }

        LOG_DEBUG("EGL Surface was requested before flutter supplied the surface dimensions.\n");

        store = gbm_surface_backing_store_new_with_egl_config(
            window->tracer,
            POINT(window->selected_mode->hdisplay, window->selected_mode->vdisplay),
            drmdev_get_gbm_device(window->drmdev),
            window->renderer,
            window->has_forced_pixel_format ? window->forced_pixel_format : kARGB8888,
            window->egl_config
        );
        if (store == NULL) {
            window_unlock(window);
            return EGL_NO_SURFACE;
        }

        window->egl_backing_store = CAST_GBM_SURFACE_BACKING_STORE_UNCHECKED(surface_ref(CAST_SURFACE_UNCHECKED(store)));
        window->backing_store = CAST_BACKING_STORE_UNCHECKED(store);
    }

    window_unlock(window);

    return gbm_surface_backing_store_get_egl_surface(window->egl_backing_store);
}

static void on_frame_begin_signal_semaphore(void *userdata, uint64_t vblank_ns, uint64_t next_vblank_ns) {
    sem_t *sem;

    DEBUG_ASSERT_NOT_NULL(userdata);
    sem = userdata;
    (void) vblank_ns;
    (void) next_vblank_ns;

    sem_post(sem);
}

static int window_begin_frame_locked(struct window *window, uint64_t vblank_ns, uint64_t next_vblank_ns) {
    struct frame_req *req;
    int ok;

    DEBUG_ASSERT_NOT_NULL(window);

    ok = queue_peek(&window->frame_req_queue, (void **) &req);
    if (ok != 0) {
        return ok;
    }

    DEBUG_ASSERT(req->signaled == false);

    req->cb(req->userdata, vblank_ns, next_vblank_ns);

#ifdef DEBUG
    req->signaled = true;
#endif

    return 0;
}

MAYBE_UNUSED static int window_begin_frame(struct window *window, uint64_t vblank_ns, uint64_t next_vblank_ns) {
    int ok;

    DEBUG_ASSERT_NOT_NULL(window);

    window_lock(window);
    ok = window_begin_frame_locked(window, vblank_ns, next_vblank_ns);
    window_unlock(window);

    return ok;
}

static int window_pop_frame_locked(struct window *window) {
    return queue_dequeue(&window->frame_req_queue, NULL);
}

static int window_request_frame_locked(struct window *window, compositor_frame_begin_cb_t cb, void *userdata) {
    struct frame_req req;
    bool signal_immediately;
    int ok;

    DEBUG_ASSERT_NOT_NULL(window);
    DEBUG_ASSERT_NOT_NULL(cb);

    req.cb = cb;
    req.userdata = userdata;
#ifdef DEBUG
    req.signaled = false;
#endif

    // if no frame is queued right now, we can immediately start the frame.
    signal_immediately = queue_peek(&window->frame_req_queue, NULL) == EAGAIN;

    ok = queue_enqueue(&window->frame_req_queue, &req);
    if (ok != 0) {
        return ok;
    }

    if (signal_immediately) {
        ok = window_begin_frame_locked(
            window,
            get_monotonic_time(),
            get_monotonic_time() + (uint64_t) (1000000000.0 / window->refresh_rate)
        );
        if (ok != 0) {
            return ok;
        }
    }

    return 0;
}

static int window_request_frame(struct window *window, compositor_frame_begin_cb_t cb, void *userdata) {
    int ok;

    DEBUG_ASSERT_NOT_NULL(window);

    window_lock(window);
    ok = window_request_frame_locked(window, cb, userdata);
    window_unlock(window);

    return ok;
}

static int window_request_frame_and_wait_for_begin(struct window *window) {
    sem_t sem;
    int ok;

    ok = sem_init(&sem, 0, 0);
    if (ok != 0) {
        ok = errno;
        LOG_ERROR("Could not initialize semaphore for waiting for frame begin. sem_init: %s\n", strerror(ok));
        return ok;
    }

    ok = window_request_frame(window, on_frame_begin_signal_semaphore, &sem);
    if (ok != 0) {
        sem_destroy(&sem);
        return ok;
    }

    while (1) {
        ok = sem_wait(&sem);
        if ((ok < 0) && (errno != EINTR)) {
            ok = errno;
            LOG_ERROR("Could not blockingly wait for frame begin. sem_wait: %s\n", strerror(ok));
            sem_destroy(&sem);
            return ok;
        } else if (ok == 0) {
            break;
        }
    }

    ok = sem_destroy(&sem);
    DEBUG_ASSERT(ok == 0);
    (void) ok;

    return 0;
}

MAYBE_UNUSED static int window_on_rendering_complete(struct window *window) {
    int ok;

    // if we're using triple buffering, we can immediately start the next frame.
    // for double buffering we need to wait till we have a buffer again, so after
    // the next pageflip
    if (window->present_mode == kTripleBufferedVsync_PresentMode) {
        window_lock(window);

        ok = window_pop_frame_locked(window);
        if (ok != EAGAIN) {
            goto fail_unlock;
        }

        uint64_t time = get_monotonic_time();

        ok = window_begin_frame_locked(window, time, time + (uint64_t) (1000000000.0 / window->refresh_rate));
        if (ok != EAGAIN) {
            goto fail_unlock;
        }

        window_unlock(window);
    }

    return 0;

fail_unlock:
    window_unlock(window);
    return ok;
}

static int window_on_pageflip(struct window *window, uint64_t vblank_ns, uint64_t next_vblank_ns) {
    int ok;

    TRACER_INSTANT(window->tracer, "window_on_pageflip");

    // Whenever the pageflip was completed,
    // we have a new buffer available that we can render into.
    // This only applies to double-buffering though, for triple buffering
    // we will have started the next frame at an earlier point.
    if (window->present_mode == kDoubleBufferedVsync_PresentMode) {
        window_lock(window);

        ok = window_pop_frame_locked(window);
        if (ok != 0 && ok != EAGAIN) {
            goto fail_unlock;
        }

        ok = window_begin_frame_locked(window, vblank_ns, next_vblank_ns);
        if (ok != 0 && ok != EAGAIN) {
            goto fail_unlock;
        }

        window_unlock(window);
    } else if (window->present_mode == kTripleBufferedVsync_PresentMode) {
        window_lock(window);

        if (window->next_frame != NULL) {
            TRACER_BEGIN(window->tracer, "kms_req_builder_commit");
            ok = kms_req_commit(window->next_frame, false);
            TRACER_END(window->tracer, "kms_req_builder_commit");

            if (ok != 0) {
                LOG_ERROR("Could not present frame. kms_req_builder_commit: %s\n", strerror(ok));
                goto fail_unlock;
            }

            if (window->set_set_mode) {
                window->set_mode = false;
                window->set_set_mode = false;
            }

            kms_req_unrefp(&window->next_frame);
        } else {
            window->present_immediately = true;
        }

        window_unlock(window);
    }

    return 0;

fail_unlock:
    window_unlock(window);
    return ok;
}

/**
 * @brief The flutter compositor. Responsible for taking the FlutterLayers, processing them into a struct fl_layer_composition*, then passing
 * those to the window so it can show it on screen.
 *
 * Right now this is only supports a single output screen only, but in the future we might add multi-screen support.
 * (Possibly one Flutter Engine per view)
 */
struct compositor {
    refcount_t n_refs;
    pthread_mutex_t mutex;

    struct tracer *tracer;
    struct window main_window;
    struct fl_layer_composition *composition;
    struct pointer_set platform_views;

    FlutterCompositor flutter_compositor;
};

struct platform_view_with_id {
    int64_t id;
    struct surface *surface;
};

static bool on_flutter_present_layers(const FlutterLayer **layers, size_t layers_count, void *userdata);

static bool on_flutter_create_backing_store(
    const FlutterBackingStoreConfig *config,
    FlutterBackingStore *backing_store_out,
    void *userdata
);

static bool on_flutter_collect_backing_store(const FlutterBackingStore *fl_store, void *userdata);

ATTR_MALLOC struct compositor *compositor_new(
    // clang-format off
    struct drmdev *drmdev,
    struct tracer *tracer,
    struct gl_renderer *renderer,
    bool has_rotation, drm_plane_transform_t rotation,
    bool has_orientation, enum device_orientation orientation,
    bool has_explicit_dimensions, int width_mm, int height_mm,
    EGLConfig egl_config,
    bool has_forced_pixel_format, enum pixfmt forced_pixel_format,
    bool use_frame_requests,
    enum present_mode present_mode
    // clang-format on
) {
    struct compositor *compositor;
    int ok;

    DEBUG_ASSERT_MSG(
        egl_config == EGL_NO_CONFIG_KHR || has_forced_pixel_format == true,
        "If an explicit EGLConfig is given, a pixel format must be given too."
    );

    LOG_DEBUG("Has forced pixel format: %s\n", has_forced_pixel_format ? "yes" : "no");

    compositor = malloc(sizeof *compositor);
    if (compositor == NULL) {
        goto fail_return_null;
    }

    ok = pthread_mutex_init(&compositor->mutex, NULL);
    if (ok != 0) {
        goto fail_free_compositor;
    }

    ok = pset_init(&compositor->platform_views, PSET_DEFAULT_MAX_SIZE);
    if (ok != 0) {
        goto fail_destroy_mutex;
    }

    ok = window_init(
        &compositor->main_window,
        compositor,
        tracer,
        drmdev,
        renderer,
        has_rotation,
        rotation,
        has_orientation,
        orientation,
        has_explicit_dimensions,
        width_mm,
        height_mm,
        use_frame_requests,
        has_forced_pixel_format,
        forced_pixel_format,
        egl_config,
        present_mode
    );
    if (ok != 0) {
        LOG_ERROR("Could not initialize main window.\n");
        goto fail_deinit_platform_views;
    }

    compositor->n_refs = REFCOUNT_INIT_1;
    compositor->composition = NULL;
    // just so we get an error if the FlutterCompositor struct was updated
    COMPILE_ASSERT(sizeof(FlutterCompositor) == 24);
    compositor->flutter_compositor = (FlutterCompositor
    ){ .struct_size = sizeof(FlutterCompositor),
       .user_data = compositor,
       .create_backing_store_callback = on_flutter_create_backing_store,
       .collect_backing_store_callback = on_flutter_collect_backing_store,
       .present_layers_callback = on_flutter_present_layers,
       .avoid_backing_store_cache = true };
    compositor->tracer = tracer_ref(tracer);
    return compositor;

fail_deinit_platform_views:
    pset_deinit(&compositor->platform_views);

fail_destroy_mutex:
    pthread_mutex_destroy(&compositor->mutex);

fail_free_compositor:
    free(compositor);

fail_return_null:
    return NULL;
}

ATTR_MALLOC struct compositor *compositor_new_vulkan(
    // clang-format off
    struct drmdev *drmdev,
    struct tracer *tracer,
    struct vk_renderer *renderer,
    bool has_rotation, drm_plane_transform_t rotation,
    bool has_orientation, enum device_orientation orientation,
    bool has_explicit_dimensions, int width_mm, int height_mm,
    bool has_forced_pixel_format, enum pixfmt forced_pixel_format,
    bool use_frame_requests,
    enum present_mode present_mode
    // clang-format on
) {
    struct compositor *compositor;
    int ok;

    LOG_DEBUG("Has forced pixel format: %s\n", has_forced_pixel_format ? "yes" : "no");

    compositor = malloc(sizeof *compositor);
    if (compositor == NULL) {
        goto fail_return_null;
    }

    ok = pthread_mutex_init(&compositor->mutex, NULL);
    if (ok != 0) {
        goto fail_free_compositor;
    }

    ok = pset_init(&compositor->platform_views, PSET_DEFAULT_MAX_SIZE);
    if (ok != 0) {
        goto fail_destroy_mutex;
    }

    ok = window_init_vulkan(
        &compositor->main_window,
        compositor,
        tracer,
        drmdev,
        renderer,
        has_rotation, rotation,
        has_orientation, orientation,
        has_explicit_dimensions, width_mm, height_mm,
        use_frame_requests,
        has_forced_pixel_format, forced_pixel_format,
        present_mode
    );
    if (ok != 0) {
        LOG_ERROR("Could not initialize main window.\n");
        goto fail_deinit_platform_views;
    }

    compositor->n_refs = REFCOUNT_INIT_1;
    compositor->composition = NULL;
    // just so we get an error if the FlutterCompositor struct was updated
    COMPILE_ASSERT(sizeof(FlutterCompositor) == 24);
    compositor->flutter_compositor = (FlutterCompositor){
        .struct_size = sizeof(FlutterCompositor),
        .user_data = compositor,
        .create_backing_store_callback = on_flutter_create_backing_store,
        .collect_backing_store_callback = on_flutter_collect_backing_store,
        .present_layers_callback = on_flutter_present_layers,
        .avoid_backing_store_cache = true,
    };
    compositor->tracer = tracer_ref(tracer);
    return compositor;

fail_deinit_platform_views:
    pset_deinit(&compositor->platform_views);

fail_destroy_mutex:
    pthread_mutex_destroy(&compositor->mutex);

fail_free_compositor:
    free(compositor);

fail_return_null:
    return NULL;
}

void compositor_destroy(struct compositor *compositor) {
    struct platform_view_with_id *view;

    queue_deinit(&compositor->main_window.frame_req_queue);
    for_each_pointer_in_pset(&compositor->platform_views, view) {
        surface_unref(view->surface);
        free(view);
    }
    pset_deinit(&compositor->platform_views);
    pthread_mutex_destroy(&compositor->mutex);
    free(compositor);
}

DEFINE_REF_OPS(compositor, n_refs)

DEFINE_STATIC_LOCK_OPS(compositor, mutex)

void compositor_get_view_geometry(struct compositor *compositor, struct view_geometry *view_geometry_out) {
    return window_get_view_geometry(&compositor->main_window, view_geometry_out);
}

ATTR_PURE double compositor_get_refresh_rate(struct compositor *compositor) {
    DEBUG_ASSERT_NOT_NULL(compositor);
    return window_get_refresh_rate(&compositor->main_window);
}

int compositor_get_next_vblank(struct compositor *compositor, uint64_t *next_vblank_ns_out) {
    DEBUG_ASSERT_NOT_NULL(compositor);
    DEBUG_ASSERT_NOT_NULL(next_vblank_ns_out);
    return window_get_next_vblank(&compositor->main_window, next_vblank_ns_out);
}

static int compositor_push_composition(struct compositor *compositor, struct fl_layer_composition *composition) {
    int ok;

    fl_layer_composition_ref(composition);
    if (compositor->composition) {
        fl_layer_composition_unref(compositor->composition);
    }
    compositor->composition = composition;

    TRACER_BEGIN(compositor->tracer, "window_push_composition");
    ok = window_push_composition(&compositor->main_window, composition);
    TRACER_END(compositor->tracer, "window_push_composition");

    return ok;
}

static void fill_platform_view_layer_props(
    struct fl_layer_props *props_out,
    const FlutterPoint *offset,
    const FlutterSize *size,
    const FlutterPlatformViewMutation **mutations,
    size_t n_mutations,
    const FlutterTransformation *display_to_view_transform,
    const FlutterTransformation *view_to_display_transform,
    double device_pixel_ratio
) {
    (void) view_to_display_transform;

    /**
	 * inversion for
	 * ```
	 * const auto transformed_layer_bounds =
     *     root_surface_transformation_.mapRect(layer_bounds);
	 * ```
	 */

    struct quad quad = transform_aa_rect(
        *display_to_view_transform,
        (struct aa_rect){ .offset.x = offset->x, .offset.y = offset->y, .size.x = size->width, .size.y = size->height }
    );

    struct aa_rect rect = get_aa_bounding_rect(quad);

    /**
	 * inversion for
	 * ```
	 * const auto layer_bounds =
     *     SkRect::MakeXYWH(params.finalBoundingRect().x(),
     *                      params.finalBoundingRect().y(),
     *                      params.sizePoints().width() * device_pixel_ratio_,
     *                      params.sizePoints().height() * device_pixel_ratio_
     *     );
	 * ```
	 */

    rect.size.x /= device_pixel_ratio;
    rect.size.y /= device_pixel_ratio;

    // okay, now we have the params.finalBoundingRect().x() in aa_back_transformed.x and
    // params.finalBoundingRect().y() in aa_back_transformed.y.
    // those are flutter view coordinates, so we still need to transform them to display coordinates.

    // However, there are also calculated as a side-product of calculating the size of the quadrangle.
    // So we'll avoid calculating them for now. Calculation of the size may fail when the offset
    // given to `SceneBuilder.addPlatformView` (https://api.flutter.dev/flutter/dart-ui/SceneBuilder/addPlatformView.html)
    // is not zero. (Don't really know what to do in that case)

    rect.offset.x = 0;
    rect.offset.y = 0;
    quad = get_quad(rect);

    double rotation = 0, opacity = 1;
    for (int i = n_mutations - 1; i >= 0; i--) {
        if (mutations[i]->type == kFlutterPlatformViewMutationTypeTransformation) {
            quad = transform_quad(mutations[i]->transformation, quad);

            double rotz = atan2(mutations[i]->transformation.skewX, mutations[i]->transformation.scaleX) * 180.0 / M_PI;
            if (rotz < 0) {
                rotz += 360;
            }

            rotation += rotz;
        } else if (mutations[i]->type == kFlutterPlatformViewMutationTypeOpacity) {
            opacity *= mutations[i]->opacity;
        }
    }

    rotation = fmod(rotation, 360.0);

    /// TODO: Implement axis aligned rectangle detection
    props_out->is_aa_rect = false;
    props_out->aa_rect = AA_RECT_FROM_COORDS(0, 0, 0, 0);
    props_out->quad = quad;
    props_out->opacity = 0;
    props_out->rotation = rotation;

    /// TODO: Implement clip rects
    props_out->n_clip_rects = 0;
    props_out->clip_rects = NULL;
}

static int compositor_push_fl_layers(struct compositor *compositor, size_t n_fl_layers, const FlutterLayer **fl_layers) {
    struct fl_layer_composition *composition;
    int ok;

    composition = fl_layer_composition_new(n_fl_layers);
    if (composition == NULL) {
        return ENOMEM;
    }

    compositor_lock(compositor);

    for (int i = 0; i < n_fl_layers; i++) {
        const FlutterLayer *fl_layer = fl_layers[i];
        struct fl_layer *layer = fl_layer_composition_peek_layer(composition, i);

        if (fl_layer->type == kFlutterLayerContentTypeBackingStore) {
            /// TODO: Implement
            layer->surface = surface_ref(CAST_SURFACE(fl_layer->backing_store->user_data));

            // Tell the surface that flutter has rendered into this framebuffer / texture / image.
            // It'll also read the did_update field and not update the surface revision in that case.
            backing_store_queue_present(CAST_BACKING_STORE(layer->surface), fl_layer->backing_store);

            layer->props.is_aa_rect = true;
            layer->props.aa_rect =
                AA_RECT_FROM_COORDS(fl_layer->offset.y, fl_layer->offset.y, fl_layer->size.width, fl_layer->size.height);
            layer->props.quad = get_quad(layer->props.aa_rect);
            layer->props.opacity = 1.0;
            layer->props.rotation = 0.0;
            layer->props.n_clip_rects = 0;
            layer->props.clip_rects = NULL;
        } else {
            DEBUG_ASSERT_EQUALS(fl_layer->type, kFlutterLayerContentTypePlatformView);

            /// TODO: Maybe always check if the ID is valid?
#if DEBUG
            // if we're in debug mode, we actually check if the ID is a valid,
            // registered ID.
            /// TODO: Implement
            layer->surface =
                surface_ref(compositor_get_view_by_id_locked(compositor, fl_layer->platform_view->identifier));
#else
            // in release mode, we just assume the id is valid.
            // Since the surface constructs the ID by just casting the surface pointer to an int64_t,
            // we can easily cast it back without too much trouble.
            // Only problem is if the id is garbage, we won't notice and the returned surface is garbage too.
            layer->surface = surface_ref(surface_from_id(fl_layer->platform_view->identifier));
#endif

            // The coordinates flutter gives us are a bit buggy, so calculating the right geometry is really a problem on its own
            /// TODO: Don't unconditionally take the geometry from the main window.
            fill_platform_view_layer_props(
                &layer->props,
                &fl_layer->offset,
                &fl_layer->size,
                fl_layer->platform_view->mutations,
                fl_layer->platform_view->mutations_count,
                &compositor->main_window.display_to_view_transform,
                &compositor->main_window.view_to_display_transform,
                compositor->main_window.pixel_ratio
            );
        }
    }

    compositor_unlock(compositor);

    TRACER_BEGIN(compositor->tracer, "compositor_push_composition");
    ok = compositor_push_composition(compositor, composition);
    TRACER_END(compositor->tracer, "compositor_push_composition");

    fl_layer_composition_unref(composition);

    return 0;

    //fail_free_composition:
    //fl_layer_composition_unref(composition);
    return ok;
}

static bool on_flutter_present_layers(const FlutterLayer **layers, size_t layers_count, void *userdata) {
    struct compositor *compositor;
    int ok;

    DEBUG_ASSERT_NOT_NULL(layers);
    DEBUG_ASSERT(layers_count > 0);
    DEBUG_ASSERT_NOT_NULL(userdata);
    compositor = userdata;

    TRACER_BEGIN(compositor->tracer, "compositor_push_fl_layers");
    ok = compositor_push_fl_layers(compositor, layers_count, layers);
    TRACER_END(compositor->tracer, "compositor_push_fl_layers");

    if (ok != 0) {
        return false;
    }

    return true;
}

int compositor_set_platform_view(struct compositor *compositor, int64_t id, struct surface *surface) {
    struct platform_view_with_id *data;
    int ok;

    DEBUG_ASSERT_NOT_NULL(compositor);
    DEBUG_ASSERT(id != 0);
    DEBUG_ASSERT_NOT_NULL(surface);

    compositor_lock(compositor);

    for_each_pointer_in_pset(&compositor->platform_views, data) {
        if (data->id == id) {
            break;
        }
    }

    if (data == NULL) {
        if (surface == NULL) {
            data = malloc(sizeof *data);
            if (data == NULL) {
                ok = ENOMEM;
                goto fail_unlock;
            }

            data->id = id;
            data->surface = surface;
            pset_put(&compositor->platform_views, data);
        }
    } else {
        DEBUG_ASSERT_NOT_NULL(data->surface);
        if (surface == NULL) {
            pset_remove(&compositor->platform_views, data);
            surface_unref(data->surface);
            free(data);
        } else {
            surface_ref(surface);
            surface_unref(data->surface);
            data->surface = surface;
        }
    }

    compositor_unlock(compositor);
    return 0;

fail_unlock:
    compositor_unlock(compositor);
    return ok;
}

struct surface *compositor_get_view_by_id_locked(struct compositor *compositor, int64_t view_id) {
    struct platform_view_with_id *data;

    for_each_pointer_in_pset(&compositor->platform_views, data) {
        if (data->id == view_id) {
            return data->surface;
        }
    }

    return NULL;
}

static struct backing_store *compositor_create_backing_store(struct compositor *compositor, struct point size) {
    DEBUG_ASSERT_NOT_NULL(compositor);
    return window_create_backing_store(&compositor->main_window, size);
}

bool compositor_has_egl_surface(struct compositor *compositor) {
    return window_has_egl_surface(&compositor->main_window);
}

EGLSurface compositor_get_egl_surface(struct compositor *compositor) {
    return window_get_egl_surface(&compositor->main_window);
}

int compositor_request_frame(struct compositor *compositor, compositor_frame_begin_cb_t cb, void *userdata) {
    DEBUG_ASSERT_NOT_NULL(compositor);
    return window_request_frame(&compositor->main_window, cb, userdata);
}

static bool on_flutter_create_backing_store(
    const FlutterBackingStoreConfig *config,
    FlutterBackingStore *backing_store_out,
    void *userdata
) {
    struct backing_store *store;
    struct compositor *compositor;
    int ok;

    DEBUG_ASSERT_NOT_NULL(config);
    DEBUG_ASSERT_NOT_NULL(backing_store_out);
    DEBUG_ASSERT_NOT_NULL(userdata);
    compositor = userdata;

    // we have a reference on this store.
    // i.e. when we don't use it, we need to unref it.
    store = compositor_create_backing_store(compositor, POINT(config->size.width, config->size.height));
    if (store == NULL) {
        LOG_ERROR("Couldn't create backing store for flutter to render into.\n");
        return false;
    }

    COMPILE_ASSERT(sizeof(FlutterBackingStore) == 56);
    memset(backing_store_out, 0, sizeof *backing_store_out);
    backing_store_out->struct_size = sizeof(FlutterBackingStore);

    /// TODO: Make this better
    // compositor_on_event_fd_ready(compositor);

    // backing_store_fill asserts that the user_data is null so it can make sure
    // any concrete backing_store_fill implementation doesn't try to set the user_data.
    // so we set the user_data after the fill
    ok = backing_store_fill(store, backing_store_out);
    if (ok != 0) {
        LOG_ERROR("Couldn't fill flutter backing store with concrete OpenGL framebuffer/texture or Vulkan image.\n");
        surface_unref(CAST_SURFACE_UNCHECKED(store));
        return false;
    }

    // now we can set the user_data.
    backing_store_out->user_data = store;

    return true;
}

static bool on_flutter_collect_backing_store(const FlutterBackingStore *fl_store, void *userdata) {
    struct compositor *compositor;
    struct surface *s;

    DEBUG_ASSERT_NOT_NULL(fl_store);
    DEBUG_ASSERT_NOT_NULL(userdata);
    s = CAST_SURFACE(fl_store->user_data);
    compositor = userdata;

    (void) compositor;

    surface_unref(s);
    return true;
}

const FlutterCompositor *compositor_get_flutter_compositor(struct compositor *compositor) {
    DEBUG_ASSERT_NOT_NULL(compositor);
    return &compositor->flutter_compositor;
}

int compositor_get_event_fd(struct compositor *compositor) {
    DEBUG_ASSERT_NOT_NULL(compositor);
    return drmdev_get_event_fd(compositor->main_window.drmdev);
}

int compositor_on_event_fd_ready(struct compositor *compositor) {
    DEBUG_ASSERT_NOT_NULL(compositor);
    return drmdev_on_event_fd_ready(compositor->main_window.drmdev);
}
