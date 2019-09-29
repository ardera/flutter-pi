#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "pluginregistry.h"

#define TESTPLUGIN_CHANNEL_JSON "plugins.flutter-pi.io/testjson"
#define TESTPLUGIN_CHANNEL_STD "plugins.flutter-pi.io/teststd"
#define INDENT_STRING "                    "

int __printJSON(struct JSONMsgCodecValue *value, int indent) {
    switch (value->type) {
        case kJSNull:
            printf("null");
            break;
        case kJSTrue:
            printf("true");
            break;
        case kJSFalse:
            printf("false");
            break;
        case kJSNumber:
            printf("%f", value->number_value);
            break;
        case kJSString:
            printf("\"%s\"", value->string_value);
            break;
        case kJSArray:
            printf("[\n");
            for (int i = 0; i < value->size; i++) {
                printf("%.*s", indent + 2, INDENT_STRING);
                __printJSON(&(value->array[i]), indent + 2);
                if (i+1 != value->size) printf(",\n", indent + 2, INDENT_STRING);
            }
            printf("\n%.*s]", indent, INDENT_STRING);
            break;
        case kJSObject:
            printf("{\n");
            for (int i = 0; i < value->size; i++) {
                printf("%.*s\"%s\": ", indent + 2, INDENT_STRING, value->keys[i]);
                __printJSON(&(value->values[i]), indent + 2);
                if (i+1 != value->size) printf(",\n", indent +2, INDENT_STRING);
            }
            printf("\n%.*s}", indent, INDENT_STRING);
            break;
        default: break;
    }

    return 0;
}
int printJSON(struct JSONMsgCodecValue *value, int indent) {
    printf("%.*s", indent, INDENT_STRING);
    __printJSON(value, indent);
    printf("\n");
}
int __printStd(struct StdMsgCodecValue *value, int indent) {
    switch (value->type) {
        case kNull:
            printf("null");
            break;
        case kTrue:
            printf("true");
            break;
        case kFalse:
            printf("false");
            break;
        case kInt32:
            printf("%" PRIi32, value->int32_value);
            break;
        case kInt64:
            printf("%" PRIi64, value->int64_value);
            break;
        case kFloat64:
            printf("%lf", value->float64_value);
            break;
        case kString:
        case kLargeInt:
            printf("\"%s\"", value->string_value);
            break;
        case kUInt8Array:
            printf("(uint8_t) [");
            for (int i = 0; i < value->size; i++) {
                printf("0x%02X", value->uint8array[i]);
                if (i + 1 != value->size) printf(", ");
            }
            printf("]");
            break;
        case kInt32Array:
            printf("(int32_t) [");
            for (int i = 0; i < value->size; i++) {
                printf("%" PRIi32, value->int32array[i]);
                if (i + 1 != value->size) printf(", ");
            }
            printf("]");
            break;
        case kInt64Array:
            printf("(int64_t) [");
            for (int i = 0; i < value->size; i++) {
                printf("%" PRIi64, value->int64array[i]);
                if (i + 1 != value->size) printf(", ");
            }
            printf("]");
            break;
        case kFloat64Array:
            printf("(double) [");
            for (int i = 0; i < value->size; i++) {
                printf("%ld", value->float64array[i]);
                if (i + 1 != value->size) printf(", ");
            }
            printf("]");
            break;
        case kList:
            printf("[\n");
            for (int i = 0; i < value->size; i++) {
                printf("%.*s", indent + 2, INDENT_STRING);
                __printStd(&(value->list[i]), indent + 2);
                if (i + 1 != value->size) printf(",\n");
            }
            printf("\n%.*s]", indent, INDENT_STRING);
            break;
        case kMap:
            printf("{\n");
            for (int i = 0; i < value->size; i++) {
                printf("%.*s", indent + 2, INDENT_STRING);
                __printStd(&(value->keys[i]), indent + 2);
                printf(": ");
                __printStd(&(value->values[i]), indent + 2);
                if (i + 1 != value->size) printf(",\n");
            }
            printf("\n%.*s}", indent, INDENT_STRING);
            break;
        default:
            break;
    }
}
int printStd(struct StdMsgCodecValue *value, int indent) {
    printf("%.*s", indent, INDENT_STRING);
    __printStd(value, indent);
    printf("\n");
}

#undef INDENT_STRING


