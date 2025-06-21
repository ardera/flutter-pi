#define _POSIX_C_SOURCE 200809L
#include <unistd.h>

#include <sentry.h>

#include "flutter-pi.h"
#include "platformchannel.h"
#include "pluginregistry.h"
#include "util/logging.h"

#define DISPLAY_MANAGER_CHANNEL "multidisplay/display_manager"
#define VIEW_CONTROLLER_CHANNEL "multidisplay/view_controller"
#define EVENTS_CHANNEL "multidisplay/events"

#define MULTIDISPLAY_PLUGIN_DEBUG 1
#define LOG_MULTIDISPLAY_DEBUG(fmt, ...)         \
    do {                                   \
        if (MULTIDISPLAY_PLUGIN_DEBUG)           \
            LOG_DEBUG(fmt, ##__VA_ARGS__); \
    } while (0)

struct multidisplay_plugin {
    bool has_listener;
};

static void send_display_update(struct multidisplay_plugin *plugin) {
    
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

enum plugin_init_result multidisplay_plugin_deinit(struct flutterpi *flutterpi, void **userdata_out) {
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
        EVENTS_CHANNEL,
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

void multidisplay_plugin_init(struct flutterpi *flutterpi, void *userdata) {
    struct multidisplay_plugin *plugin;

    ASSERT_NOT_NULL(userdata);
    plugin = userdata;

    plugin_registry_remove_receiver_v2_locked(flutterpi_get_plugin_registry(flutterpi), EVENTS_CHANNEL);
    plugin_registry_remove_receiver_v2_locked(flutterpi_get_plugin_registry(flutterpi), VIEW_CONTROLLER_CHANNEL);
    plugin_registry_remove_receiver_v2_locked(flutterpi_get_plugin_registry(flutterpi), DISPLAY_MANAGER_CHANNEL);
    free(plugin);  
}

FLUTTERPI_PLUGIN("multidisplay", multidisplay_plugin_init, multidisplay_plugin_deinit, NULL);
