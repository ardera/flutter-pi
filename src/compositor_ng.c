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
#include <klib/khash.h>
#include <klib/kvec.h>
#include <mesa3d/dynarray.h>
#include <systemd/sd-event.h>

#include "cursor.h"
#include "dummy_render_surface.h"
#include "flutter-pi.h"
#include "frame_scheduler.h"
#include "kms/drmdev.h"
#include "kms/monitor.h"
#include "kms/req_builder.h"
#include "kms/resources.h"
#include "notifier_listener.h"
#include "pixel_format.h"
#include "render_surface.h"
#include "surface.h"
#include "tracer.h"
#include "user_input.h"
#include "util/collection.h"
#include "util/event_loop.h"
#include "util/khash_uint32.h"
#include "util/logging.h"
#include "util/refcounting.h"
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

KHASH_MAP_INIT_UINT32(connector_display_ids, int64_t)
KHASH_SET_INIT_UINT32(connector_set)

KHASH_MAP_INIT_INT64(view, struct window *)
KHASH_MAP_INIT_INT64(platform_view, struct surface *)

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

struct display {
    size_t fl_display_id;

    // This depends on the current mode of the CRTC.
    double refresh_rate;
    struct vec2i size;

    struct vec2i physical_size;
    double device_pixel_ratio;
};

struct connector {
    char *name;
    enum connector_type type;
    const char *type_name;

    bool has_display;
    struct display display;
};

struct display_setup {
    refcount_t n_refs;

    size_t n_connectors;
    struct connector connectors[];
};

/**
 * @brief Gets the name for a DRM connector type.
 * 
 * Follows the kernel naming.
 */
static const char *get_connector_type_name(uint32_t connector_type) {
    switch (connector_type) {
        case DRM_MODE_CONNECTOR_Unknown: return "Unknown";
        case DRM_MODE_CONNECTOR_VGA: return "VGA";
        case DRM_MODE_CONNECTOR_DVII: return "DVI-I";
        case DRM_MODE_CONNECTOR_DVID: return "DVI-D";
        case DRM_MODE_CONNECTOR_DVIA: return "DVI-A";
        case DRM_MODE_CONNECTOR_Composite: return "Composite";
        case DRM_MODE_CONNECTOR_SVIDEO: return "SVIDEO";
        case DRM_MODE_CONNECTOR_LVDS: return "LVDS";
        case DRM_MODE_CONNECTOR_Component: return "Component";
        case DRM_MODE_CONNECTOR_9PinDIN: return "DIN";
        case DRM_MODE_CONNECTOR_DisplayPort: return "DP";
        case DRM_MODE_CONNECTOR_HDMIA: return "HDMI-A";
        case DRM_MODE_CONNECTOR_HDMIB: return "HDMI-B";
        case DRM_MODE_CONNECTOR_TV: return "TV";
        case DRM_MODE_CONNECTOR_eDP: return "eDP";
        case DRM_MODE_CONNECTOR_VIRTUAL: return "Virtual";
        case DRM_MODE_CONNECTOR_DSI: return "DSI";
        case DRM_MODE_CONNECTOR_DPI: return "DPI";
        case DRM_MODE_CONNECTOR_WRITEBACK: return "Writeback";
#ifdef DRM_MODE_CONNECTOR_SPI
        case DRM_MODE_CONNECTOR_SPI: return "SPI";
#endif
#ifdef DRM_MODE_CONNECTOR_USB
        case DRM_MODE_CONNECTOR_USB: return "USB";
#endif
        default: return NULL;
    }
}

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
#ifdef DRM_MODE_CONNECTOR_SPI
    [DRM_MODE_CONNECTOR_SPI] = CONNECTOR_TYPE_OTHER,
#endif
#ifdef DRM_MODE_CONNECTOR_USB
    [DRM_MODE_CONNECTOR_USB] = CONNECTOR_TYPE_OTHER,
#endif
};

bool connector_init(const struct drm_connector *connector, int64_t fl_display_id, struct connector *out) {
    // We unfortunately can't use drmModeGetConnectorTypeName yet
    // because it's too new.
    const char *type_name = get_connector_type_name(connector->type);
    if (type_name == NULL) {
        // if we don't know this type, skip it.
        return false;
    }

    out->name = NULL;
    asprintf(&out->name, "%s-%" PRIu32, type_name, connector->id);
    if (out->name == NULL) {
        return false;
    }

    assert(connector->type < ARRAY_SIZE(connector_types));
    out->type = connector_types[connector->type];
    out->type_name = type_name;

    if (connector->variable_state.connection_state == DRM_CONNSTATE_CONNECTED) {
        out->has_display = true;

        out->display.fl_display_id = fl_display_id;
        out->display.refresh_rate = 60.0;
        out->display.size = VEC2I(0, 0);
        out->display.device_pixel_ratio = 1.0;

        out->display.physical_size = VEC2I(connector->variable_state.width_mm, connector->variable_state.height_mm);
    } else {
        out->has_display = false;
    }

    return true;
}

void connector_fini(struct connector *connector) {
    free(connector->name);
}

struct display_setup *display_setup_new(struct drm_resources *resources, khash_t(connector_display_ids) * connectors) {
    struct display_setup *s;

    s = calloc(1, sizeof *s + sizeof(struct connector) * resources->n_connectors);
    if (s == NULL) {
        return NULL;
    }

