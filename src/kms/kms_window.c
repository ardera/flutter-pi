#define _GNU_SOURCE /* for asprintf */

#include "kms_window.h"

#include "util/refcounting.h"
#include "window.h"
#include "window_private.h"

struct cursor_buffer {
    refcount_t n_refs;

    const struct pointer_icon *icon;
    enum pixfmt format;
    int width, height;
    drm_plane_transform_t rotation;

    struct drmdev *drmdev;
    int drm_fb_id;
    struct gbm_bo *bo;

    struct vec2i hotspot;
};

static struct vec2i get_rotated_hotspot(const struct pointer_icon *icon, drm_plane_transform_t rotation) {
    struct vec2i size;
    struct vec2i hotspot;

    assert(PLANE_TRANSFORM_IS_ONLY_ROTATION(rotation));
    size = pointer_icon_get_size(icon);
    hotspot = pointer_icon_get_hotspot(icon);

    if (rotation.rotate_0) {
        return hotspot;
    } else if (rotation.rotate_90) {
        return VEC2I(size.y - hotspot.y - 1, hotspot.x);
    } else if (rotation.rotate_180) {
        return VEC2I(size.x - hotspot.x - 1, size.y - hotspot.y - 1);
    } else {
        ASSUME(rotation.rotate_270);
        return VEC2I(hotspot.y, size.x - hotspot.x - 1);
    }
}

static struct cursor_buffer *cursor_buffer_new(struct drmdev *drmdev, const struct pointer_icon *icon, drm_plane_transform_t rotation) {
    struct cursor_buffer *b;
    struct gbm_bo *bo;
    uint32_t fb_id;
    struct vec2i size, rotated_size;
    int ok;

    ASSERT_NOT_NULL(drmdev);
    assert(PLANE_TRANSFORM_IS_ONLY_ROTATION(rotation));

    size = pointer_icon_get_size(icon);
    rotated_size = size;

    b = malloc(sizeof *b);
    if (b == NULL) {
        return NULL;
    }

    if (rotation.rotate_90 || rotation.rotate_270) {
        rotated_size = vec2i_swap_xy(size);
    }

    bo = gbm_bo_create(
        drmdev_get_gbm_device(drmdev),
        rotated_size.x,
        rotated_size.y,
        get_pixfmt_info(PIXFMT_ARGB8888)->gbm_format,
        GBM_BO_USE_LINEAR | GBM_BO_USE_SCANOUT | GBM_BO_USE_WRITE | GBM_BO_USE_CURSOR
    );
    if (bo == NULL) {
        LOG_ERROR("Could not create GBM buffer for uploading mouse cursor icon. gbm_bo_create: %s\n", strerror(errno));
        goto fail_free_b;
    }

    if (gbm_bo_get_stride(bo) != rotated_size.x * 4) {
        LOG_ERROR("GBM BO has unsupported framebuffer stride %u, expected was: %d\n", gbm_bo_get_stride(bo), size.x * 4);
        goto fail_destroy_bo;
    }

    uint32_t *pixel_data = pointer_icon_dup_pixels(icon);
    if (pixel_data == NULL) {
        goto fail_destroy_bo;
    }

    if (rotation.rotate_0) {
        ok = gbm_bo_write(bo, pixel_data, gbm_bo_get_stride(bo) * size.y);
        if (ok != 0) {
            LOG_ERROR("Couldn't write cursor icon to GBM BO. gbm_bo_write: %s\n", strerror(errno));
            goto fail_free_duped_pixel_data;
        }
    } else {
        ASSUME(rotation.rotate_90 || rotation.rotate_180 || rotation.rotate_270);

        uint32_t *rotated = malloc(size.x * size.y * 4);
        if (rotated == NULL) {
            goto fail_free_duped_pixel_data;
        }

        for (int y = 0; y < size.y; y++) {
            for (int x = 0; x < size.x; x++) {
                int buffer_x, buffer_y;
                if (rotation.rotate_90) {
                    buffer_x = size.y - y - 1;
                    buffer_y = x;
                } else if (rotation.rotate_180) {
                    buffer_x = size.y - y - 1;
                    buffer_y = size.x - x - 1;
                } else {
                    ASSUME(rotation.rotate_270);
                    buffer_x = y;
                    buffer_y = size.x - x - 1;
                }

                int buffer_offset = rotated_size.x * buffer_y + buffer_x;
                int cursor_offset = size.x * y + x;

                rotated[buffer_offset] = pixel_data[cursor_offset];
            }
        }

        ok = gbm_bo_write(bo, rotated, gbm_bo_get_stride(bo) * rotated_size.y);

        free(rotated);

        if (ok != 0) {
            LOG_ERROR("Couldn't write rotated cursor icon to GBM BO. gbm_bo_write: %s\n", strerror(errno));
            goto fail_free_duped_pixel_data;
        }
    }

