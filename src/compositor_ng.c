// SPDX-License-Identifier: MIT
/*
 * compositor-ng
 *
 * - a reimplementation of the flutter compositor
 * - takes flutter layers as input, composits them into multiple hw planes, outputs them to the modesetting interface
 *
 * Copyright (c) 2022, Hannes Winkler <hanneswinkler2000@web.de>
 */

#define _GNU_SOURCE
#include "compositor_ng.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>

#include <pthread.h>
#include <semaphore.h>

#include <flutter_embedder.h>
#include <mesa3d/dynarray.h>
#include <systemd/sd-event.h>

#include "cursor.h"
#include "dummy_render_surface.h"
#include "flutter-pi.h"
#include "frame_scheduler.h"
#include "kms/drmdev.h"
#include "kms/req_builder.h"
#include "kms/resources.h"
#include "kms/monitor.h"
#include "notifier_listener.h"
#include "pixel_format.h"
#include "render_surface.h"
#include "surface.h"
#include "tracer.h"
#include "util/collection.h"
#include "util/logging.h"
#include "util/refcounting.h"
#include "util/khash.h"
#include "util/event_loop.h"
#include "window.h"

#include "config.h"

#ifdef HAVE_GBM
    #include <gbm.h>
#endif

#ifdef HAVE_EGL_GLES2
    #include "egl.h"
    #include "egl_gbm_render_surface.h"
    #include "gl_renderer.h"
#endif

#ifdef HAVE_VULKAN
    #include "vk_gbm_render_surface.h"
    #include "vk_renderer.h"
#endif

/**
 * @brief A nicer, ref-counted version of the FlutterLayer's passed by the engine to the present layer callback.
 *
 * Differences to the FlutterLayer's passed to the present layer callback:
 *  - for platform platform_views:
 *    - struct platform_view* object as the platform view instead of int64_t view id
 *    - position is given as a quadrilateral or axis-aligned rectangle instead of a bunch of (broken) transforms
 *    - same for clip rects
 *    - opacity and rotation as individual numbers
 *  - for backing stores:
 *    - struct render_surface* object instead of FlutterBackingStore
 *    - offset & size as as a struct aa_rect
 *  - refcounted
 */

struct fl_layer_composition *fl_layer_composition_new(size_t n_layers) {
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

size_t fl_layer_composition_get_n_layers(struct fl_layer_composition *composition) {
    ASSERT_NOT_NULL(composition);
    return composition->n_layers;
}

struct fl_layer *fl_layer_composition_peek_layer(struct fl_layer_composition *composition, int layer) {
    ASSERT_NOT_NULL(composition);
    ASSERT_NOT_NULL(composition->layers);
    assert(layer >= 0 && layer < composition->n_layers);
    return composition->layers + layer;
}

void fl_layer_composition_destroy(struct fl_layer_composition *composition) {
    ASSERT_NOT_NULL(composition);

    for (int i = 0; i < composition->n_layers; i++) {
        surface_unref(composition->layers[i].surface);
        if (composition->layers[i].props.clip_rects != NULL) {
            free(composition->layers[i].props.clip_rects);
        }
    }

    free(composition);
}

DEFINE_REF_OPS(fl_layer_composition, n_refs)

KHASH_MAP_INIT_INT64(view, struct window *)
KHASH_MAP_INIT_INT64(platform_view, struct surface *)

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
    struct window *main_window;

    int64_t next_view_id;
    khash_t(view) *views;

    int64_t next_platform_view_id;
    khash_t(platform_view) *platform_views;

    FlutterCompositor flutter_compositor;

    struct vec2f cursor_pos;

    struct drmdev *drmdev;
    struct drm_monitor *monitor;
    struct drm_resources *resources;

    struct evloop *raster_loop;
    struct evsrc *drm_monitor_evsrc;

    struct fl_display_interface display_interface;
    struct display_setup *display_setup;
    struct notifier display_setup_notifier;
};

static bool on_flutter_present_layers(const FlutterLayer **layers, size_t layers_count, void *userdata);

static bool
on_flutter_create_backing_store(const FlutterBackingStoreConfig *config, FlutterBackingStore *backing_store_out, void *userdata);

static bool on_flutter_collect_backing_store(const FlutterBackingStore *fl_store, void *userdata);

static bool on_flutter_present_view(const FlutterPresentViewInfo *present_info);

static enum event_handler_return on_drm_monitor_ready(int fd, uint32_t events, void *userdata) {
    struct compositor *compositor;

