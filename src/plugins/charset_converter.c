#include "plugins/charset_converter.h"
#include "flutter-pi.h"
#include "pluginregistry.h"
#include "util/logging.h"
#include <iconv.h>

static bool convert(char *inbuf, char *outbuf, size_t len, const char *from, const char *to)
{
    iconv_t iconv_cd = iconv_open(to, from);
    if (iconv_cd == (iconv_t) -1) {
        LOG_ERROR("Conversion from charset \"%s\" to charset \"%s\" is not supported. iconv_open: %s\n", from, to, strerror(errno));
        return false;
    }

    size_t inlen = len;
    size_t outlen = len;
    size_t res = 0;

    while (inlen > 0 && outlen > 0) {
        res = iconv(iconv_cd, &inbuf, &inlen, &outbuf, &outlen);
        if (res == 0) {
            break;
        }

        if (res == (size_t) (-1)) {
            iconv_close(iconv_cd);
            *outbuf = '\0';

            return false;
        }
    }

    iconv_close(iconv_cd);
    *outbuf = '\0';

    return true;
}

static int on_encode(struct platch_obj *object, FlutterPlatformMessageResponseHandle *response_handle) {
    struct std_value *args, *tmp;
    char *charset, *input, *output;

    args = &object->std_arg;

    if (args == NULL || !STDVALUE_IS_MAP(*args)) {
        return platch_respond_illegal_arg_std(response_handle, "Expected `arg` to be a map.");
    }

    tmp = stdmap_get_str(&object->std_arg, "charset");
    if (tmp == NULL || !STDVALUE_IS_STRING(*tmp)) {
        return platch_respond_illegal_arg_std(response_handle, "Expected `arg['charset'] to be a string.");
    }

    charset = STDVALUE_AS_STRING(*tmp);

    tmp = stdmap_get_str(&object->std_arg, "data");
    if (tmp == NULL || !STDVALUE_IS_STRING(*tmp)) {
        return platch_respond_illegal_arg_std(response_handle, "Expected `arg['data'] to be a string.");
    }

    input = STDVALUE_AS_STRING(*tmp);
    output = malloc(strlen(input) + 1);

    bool res = convert(input, output, strlen(input) + 1, "UTF-8", charset);
    if(!res) {
        free(output);
        return platch_respond_error_std(response_handle, "error_id", "charset_name_unrecognized", NULL);
    }

    int ok = platch_respond_success_std(
        response_handle,
        &(struct std_value) {
            .type = kStdUInt8Array,
            .size = strlen(output),
            .uint8array = (uint8_t*) output,
        }
    );

    free(output);

    return ok;
}

static int on_decode(struct platch_obj *object, FlutterPlatformMessageResponseHandle *response_handle) {
    struct std_value *args, *tmp;
    char *charset, *output;
    const uint8_t *input;

    args = &object->std_arg;

    if (args == NULL || !STDVALUE_IS_MAP(*args)) {
        return platch_respond_illegal_arg_std(response_handle, "Expected `arg` to be a map.");
    }

    tmp = stdmap_get_str(&object->std_arg, "charset");
    if (tmp == NULL || !STDVALUE_IS_STRING(*tmp)) {
        return platch_respond_illegal_arg_std(response_handle, "Expected `arg['charset'] to be a string.");
    }

    charset = STDVALUE_AS_STRING(*tmp);

    tmp = stdmap_get_str(&object->std_arg, "data");
    if (tmp == NULL || (*tmp).type != kStdUInt8Array ) {
        return platch_respond_illegal_arg_std(response_handle, "Expected `arg['data'] to be a uint8_t list.");
    }

    input = tmp->uint8array;
    output = malloc(strlen((char*) input) + 1);

    bool res = convert((char*) input, output, strlen((char*) input) + 1, "UTF-8", charset);
    if(!res) {
        free(output);
        return platch_respond_error_std(response_handle, "error_id", "charset_name_unrecognized", NULL);
    }

    int ok = platch_respond_success_std(
        response_handle,
        &(struct std_value) {
            .type = kStdUInt8Array,
            .size = strlen(output),
            .uint8array = (uint8_t*) output,
        }
    );

    free(output);

    return ok;
}