int TestPlugin_onReceiveResponseJSON(struct ChannelObject *object, void *userdata) {
    uint64_t dt = FlutterEngineGetCurrentTime() - *((uint64_t*) userdata);
    free(userdata);
    
    if (object->codec == kNotImplemented) {
        printf("channel " TESTPLUGIN_CHANNEL_JSON " not implented on flutter side\n");
        return 0;
    }

    if (object->success) {
        printf("TestPlugin_onReceiveResponseJSON(dt: %lluns)\n"
               "  success\n"
               "  result:\n", dt);
        printJSON(&object->jsresult, 4);
    } else {
        printf("TestPlugin_onReceiveResponseJSON(dt: %lluns)\n", dt);
        printf("  failure\n"
               "  error code: %s\n"
               "  error message: %s\n"
               "  error details:\n", object->errorcode, (object->errormessage != NULL) ? object->errormessage : "null");
        printJSON(&object->jsresult, 4);
    }

    return 0;
}
int TestPlugin_sendJSON() {
    uint64_t* time = malloc(sizeof(uint64_t));
    *time = FlutterEngineGetCurrentTime();

    char *method = "test";
    struct JSONMsgCodecValue argument = {
        .type = kJSObject,
        .size = 5,
        .keys = (char*[]) {
            "key1",
            "key2",
            "key3",
            "key4",
            "array"
        },
        .values = (struct JSONMsgCodecValue[]) {
            {.type = kJSString, .string_value = "value1"},
            {.type = kJSTrue},
            {.type = kJSNumber, .number_value = -1000},
            {.type = kJSNumber, .number_value = -5.0005},
            {.type = kJSArray, .size = 2, .array = (struct JSONMsgCodecValue[]) {
                {.type = kJSString, .string_value = "array1"},
                {.type = kJSNumber, .number_value = 2}
            }}
        },
    };

    int ok = PlatformChannel_jsoncall(TESTPLUGIN_CHANNEL_JSON, method, &argument, TestPlugin_onReceiveResponseJSON, time);
    if (ok != 0) {
        printf("Could not MethodCall JSON: %s\n", strerror(ok));
    }
}
int TestPlugin_onReceiveResponseStd(struct ChannelObject *object, void *userdata) {
    uint64_t dt = FlutterEngineGetCurrentTime() - *((uint64_t*) userdata);
    free(userdata);

    if (object->codec == kNotImplemented) {
        printf("channel " TESTPLUGIN_CHANNEL_STD " not implented on flutter side\n");
        return 0;
    }

    if (object->success) {
        printf("TestPlugin_onReceiveResponseStd(dt: %lluns)\n"
               "  success\n"
               "  result:\n", dt);
        printStd(&object->stdresult, 4);
    } else {
        printf("TestPlugin_onReceiveResponseStd(dt: %lluns)\n", dt);
        printf("  failure\n"
               "  error code: %s\n"
               "  error message: %s\n"
               "  error details:\n", object->errorcode, (object->errormessage != NULL) ? object->errormessage : "null");
        printStd(&object->stdresult, 4);
    }

    return 0;
}
int TestPlugin_sendStd() {
    uint64_t *time = malloc(sizeof(uint64_t));
    *time = FlutterEngineGetCurrentTime();

    char *method = "test";
    struct StdMsgCodecValue argument = {
        .type = kMap,
        .size = 7,
        .keys = (struct StdMsgCodecValue[]) {
            {.type = kString, .string_value = "key1"},
            {.type = kString, .string_value = "key2"},
            {.type = kString, .string_value = "key3"},
            {.type = kString, .string_value = "key4"},
            {.type = kInt32, .int32_value = 5},
            {.type = kString, .string_value = "timestamp"},
            {.type = kString, .string_value = "array"}
        },
        .values = (struct StdMsgCodecValue[]) {
            {.type = kString, .string_value = "value1"},
            {.type = kTrue},
            {.type = kInt32, .int32_value = -1000},
            {.type = kFloat64, .float64_value = -5.0005},
            {.type = kUInt8Array, .uint8array = (uint8_t[]) {0x00, 0x01, 0x02, 0x03, 0xFF}, .size = 5},
            {.type = kInt64, .int64_value = *time & 0x7FFFFFFFFFFFFFFF},
            {.type = kList, .size = 2, .list = (struct StdMsgCodecValue[]) {
                {.type = kString, .string_value = "array1"},
                {.type = kInt32, .int32_value = 2}
            }}
        },
    };

    PlatformChannel_stdcall(TESTPLUGIN_CHANNEL_STD, method, &argument, TestPlugin_onReceiveResponseStd, time);
}


int TestPlugin_onReceiveJSON(char *channel, struct ChannelObject *object, FlutterPlatformMessageResponseHandle *responsehandle) {
    printf("TestPlugin_onReceiveJSON(channel: %s)\n"
           "  method: %s\n"
           "  args: \n", channel, object->method);
    printJSON(&(object->jsarg), 4);
    
    TestPlugin_sendJSON();

    return PlatformChannel_respond(responsehandle, &(struct ChannelObject) {
        .codec = kJSONMethodCallResponse,
        .success = true,
        .jsresult = {
            .type = kJSTrue
        }
    });
}
int TestPlugin_onReceiveStd(char *channel, struct ChannelObject *object, FlutterPlatformMessageResponseHandle *responsehandle) {
    printf("TestPlugin_onReceiveStd(channel: %s)\n"
           "  method: %s\n"
           "  args: \n", channel, object->method);

    printStd(&(object->stdarg), 4);

    TestPlugin_sendStd();
    
    return PlatformChannel_respond(
        responsehandle,
        &(struct ChannelObject) {
            .codec = kStandardMethodCallResponse,
            .success = true,
            .stdresult = {
                .type = kTrue
            }
        }
    );
}


int TestPlugin_init(void) {
    printf("Initializing Testplugin\n");
    PluginRegistry_setReceiver(TESTPLUGIN_CHANNEL_JSON, kJSONMethodCall, TestPlugin_onReceiveJSON);
    PluginRegistry_setReceiver(TESTPLUGIN_CHANNEL_STD, kStandardMethodCall, TestPlugin_onReceiveStd);
    return 0;
}
int TestPlugin_deinit(void) {
    printf("Deinitializing Testplugin\n");
    return 0;
}