    ASSERT_NOT_NULL(userdata);
    (void) fd;
    (void) events;
    
    compositor = userdata;

    // This will in turn probobly call on_drm_uevent.
    drm_monitor_dispatch(compositor->monitor);

    return EVENT_HANDLER_CONTINUE;
}

static void on_drm_uevent(const struct drm_uevent *event, void *userdata) {
    struct compositor *compositor;

    ASSERT_NOT_NULL(event);
    ASSERT_NOT_NULL(userdata);
    compositor = userdata;

    drm_resources_update(compositor->resources, drmdev_get_modesetting_fd(compositor->drmdev), event);
    drm_resources_apply_rockchip_workaround(compositor->resources);

    /// TODO: Check for added displays
    UNIMPLEMENTED();
}

static const struct drm_uevent_listener uevent_listener = {
    .on_uevent = on_drm_uevent,
};

static void connector_init(const struct drm_connector *connector, struct connector *out) {
    static const enum connector_type connector_types[] = {
        [DRM_MODE_CONNECTOR_Unknown] = CONNECTOR_TYPE_OTHER,
        [DRM_MODE_CONNECTOR_VGA] = CONNECTOR_TYPE_VGA,
        [DRM_MODE_CONNECTOR_DVII] = CONNECTOR_TYPE_DVI,
        [DRM_MODE_CONNECTOR_DVID] = CONNECTOR_TYPE_DVI,
        [DRM_MODE_CONNECTOR_DVIA] = CONNECTOR_TYPE_DVI,
        [DRM_MODE_CONNECTOR_Composite] = CONNECTOR_TYPE_OTHER,
        [DRM_MODE_CONNECTOR_SVIDEO] = CONNECTOR_TYPE_OTHER,
        [DRM_MODE_CONNECTOR_LVDS] = CONNECTOR_TYPE_LVDS,
        [DRM_MODE_CONNECTOR_Component] = CONNECTOR_TYPE_OTHER,
        [DRM_MODE_CONNECTOR_9PinDIN] = CONNECTOR_TYPE_OTHER,
        [DRM_MODE_CONNECTOR_DisplayPort] = CONNECTOR_TYPE_DISPLAY_PORT,
        [DRM_MODE_CONNECTOR_HDMIA] = CONNECTOR_TYPE_HDMI,
        [DRM_MODE_CONNECTOR_HDMIB] = CONNECTOR_TYPE_HDMI,
        [DRM_MODE_CONNECTOR_TV] = CONNECTOR_TYPE_TV,
        [DRM_MODE_CONNECTOR_eDP] = CONNECTOR_TYPE_EDP,
        [DRM_MODE_CONNECTOR_VIRTUAL] = CONNECTOR_TYPE_OTHER,
        [DRM_MODE_CONNECTOR_DSI] = CONNECTOR_TYPE_DSI,
        [DRM_MODE_CONNECTOR_DPI] = CONNECTOR_TYPE_DPI,
        [DRM_MODE_CONNECTOR_WRITEBACK] = CONNECTOR_TYPE_OTHER,
        [DRM_MODE_CONNECTOR_SPI] = CONNECTOR_TYPE_OTHER,
        [DRM_MODE_CONNECTOR_USB] = CONNECTOR_TYPE_OTHER,
    };

    enum connector_type type = connector_types[connector->type];
    const char *type_name = drmModeGetConnectorTypeName(connector->type) ?: "Unknown";

    out->id = asprintf("%s-%"PRIu32, type_name, connector->id);
    out->type = type;
    if (type == CONNECTOR_TYPE_OTHER) {
        out->other_type_name = type_name;
    } else {
        out->other_type_name = NULL;
    }
}

static void connector_fini(struct connector *connector) {
    free(connector->id);
}

static void display_init(struct display *display, struct drm_crtc *crtc, struct drm_encoder *encoder, struct drm_connector *connector) {
    
}

static void display_fini(struct display *display) {
    
}

struct display_setup *display_setup_new(struct drm_resources *resources) {
    struct display_setup *setup = malloc(sizeof *setup);
    if (setup == NULL) {
        return NULL;
    }

    setup->n_connectors = resources->n_connectors;
    setup->connectors = malloc(setup->n_connectors * sizeof *setup->connectors);
    if (setup->connectors == NULL) {
        goto fail_free_setup;
    }

    for (int i = 0; i < setup->n_connectors; i++) {
        connector_init(resources->connectors + i, setup->connectors + i);
    }

