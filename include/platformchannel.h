#ifndef _METHODCHANNEL_H
#define _METHODCHANNEL_H

#include <stdint.h>
#include <errno.h>
#include <flutter_embedder.h>

#define JSON_DECODE_TOKENLIST_SIZE  128

// andrew
// only 32bit support for now.
#define __ALIGN4_REMAINING(value, remaining, ...) __align((uint32_t*) (value), 4, remaining)
#define __ALIGN8_REMAINING(value, remaining, ...) __align((uint32_t*) (value), 8, remaining)
#define align4(...) __ALIGN4_REMAINING(__VA_ARGS__, NULL)
#define align8(...) __ALIGN8_REMAINING(__VA_ARGS__, NULL)

#define alignmentDiff(value, alignment) __alignmentDiff((uint32_t) value, alignment)

#define __ADVANCE_REMAINING(value, n, remaining, ...) __advance((uint32_t*) (value), n, remaining)
#define advance(...) __ADVANCE_REMAINING(__VA_ARGS__, NULL)


/*
 * It may be simpler for plugins if the two message value types were unified.
 * But from a performance POV, this doesn't make sense. number arrays in StandardMessageCodec
 * are 4 or 8 -byte aligned for faster access. We don't have to copy them, StdMsgCodecValue.int64array (as an example)
 * is just a pointer to that portion of the buffer, where the array is located.
 * 
 * However, JSON and thus JSON Message Handlers have no idea what a int64array is, they just know of JSON arrays.
 * This means we'd have to implicitly convert the int64array into a JSON array when we want to unify the two message value types,
 * and this costs all the performance we (more precisely, the flutter engineers) gained by memory-aligning the arrays in StdMsgCodecValue.
 * 
 * Let's just hope the flutter team doesn't randomly switch codecs of platform channels. Receive Handlers would
 * need to be rewritten every time they do. The handlers not needing to be rewritten would probably be the only advantage
 * of using a unified message value type.
 */
enum JSONMsgCodecValueType {
    kJSNull, kJSTrue, kJSFalse, kJSNumber, kJSString, kJSArray, kJSObject
};
struct JSONMsgCodecValue {
    enum JSONMsgCodecValueType type;
    union {
        double number_value;
        char  *string_value;
        struct {
            size_t size;
            union {
                struct JSONMsgCodecValue *array;
                struct {
                    char                    **keys;
                    struct JSONMsgCodecValue *values;
                };
            };
        };
    };
};

enum StdMsgCodecValueType {
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
};
struct StdMsgCodecValue {
    enum StdMsgCodecValueType type;
    union {
        bool    bool_value;
        int32_t int32_value;
        int64_t int64_value;
        double  float64_value;
        char*   string_value;
        struct {
            size_t size;
            union {
                uint8_t* uint8array;
                int32_t* int32array;
                int64_t* int64array;
                double*  float64array;
                struct StdMsgCodecValue* list;
                struct {
                    struct StdMsgCodecValue* keys;
                    struct StdMsgCodecValue* values;
                };
            };
        };
    };
};

/// codec of an abstract channel object
/// These tell this API how it should encode ChannelObjects -> platform messages
/// and how to decode platform messages -> ChannelObjects.
enum ChannelCodec {
    kNotImplemented,
    kStringCodec,
    kBinaryCodec,
    kJSONMessageCodec,
    kStandardMessageCodec,
    kStandardMethodCall,
    kStandardMethodCallResponse,
    kJSONMethodCall,
    kJSONMethodCallResponse
};

/// Abstract Channel Object.
/// Different properties are "valid" for different codecs:
///   kNotImplemented:
///     no values associated with this "codec".
///     this represents a platform message with no buffer and zero length. (so just an empty response)
///   kStringCodec:
///     - string_value is the raw byte data of a platform message, but with an additional null-byte at the end.
///   kBinaryCodec:
///     - binarydata is an array of the raw byte data of a platform message,
///     - binarydata_size is the size of that byte data in uint8_t's.
///   kJSONMessageCodec:
///     - jsonmsgcodec_value
///   kStdMsgCodecValue:
///     - stdmsgcodec_value
///   kStandardMethodCall:
///     - "method" is the method you'd like to call, or the method that was called
///       by flutter.
///     - stdarg contains the argument to that method call.
///   kJSONMethodCall:
///     - "method" is the method you'd like to call, or the method that was called
///       by flutter.
///     - jsarg contains the argument to that method call.
///   kStandardMethodCallResponse or kJSONMethodCallResponse:
///     - "success" is whether the method call (send to flutter or received from flutter)
///       was succesful, i.e. no errors ocurred.
///       if success is false,
///         - errorcode must be set (by you or by flutter) to point to a valid null-terminated string,
///         - errormessage is either pointing to a valid null-terminated string or is set to NULL,
///         - if the codec is kStandardMethodCallResponse,
///             stderrordetails may be set to any StdMsgCodecValue or NULL.
///         - if the codec is kJSONMethodCallResponse,
///             jserrordetails may be set to any JSONMSgCodecValue or NULL.
struct ChannelObject {
    enum ChannelCodec codec;
    union {
        char                    *string_value;
        struct {
            size_t   binarydata_size;
            uint8_t *binarydata;
        };
        struct JSONMsgCodecValue jsonmsgcodec_value;
        struct StdMsgCodecValue  stdmsgcodec_value;
        struct {
            char *method;
            union {
                struct StdMsgCodecValue  stdarg;
                struct JSONMsgCodecValue jsarg;
            };
        };
        struct {
            bool success;
            union {
                struct StdMsgCodecValue  stdresult;
                struct JSONMsgCodecValue jsresult;
            };
            char *errorcode;
            char *errormessage;
            union {
                struct StdMsgCodecValue stderrordetails;
                struct JSONMsgCodecValue jserrordetails;
            };
        };
    };
};

