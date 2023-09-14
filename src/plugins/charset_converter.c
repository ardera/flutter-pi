#include "plugins/charset_converter.h"

//#include <inttypes.h>
//#include <stdio.h>
//#include <stdlib.h>
//#include <string.h>

#include "flutter-pi.h"
#include "pluginregistry.h"

static int on_receive(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *responsehandle) {
    printf(
        "[charset converter plugin] on_receive_std(channel: %s)\n"
        "  method: %s\n"
        "  args: \n",
        channel,
        object->method
    );

    return platch_respond(
        responsehandle,
        &(struct platch_obj){ .codec = kStandardMethodCallResponse, .success = true, .std_result = { .type = kStdTrue } }
    );
}

enum plugin_init_result init(struct flutterpi *flutterpi, void **userdata_out) {
    int ok;

    ok = plugin_registry_set_receiver_locked(CHARSET_CONVERTER_CHANNEL, kStandardMethodCall, on_receive);
    if (ok != 0) {
        return PLUGIN_INIT_RESULT_ERROR;
    }

    *userdata_out = NULL;

    return PLUGIN_INIT_RESULT_INITIALIZED;
}

void deinit(struct flutterpi *flutterpi, void *userdata) {
    plugin_registry_remove_receiver_v2_locked(flutterpi_get_plugin_registry(flutterpi), CHARSET_CONVERTER_CHANNEL);
    return 0;
}

FLUTTERPI_PLUGIN("charset converter plugin", charset_converter_plugin, init, deinit)