    setup->n_displays = 0;
    drm_resources_for_each_connector(resources, connector) {
        if (connector->variable_state.connection_state == DRM_CONNSTATE_CONNECTED) {
            setup->n_displays++;
        }
    }

    setup->displays = malloc(setup->n_displays * sizeof *setup->displays);
    if (setup->displays == NULL) {
        goto fail_fini_connectors;
    }

    return setup;

fail_fini_connectors:
    for (int i = 0; i < setup->n_connectors; i++) {
        connector_fini(setup->connectors + i);
    }
    free(setup->connectors);

fail_free_setup:
    free(setup->connectors);
    return NULL;
}

MUST_CHECK struct compositor *compositor_new_multiview(
    struct tracer *tracer,
    struct evloop *raster_loop,
    struct window *main_window,
    struct udev *udev,
    struct drmdev *drmdev,
    struct drm_resources *resources,
    const struct fl_display_interface *display_interface
) {
    struct compositor *c;
    int bucket_status;

    ASSERT_NOT_NULL(tracer);
    ASSERT_NOT_NULL(raster_loop);
    ASSERT_NOT_NULL(main_window);
    ASSERT_NOT_NULL(drmdev);

    c = calloc(1, sizeof *c);
    if (c == NULL) {
        return NULL;
    }

    mutex_init(&c->mutex);
    c->views = kh_init(view);
    c->platform_views = kh_init(platform_view);

    khiter_t entry = kh_put(view, c->views, 0, &bucket_status);
    if (bucket_status == -1) {
        goto fail_return_null;
    }

    kh_value(c->views, entry) = window_ref(main_window);

    c->n_refs = REFCOUNT_INIT_1;
    c->main_window = window_ref(main_window);

    // just so we get an error if the FlutterCompositor struct was updated
    COMPILE_ASSERT(sizeof(FlutterCompositor) == 28 || sizeof(FlutterCompositor) == 56);
    memset(&c->flutter_compositor, 0, sizeof(FlutterCompositor));

    c->flutter_compositor.struct_size = sizeof(FlutterCompositor);
    c->flutter_compositor.user_data = c;
    c->flutter_compositor.create_backing_store_callback = on_flutter_create_backing_store;
    c->flutter_compositor.collect_backing_store_callback = on_flutter_collect_backing_store;
    c->flutter_compositor.present_layers_callback = NULL;
    c->flutter_compositor.avoid_backing_store_cache = true;
    c->flutter_compositor.present_view_callback = on_flutter_present_view;

    c->tracer = tracer_ref(tracer);
    c->cursor_pos = VEC2F(0, 0);
    
    c->next_view_id = 1;
    c->next_platform_view_id = 1;

    c->raster_loop = evloop_ref(raster_loop);
    c->drmdev = drmdev_ref(drmdev);

    if (udev == NULL) {
        udev = udev_new();
        if (udev == NULL) {
            LOG_ERROR("Couldn't create udev.\n");
            goto fail_return_null;
        }
    } else {
        udev_ref(udev);
    }

    c->monitor = drm_monitor_new(NULL, udev, &uevent_listener, c);

    udev_unref(udev);

    if (c->monitor == NULL) {
        goto fail_return_null;
    }

    c->resources = resources != NULL ? drm_resources_ref(resources) : drmdev_query_resources(drmdev);
    c->drm_monitor_evsrc = evloop_add_io(raster_loop, drm_monitor_get_fd(c->monitor), EPOLLIN, on_drm_monitor_ready, c);

    value_notifier_init(&c->display_setup_notifier, );
    return c;

fail_return_null:
    return NULL;
}

struct compositor *compositor_new_singleview(
    struct tracer *tracer,
    struct evloop *raster_loop,
    struct window *window,
    const struct fl_display_interface *display_interface
) {
    return compositor_new_multiview(tracer, raster_loop, window, NULL, NULL, NULL, display_interface);
}

void compositor_destroy(struct compositor *compositor) {
    struct window *window;
    kh_foreach_value(compositor->views, window,
        window_unref(window);
    );

    kh_destroy(view, compositor->views);

    struct surface *surface;
    kh_foreach_value(compositor->platform_views, surface,
        surface_unref(surface);
    )

    kh_destroy(platform_view, compositor->platform_views);

    if (compositor->drm_monitor_evsrc != NULL) {
        evsrc_destroy(compositor->drm_monitor_evsrc);
    }
    if (compositor->monitor != NULL) {
        drm_monitor_destroy(compositor->monitor);
    }
    if (compositor->resources != NULL) {
        drm_resources_unref(compositor->resources);
    }
    if (compositor->drmdev != NULL) {
        drmdev_unref(compositor->drmdev);
    }
    evloop_unref(compositor->raster_loop);

    tracer_unref(compositor->tracer);
    window_unref(compositor->main_window);
    pthread_mutex_destroy(&compositor->mutex);
    free(compositor);
}