static int on_available_charsets(struct platch_obj *object, FlutterPlatformMessageResponseHandle *response_handle) {
    (void) object;

    char* output;
    size_t length, count;
    FILE *fp;

    // Count the available charsets
    fp = popen("iconv --list | wc -w", "r");
    if(!fp) {
        return platch_respond_error_std(response_handle, "error_id", "charsets_not_available", NULL);
    }

    while (getline(&output, &length, fp) >= 0) {
        count = atoi(output);
    }

    pclose(fp);

    fp = popen("iconv --list", "r");
    if(!fp) {
        return platch_respond_error_std(response_handle, "error_id", "charsets_not_available", NULL);
    }

    struct std_value values;

    values.type = kStdList;
    values.size = count;
    values.list = alloca(sizeof(struct std_value) * count);

    for (int index = 0; index < count; index++) {
        if(getline(&output, &length, fp) < 0) {
            break;
        }

        strtok(output, "/");

        values.list[index].type = kStdString;
        values.list[index].string_value = strdup(output);
    }

    pclose(fp);

    return platch_respond_success_std(response_handle, &values);
}

static int on_check(struct platch_obj *object, FlutterPlatformMessageResponseHandle *response_handle) {
    struct std_value *args, *tmp;
    char *charset;

    args = &object->std_arg;

    if (args == NULL || !STDVALUE_IS_MAP(*args)) {
        return platch_respond_illegal_arg_std(response_handle, "Expected `arg` to be a map.");
    }

    tmp = stdmap_get_str(&object->std_arg, "charset");
    if (tmp == NULL || !STDVALUE_IS_STRING(*tmp)) {
        return platch_respond_illegal_arg_std(response_handle, "Expected `arg['charset'] to be a string.");
    }

    charset = STDVALUE_AS_STRING(*tmp);

    iconv_t iconv_cd = iconv_open("UTF-8", charset);
    if (iconv_cd == (iconv_t) -1) {
        return platch_respond(
            response_handle,
            &(struct platch_obj){ .codec = kStandardMethodCallResponse, .success = true, .std_result = { .type = kStdFalse } }
        );
    }

    iconv_close(iconv_cd);

    return platch_respond(
        response_handle,
        &(struct platch_obj){ .codec = kStandardMethodCallResponse, .success = true, .std_result = { .type = kStdTrue } }
    );
}

static int on_receive(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *response_handle) {
    (void) channel;

    const char *method;
    method = object->method;

    if (streq(method, "encode")) {
        return on_encode(object, response_handle);
    } else if (streq(method, "decode")) {
        return on_decode(object, response_handle);
    } else if (streq(method, "availableCharsets")) {
        return on_available_charsets(object, response_handle);
    } else if (streq(method, "check")) {
        return on_check(object, response_handle);
    }

    return platch_respond_not_implemented(response_handle);
}

enum plugin_init_result charset_converter_init(struct flutterpi *flutterpi, void **userdata_out) {
    (void) flutterpi;

    int ok;

    ok = plugin_registry_set_receiver_locked(CHARSET_CONVERTER_CHANNEL, kStandardMethodCall, on_receive);
    if (ok != 0) {
        return PLUGIN_INIT_RESULT_ERROR;
    }

    *userdata_out = NULL;

    return PLUGIN_INIT_RESULT_INITIALIZED;
}

void charset_converter_deinit(struct flutterpi *flutterpi, void *userdata) {
    (void) userdata;

    plugin_registry_remove_receiver_v2_locked(flutterpi_get_plugin_registry(flutterpi), CHARSET_CONVERTER_CHANNEL);
}

FLUTTERPI_PLUGIN("charset converter plugin", charset_converter_plugin, charset_converter_init, charset_converter_deinit)