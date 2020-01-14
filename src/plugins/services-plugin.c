#include <ctype.h>
#include <errno.h>

#include <flutter-pi.h>
#include <pluginregistry.h>
#include "services-plugin.h"

struct {
    char label[256];
    uint32_t primaryColor;  // ARGB8888 (blue is the lowest byte)
    char isolateId[32];
} ServicesPlugin = {0};


int Services_onReceiveNavigation(char *channel, struct ChannelObject *object, FlutterPlatformMessageResponseHandle *responsehandle) {
    return PlatformChannel_respondNotImplemented(responsehandle);
}

int Services_onReceiveIsolate(char *channel, struct ChannelObject *object, FlutterPlatformMessageResponseHandle *responsehandle) {
    memset(&(ServicesPlugin.isolateId), sizeof(ServicesPlugin.isolateId), 0);
    memcpy(ServicesPlugin.isolateId, object->binarydata, object->binarydata_size);
    
    return PlatformChannel_respondNotImplemented(responsehandle);
}

int Services_onReceivePlatform(char *channel, struct ChannelObject *object, FlutterPlatformMessageResponseHandle *responsehandle) {
    struct JSONMsgCodecValue *value;
    struct JSONMsgCodecValue *arg = &(object->jsarg);
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
        
        value = &object->jsarg;
        
        if (value->type != kJSArray)
            return PlatformChannel_respondError(responsehandle, kJSONMethodCallResponse, "illegalargument", "Expected List as argument", NULL);
        
        if (value->size == 0)
            return PlatformChannel_respondError(responsehandle, kJSONMethodCallResponse, "illegalargument", "Argument List must have at least one value", NULL);


        bool preferred_orientations[kLandscapeRight+1] = {0};

        for (int i = 0; i < value->size; i++) {

            if (value->array[i].type != kJSString) {
                return PlatformChannel_respondError(
                    responsehandle, kJSONMethodCallResponse,
                    "illegalargument", "Argument List should only contain strings", NULL
                );
            }
            
            enum device_orientation o = ORIENTATION_FROM_STRING(value->array[i].string_value);

            if (o == -1) {
                return PlatformChannel_respondError(
                    responsehandle, kJSONMethodCallResponse,
                    "illegalargument",
                    "Argument List elements should values of the DeviceOrientation enum",
                    NULL
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
        if (value && (value->type == kJSString))
            snprintf(ServicesPlugin.label, sizeof(ServicesPlugin.label), "%s", value->string_value);
        
        return PlatformChannel_respond(responsehandle, &(struct ChannelObject) {
            .codec = kJSONMethodCallResponse,
            .success = true,
            .jsresult = {
                .type = kNull
            }
        });
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

    return PlatformChannel_respondNotImplemented(responsehandle);
}

int Services_onReceiveAccessibility(char *channel, struct ChannelObject *object, FlutterPlatformMessageResponseHandle *responsehandle) {
    return PlatformChannel_respondNotImplemented(responsehandle);
}


int Services_init(void) {
    int ok;

    ok = PluginRegistry_setReceiver("flutter/navigation", kJSONMethodCall, Services_onReceiveNavigation);
    if (ok != 0) {
        printf("Could not set flutter/navigation ChannelObject receiver: %s\n", strerror(ok));
        return ok;
    }

    ok = PluginRegistry_setReceiver("flutter/isolate", kBinaryCodec, Services_onReceiveIsolate);
    if (ok != 0) {
        printf("Could not set flutter/isolate  ChannelObject receiver: %s\n", strerror(ok));
        return ok;
    }

    ok = PluginRegistry_setReceiver("flutter/platform", kJSONMethodCall, Services_onReceivePlatform);
    if (ok != 0) {
        printf("Could not set flutter/platform ChannelObject receiver: %s\n", strerror(ok));
        return ok;
    }

    ok = PluginRegistry_setReceiver("flutter/accessibility", kBinaryCodec, Services_onReceiveAccessibility);
    if (ok != 0) {
        printf("Could not set flutter/accessibility  ChannelObject receiver: %s\n", strerror(ok));
        return ok;
    }

    printf("Initialized Services plugin.\n");
}

int Services_deinit(void) {
    printf("Deinitialized Services plugin.\n");
}