DEFINE_REF_OPS(compositor, n_refs)

DEFINE_STATIC_LOCK_OPS(compositor, mutex)

static struct surface *compositor_get_platform_view_by_id_locked(struct compositor *compositor, int64_t view_id) {
    khiter_t entry = kh_get(platform_view, compositor->platform_views, view_id);
    if (entry != kh_end(compositor->platform_views)) {
        return surface_ref(kh_value(compositor->platform_views, entry));
    }

    return NULL;
}

static struct surface *compositor_get_platform_view_by_id(struct compositor *compositor, int64_t view_id) {
    compositor_lock(compositor);
    struct surface *surface = compositor_get_platform_view_by_id_locked(compositor, view_id);
    compositor_unlock(compositor);

    return surface;
}

static struct window *compositor_get_view_by_id_locked(struct compositor *compositor, int64_t view_id) {
    struct window *window = NULL;

    khiter_t entry = kh_get(view, compositor->views, view_id);
    if (entry != kh_end(compositor->views)) {
        window = window_ref(kh_value(compositor->views, entry));
    }

    return window;
}

static struct window *compositor_get_view_by_id(struct compositor *compositor, int64_t view_id) {
    compositor_lock(compositor);
    struct window *window = compositor_get_view_by_id_locked(compositor, view_id);
    compositor_unlock(compositor);
    
    return window;
}

void compositor_get_view_geometry(struct compositor *compositor, struct view_geometry *view_geometry_out) {
    *view_geometry_out = window_get_view_geometry(compositor->main_window);
}

ATTR_PURE double compositor_get_refresh_rate(struct compositor *compositor) {
    return window_get_refresh_rate(compositor->main_window);
}

int compositor_get_next_vblank(struct compositor *compositor, uint64_t *next_vblank_ns_out) {
    ASSERT_NOT_NULL(compositor);
    ASSERT_NOT_NULL(next_vblank_ns_out);
    return window_get_next_vblank(compositor->main_window, next_vblank_ns_out);
}

static int compositor_push_composition(struct compositor *compositor, bool has_view_id, int64_t view_id, struct fl_layer_composition *composition) {
    struct window *window;
    int ok;

    if (has_view_id) {
        window = compositor_get_view_by_id(compositor, view_id);
        if (window == NULL) {
            LOG_ERROR("Couldn't find window with id %" PRId64 " to push composition to.\n", view_id);
            return EINVAL;
        }
    } else {
        window = window_ref(compositor->main_window);
    }

    TRACER_BEGIN(compositor->tracer, "window_push_composition");
    ok = window_push_composition(window, composition);
    TRACER_END(compositor->tracer, "window_push_composition");

    window_unref(window);

    return ok;
}

static void fill_platform_view_layer_props(
    struct fl_layer_props *props_out,
    const FlutterPoint *offset,
    const FlutterSize *size,
    const FlutterPlatformViewMutation **mutations,
    size_t n_mutations,
    const struct mat3f *display_to_view_transform,
    const struct mat3f *view_to_display_transform,
    float device_pixel_ratio
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
        FLUTTER_TRANSFORM_AS_MAT3F(*display_to_view_transform),
        (struct aa_rect){ .offset.x = offset->x, .offset.y = offset->y, .size.x = size->width, .size.y = size->height }
    );

    struct aa_rect rect = quad_get_aa_bounding_rect(quad);

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

    rect.size.x /= (double) device_pixel_ratio;
    rect.size.y /= (double) device_pixel_ratio;

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
            quad = transform_quad(FLUTTER_TRANSFORM_AS_MAT3F(mutations[i]->transformation), quad);

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
    props_out->opacity = opacity;
    props_out->rotation = rotation;

    /// TODO: Implement clip rects
    props_out->n_clip_rects = 0;
    props_out->clip_rects = NULL;
}

