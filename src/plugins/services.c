#include "plugins/services.h"

#include <ctype.h>
#include <errno.h>

#include "cursor.h"
#include "flutter-pi.h"
#include "pluginregistry.h"
#include "util/asserts.h"
#include "util/logging.h"

struct plugin {
    struct flutterpi *flutterpi;
    char label[256];
    uint32_t primary_color;  // ARGB8888 (blue is the lowest byte)
    char isolate_id[32];
};

static void on_receive_navigation(ASSERTED void *userdata, const FlutterPlatformMessage *message) {
    ASSUME(userdata);
    ASSUME(message);
    platch_respond_not_implemented(message->response_handle);
}

static void on_receive_isolate(void *userdata, const FlutterPlatformMessage *message) {
    struct plugin *plugin;

    ASSUME(userdata);
    ASSUME(message);
    plugin = userdata;

    ASSERT(message->message_size <= sizeof(plugin->isolate_id));
    memcpy(plugin->isolate_id, message->message, message->message_size);
    platch_respond_not_implemented(message->response_handle);
}

static void on_receive_platform(void *userdata, const FlutterPlatformMessage *message) {
    struct platch_obj object;
    struct json_value *value;
    struct json_value *arg;
    struct plugin *plugin;
    int ok;

    ASSUME(userdata);
    ASSUME(message);
    plugin = userdata;

    ok = platch_decode(message->message, message->message_size, kJSONMethodCall, &object);
    if (ok != 0) {
        platch_respond_error_json(message->response_handle, "malformed-message", "The platform channel message was malformed.", NULL);
        return;
    }

    arg = &(object.json_arg);

    if (streq(object.method, "Clipboard.setData")) {
        /*
         *  Clipboard.setData(Map data)
         *      Places the data from the text entry of the argument,
         *      which must be a Map, onto the system clipboard.
         */
    } else if (streq(object.method, "Clipboard.getData")) {
        /*
         *  Clipboard.getData(String format)
         *      Returns the data that has the format specified in the argument
         *      from the system clipboard. The only currently supported is "text/plain".
         *      The result is a Map with a single key, "text".
         */
    } else if (streq(object.method, "HapticFeedback.vibrate")) {
        /*
         *  HapticFeedback.vibrate(void)
         *      Triggers a system-default haptic response.
         */
    } else if (streq(object.method, "SystemSound.play")) {
        /*
         *  SystemSound.play(String soundName)
         *      Triggers a system audio effect. The argument must
         *      be a String describing the desired effect; currently only "click" is
         *      supported.
         */
    } else if (streq(object.method, "SystemChrome.setPreferredOrientations")) {
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
    } else if (streq(object.method, "SystemChrome.setApplicationSwitcherDescription")) {
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
        if (value && (value->type == kJsonString)) {
            strncpy(plugin->label, value->string_value, sizeof(plugin->label) - 1);
        }

        platch_free_obj(&object);
        platch_respond_success_json(message->response_handle, NULL);
        return;
    } else if (streq(object.method, "SystemChrome.setEnabledSystemUIOverlays")) {
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
    } else if (streq(object.method, "SystemChrome.restoreSystemUIOverlays")) {
        /*
         * SystemChrome.restoreSystemUIOverlays(void)
         */
    } else if (streq(object.method, "SystemChrome.setSystemUIOverlayStyle")) {
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
    } else if (streq(object.method, "SystemNavigator.pop")) {
        LOG_DEBUG("received SystemNavigator.pop. Exiting...\n");
        flutterpi_schedule_exit(flutterpi);
    }

    platch_free_obj(&object);
    platch_respond_not_implemented(message->response_handle);
}

static void on_receive_accessibility(ASSERTED void *userdata, const FlutterPlatformMessage *message) {
    ASSUME(userdata);
    ASSUME(message);
    platch_respond_not_implemented(message->response_handle);
}

static void on_receive_platform_views(ASSERTED void *userdata, const FlutterPlatformMessage *message) {
    ASSUME(userdata);
    ASSUME(message);

    // if (streq("create", object->method)) {
    //     return platch_respond_not_implemented(responsehandle);
    // } else if (streq("dispose", object->method)) {
    //     return platch_respond_not_implemented(responsehandle);
    // }

    platch_respond_not_implemented(message->response_handle);
}

