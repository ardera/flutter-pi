#include "plugins/charset_converter.h"
#include "flutter-pi.h"
#include "pluginregistry.h"
#include "util/logging.h"
#include <iconv.h>

static bool convert(char *buf, char *outbuf, size_t len, const char *from, const char *to)
{
    iconv_t iconv_cd;
    if ((iconv_cd = iconv_open(to, from)) == (iconv_t) -1) {
        LOG_ERROR("Cannot open iconv from %s to %s\n", from, to);
        return false;
    }

    char *inbuf = buf;
    size_t inlen = len;
    size_t outlen = len;
    size_t res = 0;

    while (inlen > 0 && outlen > 0) {
        res = iconv(iconv_cd, &inbuf, &inlen, &outbuf, &outlen);
        if (res == 0)
            break;

        if (res == (size_t) (-1))
        {
            if (errno != EILSEQ && errno != EINVAL)
            {
                iconv_close(iconv_cd);
                *outbuf = '\0';

                return true;
            }
            else if (inbuf < outbuf) {
                iconv_close(iconv_cd);
                *outbuf = '\0';

                return true;
            }
        }

        if (inlen > 0 && outlen > 0) {
            *outbuf++ = *inbuf++;
            inlen--;
            outlen--;
        }
    }

    iconv_close(iconv_cd);
    *outbuf = '\0';
    return true;
}

static int on_encode(struct platch_obj *object, FlutterPlatformMessageResponseHandle *responseHandle) {
    struct std_value *args, *tmp;
    char *charset, *data;

    args = &object->std_arg;

    if (args == NULL || !STDVALUE_IS_MAP(*args)) {
        return platch_respond_illegal_arg_std(responseHandle, "Expected `arg` to be a map.");
    }

    tmp = stdmap_get_str(&object->std_arg, "charset");
    if (tmp == NULL || !STDVALUE_IS_STRING(*tmp)) {
        LOG_ERROR("Call missing mandatory parameter charset.\n");
        return platch_respond_illegal_arg_std(responseHandle, "Expected `arg['charset'] to be a string.");
    }

    charset = STDVALUE_AS_STRING(*tmp);

    tmp = stdmap_get_str(&object->std_arg, "data");
    if (tmp == NULL || !STDVALUE_IS_STRING(*tmp)) {
        LOG_ERROR("Call missing mandatory parameter data.\n");
        return platch_respond_illegal_arg_std(responseHandle, "Expected `arg['data'] to be a string.");
    }

    data = STDVALUE_AS_STRING(*tmp);

    char* from = (char*) malloc(strlen(data) + 1);
    char* to = (char*) malloc(strlen(data) + 1);
    strcpy(from, data);

    auto res = convert(from, to, strlen(from) + 1, "UTF-8", charset);
    if(!res)
    {
        return platch_respond_error_std(responseHandle, "error_id", "charset_name_unrecognized", NULL);
    }

    uint8_t* output;
    output = (uint8_t*)malloc(strlen(to));

    for(int i = 0; i < strlen(to); i++)
    {
        output[i] = (uint8_t)to[i];
    }

    return platch_respond(
        responseHandle,
        &(struct platch_obj){ .codec = kStandardMethodCallResponse, .success = true, .std_result = { .type = kStdUInt8Array, .uint8array = &output[0], .size = strlen(to) }, }
    );
}

static int on_available_charsets(struct platch_obj *object, FlutterPlatformMessageResponseHandle *responseHandle) {
    auto list = (struct std_value[]){ { .type = kStdString, .string_value = "Not available for Linux." }};

    return platch_respond(
        responseHandle,
        &(struct platch_obj){ .codec = kStandardMethodCallResponse, .success = true, .std_result = { .type = kStdList, .list = list, .size = 1 }, }
    );
}

static int on_check(struct platch_obj *object, FlutterPlatformMessageResponseHandle *responseHandle) {
    struct std_value *args, *tmp;
    char *charset;

    args = &object->std_arg;

    if (args == NULL || !STDVALUE_IS_MAP(*args)) {
        return platch_respond_illegal_arg_std(responseHandle, "Expected `arg` to be a map.");
    }

    tmp = stdmap_get_str(&object->std_arg, "charset");
    if (tmp == NULL || !STDVALUE_IS_STRING(*tmp)) {
        LOG_ERROR("Call missing mandatory parameter charset.\n");
        return platch_respond_illegal_arg_std(responseHandle, "Expected `arg['charset'] to be a string.");
    }

    charset = STDVALUE_AS_STRING(*tmp);

    iconv_t iconv_cd;
    if ((iconv_cd = iconv_open("UTF-8", charset)) == (iconv_t) -1) {
        return platch_respond(
            responseHandle,
            &(struct platch_obj){ .codec = kStandardMethodCallResponse, .success = true, .std_result = { .type = kStdFalse } }
        );
    }

    return platch_respond(
        responseHandle,
        &(struct platch_obj){ .codec = kStandardMethodCallResponse, .success = true, .std_result = { .type = kStdTrue } }
    );
}

static int on_receive(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *responseHandle) {
    const char *method;
    method = object->method;

    if (streq(method, "encode")) {
        return on_encode(object, responseHandle);
    } else if (streq(method, "availableCharsets")) {
        return on_available_charsets(object, responseHandle);
    } else if (streq(method, "check")) {
        return on_check(object, responseHandle);
    }

    return platch_respond_not_implemented(responseHandle);
}

enum plugin_init_result charset_converter_init(struct flutterpi *flutterpi, void **userdata_out) {
    int ok;

    ok = plugin_registry_set_receiver_locked(CHARSET_CONVERTER_CHANNEL, kStandardMethodCall, on_receive);
    if (ok != 0) {
        return PLUGIN_INIT_RESULT_ERROR;
    }

    *userdata_out = NULL;

    return PLUGIN_INIT_RESULT_INITIALIZED;
}

void charset_converter_deinit(struct flutterpi *flutterpi, void *userdata) {
    plugin_registry_remove_receiver_v2_locked(flutterpi_get_plugin_registry(flutterpi), CHARSET_CONVERTER_CHANNEL);
    return 0;
}

FLUTTERPI_PLUGIN("charset converter plugin", charset_converter_plugin, charset_converter_init, charset_converter_deinit)