static int compositor_push_fl_layers(struct compositor *compositor, bool has_view_id, int64_t view_id, size_t n_fl_layers, const FlutterLayer **fl_layers) {
    struct fl_layer_composition *composition;
    struct view_geometry geometry;
    struct window *window;
    int ok;

    window = has_view_id ? compositor_get_view_by_id(compositor, view_id) : window_ref(compositor->main_window);
    if (window == NULL) {
        LOG_ERROR("Couldn't find window with id %" PRId64 " to push flutter layers to.\n", view_id);
        return EINVAL;
    }

    geometry = window_get_view_geometry(window);

    window_unrefp(&window);

    composition = fl_layer_composition_new(n_fl_layers);
    if (composition == NULL) {
        return ENOMEM;
    }

    for (int i = 0; i < n_fl_layers; i++) {
        const FlutterLayer *fl_layer = fl_layers[i];
        struct fl_layer *layer = fl_layer_composition_peek_layer(composition, i);

        if (fl_layer->type == kFlutterLayerContentTypeBackingStore) {
            /// TODO: Implement
            layer->surface = surface_ref(CAST_SURFACE(fl_layer->backing_store->user_data));

            // Tell the surface that flutter has rendered into this framebuffer / texture / image.
            // It'll also read the did_update field and not update the surface revision in that case.
            render_surface_queue_present(CAST_RENDER_SURFACE(layer->surface), fl_layer->backing_store);

            layer->props.is_aa_rect = true;
            layer->props.aa_rect = AA_RECT_FROM_COORDS(fl_layer->offset.y, fl_layer->offset.y, fl_layer->size.width, fl_layer->size.height);
            layer->props.quad = get_quad(layer->props.aa_rect);
            layer->props.opacity = 1.0;
            layer->props.rotation = 0.0;
            layer->props.n_clip_rects = 0;
            layer->props.clip_rects = NULL;
        } else {
            ASSUME(fl_layer->type == kFlutterLayerContentTypePlatformView);

            layer->surface = compositor_get_platform_view_by_id(compositor, fl_layer->platform_view->identifier);
            if (layer->surface == NULL) {
                /// TODO: Just leave the layer away in this case.
                LOG_ERROR("Invalid platform view id %" PRId64 " in flutter layer.\n", fl_layer->platform_view->identifier);

                layer->surface =
                    CAST_SURFACE(dummy_render_surface_new(compositor->tracer, VEC2I(fl_layer->size.width, fl_layer->size.height)));
            }

            // The coordinates flutter gives us are a bit buggy, so calculating the right geometry is really a problem on its own
            /// TODO: Don't unconditionally take the geometry from the main window.
            fill_platform_view_layer_props(
                &layer->props,
                &fl_layer->offset,
                &fl_layer->size,
                fl_layer->platform_view->mutations,
                fl_layer->platform_view->mutations_count,
                &geometry.display_to_view_transform,
                &geometry.view_to_display_transform,
                geometry.device_pixel_ratio
            );
        }
    }

    TRACER_BEGIN(compositor->tracer, "compositor_push_composition");
    ok = compositor_push_composition(compositor, has_view_id, view_id, composition);
    TRACER_END(compositor->tracer, "compositor_push_composition");

    fl_layer_composition_unref(composition);
    return ok;
}

/// TODO: Remove
UNUSED static bool on_flutter_present_layers(const FlutterLayer **layers, size_t layers_count, void *userdata) {
    struct compositor *compositor;
    int ok;

    ASSERT_NOT_NULL(layers);
    assert(layers_count > 0);
    ASSERT_NOT_NULL(userdata);
    compositor = userdata;

    TRACER_BEGIN(compositor->tracer, "compositor_push_fl_layers");
    ok = compositor_push_fl_layers(compositor, false, -1, layers_count, layers);
    TRACER_END(compositor->tracer, "compositor_push_fl_layers");

    if (ok != 0) {
        return false;
    }

    return true;
}

static bool on_flutter_present_view(const FlutterPresentViewInfo *present_info) {
    struct compositor *compositor;
    int ok;

    ASSERT_NOT_NULL(present_info);
    compositor = present_info->user_data;

    TRACER_BEGIN(compositor->tracer, "compositor_push_fl_layers");
    ok = compositor_push_fl_layers(compositor, true, present_info->view_id, present_info->layers_count, present_info->layers);
    TRACER_END(compositor->tracer, "compositor_push_fl_layers");

    if (ok != 0) {
        return false;
    }

    return true;
}