/// A Callback that is called when a response to a platform message you send to flutter
/// arrives. "object" is the platform message decoded using the "codec" you gave to PlatformChannel_send,
/// "userdata" is the userdata you gave to PlatformChannel_send.
typedef int (*PlatformMessageResponseCallback)(struct ChannelObject *object, void *userdata);


/// decodes a platform message (represented by `buffer` and `size`) as the given codec,
/// and puts the result into object_out.
/// This method will (in some cases) dynamically allocate memory,
/// so you should always call PlatformChannel_free(object_out) when you don't need it anymore.
/// 
/// Additionally, PlatformChannel_decode currently "borrows" from the buffer, so if the buffer
/// is freed by flutter, the contents of object_out will in many cases be bogus.
/// If you'd like object_out to be persistent and not depend on the lifetime of the buffer,
/// you'd have to manually deep-copy it.
int PlatformChannel_decode(uint8_t *buffer, size_t size, enum ChannelCodec codec, struct ChannelObject *object_out);

/// decodes a JSON String into a JSONMsgCodecValue
int PlatformChannel_decodeJSON(char *string, struct JSONMsgCodecValue *out)

/// Encodes a generic ChannelObject into a buffer (that is, too, allocated by PlatformChannel_encode)
/// A pointer to the buffer is put into buffer_out and the size of that buffer into size_out.
/// The lifetime of the buffer is independent of the ChannelObject, so contents of the ChannelObject
///   can be freed after the object was encoded.
int PlatformChannel_encode(struct ChannelObject *object, uint8_t **buffer_out, size_t *size_out);

/// Encodes a generic ChannelObject (anything, string/binary codec or Standard/JSON Method Calls and responses) as a platform message
/// and sends it to flutter on channel `channel`
/// If you supply a response callback (i.e. on_response is != NULL):
///   when flutter responds to this message, it is automatically decoded using the codec given in `response_codec`.
///   Then, on_response is called with the decoded ChannelObject and the userdata as an argument.
///   It's possible that flutter won't respond to your platform message, like when sending events via an EventChannel.
/// userdata can be NULL.
int PlatformChannel_send(char *channel, struct ChannelObject *object, enum ChannelCodec response_codec, PlatformMessageResponseCallback on_response, void *userdata);

/// Encodes a StandardMethodCodec method call as a platform message and sends it to flutter
/// on channel `channel`. This is just a wrapper around PlatformChannel_send
/// that builds the ChannelObject for you.
/// The response_codec is kStandardMethodCallResponse. userdata can be NULL.
int PlatformChannel_stdcall(char *channel, char *method, struct StdMsgCodecValue *argument, PlatformMessageResponseCallback on_response, void *userdata);

/// Encodes a JSONMethodCodec method call as a platform message and sends it to flutter
/// on channel `channel`. This is just a wrapper around PlatformChannel_send
/// that builds the ChannelObject for you.
/// The response_codec is kJSONMethodCallResponse. userdata can be NULL.
int PlatformChannel_jsoncall(char *channel, char *method, struct JSONMsgCodecValue *argument, PlatformMessageResponseCallback on_response, void *userdata);

/// Responds to a platform message. You can (of course) only respond once to a platform message,
/// i.e. a FlutterPlatformMessageResponseHandle can only be used once.
/// The codec of `response` can be any of the available codecs.
int PlatformChannel_respond(FlutterPlatformMessageResponseHandle *handle, struct ChannelObject *response);

/// Tells flutter that the platform message that was sent to you was not handled.
///   (for example, there's no plugin that is using this channel, or there is a plugin
///   but it doesn't want to respond.)
/// You should always use this instead of not replying to a platform message, since not replying could cause a memory leak.
/// When flutter receives this response, it will throw a MissingPluginException.
///   For most channel used by the ServicesPlugin, this is not too bad since it
///   specifies many of the channels used as OptionalMethodChannels. (which will silently catch the MissingPluginException)
int PlatformChannel_respondNotImplemented(FlutterPlatformMessageResponseHandle *handle);