    free(pixel_data);

    fb_id = drmdev_add_fb(
        drmdev,
        rotated_size.x,
        rotated_size.y,
        PIXFMT_ARGB8888,
        gbm_bo_get_handle(bo).u32,
        gbm_bo_get_stride(bo),
        gbm_bo_get_offset(bo, 0),
        gbm_bo_get_modifier(bo) != DRM_FORMAT_MOD_INVALID,
        gbm_bo_get_modifier(bo)
    );
    if (fb_id == 0) {
        goto fail_destroy_bo;
    }

    b->n_refs = REFCOUNT_INIT_1;
    b->icon = icon;
    b->format = PIXFMT_ARGB8888;
    b->width = rotated_size.x;
    b->height = rotated_size.y;
    b->rotation = rotation;
    b->drmdev = drmdev_ref(drmdev);
    b->drm_fb_id = fb_id;
    b->bo = bo;
    b->hotspot = get_rotated_hotspot(icon, rotation);
    return b;

fail_free_duped_pixel_data:
    free(pixel_data);

fail_destroy_bo:
    gbm_bo_destroy(bo);

fail_free_b:
    free(b);
    return NULL;
}

static void cursor_buffer_destroy(struct cursor_buffer *buffer) {
    drmdev_rm_fb(buffer->drmdev, buffer->drm_fb_id);
    gbm_bo_destroy(buffer->bo);
    drmdev_unref(buffer->drmdev);
    free(buffer);
}

DEFINE_STATIC_REF_OPS(cursor_buffer, n_refs)

