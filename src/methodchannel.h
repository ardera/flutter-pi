#ifndef _METHODCHANNEL_H
#define _METHODCHANNEL_H

#include <stdint.h>
#include <flutter_embedder.h>

typedef enum {
    kNull = 0,
    kTrue,
    kFalse,
    kInt32,
    kInt64,
    kLargeInt, // treat as kString
    kFloat64,
    kString,
    kUInt8Array,
    kInt32Array,
    kInt64Array,
    kFloat64Array,
    kList,
    kMap
} MessageValueDiscriminator;

struct MessageChannelValue {
    MessageValueDiscriminator type;
    union {
        bool bool_value;
        int32_t int_value;
        int64_t long_value;
        double double_value;
        char* string_value;
        struct {
            size_t size;
            uint8_t* array;
        } bytearray_value;
        struct {
            size_t size;
            int32_t* array;
        } intarray_value;
        struct {
            size_t size;
            int64_t* array;
        } longarray_value;
        struct {
            size_t size;
            double* array;
        } doublearray_value;
        struct {
            size_t size;
            struct MessageChannelValue* list;
        } list_value;
        struct {
            size_t size;
            struct MessageChannelValue* map;
        } map_value;
    };
};

struct MethodCall {
    enum {
        kStandardProtocol,
        kJSONProtocol
    } protocol;
    char* method;
    struct MessageChannelValue argument;
};

bool MethodChannel_call(char* channel, char* method, struct MessageChannelValue* argument);
bool MethodChannel_respond(FlutterPlatformMessageResponseHandle* response_handle, struct MessageChannelValue* response_value);
bool MethodChannel_decode(size_t buffer_size, uint8_t* buffer, struct MethodCall** presult);
bool MethodChannel_freeMethodCall(struct MethodCall** pmethodcall);

#endif