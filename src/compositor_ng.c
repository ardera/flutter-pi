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

#include <render_surface.h>
#include <collection.h>
#include <compositor_ng.h>
#include <egl.h>
#include <flutter-pi.h>
#include <egl_gbm_render_surface.h>
#include <vk_gbm_render_surface.h>
#include <gl_renderer.h>
#include <vk_renderer.h>
#include <modesetting.h>
#include <notifier_listener.h>
#include <pixel_format.h>
#include <surface.h>
#include <tracer.h>
#include <window.h>
#include <frame_scheduler.h>

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
    DEBUG_ASSERT_NOT_NULL(composition);
    return composition->n_layers;
}

struct fl_layer *fl_layer_composition_peek_layer(struct fl_layer_composition *composition, int layer) {
    DEBUG_ASSERT_NOT_NULL(composition);
    DEBUG_ASSERT_NOT_NULL(composition->layers);
    DEBUG_ASSERT(layer >= 0 && layer < composition->n_layers);
    return composition->layers + layer;
}

void fl_layer_composition_destroy(struct fl_layer_composition *composition) {
    DEBUG_ASSERT_NOT_NULL(composition);

    for (int i = 0; i < composition->n_layers; i++) {
        surface_unref(composition->layers[i].surface);
        if (composition->layers[i].props.clip_rects != NULL) {
            free(composition->layers[i].props.clip_rects);
        }
    }

    free(composition);
}

DEFINE_REF_OPS(fl_layer_composition, n_refs)

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
    struct tracer *tracer,
    struct window *main_window
) {
    struct compositor *compositor;
    int ok;

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

    
    compositor->n_refs = REFCOUNT_INIT_1;
    // just so we get an error if the FlutterCompositor struct was updated
    COMPILE_ASSERT(sizeof(FlutterCompositor) == 24);
    compositor->main_window = window_ref(main_window);
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

fail_destroy_mutex:
    pthread_mutex_destroy(&compositor->mutex);

fail_free_compositor:
    free(compositor);

fail_return_null:
    return NULL;
}

void compositor_destroy(struct compositor *compositor) {
    struct platform_view_with_id *view;

    for_each_pointer_in_pset(&compositor->platform_views, view) {
        surface_unref(view->surface);
        free(view);
    }
    pset_deinit(&compositor->platform_views);
    window_unref(compositor->main_window);
    pthread_mutex_destroy(&compositor->mutex);
    free(compositor);
}

DEFINE_REF_OPS(compositor, n_refs)

DEFINE_STATIC_LOCK_OPS(compositor, mutex)

void compositor_get_view_geometry(struct compositor *compositor, struct view_geometry *view_geometry_out) {
    *view_geometry_out = window_get_view_geometry(compositor->main_window);
}

ATTR_PURE double compositor_get_refresh_rate(struct compositor *compositor) {
    return window_get_refresh_rate(compositor->main_window);
}

int compositor_get_next_vblank(struct compositor *compositor, uint64_t *next_vblank_ns_out) {
    DEBUG_ASSERT_NOT_NULL(compositor);
    DEBUG_ASSERT_NOT_NULL(next_vblank_ns_out);
    return window_get_next_vblank(compositor->main_window, next_vblank_ns_out);
}

static int compositor_push_composition(struct compositor *compositor, struct fl_layer_composition *composition) {
    int ok;

    TRACER_BEGIN(compositor->tracer, "window_push_composition");
    ok = window_push_composition(compositor->main_window, composition);
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
            render_surface_queue_present(CAST_RENDER_SURFACE(layer->surface), fl_layer->backing_store);

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

            struct view_geometry geometry = window_get_view_geometry(compositor->main_window);

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

bool compositor_has_egl_surface(struct compositor *compositor) {
    return window_has_egl_surface(compositor->main_window);
}

EGLSurface compositor_get_egl_surface(struct compositor *compositor) {
    return window_get_egl_surface(compositor->main_window);
}

static bool on_flutter_create_backing_store(
    const FlutterBackingStoreConfig *config,
    FlutterBackingStore *backing_store_out,
    void *userdata
) {
    struct render_surface *s;
    struct compositor *compositor;
    int ok;

    DEBUG_ASSERT_NOT_NULL(config);
    DEBUG_ASSERT_NOT_NULL(backing_store_out);
    DEBUG_ASSERT_NOT_NULL(userdata);
    compositor = userdata;

    // we have a reference on this surface.
    // i.e. when we don't use it, we need to unref it.
    s = window_create_render_surface(compositor->main_window, POINT(config->size.width, config->size.height));
    if (s == NULL) {
        LOG_ERROR("Couldn't create render surface for flutter to render into.\n");
        return false;
    }

    COMPILE_ASSERT(sizeof(FlutterBackingStore) == 56);
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
        surface_unref(CAST_SURFACE_UNCHECKED(s));
        return false;
    }

    // now we can set the user_data.
    backing_store_out->user_data = s;

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