static int select_mode(
    struct drm_resources *resources,
    struct drm_connector **connector_out,
    struct drm_encoder **encoder_out,
    struct drm_crtc **crtc_out,
    drmModeModeInfo **mode_out,
    const char *desired_videomode
) {
    int ok;

    // find any connected connector
    struct drm_connector *connector = NULL;
    drm_resources_for_each_connector(resources, connector_it) {
        if (connector_it->variable_state.connection_state == DRM_CONNSTATE_CONNECTED) {
            connector = connector_it;
            break;
        }
    }

    if (connector == NULL) {
        LOG_ERROR("Could not find a connected connector!\n");
        return EINVAL;
    }

    drmModeModeInfo *mode = NULL;
    if (desired_videomode != NULL) {
        drm_connector_for_each_mode(connector, mode_iter) {
            char *modeline = NULL, *modeline_nohz = NULL;

            ok = asprintf(&modeline, "%" PRIu16 "x%" PRIu16 "@%" PRIu32, mode_iter->hdisplay, mode_iter->vdisplay, mode_iter->vrefresh);
            if (ok < 0) {
                return ENOMEM;
            }

            ok = asprintf(&modeline_nohz, "%" PRIu16 "x%" PRIu16, mode_iter->hdisplay, mode_iter->vdisplay);
            if (ok < 0) {
                return ENOMEM;
            }

            if (streq(modeline, desired_videomode)) {
                // Probably a bit superfluos, but the refresh rate can still vary in the decimal places.
                if (mode == NULL || (mode_get_vrefresh(mode_iter) > mode_get_vrefresh(mode))) {
                    mode = mode_iter;
                }
            } else if (streq(modeline_nohz, desired_videomode)) {
                if (mode == NULL || (mode_get_vrefresh(mode_iter) > mode_get_vrefresh(mode))) {
                    mode = mode_iter;
                }
            }

            free(modeline);
            free(modeline_nohz);
        }

        if (mode == NULL) {
            LOG_ERROR("Didn't find a videomode matching \"%s\"! Falling back to display preferred mode.\n", desired_videomode);
        }
    }

    // Find the preferred mode (GPU drivers _should_ always supply a preferred mode, but of course, they don't)
    // Alternatively, find the mode with the highest width*height. If there are multiple modes with the same w*h,
    // prefer higher refresh rates. After that, prefer progressive scanout modes.
    if (mode == NULL) {
        drm_connector_for_each_mode(connector, mode_iter) {
            if (mode_iter->type & DRM_MODE_TYPE_PREFERRED) {
                mode = mode_iter;
                break;
            } else if (mode == NULL) {
                mode = mode_iter;
            } else {
                int area = mode_iter->hdisplay * mode_iter->vdisplay;
                int old_area = mode->hdisplay * mode->vdisplay;

                if ((area > old_area) || ((area == old_area) && (mode_iter->vrefresh > mode->vrefresh)) ||
                    ((area == old_area) && (mode_iter->vrefresh == mode->vrefresh) && ((mode->flags & DRM_MODE_FLAG_INTERLACE) == 0))) {
                    mode = mode_iter;
                }
            }
        }

        if (mode == NULL) {
            LOG_ERROR("Could not find a preferred output mode!\n");
            return EINVAL;
        }
    }

    ASSERT_NOT_NULL(mode);

    // Find the encoder that's linked to the connector right now
    struct drm_encoder *encoder = NULL;
    drm_resources_for_each_encoder(resources, encoder_it) {
        if (encoder_it->id == connector->committed_state.encoder_id) {
            encoder = encoder_it;
            break;
        }
    }

    // Otherwise use use any encoder that the connector supports linking to
    if (encoder == NULL) {
        for (int i = 0; i < connector->n_encoders; i++, encoder = NULL) {
            drm_resources_for_each_encoder(resources, encoder_it) {
                if (encoder_it->id == connector->encoders[i]) {
                    encoder = encoder_it;
                    break;
                }
            }

            if (encoder && encoder->possible_crtcs) {
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
    struct drm_crtc *crtc = NULL;
    drm_resources_for_each_crtc(resources, crtc_it) {
        if (crtc_it->id == encoder->variable_state.crtc_id) {
            crtc = crtc_it;
            break;
        }
    }

    // Otherwise use any CRTC that this encoder supports linking to
    if (crtc == NULL) {
        drm_resources_for_each_crtc(resources, crtc_it) {
            if (encoder->possible_crtcs & crtc_it->bitmask) {
                // find a CRTC that is possible to use with this encoder
                crtc = crtc_it;
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

static const struct window_ops kms_window_ops;

struct kms_window {
    struct window base;

    struct drmdev *drmdev;
    struct drm_resources *resources;
    struct drm_connector *connector;
    struct drm_encoder *encoder;
    struct drm_crtc *crtc;
    drmModeModeInfo *mode;

    bool should_apply_mode;

    const struct pointer_icon *pointer_icon;
    struct cursor_buffer *cursor;

    bool cursor_works;
};

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
) {
    struct drm_connector *selected_connector;
    struct drm_encoder *selected_encoder;
    struct drm_crtc *selected_crtc;
    drmModeModeInfo *selected_mode;
    bool has_dimensions;
    int ok;

    ASSERT_NOT_NULL(drmdev);
    ASSERT_NOT_NULL(resources);

    struct kms_window *window = malloc(sizeof *window);
    if (window == NULL) {
        return NULL;
    }

    ok = select_mode(resources, &selected_connector, &selected_encoder, &selected_crtc, &selected_mode, desired_videomode);
    if (ok != 0) {
        goto fail_free_window;
    }

    if (has_explicit_dimensions) {
        has_dimensions = true;
    } else if (selected_connector->variable_state.width_mm % 10 || selected_connector->variable_state.height_mm % 10) {
        // as a heuristic, assume the physical dimensions are valid if they're not both multiples of 10.
        // dimensions like 160x90mm, 150x100mm are often bogus.
        has_dimensions = true;
        width_mm = selected_connector->variable_state.width_mm;
        height_mm = selected_connector->variable_state.height_mm;
    } else if (selected_connector->type == DRM_MODE_CONNECTOR_DSI && selected_connector->variable_state.width_mm == 0 &&
               selected_connector->variable_state.height_mm == 0) {
        // assume this is the official Raspberry Pi DSI display.
        has_dimensions = true;
        width_mm = 155;
        height_mm = 86;
    } else {
        has_dimensions = false;
    }

    ok = window_init(
        // clang-format off
        &window->base,
        tracer,
        scheduler,
        has_rotation, rotation,
        has_orientation, orientation,
        selected_mode->hdisplay, selected_mode->vdisplay,
        has_dimensions, width_mm, height_mm,
        mode_get_vrefresh(selected_mode),
        has_forced_pixel_format, forced_pixel_format,
        renderer_type,
        gl_renderer,
        vk_renderer
        // clang-format on
    );
    if (ok != 0) {
        free(window);
        return NULL;
    }

    LOG_DEBUG_UNPREFIXED(
        "display mode:\n"
        "  resolution: %" PRIu16 " x %" PRIu16
        "\n"
        "  refresh rate: %fHz\n"
        "  physical size: %dmm x %dmm\n"
        "  flutter device pixel ratio: %f\n"
        "  pixel format: %s\n",
        selected_mode->hdisplay,
        selected_mode->vdisplay,
        (double) mode_get_vrefresh(selected_mode),
        width_mm,
        height_mm,
        (double) window->base.pixel_ratio,
        has_forced_pixel_format ? get_pixfmt_info(forced_pixel_format)->name : "(any)"
    );

    window->drmdev = drmdev_ref(drmdev);
    window->resources = drm_resources_ref(resources);
    window->connector = selected_connector;
    window->encoder = selected_encoder;
    window->crtc = selected_crtc;
    window->mode = selected_mode;
    window->should_apply_mode = true;
    window->cursor = NULL;
    window->pointer_icon = NULL;
    window->cursor_works = true;
    window->base.ops = kms_window_ops;
    return (struct window *) window;

fail_free_window:
    free(window);
    return NULL;
}

void kms_window_deinit(struct window *window) {
    /// TODO: Do we really need to do this?
    /*
    struct kms_req_builder *builder;
    struct kms_req *req;
    int ok;

    builder = drmdev_create_request_builder(window->kms.drmdev, window->kms.crtc->id);
    ASSERT_NOT_NULL(builder);

    ok = kms_req_builder_unset_mode(builder);
    ASSERT_EQUALS(ok, 0);

    req = kms_req_builder_build(builder);
    ASSERT_NOT_NULL(req);

    kms_req_builder_unref(builder);

    ok = kms_req_commit_blocking(req, NULL);
    ASSERT_EQUALS(ok, 0);
    (void) ok;

    kms_req_unref(req);
    */

    struct kms_window *kms_window = (struct kms_window *) window;

    if (kms_window->cursor != NULL) {
        cursor_buffer_unref(kms_window->cursor);
    }
    drm_resources_unref(kms_window->resources);
    drmdev_unref(kms_window->drmdev);
    window_deinit(window);
}

struct frame {
    struct tracer *tracer;
    struct kms_req *req;
    struct drmdev *drmdev;
    struct frame_scheduler *scheduler;
    bool unset_should_apply_mode_on_commit;
};

UNUSED static void on_scanout(uint64_t vblank_ns, void *userdata) {
    struct frame *frame;

    ASSERT_NOT_NULL(userdata);
    frame = userdata;

    // This potentially presents a new frame.
    frame_scheduler_on_scanout(frame->scheduler, true, vblank_ns);

    frame_scheduler_unref(frame->scheduler);
    tracer_unref(frame->tracer);
    kms_req_unref(frame->req);
    free(frame);
}

static void on_present_frame(void *userdata) {
    struct frame *frame;
    int ok;

    ASSERT_NOT_NULL(userdata);

    frame = userdata;

    {
        // Keep our own reference on tracer, because the frame might be destroyed
        // after kms_req_commit_nonblocking returns.
        struct tracer *tracer = tracer_ref(frame->tracer);

        // The pageflip events might be handled on a different thread, so on_scanout
        // might already be executed and the frame instance already freed once
        // kms_req_commit_nonblocking returns.
        TRACER_BEGIN(tracer, "kms_req_commit_nonblocking");
        ok = kms_req_commit_nonblocking(frame->req, frame->drmdev, on_scanout, frame, NULL);
        TRACER_END(tracer, "kms_req_commit_nonblocking");

        tracer_unref(tracer);
    }

    if (ok != 0) {
        LOG_ERROR("Could not commit frame request.\n");
        frame_scheduler_unref(frame->scheduler);

        // Analyzer thinks the tracer might already be destroyed by the tracer_unref
        // above. We know that's not possible.
        ANALYZER_SUPPRESS(tracer_unref(frame->tracer));

        kms_req_unref(frame->req);
        free(frame);
    }
}

static void on_cancel_frame(void *userdata) {
    struct frame *frame;
    ASSERT_NOT_NULL(userdata);

    frame = userdata;

    frame_scheduler_unref(frame->scheduler);
    tracer_unref(frame->tracer);
    kms_req_unref(frame->req);
    drmdev_unref(frame->drmdev);
    free(frame);
}

static int kms_window_push_composition_locked(struct kms_window *w, struct fl_layer_composition *composition) {
    struct kms_req_builder *builder;
    struct kms_req *req;
    struct frame *frame;
    int ok;

    ASSERT_NOT_NULL(w);
    ASSERT_NOT_NULL(composition);

    // If flutter won't request frames (because the vsync callback is broken),
    // we'll wait here for the previous frame to be presented / rendered.
    // Otherwise the surface_swap_buffers at the bottom might allocate an
    // additional buffer and we'll potentially use more buffers than we're
    // trying to use.
    // if (!window->use_frame_requests) {
    //     TRACER_BEGIN(window->tracer, "window_request_frame_and_wait_for_begin");
    //     ok = window_request_frame_and_wait_for_begin(window);
    //     TRACER_END(window->tracer, "window_request_frame_and_wait_for_begin");
    //     if (ok != 0) {
    //         LOG_ERROR("Could not wait for frame begin.\n");
    //         return ok;
    //     }
    // }

    /// TODO: If we don't have new revisions, we don't need to scanout anything.
    fl_layer_composition_swap_ptrs(&w->base.composition, composition);

    builder = kms_req_builder_new_atomic(w->drmdev, w->resources, w->crtc->id);
    if (builder == NULL) {
        ok = ENOMEM;
        goto fail_unref_builder;
    }

    // We only set the mode once, at the first atomic request.
    if (w->should_apply_mode) {
        ok = kms_req_builder_set_connector(builder, w->connector->id);
        if (ok != 0) {
            LOG_ERROR("Couldn't select connector.\n");
            ok = EIO;
            goto fail_unref_builder;
        }

        ok = kms_req_builder_set_mode(builder, w->mode);
        if (ok != 0) {
            LOG_ERROR("Couldn't apply output mode.\n");
            ok = EIO;
            goto fail_unref_builder;
        }
    }

    for (size_t i = 0; i < fl_layer_composition_get_n_layers(composition); i++) {
        struct fl_layer *layer = fl_layer_composition_peek_layer(composition, i);

        ok = surface_present_kms(layer->surface, &layer->props, builder);
        if (ok != 0) {
            LOG_ERROR("Couldn't present flutter layer on screen. surface_present_kms: %s\n", strerror(ok));
            goto fail_unref_builder;
        }
    }

    // add cursor infos
    if (w->cursor != NULL) {
        ok = kms_req_builder_push_fb_layer(
            builder,
            &(const struct kms_fb_layer){
                .drm_fb_id = w->cursor->drm_fb_id,
                .format = w->cursor->format,
                .has_modifier = true,
                .modifier = DRM_FORMAT_MOD_LINEAR,
                .src_x = 0,
                .src_y = 0,
                .src_w = ((uint16_t) w->cursor->width) << 16,
                .src_h = ((uint16_t) w->cursor->height) << 16,
                .dst_x = (int32_t) (w->base.cursor_pos.x) - w->cursor->hotspot.x,
                .dst_y = (int32_t) (w->base.cursor_pos.y) - w->cursor->hotspot.y,
                .dst_w = w->cursor->width,
                .dst_h = w->cursor->height,
                .has_rotation = false,
                .rotation = PLANE_TRANSFORM_NONE,
                .has_in_fence_fd = false,
                .in_fence_fd = 0,
                .prefer_cursor = true,
            },
            cursor_buffer_unref_void,
            NULL,
            w->cursor
        );
        if (ok != 0) {
            LOG_ERROR("Couldn't present cursor. Hardware cursor will be disabled.\n");

            w->cursor_works = false;
            w->base.cursor_enabled = false;
            cursor_buffer_unrefp(&w->cursor);
        } else {
            cursor_buffer_ref(w->cursor);
        }
    }

    req = kms_req_builder_build(builder);
    if (req == NULL) {
        ok = ENOMEM;
        goto fail_unref_builder;
    }

    kms_req_builder_unref(builder);
    builder = NULL;

    frame = malloc(sizeof *frame);
    if (frame == NULL) {
        ok = ENOMEM;
        goto fail_unref_req;
    }

    frame->req = req;
    frame->tracer = tracer_ref(w->base.tracer);
    frame->drmdev = drmdev_ref(w->drmdev);
    frame->scheduler = frame_scheduler_ref(w->base.frame_scheduler);
    frame->unset_should_apply_mode_on_commit = w->should_apply_mode;

    frame_scheduler_present_frame(w->base.frame_scheduler, on_present_frame, frame, on_cancel_frame);

    // if (window->present_mode == kDoubleBufferedVsync_PresentMode) {
    //     TRACER_BEGIN(window->tracer, "kms_req_builder_commit");
    //     ok = kms_req_commit(req, /* blocking: */ false);
    //     TRACER_END(window->tracer, "kms_req_builder_commit");
    //
    //     if (ok != 0) {
    //         LOG_ERROR("Could not commit frame request.\n");
    //         goto fail_unref_window2;
    //     }
    //
    //     if (window->set_set_mode) {
    //         window->set_mode = false;
    //         window->set_set_mode = false;
    //     }
    // } else {
    //     ASSERT_EQUALS(window->present_mode, kTripleBufferedVsync_PresentMode);
    //
    //     if (window->present_immediately) {
    //         TRACER_BEGIN(window->tracer, "kms_req_builder_commit");
    //         ok = kms_req_commit(req, /* blocking: */ false);
    //         TRACER_END(window->tracer, "kms_req_builder_commit");
    //
    //         if (ok != 0) {
    //             LOG_ERROR("Could not commit frame request.\n");
    //             goto fail_unref_window2;
    //         }
    //
    //         if (window->set_set_mode) {
    //             window->set_mode = false;
    //             window->set_set_mode = false;
    //         }
    //
    //         window->present_immediately = false;
    //     } else {
    //         if (window->next_frame != NULL) {
    //             /// FIXME: Call the release callbacks when the kms_req is destroyed, not when it's unrefed.
    //             /// Not sure this here will lead to the release callbacks being called multiple times.
    //             kms_req_call_release_callbacks(window->next_frame);
    //             kms_req_unref(window->next_frame);
    //         }
    //
    //         window->next_frame = kms_req_ref(req);
    //         window->set_set_mode = window->set_mode;
    //     }
    // }

    // KMS Req is committed now and drmdev keeps a ref
    // on it internally, so we don't need to keep this one.
    // kms_req_unref(req);

    // window_on_rendering_complete(window);

    return 0;

fail_unref_req:
    kms_req_unref(req);
    return ok;

fail_unref_builder:
    kms_req_builder_unref(builder);
    return ok;
}

static int kms_window_push_composition(struct window *window, struct fl_layer_composition *composition) {
    mutex_lock(&window->lock);

    int ok = kms_window_push_composition_locked((struct kms_window *) window, composition);

    mutex_unlock(&window->lock);

    return ok;
}

static bool count_modifiers_for_pixel_format(
    UNUSED struct drm_plane *plane,
    UNUSED int index,
    enum pixfmt pixel_format,
    UNUSED uint64_t modifier,
    void *userdata
) {
    struct {
        enum pixfmt format;
        uint64_t *modifiers;
        size_t n_modifiers;
        int index;
    } *context = userdata;

    if (pixel_format == context->format) {
        context->n_modifiers++;
    }

    return true;
}

static bool extract_modifiers_for_pixel_format(
    UNUSED struct drm_plane *plane,
    UNUSED int index,
    enum pixfmt pixel_format,
    uint64_t modifier,
    void *userdata
) {
    struct {
        enum pixfmt format;
        uint64_t *modifiers;
        size_t n_modifiers;
        int index;
    } *context = userdata;

    if (pixel_format == context->format) {
        context->modifiers[context->index++] = modifier;
    }

    return true;
}

static struct render_surface *kms_window_get_render_surface_internal(struct kms_window *window, bool has_size, UNUSED struct vec2i size) {
    struct render_surface *render_surface;

    ASSERT_NOT_NULL(window);

    if (window->base.render_surface != NULL) {
        return window->base.render_surface;
    }

    if (!has_size) {
        // Flutter wants a render surface, but hasn't told us the backing store dimensions yet.
        // Just make a good guess about the dimensions.
        LOG_DEBUG("Flutter requested render surface before supplying surface dimensions.\n");
        size = VEC2I(window->mode->hdisplay, window->mode->vdisplay);
    }

    enum pixfmt pixel_format;
    if (window->base.has_forced_pixel_format) {
        pixel_format = window->base.forced_pixel_format;
    } else {
        // Actually, more devices support ARGB8888 might sometimes not be supported by devices,
        // for example for primary planes. But we can just cast ARGB8888 to XRGB8888 if we need to,
        // and ARGB8888 is still a good default choice because casting XRGB to ARGB might not work,
        // and sometimes we need alpha for overlay planes.
        // Also vulkan doesn't work with XRGB yet so we definitely need to use ARGB to vulkan too.
        pixel_format = PIXFMT_ARGB8888;
    }

    // Possibly populate this with the supported modifiers for this pixel format.
    // If no plane lists modifiers for this pixel format, this will be left at NULL,
    // and egl_gbm_render_surface_new... will create the GBM surface using usage flags
    // (GBM_USE_SCANOUT | GBM_USE_RENDER) instead.
    uint64_t *allowed_modifiers = NULL;
    size_t n_allowed_modifiers = 0;

    // For now just set the supported modifiers for the first plane that supports this pixel format
    // as the allowed modifiers.
    /// TODO: Find a way to rank pixel formats, maybe by number of planes that support them for scanout.
    drm_resources_for_each_plane(window->resources, plane_it) {
        if (!(plane_it->possible_crtcs & window->crtc->bitmask)) {
            // Only query planes that are possible to connect to the CRTC we're using.
            continue;
        }

        if (plane_it->type != DRM_PRIMARY_PLANE && plane_it->type != DRM_OVERLAY_PLANE) {
            // We explicitly only look for primary and overlay planes.
            continue;
        }

        if (!plane_it->supports_modifiers) {
            // The plane does not have an IN_FORMATS property and does not support
            // explicit modifiers.
            //
            // Calling drm_plane_for_each_modified_format below will segfault.
            continue;
        }

        struct {
            enum pixfmt format;
            uint64_t *modifiers;
            size_t n_modifiers;
            int index;
        } context = {
            .format = pixel_format,
            .modifiers = NULL,
            .n_modifiers = 0,
            .index = 0,
        };

        // First, count the allowed modifiers for this pixel format.
        drm_plane_for_each_modified_format(plane_it, count_modifiers_for_pixel_format, &context);

        if (context.n_modifiers > 0) {
            n_allowed_modifiers = context.n_modifiers;
            allowed_modifiers = calloc(n_allowed_modifiers, sizeof(*context.modifiers));
            context.modifiers = allowed_modifiers;

            // Next, fill context.modifiers with the allowed modifiers.
            drm_plane_for_each_modified_format(plane_it, extract_modifiers_for_pixel_format, &context);
        } else {
            n_allowed_modifiers = 0;
            allowed_modifiers = NULL;
        }
        break;
    }

    if (window->base.renderer_type == kOpenGL_RendererType) {
        // opengl
#ifdef HAVE_EGL_GLES2
    // EGL_NO_CONFIG_KHR is defined by EGL_KHR_no_config_context.
    #ifndef EGL_KHR_no_config_context
        #error "EGL header definitions for extension EGL_KHR_no_config_context are required."
    #endif

        struct egl_gbm_render_surface *egl_surface = egl_gbm_render_surface_new_with_egl_config(
            window->base.tracer,
            size,
            gl_renderer_get_gbm_device(window->base.gl_renderer),
            window->base.gl_renderer,
            pixel_format,
            EGL_NO_CONFIG_KHR,
            allowed_modifiers,
            n_allowed_modifiers
        );
        if (egl_surface == NULL) {
            LOG_ERROR("Couldn't create EGL GBM rendering surface.\n");
            render_surface = NULL;
        } else {
            render_surface = CAST_RENDER_SURFACE(egl_surface);
        }

#else
        UNREACHABLE();
#endif
    } else {
        ASSUME(window->base.renderer_type == kVulkan_RendererType);

        // vulkan
#ifdef HAVE_VULKAN
        struct vk_gbm_render_surface *vk_surface = vk_gbm_render_surface_new(
            window->base.tracer,
            size,
            drmdev_get_gbm_device(window->drmdev),
            window->base.vk_renderer,
            pixel_format
        );
        if (vk_surface == NULL) {
            LOG_ERROR("Couldn't create Vulkan GBM rendering surface.\n");
            render_surface = NULL;
        } else {
            render_surface = CAST_RENDER_SURFACE(vk_surface);
        }
#else
        UNREACHABLE();
#endif
    }

    if (allowed_modifiers != NULL) {
        free(allowed_modifiers);
    }

    window->base.render_surface = render_surface;
    return render_surface;
}

static struct render_surface *kms_window_get_render_surface(struct window *window, struct vec2i size) {
    ASSERT_NOT_NULL(window);
    return kms_window_get_render_surface_internal((struct kms_window *) window, true, size);
}

#ifdef HAVE_EGL_GLES2
static bool kms_window_has_egl_surface(struct window *window) {
    if (window->renderer_type == kOpenGL_RendererType) {
        return window->render_surface != NULL;
    } else {
        return false;
    }
}

static EGLSurface kms_window_get_egl_surface(struct window *window) {
    if (window->renderer_type == kOpenGL_RendererType) {
        struct render_surface *render_surface = kms_window_get_render_surface_internal((struct kms_window *) window, false, VEC2I(0, 0));
        return egl_gbm_render_surface_get_egl_surface(CAST_EGL_GBM_RENDER_SURFACE(render_surface));
    } else {
        return EGL_NO_SURFACE;
    }
}
#endif

static int kms_window_set_cursor_locked(
    // clang-format off
    struct window *window,
    bool has_enabled, bool enabled,
    bool has_kind, enum pointer_kind kind,
    bool has_pos, struct vec2i pos
    // clang-format on
) {
    const struct pointer_icon *icon;
    struct cursor_buffer *cursor;
    struct kms_window *w;

    ASSERT_NOT_NULL(window);
    w = (struct kms_window *) window;

    if (has_kind) {
        if (w->pointer_icon == NULL || pointer_icon_get_kind(w->pointer_icon) != kind) {
            w->pointer_icon = pointer_icon_for_details(kind, w->base.pixel_ratio);
            ASSERT_NOT_NULL(w->pointer_icon);
        }
    }

    enabled = has_enabled ? enabled : w->base.cursor_enabled;
    icon = has_kind ? pointer_icon_for_details(kind, w->base.pixel_ratio) : w->pointer_icon;
    pos = has_pos ? pos : vec2f_round_to_integer(w->base.cursor_pos);
    cursor = w->cursor;

    if (enabled && !w->cursor_works) {
        // hardware cursor is disabled, so we can't enable it.
        return EIO;
    }

    if (enabled && icon == NULL) {
        // default to the arrow icon.
        icon = pointer_icon_for_details(POINTER_KIND_BASIC, w->base.pixel_ratio);
        ASSERT_NOT_NULL(icon);
    }

    if (w->pointer_icon != icon) {
        w->pointer_icon = icon;
    }

    if (enabled) {
        if (cursor == NULL || icon != cursor->icon) {
            cursor = cursor_buffer_new(w->drmdev, w->pointer_icon, w->base.rotation);
            if (cursor == NULL) {
                return EIO;
            }

            cursor_buffer_swap_ptrs(&w->cursor, cursor);

            // cursor is created with refcount 1. cursor_buffer_swap_ptrs
            // increases refcount by one. deref here so we don't leak a
            // reference.
            cursor_buffer_unrefp(&cursor);

            // apply the new cursor icon & position by scanning out a new frame.
            w->base.cursor_pos = VEC2F(pos.x, pos.y);
            if (w->base.composition != NULL) {
                kms_window_push_composition_locked(w, w->base.composition);
            }
        } else if (has_pos) {
            // apply the new cursor position using drmModeMoveCursor
            w->base.cursor_pos = VEC2F(pos.x, pos.y);
            drmdev_move_cursor(w->drmdev, w->crtc->id, vec2i_sub(pos, w->cursor->hotspot));
        }
    } else {
        if (w->cursor != NULL) {
            cursor_buffer_unrefp(&w->cursor);
        }
    }

    w->base.cursor_enabled = enabled;
    return 0;
}

static input_device_match_score_t kms_window_match_input_device(UNUSED struct window *window, UNUSED struct user_input_device *device) {
    return 1;
}

static const struct window_ops kms_window_ops = {
    .deinit = kms_window_deinit,
    .push_composition = kms_window_push_composition,
    .get_render_surface = kms_window_get_render_surface,
#ifdef HAVE_EGL_GLES2
    .has_egl_surface = kms_window_has_egl_surface,
    .get_egl_surface = kms_window_get_egl_surface,
#endif
    .set_cursor_locked = kms_window_set_cursor_locked,
    .match_input_device = kms_window_match_input_device,
};