static void on_receive_mouse_cursor(ASSERTED void *userdata, const FlutterPlatformMessage *message) {
    const struct raw_std_value *method_call;
    const struct raw_std_value *arg;
    struct plugin *plugin;

    ASSERT_NOT_NULL(userdata);
    plugin = userdata;

    method_call = (const struct raw_std_value *) (message->message);

    if (!raw_std_method_call_check(method_call, message->message_size)) {
        platch_respond_illegal_arg_std(message->response_handle, "Malformed platform message.");
        return;
    }

    arg = raw_std_method_call_get_arg(method_call);

    if (raw_std_method_call_is_method(method_call, "activateSystemCursor")) {
        if (!raw_std_value_is_map(arg)) {
            platch_respond_illegal_arg_std(message->response_handle, "Expected `arg` to be a Map.");
            return;
        }

        bool has_device = false;
        UNUSED int64_t device;

        bool has_kind = false;
        enum pointer_kind kind = POINTER_KIND_BASIC;

        for_each_entry_in_raw_std_map(key, value, arg) {
            if (!raw_std_value_is_string(key)) {
                continue;
            }

            if (raw_std_string_equals(key, "device")) {
                if (!raw_std_value_is_int(value)) {
                    platch_respond_illegal_arg_std(message->response_handle, "Expected `arg['device']` to be an int.");
                    return;
                }

                has_device = true;
                device = raw_std_value_as_int(value);
            } else if (raw_std_string_equals(key, "kind")) {
                if (!raw_std_value_is_string(value)) {
                    platch_respond_illegal_arg_std(message->response_handle, "Expected `arg['kind']` to be a string.");
                    return;
                }

                if (raw_std_string_equals(value, "none")) {
                    kind = POINTER_KIND_NONE;
                } else if (raw_std_string_equals(value, "basic")) {
                    kind = POINTER_KIND_BASIC;
                } else if (raw_std_string_equals(value, "click")) {
                    kind = POINTER_KIND_CLICK;
                } else if (raw_std_string_equals(value, "forbidden")) {
                    kind = POINTER_KIND_FORBIDDEN;
                } else if (raw_std_string_equals(value, "wait")) {
                    kind = POINTER_KIND_WAIT;
                } else if (raw_std_string_equals(value, "progress")) {
                    kind = POINTER_KIND_PROGRESS;
                } else if (raw_std_string_equals(value, "contextMenu")) {
                    kind = POINTER_KIND_CONTEXT_MENU;
                } else if (raw_std_string_equals(value, "help")) {
                    kind = POINTER_KIND_HELP;
                } else if (raw_std_string_equals(value, "text")) {
                    kind = POINTER_KIND_TEXT;
                } else if (raw_std_string_equals(value, "verticalText")) {
                    kind = POINTER_KIND_VERTICAL_TEXT;
                } else if (raw_std_string_equals(value, "cell")) {
                    kind = POINTER_KIND_CELL;
                } else if (raw_std_string_equals(value, "precise")) {
                    kind = POINTER_KIND_PRECISE;
                } else if (raw_std_string_equals(value, "move")) {
                    kind = POINTER_KIND_MOVE;
                } else if (raw_std_string_equals(value, "grab")) {
                    kind = POINTER_KIND_GRAB;
                } else if (raw_std_string_equals(value, "grabbing")) {
                    kind = POINTER_KIND_GRABBING;
                } else if (raw_std_string_equals(value, "noDrop")) {
                    kind = POINTER_KIND_NO_DROP;
                } else if (raw_std_string_equals(value, "alias")) {
                    kind = POINTER_KIND_ALIAS;
                } else if (raw_std_string_equals(value, "copy")) {
                    kind = POINTER_KIND_COPY;
                } else if (raw_std_string_equals(value, "disappearing")) {
                    kind = POINTER_KIND_DISAPPEARING;
                } else if (raw_std_string_equals(value, "allScroll")) {
                    kind = POINTER_KIND_ALL_SCROLL;
                } else if (raw_std_string_equals(value, "resizeLeftRight")) {
                    kind = POINTER_KIND_RESIZE_LEFT_RIGHT;
                } else if (raw_std_string_equals(value, "resizeUpDown")) {
                    kind = POINTER_KIND_RESIZE_UP_DOWN;
                } else if (raw_std_string_equals(value, "resizeUpLeftDownRight")) {
                    kind = POINTER_KIND_RESIZE_UP_LEFT_DOWN_RIGHT;
                } else if (raw_std_string_equals(value, "resizeUpRightDownLeft")) {
                    kind = POINTER_KIND_RESIZE_UP_RIGHT_DOWN_LEFT;
                } else if (raw_std_string_equals(value, "resizeUp")) {
                    kind = POINTER_KIND_RESIZE_UP;
                } else if (raw_std_string_equals(value, "resizeDown")) {
                    kind = POINTER_KIND_RESIZE_DOWN;
                } else if (raw_std_string_equals(value, "resizeLeft")) {
                    kind = POINTER_KIND_RESIZE_LEFT;
                } else if (raw_std_string_equals(value, "resizeRight")) {
                    kind = POINTER_KIND_RESIZE_RIGHT;
                } else if (raw_std_string_equals(value, "resizeUpLeft")) {
                    kind = POINTER_KIND_RESIZE_UP_LEFT;
                } else if (raw_std_string_equals(value, "resizeUpRight")) {
                    kind = POINTER_KIND_RESIZE_UP_RIGHT;
                } else if (raw_std_string_equals(value, "resizeDownLeft")) {
                    kind = POINTER_KIND_RESIZE_DOWN_LEFT;
                } else if (raw_std_string_equals(value, "resizeDownRight")) {
                    kind = POINTER_KIND_RESIZE_DOWN_RIGHT;
                } else if (raw_std_string_equals(value, "resizeColumn")) {
                    kind = POINTER_KIND_RESIZE_COLUMN;
                } else if (raw_std_string_equals(value, "resizeRow")) {
                    kind = POINTER_KIND_RESIZE_ROW;
                } else if (raw_std_string_equals(value, "zoomIn")) {
                    kind = POINTER_KIND_ZOOM_IN;
                } else if (raw_std_string_equals(value, "zoomOut")) {
                    kind = POINTER_KIND_ZOOM_OUT;
                } else {
                    platch_respond_illegal_arg_std(message->response_handle, "Expected `arg['kind']` to be a valid mouse pointer kind.");
                    return;
                }

                has_kind = true;
            }
        }

        if (!has_device || !has_kind) {
            platch_respond_illegal_arg_std(message->response_handle, "Expected both `arg['device']` and `arg['kind']` to be non-null.");
            return;
        }

        flutterpi_set_pointer_kind(plugin->flutterpi, kind);

        platch_respond_success_std(message->response_handle, &STDNULL);
    } else {
        platch_respond_not_implemented(message->response_handle);
    }
}