int compositor_add_platform_view(struct compositor *compositor, struct surface *surface) {
    khiter_t entry;
    int bucket_status;

    ASSERT_NOT_NULL(compositor);
    ASSERT_NOT_NULL(surface);

    compositor_lock(compositor);

    int64_t id = compositor->next_platform_view_id++;

    entry = kh_put(platform_view, compositor->platform_views, id, &bucket_status);
    if (bucket_status == -1) {
        compositor_unlock(compositor);
        return ENOMEM;
    }

    kh_value(compositor->platform_views, entry) = surface_ref(surface);

    compositor_unlock(compositor);
    return 0;
}

void compositor_remove_platform_view(struct compositor *compositor, int64_t id) {
    khiter_t entry;

    ASSERT_NOT_NULL(compositor);

    compositor_lock(compositor);

    entry = kh_get(platform_view, compositor->platform_views, id);
    if (entry != kh_end(compositor->platform_views)) {
        surface_unref(kh_value(compositor->platform_views, entry));
        kh_del(platform_view, compositor->platform_views, entry);
    }

    compositor_unlock(compositor);
}

#ifdef HAVE_EGL_GLES2
bool compositor_has_egl_surface(struct compositor *compositor) {
    return window_has_egl_surface(compositor->main_window);
}

EGLSurface compositor_get_egl_surface(struct compositor *compositor) {
    return window_get_egl_surface(compositor->main_window);
}
#endif

static bool
on_flutter_create_backing_store(const FlutterBackingStoreConfig *config, FlutterBackingStore *backing_store_out, void *userdata) {
    struct render_surface *s;
    struct compositor *compositor;
    struct window *window;
    int ok;

    ASSERT_NOT_NULL(config);
    ASSERT_NOT_NULL(backing_store_out);
    ASSERT_NOT_NULL(userdata);
    compositor = userdata;

    window = compositor_get_view_by_id(compositor, config->view_id);
    if (window == NULL) {
        LOG_ERROR("Couldn't find window with id %" PRId64 " to create backing store for.\n", config->view_id);
        return false;
    }

    // this will not increase the refcount on the surface.
    s = window_get_render_surface(window, VEC2I((int) config->size.width, (int) config->size.height));
    if (s == NULL) {
        LOG_ERROR("Couldn't create render surface for flutter to render into.\n");
        goto fail_unref_window;
    }

    COMPILE_ASSERT(sizeof(FlutterBackingStore) == 56 || sizeof(FlutterBackingStore) == 80);
    memset(backing_store_out, 0, sizeof *backing_store_out);
    backing_store_out->struct_size = sizeof(FlutterBackingStore);

    /// TODO: Make this better
    // compositor_on_event_fd_ready(compositor);

    // render_surface_fill asserts that the user_data is null so it can make sure
    // any concrete render_surface_fill implementation doesn't try to set the user_data.
    // so we set the user_data after the fill
    ok = render_surface_fill(s, backing_store_out);
    if (ok != 0) {
        LOG_ERROR("Couldn't fill flutter backing store with concrete OpenGL framebuffer/texture or Vulkan image.\n");
        goto fail_unref_window;
    }

    // now we can set the user_data.
    backing_store_out->user_data = s;
    window_unref(window);
    return true;

fail_unref_window:
    window_unref(window);
    return false;
}

static bool on_flutter_collect_backing_store(const FlutterBackingStore *fl_store, void *userdata) {
    struct compositor *compositor;

    ASSERT_NOT_NULL(fl_store);
    ASSERT_NOT_NULL(userdata);
    compositor = userdata;

    /// TODO: What should we do here?
    (void) fl_store;
    (void) compositor;

    return true;
}

const FlutterCompositor *compositor_get_flutter_compositor(struct compositor *compositor) {
    ASSERT_NOT_NULL(compositor);
    return &compositor->flutter_compositor;
}

void compositor_set_cursor(
    struct compositor *compositor,
    bool has_enabled,
    bool enabled,
    bool has_kind,
    enum pointer_kind kind,
    bool has_delta,
    struct vec2f delta
) {
    if (!has_enabled && !has_kind && !has_delta) {
        return;
    }

    compositor_lock(compositor);

    if (has_delta) {
        // move cursor
        compositor->cursor_pos = vec2f_add(compositor->cursor_pos, delta);

        struct view_geometry viewgeo = window_get_view_geometry(compositor->main_window);

        if (compositor->cursor_pos.x < 0.0) {
            compositor->cursor_pos.x = 0.0;
        } else if (compositor->cursor_pos.x > viewgeo.view_size.x) {
            compositor->cursor_pos.x = viewgeo.view_size.x;
        }

        if (compositor->cursor_pos.y < 0.0) {
            compositor->cursor_pos.y = 0.0;
        } else if (compositor->cursor_pos.y > viewgeo.view_size.y) {
            compositor->cursor_pos.y = viewgeo.view_size.y;
        }
    }

    window_set_cursor(
        compositor->main_window,
        has_enabled,
        enabled,
        has_kind,
        kind,
        has_delta,
        VEC2I((int) round(compositor->cursor_pos.x), (int) round(compositor->cursor_pos.y))
    );

    compositor_unlock(compositor);
}

