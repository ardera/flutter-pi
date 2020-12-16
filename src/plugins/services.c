#ifdef __STDC_ALLOC_LIB__
#define __STDC_WANT_LIB_EXT2__ 1
#else
#define _POSIX_C_SOURCE 200809L
#endif

#include <ctype.h>
#include <errno.h>
#include <string.h>

#include <flutter-pi.h>
#include <compositor.h>
#include <messenger.h>
#include <plugins/services.h>
#include <dylib_deps.h>

struct plugin {
    struct flutterpi *flutterpi;
    char *label;
    char *isolate_id;

    /**
     * @brief ARGB8888 (blue is the lowest byte)
     */
    uint32_t primary_color;
};

static void on_receive_navigation(
    bool success,
    struct flutter_message_response_handle *responsehandle,
    const char *channel,
    const struct platch_obj *object,
    void *userdata
) {
    fm_respond_not_implemented(responsehandle);
}

static void on_receive_isolate(
    struct flutter_message_response_handle *responsehandle,
    const char *channel,
    const uint8_t *data,
    size_t size,
    void *userdata
) {
    struct plugin *plugin;
    char *duped;

    plugin = userdata;
    
    duped = strndup(data, size);
    if (duped == NULL) {
        LOG_SERVICES_PLUGIN_ERROR("Error duplicating isolate id: Out of memory\n");
        return;
    }

    plugin->isolate_id = duped;
    
    fm_respond_not_implemented(responsehandle);
}