enum plugin_init_result services_init(struct flutterpi *flutterpi, void **userdata_out) {
    struct plugin_registry *registry;
    struct plugin *plugin;
    int ok;

    ASSUME(flutterpi);

    registry = flutterpi_get_plugin_registry(flutterpi);

    plugin = malloc(sizeof *plugin);
    if (plugin == NULL) {
        goto fail_return_error;
    }

    plugin->flutterpi = flutterpi;

    ok = plugin_registry_set_receiver_v2_locked(registry, FLUTTER_NAVIGATION_CHANNEL, on_receive_navigation, plugin);
    if (ok != 0) {
        LOG_ERROR("Could not set \"" FLUTTER_NAVIGATION_CHANNEL "\" receiver. plugin_registry_set_receiver_v2_locked: %s\n", strerror(ok));
        goto fail_free_plugin;
    }

    ok = plugin_registry_set_receiver_v2_locked(registry, FLUTTER_ISOLATE_CHANNEL, on_receive_isolate, plugin);
    if (ok != 0) {
        LOG_ERROR("Could not set \"" FLUTTER_ISOLATE_CHANNEL "\" receiver. plugin_registry_set_receiver_v2_locked: %s\n", strerror(ok));
        goto fail_remove_navigation_receiver;
    }

    ok = plugin_registry_set_receiver_v2_locked(registry, FLUTTER_PLATFORM_CHANNEL, on_receive_platform, plugin);
    if (ok != 0) {
        LOG_ERROR("Could not set \"" FLUTTER_PLATFORM_CHANNEL "\" receiver. plugin_registry_set_receiver_v2_locked: %s\n", strerror(ok));
        goto fail_remove_isolate_receiver;
    }

