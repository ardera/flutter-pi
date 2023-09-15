#include "plugins/printing.h"
#include "flutter-pi.h"
#include "pluginregistry.h"
#include "util/logging.h"

static int on_raster_pdf(struct platch_obj *object, FlutterPlatformMessageResponseHandle *responseHandle) {
    //TODO handle
    return platch_respond(
        responseHandle,
        &(struct platch_obj){ .codec = kStandardMethodCallResponse, .success = true, .std_result = { .type = kStdTrue } }
    );
}

static int on_receive(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *responseHandle) {
    const char *method;
    method = object->method;

    if (streq(method, "rasterPdf")) {
        return on_raster_pdf(object, responseHandle);
    }

    return platch_respond_not_implemented(responseHandle);
}

enum plugin_init_result printing_init(struct flutterpi *flutterpi, void **userdata_out) {
    int ok;

    ok = plugin_registry_set_receiver_locked(PRINTING_CHANNEL, kStandardMethodCall, on_receive);
    if (ok != 0) {
        return PLUGIN_INIT_RESULT_ERROR;
    }

    *userdata_out = NULL;

    return PLUGIN_INIT_RESULT_INITIALIZED;
}

void printing_deinit(struct flutterpi *flutterpi, void *userdata) {
    plugin_registry_remove_receiver_v2_locked(flutterpi_get_plugin_registry(flutterpi), PRINTING_CHANNEL);
    return 0;
}

FLUTTERPI_PLUGIN("printing plugin", printing_plugin, printing_init, printing_deinit)