    s->n_connectors = 0;
    for (size_t i = 0; i < resources->n_connectors; i++) {
        if (resources->connectors[i].variable_state.connection_state != DRM_CONNSTATE_CONNECTED) {
            continue;
        }

        khiter_t entry = kh_get(connector_display_ids, connectors, resources->connectors[s->n_connectors].id);
        if (entry == kh_end(connectors)) {
            continue;
        }

        int64_t fl_display_id = kh_value(connectors, entry);

        bool bucket_status = connector_init(resources->connectors + i, fl_display_id, s->connectors + s->n_connectors);
        if (!bucket_status) {
            continue;
        }

        s->n_connectors++;
    }

    return s;
}

void display_setup_destroy(struct display_setup *setup) {
    for (int i = 0; i < setup->n_connectors; i++) {
        connector_fini(setup->connectors + i);
    }
    free(setup);
}

DEFINE_REF_OPS(display_setup, n_refs)

size_t display_setup_get_n_connectors(struct display_setup *setup) {
    return setup->n_connectors;
}

const struct connector *display_setup_get_connector(struct display_setup *setup, size_t index) {
    return setup->connectors + index;
}

const char *connector_get_name(const struct connector *connector) {
    return connector->name;
}

enum connector_type connector_get_type(const struct connector *connector) {
    return connector->type;
}

const char *connector_get_type_name(const struct connector *connector) {
    return connector->type_name;
}

bool connector_has_display(const struct connector *connector) {
    return connector->has_display;
}

const struct display *connector_get_display(const struct connector *connector) {
    return &connector->display;
}

size_t display_get_fl_display_id(const struct display *display) {
    return display->fl_display_id;
}

double display_get_refresh_rate(const struct display *display) {
    return display->refresh_rate;
}

struct vec2i display_get_size(const struct display *display) {
    return display->size;
}

struct vec2i display_get_physical_size(const struct display *display) {
    return display->physical_size;
}

double display_get_device_pixel_ratio(const struct display *display) {
    return display->device_pixel_ratio;
}

const char *display_get_connector_id(const struct display *display) {
    PRAGMA_DIAGNOSTIC_PUSH
    PRAGMA_DIAGNOSTIC_IGNORED("-Wpedantic")

    return CONTAINER_OF(display, struct connector, display)->name;

    PRAGMA_DIAGNOSTIC_POP
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
    struct window *implicit_view_fallback;

    int64_t next_display_id;
    khash_t(connector_display_ids) * connectors;

    int64_t next_view_id;
    khash_t(view) * views;

    int64_t next_platform_view_id;
    khash_t(platform_view) * platform_views;

    FlutterCompositor flutter_compositor;

    struct drmdev *drmdev;
    struct drm_monitor *monitor;
    struct drm_resources *resources;

    struct evloop *raster_loop;
    struct evsrc *drm_monitor_evsrc;

    struct notifier display_setup_notifier;

    bool is_startup;
    bool has_display_interface;
    struct fl_display_interface display_interface;

    bool has_pointer_event_interface;
    struct fl_pointer_event_interface pointer_event_interface;

    kvec_t(int64_t) view_layout;

    struct {
        bool enabled;
        struct vec2f pos_view;
        int64_t fl_view_id;
        struct window *window;
    } cursor;
};

static bool on_flutter_present_layers(const FlutterLayer **layers, size_t layers_count, void *userdata);

static bool
on_flutter_create_backing_store(const FlutterBackingStoreConfig *config, FlutterBackingStore *backing_store_out, void *userdata);

static bool on_flutter_collect_backing_store(const FlutterBackingStore *fl_store, void *userdata);

static bool on_flutter_present_view(const FlutterPresentViewInfo *present_info);

static int update_flutter_displays(struct compositor *c) {
    int bucket_status;

    // Allocate the display list on the stack.
    FlutterEngineDisplay *displays = NULL;
    if (c->has_display_interface) {
        displays = alloca(c->resources->n_connectors * sizeof(FlutterEngineDisplay));
        if (displays == NULL) {
            return ENOMEM;
        }
    }

    // Populate the display list, and also allocate ids for new displays,
    // remove entries for disconnected displays.
    size_t n_displays = 0;
    drm_resources_for_each_connector(c->resources, connector) {
        if (connector->variable_state.connection_state == DRM_CONNSTATE_CONNECTED) {
            int64_t id;

            khiter_t entry = kh_put(connector_display_ids, c->connectors, connector->id, &bucket_status);
            if (bucket_status == -1) {
                return ENOMEM;
            } else if (bucket_status == 0) {
                // We already know this display.
                id = kh_value(c->connectors, entry);
            } else {
                // We don't know this display yet.
                // Allocate an id for it.
                id = c->next_display_id++;
                kh_value(c->connectors, entry) = id;
            }

            if (displays != NULL) {
                memset(displays + n_displays, 0, sizeof(FlutterEngineDisplay));

                displays[n_displays].struct_size = sizeof(FlutterEngineDisplay);
                displays[n_displays].display_id = id;
                displays[n_displays].single_display = n_displays == 1;

                /// TODO: Calculate these
                displays[n_displays].refresh_rate = 0.0;
                displays[n_displays].width = 0;
                displays[n_displays].height = 0;
                displays[n_displays].device_pixel_ratio = 0.0;
                n_displays++;
            }
        } else {
            // Remove this display.
            khiter_t entry = kh_get(connector_display_ids, c->connectors, connector->id);
            if (entry != kh_end(c->connectors)) {
                kh_del(connector_display_ids, c->connectors, entry);
            }
        }
    }

    /// TODO: Remove display entries for removed connectors.
    if (displays != NULL) {
        FlutterEngineResult engine_result = c->display_interface.notify_display_update(
            c->display_interface.engine,
            kFlutterEngineDisplaysUpdateTypeStartup,
            displays,
            n_displays
        );
        if (engine_result != kSuccess) {
            LOG_ERROR(
                "Couldn't register displays to flutter engine. FlutterEngineNotifyDisplayUpdate: %s\n",
                FLUTTER_RESULT_TO_STRING(engine_result)
            );
            return EIO;
        }

        c->is_startup = false;
    }

    return 0;
}