    ok = plugin_registry_set_receiver_v2_locked(registry, FLUTTER_ACCESSIBILITY_CHANNEL, on_receive_accessibility, plugin);
    if (ok != 0) {
        LOG_ERROR(
            "Could not set \"" FLUTTER_ACCESSIBILITY_CHANNEL "\" receiver. plugin_registry_set_receiver_v2_locked: %s\n",
            strerror(ok)
        );
        goto fail_remove_platform_receiver;
    }

    ok = plugin_registry_set_receiver_v2_locked(registry, FLUTTER_PLATFORM_VIEWS_CHANNEL, on_receive_platform_views, plugin);
    if (ok != 0) {
        LOG_ERROR(
            "Could not set \"" FLUTTER_PLATFORM_VIEWS_CHANNEL "\" receiver. plugin_registry_set_receiver_v2_locked: %s\n",
            strerror(ok)
        );
        goto fail_remove_accessibility_receiver;
    }

    ok = plugin_registry_set_receiver_v2_locked(registry, FLUTTER_MOUSECURSOR_CHANNEL, on_receive_mouse_cursor, plugin);
    if (ok != 0) {
        LOG_ERROR("Could not set \"" FLUTTER_MOUSECURSOR_CHANNEL "\" receiver. plugin_registry_set_receiver_v2_locked: %s\n", strerror(ok));
        goto fail_remove_platform_views_receiver;
    }

    *userdata_out = plugin;

    return 0;

fail_remove_platform_views_receiver:
    plugin_registry_remove_receiver_v2_locked(registry, FLUTTER_PLATFORM_VIEWS_CHANNEL);

fail_remove_accessibility_receiver:
    plugin_registry_remove_receiver_v2_locked(registry, FLUTTER_ACCESSIBILITY_CHANNEL);

fail_remove_platform_receiver:
    plugin_registry_remove_receiver_v2_locked(registry, FLUTTER_PLATFORM_CHANNEL);

fail_remove_isolate_receiver:
    plugin_registry_remove_receiver_v2_locked(registry, FLUTTER_ISOLATE_CHANNEL);

fail_remove_navigation_receiver:
    plugin_registry_remove_receiver_v2_locked(registry, FLUTTER_NAVIGATION_CHANNEL);

fail_free_plugin:
    free(plugin);

fail_return_error:
    return PLUGIN_INIT_RESULT_ERROR;
}

void services_deinit(struct flutterpi *flutterpi, void *userdata) {
    struct plugin_registry *registry;
    struct plugin *plugin;

    ASSUME(flutterpi);
    ASSUME(userdata);

    registry = flutterpi_get_plugin_registry(flutterpi);
    plugin = userdata;

    plugin_registry_remove_receiver_v2_locked(registry, FLUTTER_NAVIGATION_CHANNEL);
    plugin_registry_remove_receiver_v2_locked(registry, FLUTTER_ISOLATE_CHANNEL);
    plugin_registry_remove_receiver_v2_locked(registry, FLUTTER_PLATFORM_CHANNEL);
    plugin_registry_remove_receiver_v2_locked(registry, FLUTTER_ACCESSIBILITY_CHANNEL);
    plugin_registry_remove_receiver_v2_locked(registry, FLUTTER_PLATFORM_VIEWS_CHANNEL);
    plugin_registry_remove_receiver_v2_locked(registry, FLUTTER_MOUSECURSOR_CHANNEL);
    free(plugin);
}

FLUTTERPI_PLUGIN("services", services, services_init, services_deinit)