static void on_receive_platform(
    bool success,
    struct flutter_message_response_handle *responsehandle,
    const char *channel,
    const struct platch_obj *object,
    void *userdata
) {
    const struct json_value *value, *arg;
    struct plugin *plugin;
    
    arg = &(object->json_arg);
    plugin = userdata;

    if STREQ(object->method, "Clipboard.setData") {
        /*
         *  Clipboard.setData(Map data)
         *      Places the data from the text entry of the argument,
         *      which must be a Map, onto the system clipboard.
         */
    } else if STREQ(object->method, "Clipboard.getData") {
        /*
         *  Clipboard.getData(String format)
         *      Returns the data that has the format specified in the argument
         *      from the system clipboard. The only currently supported is "text/plain".
         *      The result is a Map with a single key, "text".
         */ 
    } else if STREQ(object->method, "HapticFeedback.vibrate") {
        /*
         *  HapticFeedback.vibrate(void)
         *      Triggers a system-default haptic response.
         */
    } else if STREQ(object->method, "SystemSound.play") {
        /*
         *  SystemSound.play(String soundName)
         *      Triggers a system audio effect. The argument must
         *      be a String describing the desired effect; currently only "click" is
         *      supported.
         */
    } else if STREQ(object->method, "SystemChrome.setPreferredOrientations") {
        /*
         *  SystemChrome.setPreferredOrientations(DeviceOrientation[])
         *      Informs the operating system of the desired orientation of the display. The argument is a [List] of
         *      values which are string representations of values of the [DeviceOrientation] enum.
         * 
         *  enum DeviceOrientation {
         *      portraitUp, landscapeLeft, portraitDown, landscapeRight
         *  }
         */
        
        value = &object->json_arg;
        
        if (JSONVALUE_IS_ARRAY(*value) == false || value->size == 0) {
            fm_respond_illegal_arg_json(
                responsehandle,
                "Expected `arg` to be an array with minimum size 1."
            );
            return;
        }

        bool preferred_orientations[kLandscapeRight+1] = {0};

        for (unsigned int i = 0; i < value->size; i++) {
            enum device_orientation o;

            if (!JSONVALUE_IS_STRING(value->array[i])) {
                fm_respond_illegal_arg_json(
                    responsehandle,
                    "Expected `arg` to to only contain strings."
                );
                return;
            }

            if STREQ("DeviceOrientation.portraitUp", JSONVALUE_AS_STRING(value->array[i])) {
                o = kPortraitUp;
            } else if STREQ("DeviceOrientation.landscapeLeft", JSONVALUE_AS_STRING(value->array[i])) {
                o = kLandscapeLeft;
            } else if STREQ("DeviceOrientation.portraitDown", JSONVALUE_AS_STRING(value->array[i])) {
                o = kPortraitDown;
            } else if STREQ("DeviceOrientation.landscapeRight", JSONVALUE_AS_STRING(value->array[i])) {
                o = kLandscapeRight;
            } else {
                fm_respond_illegal_arg_json(
                    responsehandle,
                    "Expected `arg` to only contain stringifications of the "
                    "`DeviceOrientation` enum."
                );
                return;
            }

            // if the list contains the current orientation, we just return and don't change the current orientation at all.
            /// TODO: improve this, ideally the view orientation should be private
            if (o == plugin->flutterpi->view.orientation) {
                fm_respond_success_json(responsehandle, NULL);
                return;
            }

            preferred_orientations[o] = true;
        }

        // if we have to change the orientation, we go through the orientation enum in the defined order and
        // select the first one that is preferred by flutter.
        for (int i = kPortraitUp; i <= kLandscapeRight; i++) {
            if (preferred_orientations[i]) {
                FlutterEngineResult result;

                flutterpi_fill_view_properties(plugin->flutterpi, true, i, false, 0);

                /// FIXME: This will unconditionally enable the mouse cursor.
                compositor_apply_cursor_state(plugin->flutterpi->compositor, true, plugin->flutterpi->view.rotation, plugin->flutterpi->display.pixel_ratio);

                // send updated window metrics to flutter
                /// TODO: Move this into flutter-pi
                result = plugin->flutterpi->flutter.libflutter_engine->FlutterEngineSendWindowMetricsEvent(plugin->flutterpi->flutter.engine, &(const FlutterWindowMetricsEvent) {
                    .struct_size = sizeof(FlutterWindowMetricsEvent),
                    .width = plugin->flutterpi->view.width, 
                    .height = plugin->flutterpi->view.height,
                    .pixel_ratio = plugin->flutterpi->display.pixel_ratio
                });
                if (result != kSuccess) {
                    fprintf(stderr, "[services] Could not send updated window metrics to flutter. FlutterEngineSendWindowMetricsEvent: %s\n", FLUTTER_RESULT_TO_STRING(result));
                    fm_respond_error_json(responsehandle, "engine-error", "Could not send updated window metrics to flutter", NULL);
                    return;
                }

                fm_respond_success_json(responsehandle, NULL);
                return;
            }
        }

        fm_respond_illegal_arg_json(
            responsehandle,
            "Expected `arg` to contain at least one element."
        );
    } else if STREQ(object->method, "SystemChrome.setApplicationSwitcherDescription") {
        /*
         *  SystemChrome.setApplicationSwitcherDescription(Map description)
         *      Informs the operating system of the desired label and color to be used
         *      to describe the application in any system-level application lists (e.g application switchers)
         *      The argument is a Map with two keys, "label" giving a string description,
         *      and "primaryColor" giving a 32 bit integer value (the lower eight bits being the blue channel,
         *      the next eight bits being the green channel, the next eight bits being the red channel,
         *      and the high eight bits being set, as from Color.value for an opaque color).
         *      The "primaryColor" can also be zero to indicate that the system default should be used.
         */
        
        value = jsobject_get_const(arg, "label");
        if (value && (value->type == kJsonString)) {
            char *label = strdup(value->string_value);
            if (label == NULL) {
                fm_respond_native_error_json(responsehandle, ENOMEM);
                return;
            }

            plugin->label = label;
        }

        fm_respond_success_json(responsehandle, NULL);
    } else if STREQ(object->method, "SystemChrome.setEnabledSystemUIOverlays") {
        /*
         *  SystemChrome.setEnabledSystemUIOverlays(List overlays)
         *      Specifies the set of system overlays to have visible when the application
         *      is running. The argument is a List of values which are
         *      string representations of values of the SystemUIOverlay enum.
         * 
         *  enum SystemUIOverlay {
         *      top, bottom
         *  }
         * 
         */
    } else if STREQ(object->method, "SystemChrome.restoreSystemUIOverlays") {
        /*
         * SystemChrome.restoreSystemUIOverlays(void)
         */
    } else if STREQ(object->method, "SystemChrome.setSystemUIOverlayStyle") {
        /*  
         *  SystemChrome.setSystemUIOverlayStyle(struct SystemUIOverlayStyle)
         * 
         *  enum Brightness:
         *      light, dark
         * 
         *  struct SystemUIOverlayStyle:
         *      systemNavigationBarColor: null / uint32
         *      statusBarColor: null / uint32
         *      statusBarIconBrightness: null / Brightness
         *      statusBarBrightness: null / Brightness
         *      systemNavigationBarIconBrightness: null / Brightness
         */
    } else if STREQ(object->method, "SystemNavigator.pop") {
        flutterpi_schedule_exit(plugin->flutterpi);
    } else {
        fm_respond_not_implemented(responsehandle);
    }
}