static void send_window_metrics_events(struct compositor *compositor) {
    int64_t view_id;
    struct window *window;
    kh_foreach(compositor->views, view_id, window, {
        struct view_geometry geo = window_get_view_geometry(window);

        COMPILE_ASSERT(sizeof(FlutterWindowMetricsEvent) == 80 || sizeof(FlutterWindowMetricsEvent) == 96);

        FlutterWindowMetricsEvent event;
        memset(&event, 0, sizeof event);

        event.struct_size = sizeof(FlutterWindowMetricsEvent);
        event.width = (size_t) geo.view_size.x;
        event.height = (size_t) geo.view_size.y;
        event.pixel_ratio = (double) geo.device_pixel_ratio;
        event.left = 0;
        event.top = 0;
        event.physical_view_inset_top = 0;
        event.physical_view_inset_right = 0;
        event.physical_view_inset_bottom = 0;
        event.physical_view_inset_left = 0;
        event.display_id = 0;
        event.view_id = view_id;

        FlutterEngineResult engine_result =
            compositor->display_interface.send_window_metrics_event(compositor->display_interface.engine, &event);
        if (engine_result != kSuccess) {
            LOG_ERROR(
                "Could not send window metrics to flutter engine. FlutterEngineSendWindowMetricsEvent: %s\n",
                FLUTTER_RESULT_TO_STRING(engine_result)
            );
        }
    });
}

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

    update_flutter_displays(compositor);

    struct display_setup *display_setup = display_setup_new(compositor->resources, compositor->connectors);
    if (display_setup == NULL) {
        LOG_ERROR("Couldn't create display setup.\n");
        return;
    }

    notifier_notify(&compositor->display_setup_notifier, display_setup);
}

static const struct drm_uevent_listener uevent_listener = {
    .on_uevent = on_drm_uevent,
};

static void on_input(void *userdata, size_t n_events, const struct user_input_event *events);

MUST_CHECK struct compositor *compositor_new_multiview(
    struct tracer *tracer,
    struct evloop *raster_loop,
    struct udev *udev,
    struct drmdev *drmdev,
    struct drm_resources *resources,
    struct user_input *input
) {
    struct compositor *c;
    int bucket_status;

    ASSERT_NOT_NULL(tracer);
    ASSERT_NOT_NULL(raster_loop);
    ASSERT_NOT_NULL(drmdev);

    c = calloc(1, sizeof *c);
    if (c == NULL) {
        return NULL;
    }

    mutex_init(&c->mutex);
    c->views = kh_init(view);
    c->platform_views = kh_init(platform_view);
    c->connectors = kh_init(connector_display_ids);

    c->n_refs = REFCOUNT_INIT_1;
    c->implicit_view_fallback = NULL;

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

    kv_init(c->view_layout);
    c->cursor.enabled = false;
    c->cursor.pos_view = VEC2F(0, 0);
    c->cursor.fl_view_id = -1;
    c->cursor.window = NULL;

    c->next_view_id = 0;
    c->next_platform_view_id = 1;
    c->next_display_id = 1;

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

    c->is_startup = true;
    c->has_display_interface = false;

    bucket_status = update_flutter_displays(c);
    if (bucket_status != 0) {
        goto fail_return_null;
    }

    struct display_setup *display_setup = display_setup_new(c->resources, c->connectors);
    if (display_setup == NULL) {
        LOG_ERROR("Couldn't create display setup.\n");
        goto fail_return_null;
    }

    value_notifier_init(&c->display_setup_notifier, display_setup, display_setup_unref_void);

    user_input_add_primary_listener(
        input,
        USER_INPUT_DEVICE_ADDED | USER_INPUT_DEVICE_REMOVED | USER_INPUT_SLOT_ADDED | USER_INPUT_SLOT_REMOVED | USER_INPUT_POINTER |
            USER_INPUT_TOUCH | USER_INPUT_TABLET_TOOL,
        on_input,
        c
    );

    return c;

fail_return_null:
    return NULL;
}

struct compositor *
compositor_new_singleview(struct tracer *tracer, struct evloop *raster_loop, struct window *window, struct user_input *input) {
    struct compositor *c = compositor_new_multiview(tracer, raster_loop, NULL, NULL, NULL, input);
    if (c == NULL) {
        return NULL;
    }

    compositor_add_view(c, window);
    return c;
}

void compositor_destroy(struct compositor *compositor) {
    struct window *window;
    kh_foreach_value(compositor->views, window, window_unref(window););

    kh_destroy(view, compositor->views);

    struct surface *surface;
    kh_foreach_value(compositor->platform_views, surface, surface_unref(surface);)

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
    window_unref(compositor->implicit_view_fallback);
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
        window = kh_value(compositor->views, entry);
    }

    return window;
}

