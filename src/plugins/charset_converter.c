#include "plugins/charset_converter.h"
#include "flutter-pi.h"
#include "pluginregistry.h"
#include "util/logging.h"
#include <iconv.h>

static int convert(const char *from, const char *to, const char *input, size_t input_size, char **output_out, size_t *output_size_out) {
    iconv_t iconv_cd = iconv_open(to, from);
    if (iconv_cd == (iconv_t) -1) {
        LOG_ERROR("Conversion from charset \"%s\" to charset \"%s\" is not supported. iconv_open: %s\n", from, to, strerror(errno));
        return ENOENT;
    }

    size_t capacity = input_size + 16;
    char *output = malloc(capacity + 1);
    if (output == NULL) {
        iconv_close(iconv_cd);
        return ENOMEM;
    }

    char *inbuf = (char *) input;
    size_t inleft = input_size;
    size_t used = 0;

    while (true) {
        char *outbuf = output + used;
        size_t outleft = capacity - used;
        bool flush = inleft == 0;

        size_t res;
        if (flush) {
            res = iconv(iconv_cd, NULL, NULL, &outbuf, &outleft);
        } else {
            res = iconv(iconv_cd, &inbuf, &inleft, &outbuf, &outleft);
        }

        used = capacity - outleft;

        if (res != (size_t) -1) {
            if (flush) {
                break;
            }
            continue;
        }

        if (errno != E2BIG) {
            int error = errno;
            LOG_ERROR("Could not convert from charset \"%s\" to charset \"%s\". iconv: %s\n", from, to, strerror(error));
            free(output);
            iconv_close(iconv_cd);
            return error;
        }

        size_t new_capacity = capacity * 2;
        char *new_output = realloc(output, new_capacity + 1);
        if (new_output == NULL) {
            free(output);
            iconv_close(iconv_cd);
            return ENOMEM;
        }

        output = new_output;
        capacity = new_capacity;
    }

    iconv_close(iconv_cd);

    output[used] = '\0';

    *output_out = output;
    *output_size_out = used;
    return 0;
}

static int respond_conversion_error(FlutterPlatformMessageResponseHandle *response_handle, int error) {
    if (error == ENOENT) {
        return platch_respond_error_std(response_handle, "error_id", "Charset not found.", NULL);
    } else if (error == ENOMEM) {
        return platch_respond_native_error_std(response_handle, ENOMEM);
    } else {
        return platch_respond_error_std(response_handle, "error_id", strerror(error), NULL);
    }
}

static int on_encode(struct platch_obj *object, FlutterPlatformMessageResponseHandle *response_handle) {
    struct std_value *args, *tmp;
    char *charset, *input, *output;
    size_t output_size;

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

    int ok = convert("UTF-8", charset, input, strlen(input), &output, &output_size);
    if (ok != 0) {
        return respond_conversion_error(response_handle, ok);
    }

    ok = platch_respond_success_std(
        response_handle,
        &(struct std_value) {
            .type = kStdUInt8Array,
            .size = output_size,
            .uint8array = (uint8_t *) output,
        }
    );

    free(output);

    return ok;
}

static int on_decode(struct platch_obj *object, FlutterPlatformMessageResponseHandle *response_handle) {
    struct std_value *args, *tmp;
    char *charset, *output;
    size_t output_size;

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
    if (tmp == NULL || (*tmp).type != kStdUInt8Array) {
        return platch_respond_illegal_arg_std(response_handle, "Expected `arg['data'] to be a uint8_t list.");
    }

    int ok = convert("UTF-8", charset, (const char *) tmp->uint8array, tmp->size, &output, &output_size);
    if (ok != 0) {
        return respond_conversion_error(response_handle, ok);
    }

    ok = platch_respond_success_std(
        response_handle,
        &(struct std_value) {
            .type = kStdUInt8Array,
            .size = output_size,
            .uint8array = (uint8_t *) output,
        }
    );

    free(output);

    return ok;
}

static int on_available_charsets(struct platch_obj *object, FlutterPlatformMessageResponseHandle *response_handle) {
    (void) object;

    char *output = NULL;
    size_t length = 0, count = 0;
    FILE *fp;

    // Count the available charsets
    fp = popen("iconv --list | wc -w", "r");
    if(!fp) {
        return platch_respond_error_std(response_handle, "error_id", "charsets_not_available", NULL);
    }

    while (getline(&output, &length, fp) >= 0) {
        count = strtoul(output, NULL, 10);
    }

    pclose(fp);

    if (count == 0) {
        free(output);
        return platch_respond_error_std(response_handle, "error_id", "charsets_not_available", NULL);
    }

    fp = popen("iconv --list", "r");
    if(!fp) {
        free(output);
        return platch_respond_error_std(response_handle, "error_id", "charsets_not_available", NULL);
    }

    struct std_value values;

    values.type = kStdList;
    values.size = count;
    values.list = alloca(sizeof(struct std_value) * count);

    size_t index;
    for (index = 0; index < count; index++) {
        if(getline(&output, &length, fp) < 0) {
            break;
        }

        strtok(output, "/");

        values.list[index].type = kStdString;
        values.list[index].string_value = strdup(output);
    }

    pclose(fp);
    free(output);

    values.size = index;

    int ok = platch_respond_success_std(response_handle, &values);

    for (size_t i = 0; i < index; i++) {
        free(values.list[i].string_value);
    }

    return ok;
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