static void on_receive_accessibility(
    bool success,
    struct flutter_message_response_handle *responsehandle,
    const char *channel,
    const struct platch_obj *object,
    void *userdata
) {
    fm_respond_not_implemented(responsehandle);
}

static void on_receive_platform_views(
    bool success,
    struct flutter_message_response_handle *responsehandle,
    const char *channel,
    const struct platch_obj *object,
    void *userdata
) {
    int ok;
    
    if STREQ("create", object->method) {
        fm_respond_not_implemented(responsehandle);
    } else if STREQ("dispose", object->method) {
        fm_respond_not_implemented(responsehandle);
    } else {
        fm_respond_not_implemented(responsehandle);
    }
}


int services_init(struct flutterpi *flutterpi, void **userdata) {
    struct plugin *plugin;
    int ok;

    plugin = malloc(sizeof *plugin);
    if (plugin == NULL) {
        return ENOMEM;
    }

    plugin->flutterpi = flutterpi;
    plugin->label = NULL;
    plugin->isolate_id = NULL;
    plugin->primary_color = 0;

    ok = fm_set_listener(
        flutterpi->flutter_messenger,
        "flutter/navigation",
        kJSONMethodCall,
        on_receive_navigation,
        NULL,
        plugin
    );
    if (ok != 0) {
        LOG_SERVICES_PLUGIN_ERROR("Couldn't listen to platform channel \"flutter/navigation\". fm_set_listener: %s", strerror(ok));
        goto fail_free_plugin;
    }

    ok = fm_set_listener_raw(
        flutterpi->flutter_messenger,
        "flutter/isolate",
        on_receive_isolate,
        plugin
    );
    if (ok != 0) {
        LOG_SERVICES_PLUGIN_ERROR("Couldn't listen to platform channel \"flutter/isolate\". fm_set_listener_raw: %s", strerror(ok));
        goto fail_remove_navigation_receiver;
    }

    ok = fm_set_listener(
        flutterpi->flutter_messenger,
        "flutter/platform",
        kJSONMethodCall,
        on_receive_platform,
        NULL,
        plugin
    );
    if (ok != 0) {
        LOG_SERVICES_PLUGIN_ERROR("Couldn't listen to platform channel \"flutter/platform\". fm_set_listener: %s", strerror(ok));
        goto fail_remove_isolate_receiver;
    }

    ok = fm_set_listener(
        flutterpi->flutter_messenger,
        "flutter/accessibility",
        kBinaryCodec,
        on_receive_accessibility,
        NULL,
        plugin
    );
    if (ok != 0) {
        LOG_SERVICES_PLUGIN_ERROR("Couldn't listen to platform channel \"flutter/accessibility\". fm_set_listener: %s", strerror(ok));
        goto fail_remove_platform_receiver;
    }

    ok = fm_set_listener(
        flutterpi->flutter_messenger,
        "flutter/platform_views",
        kStandardMethodCall,
        on_receive_platform_views,
        NULL,
        plugin
    );
    if (ok != 0) {
        LOG_SERVICES_PLUGIN_ERROR("Couldn't listen to platform channel \"flutter/platform_views\". fm_set_listener: %s", strerror(ok));
        goto fail_remove_accessibility_receiver;
    }

    return 0;


    fail_remove_platform_views_receiver:
    fm_remove_listener(flutterpi->flutter_messenger, "flutter/platform_views");

    fail_remove_accessibility_receiver:
    fm_remove_listener(flutterpi->flutter_messenger, "flutter/accessibility");

    fail_remove_platform_receiver:
    fm_remove_listener(flutterpi->flutter_messenger, "flutter/platform");

    fail_remove_isolate_receiver:
    fm_remove_listener(flutterpi->flutter_messenger, "flutter/isolate");

    fail_remove_navigation_receiver:
    fm_remove_listener(flutterpi->flutter_messenger, "flutter/navigation");

    fail_free_plugin:
    free(plugin);

    fail_return_ok:
    return ok;
}

int services_deinit(struct flutterpi *flutterpi, void **userdata) {
    fm_remove_listener(flutterpi->flutter_messenger, "flutter/navigation");
    fm_remove_listener(flutterpi->flutter_messenger, "flutter/isolate");
    fm_remove_listener(flutterpi->flutter_messenger, "flutter/platform");
    fm_remove_listener(flutterpi->flutter_messenger, "flutter/accessibility");
    fm_remove_listener(flutterpi->flutter_messenger, "flutter/platform_views");
    return 0;
}