static struct window *compositor_get_view_by_id(struct compositor *compositor, int64_t view_id) {
    compositor_lock(compositor);
    struct window *window = compositor_get_view_by_id_locked(compositor, view_id);
    if (window) {
        window = window_ref(window);
    }
    compositor_unlock(compositor);

    return window;
}

struct window *compositor_ref_implicit_view(struct compositor *compositor) {
    ASSERT_NOT_NULL(compositor);

    mutex_lock(&compositor->mutex);

    if (compositor->implicit_view_fallback == NULL) {
        mutex_unlock(&compositor->mutex);
        return NULL;
    }

    struct window *w = window_ref(compositor->implicit_view_fallback);

    mutex_unlock(&compositor->mutex);

    return w;
}

void compositor_set_fl_display_interface(struct compositor *compositor, const struct fl_display_interface *display_interface) {
    ASSERT_NOT_NULL(compositor);
    ASSERT_NOT_NULL(display_interface);

    if (display_interface == NULL) {
        compositor->has_display_interface = false;
        return;
    }

    compositor->has_display_interface = true;
    compositor->display_interface = *display_interface;

    // register flutter displays
    update_flutter_displays(compositor);

    // send window metrics event
    send_window_metrics_events(compositor);
}

void compositor_set_fl_pointer_event_interface(
    struct compositor *compositor,
    const struct fl_pointer_event_interface *pointer_event_interface
) {
    ASSERT_NOT_NULL(compositor);
    ASSERT_NOT_NULL(pointer_event_interface);

    if (pointer_event_interface == NULL) {
        compositor->has_pointer_event_interface = false;
        return;
    } else {
        compositor->has_pointer_event_interface = true;
        compositor->pointer_event_interface = *pointer_event_interface;
    }
}

ATTR_PURE float compositor_get_refresh_rate(struct compositor *compositor) {
    return window_get_refresh_rate(compositor->implicit_view_fallback);
}

int compositor_get_next_vblank(struct compositor *compositor, uint64_t *next_vblank_ns_out) {
    ASSERT_NOT_NULL(compositor);
    ASSERT_NOT_NULL(next_vblank_ns_out);
    return window_get_next_vblank(compositor->implicit_view_fallback, next_vblank_ns_out);
}

