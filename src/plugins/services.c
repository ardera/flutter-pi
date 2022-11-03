#include <ctype.h>
#include <errno.h>

#include <flutter-pi.h>
#include <pluginregistry.h>
//#include <compositor.h>
#include <plugins/services.h>

static struct {
    char label[256];
    uint32_t primary_color;  // ARGB8888 (blue is the lowest byte)
    char isolate_id[32];
} services = {0};


static int on_receive_navigation(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *responsehandle) {
    (void) channel;
    (void) object;
    return platch_respond_not_implemented(responsehandle);
}

static int on_receive_isolate(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *responsehandle) {
    (void) channel;
    if (object->binarydata_size > sizeof(services.isolate_id)) {
        return EINVAL;
    } else {
        memcpy(services.isolate_id, object->binarydata, object->binarydata_size);
    }
    
    return platch_respond_not_implemented(responsehandle);
}

static int on_receive_platform(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *responsehandle) {
    struct json_value *value;
    struct json_value *arg = &(object->json_arg);

    (void) channel;

    if (strcmp(object->method, "Clipboard.setData") == 0) {
        /*
         *  Clipboard.setData(Map data)
         *      Places the data from the text entry of the argument,
         *      which must be a Map, onto the system clipboard.
         */
    } else if (strcmp(object->method, "Clipboard.getData") == 0) {
        /*
         *  Clipboard.getData(String format)
         *      Returns the data that has the format specified in the argument
         *      from the system clipboard. The only currently supported is "text/plain".
         *      The result is a Map with a single key, "text".
         */ 
    } else if (strcmp(object->method, "HapticFeedback.vibrate") == 0) {
        /*
         *  HapticFeedback.vibrate(void)
         *      Triggers a system-default haptic response.
         */
    } else if (strcmp(object->method, "SystemSound.play") == 0) {
        /*
         *  SystemSound.play(String soundName)
         *      Triggers a system audio effect. The argument must
         *      be a String describing the desired effect; currently only "click" is
         *      supported.
         */
    } else if (strcmp(object->method, "SystemChrome.setPreferredOrientations") == 0) {
        /*
         *  SystemChrome.setPreferredOrientations(DeviceOrientation[])
         *      Informs the operating system of the desired orientation of the display. The argument is a [List] of
         *      values which are string representations of values of the [DeviceOrientation] enum.
         * 
         *  enum DeviceOrientation {
         *      portraitUp, landscapeLeft, portraitDown, landscapeRight
         *  }
         */
        
        /// TODO: Implement

        /*
        value = &object->json_arg;
        
        if ((value->type != kJsonArray) || (value->size == 0)) {
            return platch_respond_illegal_arg_json(
                responsehandle,
                "Expected `arg` to be an array with minimum size 1."
            );
        }

        bool preferred_orientations[kLandscapeRight+1] = {0};

        for (int i = 0; i < value->size; i++) {

            if (value->array[i].type != kJsonString) {
                return platch_respond_illegal_arg_json(
                    responsehandle,
                    "Expected `arg` to to only contain strings."
                );
            }
            
            enum device_orientation o = ORIENTATION_FROM_STRING(value->array[i].string_value);

            if (o == -1) {
                return platch_respond_illegal_arg_json(
                    responsehandle,
                    "Expected `arg` to only contain stringifications of the "
                    "`DeviceOrientation` enum."
                );
            }

            // if the list contains the current orientation, we just return and don't change the current orientation at all.
            if (o == flutterpi.view.orientation) {
                return platch_respond_success_json(responsehandle, NULL);
            }

            preferred_orientations[o] = true;
        }

        // if we have to change the orientation, we go through the orientation enum in the defined order and
        // select the first one that is preferred by flutter.
        for (int i = kPortraitUp; i <= kLandscapeRight; i++) {
            if (preferred_orientations[i]) {
                FlutterEngineResult result;

                flutterpi_fill_view_properties(true, i, false, 0);

                compositor_apply_cursor_state(false, flutterpi.view.rotation, flutterpi.display.pixel_ratio);

                // send updated window metrics to flutter
                result = flutterpi.flutter.libflutter_engine.FlutterEngineSendWindowMetricsEvent(flutterpi.flutter.engine, &(const FlutterWindowMetricsEvent) {
                    .struct_size = sizeof(FlutterWindowMetricsEvent),
                    .width = flutterpi.view.width, 
                    .height = flutterpi.view.height,
                    .pixel_ratio = flutterpi.display.pixel_ratio
                });
                if (result != kSuccess) {
                    fprintf(stderr, "[services] Could not send updated window metrics to flutter. FlutterEngineSendWindowMetricsEvent: %s\n", FLUTTER_RESULT_TO_STRING(result));
                    return platch_respond_error_json(responsehandle, "engine-error", "Could not send updated window metrics to flutter", NULL);
                }

                return platch_respond_success_json(responsehandle, NULL);
            }
        }

        return platch_respond_illegal_arg_json(
            responsehandle,
            "Expected `arg` to contain at least one element."
        );
        */
    } else if (strcmp(object->method, "SystemChrome.setApplicationSwitcherDescription") == 0) {
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
        
        value = jsobject_get(arg, "label");
        if (value && (value->type == kJsonString))
            snprintf(services.label, sizeof(services.label), "%s", value->string_value);
        
        return platch_respond_success_json(responsehandle, NULL);
    } else if (strcmp(object->method, "SystemChrome.setEnabledSystemUIOverlays") == 0) {
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
    } else if (strcmp(object->method, "SystemChrome.restoreSystemUIOverlays") == 0) {
        /*
         * SystemChrome.restoreSystemUIOverlays(void)
         */
    } else if (strcmp(object->method, "SystemChrome.setSystemUIOverlayStyle") == 0) {
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
    } else if (strcmp(object->method, "SystemNavigator.pop") == 0) {
        LOG_FLUTTERPI_ERROR("received SystemNavigator.pop. Exiting...\n");
        flutterpi_schedule_exit(flutterpi);
    }

    return platch_respond_not_implemented(responsehandle);
}

