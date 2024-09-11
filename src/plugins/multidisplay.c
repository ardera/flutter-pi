#define _POSIX_C_SOURCE 200809L
#include <unistd.h>

#include <sentry.h>

#include "compositor_ng.h"
#include "flutter-pi.h"
#include "notifier_listener.h"
#include "platformchannel.h"
#include "pluginregistry.h"
#include "util/logging.h"

#define DISPLAY_MANAGER_CHANNEL "multidisplay/display_manager"
#define VIEW_CONTROLLER_CHANNEL "multidisplay/view_controller"
#define DISPLAY_SETUP_CHANNEL "multidisplay/display_setup"

#define MULTIDISPLAY_PLUGIN_DEBUG 1
#define LOG_MULTIDISPLAY_DEBUG(fmt, ...)   \
    do {                                   \
        if (MULTIDISPLAY_PLUGIN_DEBUG)     \
            LOG_DEBUG(fmt, ##__VA_ARGS__); \
    } while (0)

struct multidisplay_plugin {
    bool has_listener;
    struct listener *display_setup_listener;
};

static void send_display_update(struct multidisplay_plugin *plugin, struct display_setup *s) {
    if (!plugin->has_listener) {
        return;
    }

    struct std_value connectors;

    connectors.type = kStdList;
    connectors.size = display_setup_get_n_connectors(s);
    connectors.list = alloca(sizeof(struct std_value) * connectors.size);

    for (size_t i = 0; i < n_ranges; i++) {
        struct std_value *map = connectors.list + i;

        map->type = kStdMap;
        map->size = 3;
        map->keys = alloca(sizeof(struct std_value) * conn->size);
        map->values = alloca(sizeof(struct std_value) * conn->size);

        map->keys[0] = STDSTRING("name");
        map->values[0] = STDSTRING(connector_get_name(connector));

        map->keys[1] = STDSTRING("type");
        map->values[1] = STDSTRING(connector_get_type_name(connector));

        map->keys[2] = STDSTRING("display");
        if (connector_has_display(connector)) {
            struct display *display = connector_get_display(connector);

            struct std_value *display_map = alloca(sizeof(struct std_value));
            display_map->type = kStdMap;
            display_map->size = 7;

            display_map->keys = alloca(sizeof(struct std_value) * display_map->size);
            display_map->values = alloca(sizeof(struct std_value) * display_map->size);

            display_map->keys[0] = STDSTRING("flutterId");
            display_map->values[0] = STDINT64(display_get_fl_display_id(display));

            display_map->keys[1] = STDSTRING("refreshRate");
            display_map->values[1] = STDFLOAT64(display_get_refresh_rate(display));

            display_map->keys[2] = STDSTRING("width");
            display_map->values[2] = STDINT64(display_get_size(display).x);

            display_map->keys[3] = STDSTRING("height");
            display_map->values[3] = STDINT64(display_get_size(display).y);

            display_map->keys[4] = STDSTRING("widthMM");
            display_map->values[4] = STDINT64(display_get_physical_size(display).x);

            display_map->keys[5] = STDSTRING("heightMM");
            display_map->values[5] = STDINT64(display_get_physical_size(display).y);

            display_map->keys[6] = STDSTRING("devicePixelRatio");
            display_map->values[6] = STDFLOAT64(display_get_device_pixel_ratio(display));
        } else {
            map->values[2] = STDNULL;
        }
    }

    return platch_send_success_event_std(meta->event_channel_name, &STDMAP1(STDSTRING("connectors"), connectors));
}

static void on_display_setup_value(void *arg, void *userdata) {
    struct multidisplay_plugin *plugin;
    struct display_setup *s;

    ASSERT_NOT_NULL(userdata);
    ASSERT_NOT_NULL(value);
    plugin = userdata;
    s = arg;

    send_display_update(plugin, s);
}

static void on_display_manager_method_call(void *userdata, const FlutterPlatformMessage *message) {
    const FlutterPlatformMessageResponseHandle *responsehandle;
    const struct raw_std_value *envelope, *method, *arg;
    struct sentry_plugin *plugin;

    ASSERT_NOT_NULL(userdata);
    ASSERT_NOT_NULL(message);
    plugin = userdata;
    responsehandle = message->response_handle;
    envelope = (const struct raw_std_value *) (message->message);
    if (!raw_std_method_call_check(envelope, message->message_size)) {
        platch_respond_error_std(responsehandle, "malformed-message", "", &STDNULL);
        return;
    }

    method = raw_std_method_call_get_method(envelope);
    arg = raw_std_method_call_get_arg(envelope);

    if (raw_std_string_equals(method, "")) {
        on_init_native_sdk(plugin, arg, responsehandle);
    } else {
        platch_respond_error_std(responsehandle, "unknown-method", "", &STDNULL);
    }
}

static void on_view_controller_method_call(void *userdata, const FlutterPlatformMessage *message) {
    const FlutterPlatformMessageResponseHandle *responsehandle;
    const struct raw_std_value *envelope, *method, *arg;
    struct sentry_plugin *plugin;

    ASSERT_NOT_NULL(userdata);
    ASSERT_NOT_NULL(message);
    plugin = userdata;
    responsehandle = message->response_handle;
    envelope = (const struct raw_std_value *) (message->message);
    if (!raw_std_method_call_check(envelope, message->message_size)) {
        platch_respond_error_std(responsehandle, "malformed-message", "", &STDNULL);
        return;
    }

    method = raw_std_method_call_get_method(envelope);
    arg = raw_std_method_call_get_arg(envelope);

    if (raw_std_string_equals(method, "closeView")) {
        on_close_view(plugin, arg, responsehandle);
    } else {
        platch_respond_error_std(responsehandle, "unknown-method", "", &STDNULL);
    }
}

static void on_event_channel_listen(
    struct multidisplay_plugin *plugin,
    const struct raw_std_value *arg,
    const FlutterPlatformMessageResponseHandle *responsehandle
) {
    if (plugin->has_listener) {
        return;
    }

    plugin->has_listener = true;
    platch_respond_success_std(responsehandle, &STDNULL);

    struct compositor *comp = flutterpi_peek_compositor(flutterpi);
    ASSERT_NOT_NULL(comp);

    plugin->display_setup_listener = notifier_listen(compositor_get_display_setup_notifier(comp), on_display_setup_value, NULL, plugin);
    return;
}

static void on_event_channel_method_call(void *userdata, const FlutterPlatformMessage *message) {
    const FlutterPlatformMessageResponseHandle *responsehandle;
    const struct raw_std_value *envelope, *method, *arg;
    struct sentry_plugin *plugin;

    ASSERT_NOT_NULL(userdata);
    ASSERT_NOT_NULL(message);
    plugin = userdata;
    responsehandle = message->response_handle;
    envelope = (const struct raw_std_value *) (message->message);
    if (!raw_std_method_call_check(envelope, message->message_size)) {
        platch_respond_error_std(responsehandle, "malformed-message", "", &STDNULL);
        return;
    }

    method = raw_std_method_call_get_method(envelope);
    arg = raw_std_method_call_get_arg(envelope);

    if (raw_std_string_equals(method, "listen")) {
        on_event_channel_listen(plugin, arg, responsehandle);
    } else {
        platch_respond_error_std(responsehandle, "unknown-method", "", &STDNULL);
    }
}

enum plugin_init_result multidisplay_plugin_init(struct flutterpi *flutterpi, void **userdata_out) {
    struct multidisplay_plugin *plugin;
    int ok;

    plugin = malloc(sizeof *plugin);
    if (plugin == NULL) {
        return PLUGIN_INIT_RESULT_ERROR;
    }

    ok = plugin_registry_set_receiver_v2_locked(
        flutterpi_get_plugin_registry(flutterpi),
        DISPLAY_MANAGER_CHANNEL,
        on_display_manager_method_call,
        plugin
    );
    if (ok != 0) {
        goto fail_free_plugin;
    }

    ok = plugin_registry_set_receiver_v2_locked(
        flutterpi_get_plugin_registry(flutterpi),
        VIEW_CONTROLLER_CHANNEL,
        on_view_controller_method_call,
        plugin
    );
    if (ok != 0) {
        goto fail_remove_display_maanger_receiver;
    }

    ok = plugin_registry_set_receiver_v2_locked(
        flutterpi_get_plugin_registry(flutterpi),
        DISPLAY_SETUP_CHANNEL,
        on_event_channel_method_call,
        plugin
    );
    if (ok != 0) {
        goto fail_remove_view_controller_receiver;
    }

    *userdata_out = plugin;

    return PLUGIN_INIT_RESULT_INITIALIZED;

fail_remove_view_controller_receiver:
    plugin_registry_remove_receiver_v2_locked(flutterpi_get_plugin_registry(flutterpi), VIEW_CONTROLLER_CHANNEL);

fail_remove_display_maanger_receiver:
    plugin_registry_remove_receiver_v2_locked(flutterpi_get_plugin_registry(flutterpi), DISPLAY_MANAGER_CHANNEL);

fail_free_plugin:
    free(plugin);
    return PLUGIN_INIT_RESULT_ERROR;
}

void multidisplay_plugin_deinit(struct flutterpi *flutterpi, void *userdata) {
    struct multidisplay_plugin *plugin;

    ASSERT_NOT_NULL(userdata);
    plugin = userdata;

    plugin_registry_remove_receiver_v2_locked(flutterpi_get_plugin_registry(flutterpi), DISPLAY_SETUP_CHANNEL);
    plugin_registry_remove_receiver_v2_locked(flutterpi_get_plugin_registry(flutterpi), VIEW_CONTROLLER_CHANNEL);
    plugin_registry_remove_receiver_v2_locked(flutterpi_get_plugin_registry(flutterpi), DISPLAY_MANAGER_CHANNEL);
    free(plugin);
}

FLUTTERPI_PLUGIN("multidisplay", multidisplay, multidisplay_plugin_init, multidisplay_plugin_deinit);