static int
compositor_push_composition(struct compositor *compositor, bool has_view_id, int64_t view_id, struct fl_layer_composition *composition) {
    struct window *window;
    int bucket_status;

    if (has_view_id) {
        window = compositor_get_view_by_id(compositor, view_id);
        if (window == NULL) {
            LOG_ERROR("Couldn't find window with id %" PRId64 " to push composition to.\n", view_id);
            return EINVAL;
        }
    } else {
        window = window_ref(compositor->implicit_view_fallback);
    }

    TRACER_BEGIN(compositor->tracer, "window_push_composition");
    bucket_status = window_push_composition(window, composition);
    TRACER_END(compositor->tracer, "window_push_composition");

    window_unref(window);

    return bucket_status;
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

static int compositor_push_fl_layers(
    struct compositor *compositor,
    bool has_view_id,
    int64_t view_id,
    size_t n_fl_layers,
    const FlutterLayer **fl_layers
) {
    struct fl_layer_composition *composition;
    struct view_geometry geometry;
    struct window *window;
    int bucket_status;

    window = has_view_id ? compositor_get_view_by_id(compositor, view_id) : window_ref(compositor->implicit_view_fallback);
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

                layer->surface = CAST_SURFACE(
                    dummy_render_surface_new(compositor->tracer, VEC2I((int) fl_layer->size.width, (int) fl_layer->size.height))
                );
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
    bucket_status = compositor_push_composition(compositor, has_view_id, view_id, composition);
    TRACER_END(compositor->tracer, "compositor_push_composition");

    fl_layer_composition_unref(composition);
    return bucket_status;
}

/// TODO: Remove
UNUSED static bool on_flutter_present_layers(const FlutterLayer **layers, size_t layers_count, void *userdata) {
    struct compositor *compositor;
    int bucket_status;

    ASSERT_NOT_NULL(layers);
    assert(layers_count > 0);
    ASSERT_NOT_NULL(userdata);
    compositor = userdata;

    TRACER_BEGIN(compositor->tracer, "compositor_push_fl_layers");
    bucket_status = compositor_push_fl_layers(compositor, false, -1, layers_count, layers);
    TRACER_END(compositor->tracer, "compositor_push_fl_layers");

    if (bucket_status != 0) {
        return false;
    }

    return true;
}

static bool on_flutter_present_view(const FlutterPresentViewInfo *present_info) {
    struct compositor *compositor;
    int bucket_status;

    ASSERT_NOT_NULL(present_info);
    compositor = present_info->user_data;

    TRACER_BEGIN(compositor->tracer, "compositor_push_fl_layers");
    bucket_status = compositor_push_fl_layers(compositor, true, present_info->view_id, present_info->layers_count, present_info->layers);
    TRACER_END(compositor->tracer, "compositor_push_fl_layers");

    if (bucket_status != 0) {
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
    return window_has_egl_surface(compositor->implicit_view_fallback);
}

EGLSurface compositor_get_egl_surface(struct compositor *compositor) {
    return window_get_egl_surface(compositor->implicit_view_fallback);
}
#endif

static bool
on_flutter_create_backing_store(const FlutterBackingStoreConfig *config, FlutterBackingStore *backing_store_out, void *userdata) {
    struct render_surface *s;
    struct compositor *compositor;
    struct window *window;
    int bucket_status;

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
    bucket_status = render_surface_fill(s, backing_store_out);
    if (bucket_status != 0) {
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

void compositor_set_cursor(struct compositor *compositor, bool has_enabled, bool enabled, bool has_kind, enum pointer_kind kind) {
    if (!has_enabled && !has_kind) {
        return;
    }

    compositor_lock(compositor);

    if (compositor->cursor.window) {
        window_set_cursor(compositor->cursor.window, has_enabled, enabled, has_kind, kind, false, VEC2I(0, 0));
    }

    compositor_unlock(compositor);
}

int64_t compositor_add_view(struct compositor *compositor, struct window *window) {
    ASSERT_NOT_NULL(compositor);
    ASSERT_NOT_NULL(window);
    int64_t view_id;
    int bucket_status;

    compositor_lock(compositor);

    view_id = compositor->next_view_id++;

    khiter_t entry = kh_put(view, compositor->views, view_id, &bucket_status);
    if (bucket_status == -1) {
        compositor_unlock(compositor);
        return -1;
    }

    kh_val(compositor->views, entry) = window_ref(window);

    if (compositor->implicit_view_fallback == NULL) {
        compositor->implicit_view_fallback = window_ref(window);
    }

    kv_push(compositor->view_layout, view_id);

    compositor_unlock(compositor);

    return view_id;
}

void compositor_remove_view(struct compositor *compositor, int64_t view_id) {
    ASSERT_NOT_NULL(compositor);
    ASSERT(view_id != 0);

    compositor_lock(compositor);

    khiter_t entry = kh_get(view, compositor->views, view_id);
    if (entry != kh_end(compositor->views)) {
        struct window *window = kh_val(compositor->views, entry);

        // If the view we're removing is the view we use as an implicit view fallback,
        // unref it.
        if (compositor->implicit_view_fallback == window) {
            window_unrefp(&compositor->implicit_view_fallback);
        }

        window_unref(kh_val(compositor->views, entry));

        kh_del(view, compositor->views, entry);
    }

    // remove the view from the view layout
    for (size_t i = 0; i < kv_size(compositor->view_layout); i++) {
        if (kv_A(compositor->view_layout, i) == view_id) {
            for (size_t j = i; j < kv_size(compositor->view_layout) - 1; j++) {
                kv_A(compositor->view_layout, j) = kv_A(compositor->view_layout, j + 1);
            }
            kv_drop(compositor->view_layout, 1);
            break;
        }
    }

    compositor_unlock(compositor);
}

struct notifier *compositor_get_display_setup_notifier(struct compositor *compositor) {
    ASSERT_NOT_NULL(compositor);
    return &compositor->display_setup_notifier;
}

static int64_t determine_window_for_input_device(struct compositor *compositor, struct user_input_device *device) {
    input_device_match_score_t best_score = -1;
    int64_t best_window = -1;

    {
        int64_t view_id;
        struct window *window;
        kh_foreach(compositor->views, view_id, window, {
            input_device_match_score_t score = window_match_input_device(window, device);
            if (score > best_score) {
                best_score = score;
                best_window = view_id;
            }
        });
    }

    if (best_score >= 0) {
        return best_window;
    }

    return -1;
}

static int64_t get_view_id_for_input_device(struct compositor *compositor, struct user_input_device *device) {
    (void) compositor;

    int64_t *view_id = user_input_device_get_primary_listener_userdata(device);
    return view_id == NULL ? -1 : *view_id;
}

UNUSED static struct window *get_window_for_input_device(struct compositor *compositor, struct user_input_device *device) {
    (void) compositor;

    int64_t *view_id = user_input_device_get_primary_listener_userdata(device);

    if (view_id == NULL) {
        return NULL;
    }

    return compositor_get_view_by_id(compositor, *view_id);
}

struct fl_event_buffer {
    struct fl_pointer_event_interface pointer_event_interface;

    size_t n_events;
    FlutterPointerEvent events[64];
};

static void flush_fl_events(struct fl_event_buffer *buffer) {
    if (buffer->n_events > 0) {
        buffer->pointer_event_interface.send_pointer_event(buffer->pointer_event_interface.engine, buffer->events, buffer->n_events);
        buffer->n_events = 0;
    }
}

static void emit_fl_event(struct fl_event_buffer *buffer, const FlutterPointerEvent event) {
    if (buffer->n_events >= ARRAY_SIZE(buffer->events)) {
        flush_fl_events(buffer);
    }

    buffer->events[buffer->n_events++] = event;
}

static FlutterPointerEvent make_fl_pointer_add_event(
    uint64_t timestamp,
    struct vec2f pos_view,
    int64_t fl_device_id,
    FlutterPointerDeviceKind device_kind,
    int64_t view_id
) {
    FlutterPointerEvent event;
    memset(&event, 0, sizeof event);

    event.struct_size = sizeof(FlutterPointerEvent);
    event.phase = kAdd;
    event.timestamp = timestamp;
    event.x = pos_view.x;
    event.y = pos_view.y;
    event.device = fl_device_id;
    event.signal_kind = kFlutterPointerSignalKindNone;
    event.scroll_delta_x = 0;
    event.scroll_delta_y = 0;
    event.device_kind = device_kind;
    event.buttons = 0;
    event.pan_x = 0.0;
    event.pan_y = 0.0;
    event.scale = 1.0;
    event.rotation = 0.0;
    event.view_id = view_id;

    return event;
}

static FlutterPointerEvent make_fl_pointer_remove_event(
    uint64_t timestamp,
    struct vec2f pos_view,
    int64_t fl_device_id,
    FlutterPointerDeviceKind device_kind,
    int64_t view_id
) {
    FlutterPointerEvent event;
    memset(&event, 0, sizeof event);

    event.struct_size = sizeof(FlutterPointerEvent);
    event.phase = kRemove;
    event.timestamp = timestamp;
    event.x = pos_view.x;
    event.y = pos_view.y;
    event.device = fl_device_id;
    event.signal_kind = kFlutterPointerSignalKindNone;
    event.scroll_delta_x = 0;
    event.scroll_delta_y = 0;
    event.device_kind = device_kind;
    event.buttons = 0;
    event.pan_x = 0.0;
    event.pan_y = 0.0;
    event.scale = 1.0;
    event.rotation = 0.0;
    event.view_id = view_id;

    return event;
}

static FlutterPointerEvent make_fl_mouse_event(
    FlutterPointerPhase phase,
    uint64_t timestamp,
    struct vec2f pos_view,
    int64_t fl_device_id,
    int64_t buttons,
    int64_t view_id
) {
    FlutterPointerEvent event;
    memset(&event, 0, sizeof event);

    event.struct_size = sizeof(FlutterPointerEvent);
    event.phase = phase;
    event.timestamp = timestamp;
    event.x = pos_view.x;
    event.y = pos_view.y;
    event.device = fl_device_id;
    event.signal_kind = kFlutterPointerSignalKindNone;
    event.scroll_delta_x = 0;
    event.scroll_delta_y = 0;
    event.device_kind = kFlutterPointerDeviceKindMouse;
    event.buttons = buttons;
    event.pan_x = 0.0;
    event.pan_y = 0.0;
    event.scale = 1.0;
    event.rotation = 0.0;
    event.view_id = view_id;

    return event;
}

UNUSED static FlutterPointerEvent make_fl_mouse_scroll_event(
    FlutterPointerPhase phase,
    uint64_t timestamp,
    struct vec2f pos_view,
    int64_t fl_device_id,
    struct vec2f scroll_delta,
    int64_t buttons,
    int64_t view_id
) {
    FlutterPointerEvent event;
    memset(&event, 0, sizeof event);

    event.struct_size = sizeof(FlutterPointerEvent);
    event.phase = phase;
    event.timestamp = timestamp;
    event.x = pos_view.x;
    event.y = pos_view.y;
    event.device = fl_device_id;
    event.signal_kind = kFlutterPointerSignalKindScroll;
    event.scroll_delta_x = scroll_delta.x;
    event.scroll_delta_y = scroll_delta.y;
    event.device_kind = kFlutterPointerDeviceKindMouse;
    event.buttons = buttons;
    event.pan_x = 0.0;
    event.pan_y = 0.0;
    event.scale = 1.0;
    event.rotation = 0.0;
    event.view_id = view_id;

    return event;
}

static void on_device_added(struct compositor *compositor, struct user_input_device *device) {
    int64_t view_id = determine_window_for_input_device(compositor, device);
    if (view_id == -1) {
        return;
    }

    int64_t *id = malloc(sizeof *id);
    if (id == NULL) {
        return;
    }

    *id = view_id;

    user_input_device_set_primary_listener_userdata(device, id);
}

static void on_device_removed(struct compositor *compositor, struct user_input_device *device) {
    (void) compositor;

    int64_t *id = user_input_device_get_primary_listener_userdata(device);
    if (id != NULL) {
        free(id);
    }

    user_input_device_set_primary_listener_userdata(device, NULL);
}

static void on_slot_added(struct compositor *compositor, struct fl_event_buffer *buffer, const struct user_input_event *event) {
    int64_t view_id = get_view_id_for_input_device(compositor, event->device);
    if (view_id == -1) {
        return;
    }

    FlutterPointerEvent fl_event;
    memset(&fl_event, 0, sizeof fl_event);

    fl_event.struct_size = sizeof(FlutterPointerEvent);
    fl_event.timestamp = event->timestamp;
    fl_event.phase = kAdd;
    /// TODO: Implement
    UNIMPLEMENTED();
    fl_event.view_id = view_id;

    emit_fl_event(buffer, fl_event);
}

static void on_slot_removed(struct compositor *compositor, struct fl_event_buffer *buffer, const struct user_input_event *event) {
    int64_t view_id = get_view_id_for_input_device(compositor, event->device);
    if (view_id == -1) {
        return;
    }

    FlutterPointerEvent fl_event;
    memset(&fl_event, 0, sizeof fl_event);

    fl_event.struct_size = sizeof(FlutterPointerEvent);
    fl_event.timestamp = event->timestamp;
    fl_event.phase = kRemove;
    /// TODO: Implement
    UNIMPLEMENTED();
    fl_event.view_id = view_id;

    emit_fl_event(buffer, fl_event);
}

static void on_absolute_pointer_event(struct compositor *compositor, struct fl_event_buffer *buffer, const struct user_input_event *event) {
    ASSERT(event->pointer.is_absolute);

    int64_t view_id = get_view_id_for_input_device(compositor, event->device);
    if (view_id == -1) {
        return;
    }

    struct window *window = compositor_get_view_by_id(compositor, view_id);
    if (window == NULL) {
        return;
    }

    struct vec2f pos_view = window_transform_ndc_to_view(window, event->pointer.position_ndc);

    mutex_lock(&compositor->mutex);

    if (compositor->cursor.enabled && compositor->cursor.fl_view_id != view_id) {
        window_set_cursor(
            // clang-format off
                compositor->cursor.window,
                true, false,
                false, 0,
                false, VEC2I(0, 0)
            // clang-format on
        );
        window_unrefp(&compositor->cursor.window);

        // remove the mouse device from the old view
        // and add it to the new view.
        emit_fl_event(
            buffer,
            make_fl_pointer_remove_event(
                event->timestamp,
                compositor->cursor.pos_view,
                event->global_slot_id,
                kFlutterPointerDeviceKindMouse,
                compositor->cursor.fl_view_id
            )
        );

        emit_fl_event(
            buffer,
            make_fl_pointer_add_event(event->timestamp, pos_view, event->global_slot_id, kFlutterPointerDeviceKindMouse, view_id)
        );
    }

    compositor->cursor.enabled = true;
    compositor->cursor.fl_view_id = view_id;
    // compositor_get_view_by_id increments the refcount of the window so we don't need to do it here.
    compositor->cursor.window = window;
    compositor->cursor.pos_view = pos_view;

    mutex_unlock(&compositor->mutex);
}

static void move_cursor_to_view(
    struct compositor *compositor,
    struct fl_event_buffer *buffer,
    uint64_t timestamp,
    uint64_t device_id,
    int64_t new_view,
    struct window *new_window,
    struct vec2f new_pos
) {
    if (compositor->cursor.enabled) {
        if (compositor->cursor.fl_view_id != new_view) {
            window_set_cursor(
                // clang-format off
                compositor->cursor.window,
                true, false,
                false, 0,
                false, VEC2I(0, 0)
                // clang-format on
            );
            window_unrefp(&compositor->cursor.window);

            // remove the mouse device from the old view
            // and add it to the new view.
            emit_fl_event(
                buffer,
                make_fl_pointer_remove_event(
                    timestamp,
                    compositor->cursor.pos_view,
                    device_id,
                    kFlutterPointerDeviceKindMouse,
                    compositor->cursor.fl_view_id
                )
            );

            emit_fl_event(buffer, make_fl_pointer_add_event(timestamp, new_pos, device_id, kFlutterPointerDeviceKindMouse, new_view));

            window_set_cursor(
                // clang-format off
                compositor->cursor.window,
                true, true,
                false, 0,
                false, vec2f_round_to_integer(new_pos)
                // clang-format on
            );
        }

        compositor->cursor.fl_view_id = new_view;
        compositor->cursor.window = window_ref(new_window);
        compositor->cursor.pos_view = new_pos;
    } else {
        compositor->cursor.enabled = true;
        compositor->cursor.fl_view_id = new_view;
        compositor->cursor.window = window_ref(new_window);
        compositor->cursor.pos_view = new_pos;
    }
}

static void maybe_enable_cursor_locked(struct compositor *compositor) {
    if (!compositor->cursor.enabled) {
        int64_t first_view = kv_A(compositor->view_layout, 0);
        struct window *w = compositor_get_view_by_id_locked(compositor, first_view);

        if (w != NULL) {
            compositor->cursor.enabled = true;
            compositor->cursor.fl_view_id = first_view;
            compositor->cursor.window = window_ref(w);
            compositor->cursor.pos_view = VEC2F(0, 0);

            window_unref(w);
        }
    }
}

static void
on_relative_pointer_event_locked(struct compositor *compositor, struct fl_event_buffer *buffer, const struct user_input_event *event) {
    ASSERT(!event->pointer.is_absolute);

    // If the cursor is not enabled, we need to enable it.
    maybe_enable_cursor_locked(compositor);

    // If the cursor is still not enabled, we can't do anything.
    if (!compositor->cursor.enabled) {
        return;
    }

    int64_t current_view_id = compositor->cursor.fl_view_id;
    struct window *current_window = compositor->cursor.window;

    // move the cursor
    struct vec2f current_view_size = window_get_view_geometry(current_window).view_size;
    struct vec2f pos_current_view = vec2f_add(compositor->cursor.pos_view, event->pointer.delta);

    int64_t new_view_id = -1;
    if (pos_current_view.x < 0) {
        // look if there's a view to the left of this one.
        for (size_t i = 0; i < kv_size(compositor->view_layout); i++) {
            if (kv_A(compositor->view_layout, i) == current_view_id) {
                if (i > 0) {
                    new_view_id = kv_A(compositor->view_layout, i - 1);
                }
                break;
            }
        }
    } else if (pos_current_view.x > current_view_size.x) {
        // look if there's a view to the left of this one.
        for (size_t i = 0; i < kv_size(compositor->view_layout); i++) {
            if (kv_A(compositor->view_layout, i) == current_view_id) {
                if (i > 0) {
                    new_view_id = kv_A(compositor->view_layout, i - 1);
                }
                break;
            }
        }
    }

    struct window *new_window = NULL;
    if (new_view_id != -1) {
        // move the cursor to the right side of the view located left from the current one.
        new_window = compositor_get_view_by_id_locked(compositor, new_view_id);
    }

    // If we have a window next to this one,
    // try to move the cursor onto that window.
    if (new_window != NULL) {
        struct view_geometry new_geo = window_get_view_geometry(new_window);

        // Translate the cursor position on the current view to the
        // cursor position on the new view.
        if (pos_current_view.x < 0) {
            pos_current_view.x = new_geo.view_size.x + pos_current_view.x;
        } else if (pos_current_view.x > current_view_size.x) {
            pos_current_view.x = pos_current_view.x - current_view_size.x;
        }

        if (0 <= pos_current_view.x && pos_current_view.x <= new_geo.view_size.x && 0 <= pos_current_view.y &&
            pos_current_view.y <= new_geo.view_size.y) {
            // If the cursor is within the bounds of the new view,
            // move it there.
            move_cursor_to_view(compositor, buffer, event->timestamp, event->global_slot_id, new_view_id, new_window, pos_current_view);
        } else {
            // If the cursor is not within bounds,
            // just emit a move event.
            window_unrefp(&new_window);
        }
    }

    // If we're not moving the cursor to a new window,
    // just emit a move event.
    if (new_window == NULL) {
        pos_current_view.x = CLAMP(pos_current_view.x, 0, current_view_size.x);
        pos_current_view.y = CLAMP(pos_current_view.y, 0, current_view_size.y);

        emit_fl_event(
            buffer,
            make_fl_mouse_event(
                event->pointer.buttons & kFlutterPointerButtonMousePrimary ? kMove : kHover,
                event->timestamp,
                pos_current_view,
                event->global_slot_id,
                event->pointer.buttons,
                current_view_id
            )
        );

        compositor->cursor.pos_view = pos_current_view;
    }

    window_unrefp(&current_window);
}

static void on_pointer_scroll_event(struct compositor *compositor, struct fl_event_buffer *buffer, const struct user_input_event *event) {
    (void) compositor;
    (void) buffer;
    (void) event;
    /// TODO: Implement
    UNIMPLEMENTED();
}

static void on_pointer_button_event(struct compositor *compositor, struct fl_event_buffer *buffer, const struct user_input_event *event) {
    (void) compositor;
    (void) buffer;
    (void) event;
    /// TODO: Implement
    UNIMPLEMENTED();
}

static void on_pointer_event(struct compositor *compositor, struct fl_event_buffer *buffer, const struct user_input_event *event) {
    if (event->pointer.is_absolute) {
        on_absolute_pointer_event(compositor, buffer, event);
    } else if (event->pointer.delta.x != 0 || event->pointer.delta.y != 0) {
        on_relative_pointer_event_locked(compositor, buffer, event);
    } else if (event->pointer.scroll_delta.x != 0 || event->pointer.scroll_delta.y != 0) {
        on_pointer_scroll_event(compositor, buffer, event);
    } else if (event->pointer.changed_buttons != 0) {
        on_pointer_button_event(compositor, buffer, event);
    }
}

static void on_touch_event(struct compositor *compositor, struct fl_event_buffer *buffer, const struct user_input_event *event) {
    int64_t view_id = get_view_id_for_input_device(compositor, event->device);
    if (view_id == -1) {
        return;
    }

    FlutterPointerEvent fl_event;
    memset(&fl_event, 0, sizeof fl_event);

    fl_event.struct_size = sizeof(FlutterPointerEvent);
    fl_event.timestamp = event->timestamp;
    if (event->touch.down_changed) {
        if (event->touch.down) {
            fl_event.phase = kDown;
        } else {
            fl_event.phase = kUp;
        }
    } else {
        ASSERT(event->touch.down);
        fl_event.phase = kMove;
    }

    emit_fl_event(buffer, fl_event);
}

static void on_tablet_tool_event(struct compositor *compositor, struct fl_event_buffer *buffer, const struct user_input_event *event) {
    int64_t view_id = get_view_id_for_input_device(compositor, event->device);
    if (view_id == -1) {
        return;
    }

    FlutterPointerEvent fl_event;
    memset(&fl_event, 0, sizeof fl_event);

    fl_event.struct_size = sizeof(FlutterPointerEvent);
    fl_event.timestamp = event->timestamp;
    fl_event.phase = kMove;
    fl_event.x = event->tablet.position_ndc.x;
    fl_event.y = event->tablet.position_ndc.y;
    fl_event.device = kFlutterPointerDeviceKindStylus;
    fl_event.view_id = view_id;

    emit_fl_event(buffer, fl_event);
}

static void on_input(void *userdata, size_t n_events, const struct user_input_event *events) {
    struct compositor *compositor;
    size_t i;

    ASSERT_NOT_NULL(userdata);
    compositor = userdata;

    if (!compositor->has_pointer_event_interface) {
        return;
    }

    struct fl_event_buffer buffer = {
        .n_events = 0,
        .pointer_event_interface = compositor->pointer_event_interface,
    };

    for (i = 0; i < n_events; i++) {
        const struct user_input_event *event = events + i;

        switch (event->type) {
            case USER_INPUT_DEVICE_ADDED: on_device_added(compositor, event->device); break;
            case USER_INPUT_DEVICE_REMOVED: on_device_removed(compositor, event->device); break;
            case USER_INPUT_SLOT_ADDED: on_slot_added(compositor, &buffer, event); break;
            case USER_INPUT_SLOT_REMOVED: on_slot_removed(compositor, &buffer, event); break;
            case USER_INPUT_TOUCH: on_touch_event(compositor, &buffer, event); break;
            case USER_INPUT_TABLET_TOOL: on_tablet_tool_event(compositor, &buffer, event); break;
            case USER_INPUT_POINTER: on_pointer_event(compositor, &buffer, event); break;
            case USER_INPUT_KEY: UNREACHABLE(); break;
            default: LOG_DEBUG("Unhandled enum user_input_event: %u\n", event->type); break;
        }
    }

    flush_fl_events(&buffer);
}