static int on_receive_accessibility(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *responsehandle) {
    (void) channel;
    (void) object;
    return platch_respond_not_implemented(responsehandle);
}

static int on_receive_platform_views(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *responsehandle) {
    (void) channel;
    (void) object;

    if STREQ("create", object->method) {
        return platch_respond_not_implemented(responsehandle);
    } else if STREQ("dispose", object->method) {
        return platch_respond_not_implemented(responsehandle);
    }

    return platch_respond_not_implemented(responsehandle);
}

enum plugin_init_result services_init(struct flutterpi *flutterpi, void **userdata_out) {
    int ok;

    (void) flutterpi;
    (void) userdata_out;

    ok = plugin_registry_set_receiver(FLUTTER_NAVIGATION_CHANNEL, kJSONMethodCall, on_receive_navigation);
    if (ok != 0) {
        fprintf(stderr, "[services-plugin] could not set \"" FLUTTER_NAVIGATION_CHANNEL "\" platform message receiver: %s\n", strerror(ok));
        goto fail_return_ok;
    }

    ok = plugin_registry_set_receiver(FLUTTER_ISOLATE_CHANNEL, kBinaryCodec, on_receive_isolate);
    if (ok != 0) {
        fprintf(stderr, "[services-plugin] could not set \"" FLUTTER_ISOLATE_CHANNEL "\" ChannelObject receiver: %s\n", strerror(ok));
        goto fail_remove_navigation_receiver;
    }

    ok = plugin_registry_set_receiver(FLUTTER_PLATFORM_CHANNEL, kJSONMethodCall, on_receive_platform);
    if (ok != 0) {
        fprintf(stderr, "[services-plugin] could not set \"" FLUTTER_PLATFORM_CHANNEL "\" ChannelObject receiver: %s\n", strerror(ok));
        goto fail_remove_isolate_receiver;
    }

    ok = plugin_registry_set_receiver(FLUTTER_ACCESSIBILITY_CHANNEL, kBinaryCodec, on_receive_accessibility);
    if (ok != 0) {
        fprintf(stderr, "[services-plugin] could not set \"" FLUTTER_ACCESSIBILITY_CHANNEL "\" ChannelObject receiver: %s\n", strerror(ok));
        goto fail_remove_platform_receiver;
    }

    ok = plugin_registry_set_receiver(FLUTTER_PLATFORM_VIEWS_CHANNEL, kStandardMethodCall, on_receive_platform_views);
    if (ok != 0) {
        fprintf(stderr, "[services-plugin] could not set \"" FLUTTER_PLATFORM_VIEWS_CHANNEL "\" ChannelObject receiver: %s\n", strerror(ok));
        goto fail_remove_accessibility_receiver;
    }

    return 0;

    fail_remove_accessibility_receiver:
    plugin_registry_remove_receiver(FLUTTER_ACCESSIBILITY_CHANNEL);

    fail_remove_platform_receiver:
    plugin_registry_remove_receiver(FLUTTER_PLATFORM_CHANNEL);

    fail_remove_isolate_receiver:
    plugin_registry_remove_receiver(FLUTTER_ISOLATE_CHANNEL);

    fail_remove_navigation_receiver:
    plugin_registry_remove_receiver(FLUTTER_NAVIGATION_CHANNEL);

    fail_return_ok:
    return kError_PluginInitResult;
}

void services_deinit(struct flutterpi *flutterpi, void *userdata) {
    (void) flutterpi;
    (void) userdata;
    plugin_registry_remove_receiver(FLUTTER_NAVIGATION_CHANNEL);
    plugin_registry_remove_receiver(FLUTTER_ISOLATE_CHANNEL);
    plugin_registry_remove_receiver(FLUTTER_PLATFORM_CHANNEL);
    plugin_registry_remove_receiver(FLUTTER_ACCESSIBILITY_CHANNEL);
    plugin_registry_remove_receiver(FLUTTER_PLATFORM_VIEWS_CHANNEL);
}

FLUTTERPI_PLUGIN(
    "services",
    services,
    services_init,
    services_deinit
)
