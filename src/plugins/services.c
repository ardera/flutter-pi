#include <ctype.h>
#include <errno.h>

#include <flutter-pi.h>
#include <pluginregistry.h>
#include <plugins/services.h>

struct {
    char label[256];
    uint32_t primary_color;  // ARGB8888 (blue is the lowest byte)
    char isolate_id[32];
} services = {0};


int services_on_receive_navigation(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *responsehandle) {
    return platch_respond_not_implemented(responsehandle);
}

int services_on_receive_isolate(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *responsehandle) {
    memset(&(services.isolate_id), sizeof(services.isolate_id), 0);
    memcpy(services.isolate_id, object->binarydata, object->binarydata_size);
    
    return platch_respond_not_implemented(responsehandle);
}

int services_on_receive_platform(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *responsehandle) {
    struct json_value *value;
    struct json_value *arg = &(object->json_arg);
    int ok;
    
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
            if (o == orientation) {
                return 0;
            }

            preferred_orientations[o] = true;
        }

        // if we have to change the orientation, we go through the orientation enum in the defined order and
        // select the first one that is preferred by flutter.
        for (int i = kPortraitUp; i <= kLandscapeRight; i++) {
            if (preferred_orientations[i]) {
                post_platform_task(&(struct flutterpi_task) {
                    .type = kUpdateOrientation,
                    .orientation = i,
                    .target_time = 0
                });
                return 0;
            }
        }
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
    } else if (strcmp(object->method, "SystemNavigator.pop")) {
        printf("flutter requested application exit\n");
    }

    return platch_respond_not_implemented(responsehandle);
}

int services_on_receive_accessibility(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *responsehandle) {
    return platch_respond_not_implemented(responsehandle);
}

int services_on_receive_platform_views(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *responsehandle) {
    struct json_value *value;
    struct json_value *arg = &(object->json_arg);
    int ok;
    
    if STREQ("create", object->method) {
        return platch_respond_not_implemented(responsehandle);
    } else if STREQ("dispose", object->method) {
        return platch_respond_not_implemented(responsehandle);
    }

    return platch_respond_not_implemented(responsehandle);
}


int services_init(void) {
    int ok;

    printf("[services] Initializing...\n");

    ok = plugin_registry_set_receiver("flutter/navigation", kJSONMethodCall, services_on_receive_navigation);
    if (ok != 0) {
        fprintf(stderr, "[services-plugin] could not set \"flutter/navigation\" ChannelObject receiver: %s\n", strerror(ok));
        return ok;
    }

    ok = plugin_registry_set_receiver("flutter/isolate", kBinaryCodec, services_on_receive_isolate);
    if (ok != 0) {
        fprintf(stderr, "[services-plugin] could not set \"flutter/isolate\" ChannelObject receiver: %s\n", strerror(ok));
        return ok;
    }

    ok = plugin_registry_set_receiver("flutter/platform", kJSONMethodCall, services_on_receive_platform);
    if (ok != 0) {
        fprintf(stderr, "[services-plugin] could not set \"flutter/platform\" ChannelObject receiver: %s\n", strerror(ok));
        return ok;
    }

    ok = plugin_registry_set_receiver("flutter/accessibility", kBinaryCodec, services_on_receive_accessibility);
    if (ok != 0) {
        fprintf(stderr, "[services-plugin] could not set \"flutter/accessibility\" ChannelObject receiver: %s\n", strerror(ok));
        return ok;
    }

    ok = plugin_registry_set_receiver("flutter/platform_views", kStandardMethodCall, services_on_receive_platform_views);
    if (ok != 0) {
        fprintf(stderr, "[services-plugin] could not set \"flutter/platform_views\" ChannelObject receiver: %s\n", strerror(ok));
        return ok;
    }

    printf("[services] Done.\n");

    return 0;
}

int services_deinit(void) {
    printf("[services] deinit.\n");
    return 0;
}
