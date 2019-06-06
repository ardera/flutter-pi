#ifndef _METHODCHANNEL_H
#define _METHODCHANNEL_H

#include <stdint.h>
#include <flutter_embedder.h>

enum MessageValueDiscriminator {
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
};

FlutterPlatformMessage* buildMethodChannelMessage(const char* channel, const FlutterPlatformMessageResponseHandle* response_handle, uint64_t* arguments);

#endif