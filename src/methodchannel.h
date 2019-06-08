#ifndef _METHODCHANNEL_H
#define _METHODCHANNEL_H

#include <stdint.h>
#include <flutter_embedder.h>

typedef enum {
    kNull = 0,
    kTrue,
    kFalse,
    kTypeInt,
    kTypeLong,
    kTypeBigInt,
    kTypeDouble,
    kTypeString,
    kTypeByteArray,
    kTypeIntArray,
    kTypeLongArray,
    kTypeDoubleArray,
    kTypeList,
    kTypeMap,
    kNoValue = 0xFFFF
} MessageValueDiscriminator;

struct MethodChannelValue {
    MessageValueDiscriminator type;
    union {
        bool bool_value;
        int32_t int_value;
        int64_t long_value;
        char* bigint_value;
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
            struct MethodChannelValue* list;
        } list_value;
        struct {
            size_t size;
            struct MethodChannelValue* map;
        } map_value;
    } value;
};

struct MethodCall {
    char* method;
    struct MethodChannelValue argument;
};

bool MethodChannel_call(char* channel, char* method, struct MethodChannelValue* argument);
bool MethodChannel_respond(FlutterPlatformMessageResponseHandle* response_handle, struct MethodChannelValue* response_value);
bool MethodChannel_decode(size_t buffer_size, uint8_t* buffer, struct MethodCall* result);
bool MethodChannel_freeMethodCall(struct MethodCall* methodcall);

#endif