/// Tells flutter that the method that was called caused an error.
/// errorcode MUST be non-null, errormessage can be null.
/// when codec is one of kStandardMethodCall, kStandardMethodCallResponse, kStandardMessageCodec:
///   errordetails must either be NULL or a pointer to a struct StdMsgCodecValue.
/// when codec is one of kJSONMethodCall, kJSONMethodCallResponse or kJSONMessageCodec:
///   errordetails must either be NULL or a pointer to a struct JSONMsgCodecValue.
int PlatformChannel_respondError(FlutterPlatformMessageResponseHandle *handle, enum ChannelCodec codec, char *errorcode, char *errormessage, void *errordetails);

/// frees a ChannelObject that was decoded using PlatformChannel_decode.
/// not freeing ChannelObjects may result in a memory leak.
int PlatformChannel_free(struct ChannelObject *object);

int PlatformChannel_freeJSONMsgCodecValue(struct JSONMsgCodecValue *value, bool shallow);

/// returns true if values a and b are equal.
/// for JS arrays, the order of the values is relevant
/// (so two arrays are only equal if the same values appear in exactly same order)
/// for objects, the order of the entries is irrelevant.
bool jsvalue_equals(struct JSONMsgCodecValue *a, struct JSONMsgCodecValue *b);

/// given a JS object as an argument, it searches for an entry with key "key"
/// and returns the value associated with it.
/// if the key is not found, returns NULL.
struct JSONMsgCodecValue *jsobject_get(struct JSONMsgCodecValue *object, char *key);

/// StdMsgCodecValue equivalent of jsvalue_equals.
/// again, for lists, the order of values is relevant
/// for maps, it's not.
bool stdvalue_equals(struct StdMsgCodecValue *a, struct StdMsgCodecValue *b);

/// StdMsgCodecValue equivalent of jsobject_get, just that the key can be
/// any arbitrary StdMsgCodecValue (and must not be a string as for jsobject_get)
struct StdMsgCodecValue *stdmap_get(struct StdMsgCodecValue *map, struct StdMsgCodecValue *key);

/// Andrew: Definitions
static inline int __alignmentDiff(uint32_t value, int alignment) {
	alignment--;
	return value - (((((uint32_t) value) + alignment) | alignment) - alignment);
}
static inline void __align(uint32_t *value, int alignment, size_t *remaining) {
	if (remaining != NULL)
		*remaining -= alignmentDiff((uint32_t) *value, alignment);	
	alignment--;

	*value = (uint32_t) (((*value + alignment) | alignment) - alignment);
}
static inline void __advance(uint32_t *value, int n_bytes, size_t *remaining) {
	if (remaining != NULL)
		*remaining -= n_bytes;
	
	*value += n_bytes;
}

static inline void write8(uint8_t **pbuffer, uint8_t value) {
	*(uint8_t*) *pbuffer = value;
}
static inline void write16(uint8_t **pbuffer, uint16_t value) {
	*(uint16_t*) *pbuffer = value;
}
static inline void write32(uint8_t **pbuffer, uint32_t value) {
	*(uint32_t*) *pbuffer = value;
}
static inline void write64(uint8_t **pbuffer, uint64_t value) {
	*(uint64_t*) *pbuffer = value;
}

static inline uint8_t  read8(uint8_t **pbuffer) {
	return *(uint8_t *) *pbuffer;
}
static inline uint16_t read16(uint8_t **pbuffer) {
	return *(uint16_t *) *pbuffer;
}
static inline uint32_t read32(uint8_t **pbuffer) {
	return *(int32_t *) *pbuffer;
}
static inline uint64_t read64(uint8_t **pbuffer) {
	return *(int64_t *) *pbuffer;
}

static inline int  nSizeBytes(int size) {
	return (size < 254) ? 1 : (size <= 0xFFFF) ? 3 : 5;
}
static inline void writeSize(uint8_t **pbuffer, int size) {
	if (size < 254) {
		write8(pbuffer, (uint8_t) size);
		advance(pbuffer, 1);
	} else if (size <= 0xFFFF) {
		write8(pbuffer, 0xFE);
		advance(pbuffer, 1);

		write16(pbuffer, (uint16_t) size);
		advance(pbuffer, 2);
	} else {
		write8(pbuffer, 0xFF);
		advance(pbuffer, 1);

		write32(pbuffer, (uint32_t) size);
		advance(pbuffer, 4);
	}
}
static inline int  readSize(uint8_t **pbuffer, size_t *premaining, uint32_t *psize) {
	if (*premaining < 1) return EBADMSG;

	*psize = read8(pbuffer);
	advance(pbuffer, 1, premaining);

	if (*psize == 254) {
		if (*premaining < 2) return EBADMSG;

		*psize = read16(pbuffer);
		advance(pbuffer, 2, premaining);
	} else if (*psize == 255) {
		if (*premaining < 4) return EBADMSG;
		
		*psize = read32(pbuffer);
		advance(pbuffer, 4, premaining);
	}

	return 0;
}
#endif