static bool call_display_callback_with_drm_connector(display_callback_t callback, const struct drm_connector *connector, void *userdata) {
    (void) callback;
    (void) connector;
    (void) userdata;
    
    /// TODO: Implement
    UNIMPLEMENTED();
}

static bool call_connector_callback_with_drm_connector(connector_callback_t callback, const struct drm_connector *connector, void *userdata) {
    struct connector conn = {
        .id = NULL,
        .type = CONNECTOR_TYPE_OTHER,
        .other_type_name = NULL,
    };

    switch (connector->type) {
        case DRM_MODE_CONNECTOR_Unknown:
            conn.id = alloca_sprintf("Unknown-%" PRIu32, connector->type_id);
            conn.type = CONNECTOR_TYPE_OTHER;
            conn.other_type_name = "Unknown";
            break;
        case DRM_MODE_CONNECTOR_VGA:
            conn.id = alloca_sprintf("VGA-%" PRIu32, connector->type_id);
            conn.type = CONNECTOR_TYPE_VGA;
            break;
        case DRM_MODE_CONNECTOR_DVII:
            conn.id = alloca_sprintf("DVI-I-%" PRIu32, connector->type_id);
            conn.type = CONNECTOR_TYPE_DVI;
            break;
        case DRM_MODE_CONNECTOR_DVID:
            conn.id = alloca_sprintf("DVI-D-%" PRIu32, connector->type_id);
            conn.type = CONNECTOR_TYPE_DVI;
            break;
        case DRM_MODE_CONNECTOR_DVIA:
            conn.id = alloca_sprintf("DVI-A-%" PRIu32, connector->type_id);
            conn.type = CONNECTOR_TYPE_DVI;
            break;
        case DRM_MODE_CONNECTOR_Composite:
            conn.id = alloca_sprintf("Composite-%" PRIu32, connector->type_id);
            conn.type = CONNECTOR_TYPE_OTHER;
            conn.other_type_name = "Composite";
            break;
        case DRM_MODE_CONNECTOR_SVIDEO:
            conn.id = alloca_sprintf("SVIDEO-%" PRIu32, connector->type_id);
            conn.type = CONNECTOR_TYPE_OTHER;
            conn.other_type_name = "SVIDEO";
            break;
        case DRM_MODE_CONNECTOR_LVDS:
            conn.id = alloca_sprintf("LVDS-%" PRIu32, connector->type_id);
            conn.type = CONNECTOR_TYPE_LVDS;
            break;
        case DRM_MODE_CONNECTOR_Component:
            conn.id = alloca_sprintf("Component-%" PRIu32, connector->type_id);
            conn.type = CONNECTOR_TYPE_OTHER;
            conn.other_type_name = "Component";
            break;
        case DRM_MODE_CONNECTOR_9PinDIN:
            conn.id = alloca_sprintf("DIN-%" PRIu32, connector->type_id);
            conn.type = CONNECTOR_TYPE_OTHER;
            conn.other_type_name = "DIN";
            break;
        case DRM_MODE_CONNECTOR_DisplayPort:
            conn.id = alloca_sprintf("DP-%" PRIu32, connector->type_id);
            conn.type = CONNECTOR_TYPE_DISPLAY_PORT;
            break;
        case DRM_MODE_CONNECTOR_HDMIA:
            conn.id = alloca_sprintf("HDMI-A-%" PRIu32, connector->type_id);
            conn.type = CONNECTOR_TYPE_HDMI;
            break;
        case DRM_MODE_CONNECTOR_HDMIB:
            conn.id = alloca_sprintf("HDMI-B-%" PRIu32, connector->type_id);
            conn.type = CONNECTOR_TYPE_HDMI;
            break;
        case DRM_MODE_CONNECTOR_TV:
            conn.id = alloca_sprintf("TV-%" PRIu32, connector->type_id);
            conn.type = CONNECTOR_TYPE_TV;
            break;
        case DRM_MODE_CONNECTOR_eDP:
            conn.id = alloca_sprintf("eDP-%" PRIu32, connector->type_id);
            conn.type = CONNECTOR_TYPE_EDP;
            break;
        case DRM_MODE_CONNECTOR_VIRTUAL:
            conn.id = alloca_sprintf("Virtual-%" PRIu32, connector->type_id);
            conn.type = CONNECTOR_TYPE_OTHER;
            conn.other_type_name = "Virtual";
            break;
        case DRM_MODE_CONNECTOR_DSI:
            conn.id = alloca_sprintf("DSI-%" PRIu32, connector->type_id);
            conn.type = CONNECTOR_TYPE_DSI;
            break;
        case DRM_MODE_CONNECTOR_DPI:
            conn.id = alloca_sprintf("DPI-%" PRIu32, connector->type_id);
            conn.type = CONNECTOR_TYPE_DPI;
            break;
        case DRM_MODE_CONNECTOR_WRITEBACK:
            return true;
        case DRM_MODE_CONNECTOR_SPI:
            conn.id = alloca_sprintf("SPI-%" PRIu32, connector->type_id);
            conn.type = CONNECTOR_TYPE_OTHER;
            conn.other_type_name = "SPI";
            break;
        case DRM_MODE_CONNECTOR_USB:
            conn.id = alloca_sprintf("USB-%" PRIu32, connector->type_id);
            conn.type = CONNECTOR_TYPE_OTHER;
            conn.other_type_name = "USB";
            break;
        default:
            return true;
    }

    return callback(&conn, userdata);
}

