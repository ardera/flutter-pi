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
        int32_t int32_value;
        int64_t int64_value;
        double float64_value;
        char* string_value;
        struct {
            size_t size;
            uint8_t* array;
        } uint8array_value;
        struct {
            size_t size;
            int32_t* array;
        } int32array_value;
        struct {
            size_t size;
            int64_t* array;
        } int64array_value;
        struct {
            size_t size;
            double* array;
        } float64array_value;
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

bool PlatformChannel_calculateValueSizeInBuffer(struct MethodChannelValue* value, size_t* psize);
bool PlatformChannel_writeValueToBuffer(struct MessageChannelValue *value, uint8_t **pbuffer);
bool PlatformChannel_decodeValue(uint8_t** pbuffer, size_t* buffer_remaining, struct MessageChannelValue* value);
bool PlatformChannel_freeValue(struct MessageChannelValue* p_value);
bool PlatformChannel_sendMessage(char *channel, struct MessageChannelValue *argument);
bool PlatformChannel_decodeMessage(size_t buffer_size, uint8_t *buffer, struct MethodCall **presult);
bool PlatformChannel_respond(FlutterPlatformMessageResponseHandle *response_handle, struct MessageChannelValue *response);
bool PlatformChannel_call(char *channel, char *method, struct MessageChannelValue *argument);
bool PlatformChannel_decodeMethodCall(size_t buffer_size, uint8_t* buffer, struct MethodCall** presult);
bool PlatformChannel_freeMethodCall(struct MethodCall **pmethodcall);

#endif