int64_t compositor_add_view(
    struct compositor *compositor,
    struct window *window
) {
    ASSERT_NOT_NULL(compositor);
    ASSERT_NOT_NULL(window);
    int64_t view_id;

    compositor_lock(compositor);

    view_id = compositor->next_view_id++;

    khiter_t entry = kh_put(view, compositor->views, view_id, NULL);
    kh_val(compositor->views, entry) = window_ref(window);

    compositor_unlock(compositor);

    return view_id;
}

void compositor_remove_view(
    struct compositor *compositor,
    int64_t view_id
) {
    ASSERT_NOT_NULL(compositor);
    ASSERT(view_id != 0);

    compositor_lock(compositor);

    khiter_t entry = kh_get(view, compositor->views, view_id);
    if (entry != kh_end(compositor->views)) {
        window_unref(kh_val(compositor->views, entry));
        kh_del(view, compositor->views, entry);
    }

    compositor_unlock(compositor);
}

int compositor_put_implicit_view(
    struct compositor *compositor,
    struct window *window
) {
    int bucket_status;

    ASSERT_NOT_NULL(compositor);
    ASSERT_NOT_NULL(window);

    compositor_lock(compositor);

    khiter_t entry = kh_put(view, compositor->views, 0, &bucket_status);
    if (bucket_status == -1) {
        compositor_unlock(compositor);
        return ENOMEM;
    }

    if (bucket_status == 0) {
        window_swap_ptrs(&kh_val(compositor->views, entry), window);
    } else {
        kh_val(compositor->views, entry) = window_ref(window);
    }

    compositor_unlock(compositor);

    return 0;
}

void compositor_for_each_display(
    struct compositor *compositor,
    display_callback_t callback,
    void *userdata
) {
    struct drm_connector *connector;

    (void) compositor;
    (void) callback;
    (void) userdata;
    (void) connector;

    /// TODO: Implement
    UNIMPLEMENTED();

    /// TODO: drm_resources is not mt-safe, but compositor_for_each_connector might not be called on the raster thread
    ///   (which uses drm_resources)

    drm_resources_for_each_connector(compositor->resources, connector) {
        if (!call_display_callback_with_drm_connector(callback, connector, userdata)) {
            break;
        }
    }
}
 
void compositor_for_each_connector(
    struct compositor *compositor,
    connector_callback_t callback,
    void *userdata
) {
    /// TODO: drm_resources is not mt-safe, but compositor_for_each_connector might not be called on the raster thread
    ///   (which uses drm_resources)
    drm_resources_for_each_connector(compositor->resources, connector) {
        if (!call_connector_callback_with_drm_connector(callback, connector, userdata)) {
            break;
        }
    }
}

struct notifier *compositor_get_display_setup_notifier(struct compositor *compositor) {
    ASSERT_NOT_NULL(compositor);
    return &compositor->display_setup_notifier;
}
