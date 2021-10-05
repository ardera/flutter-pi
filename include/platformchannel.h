#ifndef _METHODCHANNEL_H
#define _METHODCHANNEL_H

#include <stdint.h>
#include <errno.h>
#include <flutter_embedder.h>

#define JSON_DECODE_TOKENLIST_SIZE  128


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
enum json_value_type {
    kJsonNull,
    kJsonTrue,
    kJsonFalse,
    kJsonNumber,
    kJsonString,
    kJsonArray, 
    kJsonObject
};
struct json_value {
    enum json_value_type type;
    union {
        double number_value;
        char  *string_value;
        struct {
            size_t size;
            union {
                struct json_value *array;
                struct {
                    char                    **keys;
                    struct json_value *values;
                };
            };
        };
    };
};

enum std_value_type {
    kStdNull = 0,
    kStdTrue,
    kStdFalse,
    kStdInt32,
    kStdInt64,
    kStdLargeInt, // treat as kString
    kStdFloat64,
    kStdString,
    kStdUInt8Array,
    kStdInt32Array,
    kStdInt64Array,
    kStdFloat64Array,
    kStdList,
    kStdMap
};
struct std_value {
    enum std_value_type type;
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
                struct std_value* list;
                struct {
                    struct std_value* keys;
                    struct std_value* values;
                };
            };
        };
    };
};

#define STDVALUE_IS_NULL(value) ((value).type == kStdNull)
#define STDNULL ((struct std_value) {.type = kStdNull})

#define STDVALUE_IS_BOOL(value) (((value).type == kStdTrue) || ((value).type == kStdFalse))
#define STDVALUE_AS_BOOL(value) ((value).type == kStdTrue)
#define STDBOOL(bool_value) ((struct std_value) {.type = (bool_value) ? kStdTrue : kStdFalse})

#define STDVALUE_IS_INT(value) (((value).type == kStdInt32) || ((value).type == kStdInt64))
#define STDVALUE_AS_INT(value) ((value).type == kStdInt32 ? (int64_t) (value).int32_value : (value).int64_value)
#define STDINT32(_int32_value) ((struct std_value) {.type = kStdInt32, .int32_value = (_int32_value)})
#define STDINT64(_int64_value) ((struct std_value) {.type = kStdInt64, .int64_value = (_int64_value)})

#define STDVALUE_IS_FLOAT(value) ((value).type == kStdFloat64)
#define STDVALUE_AS_FLOAT(value) ((value).float64_value)
#define STDFLOAT64(double_value) ((struct std_value) {.type = kStdFloat64, .float64_value = (double_value)})

#define STDVALUE_IS_NUM(value) (STDVALUE_IS_INT(value) || STDVALUE_IS_FLOAT(value))
#define STDVALUE_AS_NUM(value) (STDVALUE_IS_INT(value) ? STDVALUE_AS_INT(value) : STDVALUE_AS_FLOAT(value))

#define STDVALUE_IS_STRING(value) ((value).type == kStdString)
#define STDVALUE_AS_STRING(value) ((value).string_value)
#define STDSTRING(str) ((struct std_value) {.type = kStdString, .string_value = str})

#define STDVALUE_IS_LIST(value) ((value).type == kStdList)
#define STDVALUE_IS_SIZE(value, _size) ((value).size == (_size))
#define STDVALUE_IS_SIZED_LIST(value, _size) (STDVALUE_IS_LIST(value) && STDVALUE_IS_SIZE(value, _size))

#define STDVALUE_IS_INT_ARRAY(value) (((value).type == kStdInt32Array) || ((value).type == kStdInt64Array) || ((value).type == kStdUInt8Array))
#define STDVALUE_IS_FLOAT_ARRAY(value) ((value).type == kStdFloat64Array)
#define STDVALUE_IS_NUM_ARRAY(value) (STDVALUE_IS_INT_ARRAY(value) || STDVALUE_IS_FLOAT_ARRAY(value))

/// codec of an abstract channel object
/// These tell this API how it should encode ChannelObjects -> platform messages
/// and how to decode platform messages -> ChannelObjects.
enum platch_codec {
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

/// Platform Channel Object.
/// Different properties are "valid" for different codecs:
///   kNotImplemented:
///     no values associated with this "codec".
///     this represents a platform message with no buffer and zero length. (so just an empty response)
///   kStringCodec:
///     - string_value is the raw byte data of a platform message, but with an additional null-byte at the end.
///   kBinaryCodec:
///     - binarydata is an array of the raw byte data of a platform message,
///     - binarydata_size is the size of that byte data in bytes.
///   kJSONMessageCodec:
///     - json_value
///   kStdMsgCodecValue:
///     - std_value
///   kStandardMethodCall:
///     - "method" is the method you'd like to call, or the method that was called
///       by flutter.
///     - std_arg contains the argument to that method call.
///   kJSONMethodCall:
///     - "method" is the method you'd like to call, or the method that was called
///       by flutter.
///     - json_arg contains the argument to that method call.
///   kStandardMethodCallResponse or kJSONMethodCallResponse:
///     - "success" is whether the method call (called by flutter or by called by you)
///       if success is false,
///         - error_code must be set to point to a valid null-terminated string,
///         - error_msg is either pointing to a valid null-terminated string or is set to NULL,
///         - if the codec is kStandardMethodCallResponse,
///             std_error_details must be a valid std_value
///             ({.type = kStdNull} is possible, but not NULL).
///         - if the codec is kJSONMethodCallResponse,
///             json_error_details must be a valid json_value
///             ({.type = kJsonNull} is possible, but not NULL)
struct platch_obj {
    enum platch_codec codec;
    union {
        char                    *string_value;
        struct {
            size_t   binarydata_size;
            uint8_t *binarydata;
        };
        struct json_value json_value;
        struct std_value  std_value;
        struct {
            char *method;
            union {
                struct std_value  std_arg;
                struct json_value json_arg;
            };
        };
        struct {
            bool success;
            union {
                struct std_value  std_result;
                struct json_value json_result;
            };
            char *error_code;
            char *error_msg;
            union {
                struct std_value std_error_details;
                struct json_value json_error_details;
            };
        };
    };
};

/// A Callback that is called when a response to a platform message you send to flutter
/// arrives. "object" is the platform message decoded using the "codec" you gave to PlatformChannel_send,
/// "userdata" is the userdata you gave to PlatformChannel_send.
typedef int (*platch_msg_resp_callback)(struct platch_obj *object, void *userdata);


/// decodes a platform message (represented by `buffer` and `size`) as the given codec,
/// and puts the result into object_out.
/// This method will (in some cases) dynamically allocate memory,
/// so you should always call PlatformChannel_free(object_out) when you don't need it anymore.
/// 
/// Additionally, PlatformChannel_decode currently "borrows" from the buffer, so if the buffer
/// is freed by flutter, the contents of object_out will in many cases be bogus.
/// If you'd like object_out to be persistent and not depend on the lifetime of the buffer,
/// you'd have to manually deep-copy it.
int platch_decode(uint8_t *buffer, size_t size, enum platch_codec codec, struct platch_obj *object_out);

/// Encodes a generic ChannelObject into a buffer (that is, too, allocated by PlatformChannel_encode)
/// A pointer to the buffer is put into buffer_out and the size of that buffer into size_out.
/// The lifetime of the buffer is independent of the ChannelObject, so contents of the ChannelObject
///   can be freed after the object was encoded.
int platch_encode(struct platch_obj *object, uint8_t **buffer_out, size_t *size_out);

/// Encodes a generic ChannelObject (anything, string/binary codec or Standard/JSON Method Calls and responses) as a platform message
/// and sends it to flutter on channel `channel`
/// If you supply a response callback (i.e. on_response is != NULL):
///   when flutter responds to this message, it is automatically decoded using the codec given in `response_codec`.
///   Then, on_response is called with the decoded ChannelObject and the userdata as an argument.
///   It's possible that flutter won't respond to your platform message, like when sending events via an EventChannel.
/// userdata can be NULL.
int platch_send(char *channel,
                struct platch_obj *object,
                enum platch_codec response_codec,
                platch_msg_resp_callback on_response,
                void *userdata);

/// Encodes a StandardMethodCodec method call as a platform message and sends it to flutter
/// on channel `channel`. This is just a wrapper around PlatformChannel_send
/// that builds the ChannelObject for you.
/// The response_codec is kStandardMethodCallResponse. userdata can be NULL.
int platch_call_std(char *channel,
                    char *method,
                    struct std_value *argument,
                    platch_msg_resp_callback on_response,
                    void *userdata);

/// Encodes the arguments as a JSON method call and sends it to flutter
/// on channel [channel]. This is just a wrapper around platch_send
/// that builds the ChannelObject for you.
/// The response is automatically decoded as a JSON method call response and
/// supplied to [on_response] as an argument. userdata can be NULL.
int platch_call_json(char *channel,
                     char *method,
                     struct json_value *argument,
                     platch_msg_resp_callback on_response,
                     void *userdata);

/// Responds to a platform message. You can (of course) only respond once to a platform message,
/// i.e. a FlutterPlatformMessageResponseHandle can only be used once.
/// The codec of `response` can be any of the available codecs.
int platch_respond(FlutterPlatformMessageResponseHandle *handle,
                   struct platch_obj *response);

/// Tells flutter that the platform message that was sent to you was not handled.
///   (for example, there's no plugin that is using this channel, or there is a plugin
///   but it doesn't want to respond.)
/// You should always use this instead of not replying to a platform message, since not replying could cause a memory leak.
/// When flutter receives this response, it will throw a MissingPluginException.
///   For most channel used by the ServicesPlugin, this is not too bad since it
///   specifies many of the channels used as OptionalMethodChannels. (which will silently catch the MissingPluginException)
int platch_respond_not_implemented(FlutterPlatformMessageResponseHandle *handle);

int platch_respond_success_std(FlutterPlatformMessageResponseHandle *handle,
							   struct std_value *return_value);

int platch_respond_error_std(FlutterPlatformMessageResponseHandle *handle,
							 char *error_code,
							 char *error_msg,
							 struct std_value *error_details);

int platch_respond_illegal_arg_std(FlutterPlatformMessageResponseHandle *handle,
								   char *error_msg);

int platch_respond_native_error_std(FlutterPlatformMessageResponseHandle *handle,
									int _errno);


int platch_respond_success_json(FlutterPlatformMessageResponseHandle *handle,
								struct json_value *return_value);

int platch_respond_error_json(FlutterPlatformMessageResponseHandle *handle,
							  char *error_code,
							  char *error_msg,
							  struct json_value *error_details);

int platch_respond_illegal_arg_json(FlutterPlatformMessageResponseHandle *handle,
                                    char *error_msg);

int platch_respond_native_error_json(FlutterPlatformMessageResponseHandle *handle,
                                     int _errno);

int platch_respond_success_pigeon(
	FlutterPlatformMessageResponseHandle *handle,
	struct std_value *return_value
);

int platch_respond_error_pigeon(
	FlutterPlatformMessageResponseHandle *handle,
	char *error_code,
	char *error_msg,
	struct std_value *error_details
);

int platch_respond_illegal_arg_pigeon(
	FlutterPlatformMessageResponseHandle *handle,
	char *error_msg
);

int platch_respond_native_error_pigeon(
	FlutterPlatformMessageResponseHandle *handle,
	int _errno
);

/// Sends a success event with value `event_value` to an event channel
/// that uses the standard method codec.                                 
int platch_send_success_event_std(char *channel,
                                  struct std_value *event_value);

/// Sends an error event to an event channel that uses the standard method codec.
int platch_send_error_event_std(char *channel,
							 	char *error_code,
							 	char *error_msg,
							 	struct std_value *error_details);
/// Sends a success event with value `event_value` to an event channel
/// that uses the JSON method codec.
int platch_send_success_event_json(char *channel,
                                   struct json_value *event_value);

/// Sends an error event to an event channel that uses the JSON method codec.
int platch_send_error_event_json(char *channel,
								 char *error_code,
								 char *error_msg,
								 struct json_value *error_details);

/// frees a ChannelObject that was decoded using PlatformChannel_decode.
/// not freeing ChannelObjects may result in a memory leak.
int platch_free_obj(struct platch_obj *object);

int platch_free_json_value(struct json_value *value, bool shallow);

/// returns true if values a and b are equal.
/// for JS arrays, the order of the values is relevant
/// (so two arrays are only equal if the same values appear in exactly same order)
/// for objects, the order of the entries is irrelevant.
bool jsvalue_equals(struct json_value *a, struct json_value *b);

/// given a JS object as an argument, it searches for an entry with key "key"
/// and returns the value associated with it.
/// if the key is not found, returns NULL.
struct json_value *jsobject_get(struct json_value *object, char *key);

/// StdMsgCodecValue equivalent of jsvalue_equals.
/// again, for lists, the order of values is relevant
/// for maps, it's not.
bool stdvalue_equals(struct std_value *a, struct std_value *b);

/// StdMsgCodecValue equivalent of jsobject_get, just that the key can be
/// any arbitrary StdMsgCodecValue (and must not be a string as for jsobject_get)
struct std_value *stdmap_get(struct std_value *map, struct std_value *key);

struct std_value *stdmap_get_str(struct std_value *map, char *key);

static inline int _advance(uintptr_t *value, int n_bytes, size_t *remaining) {
    if (remaining != NULL) {
        if (*remaining < n_bytes) return EBADMSG;
        *remaining -= n_bytes;
    }

    *value += n_bytes;
    return 0;
}
static inline int _align(uintptr_t *value, int alignment, size_t *remaining) {
    int diff;

    alignment--;
	diff = ((((*value) + alignment) | alignment) - alignment) - *value;

    return _advance(value, diff, remaining);
}
static inline int _advance_size_bytes(uintptr_t *value, size_t size, size_t *remaining) {
    if (size < 254) {
		return _advance(value, 1, remaining);
	} else if (size <= 0xFFFF) {
		return _advance(value, 3, remaining);
	} else {
		return _advance(value, 5, remaining);
    }
}


static inline int _write8(uint8_t **pbuffer, uint8_t value, size_t *remaining) {
    if ((remaining != NULL) && (*remaining < 1)) {
        return EBADMSG;
    }

	*(uint8_t*) *pbuffer = value;
    
    return _advance((uintptr_t*) pbuffer, 1, remaining);
}
static inline int _write16(uint8_t **pbuffer, uint16_t value, size_t *remaining) {
    if ((remaining != NULL) && (*remaining < 2)) {
        return EBADMSG;
    }

	*(uint16_t*) *pbuffer = value;
    
    return _advance((uintptr_t*) pbuffer, 2, remaining);
}
static inline int _write32(uint8_t **pbuffer, uint32_t value, size_t *remaining) {
    if ((remaining != NULL) && (*remaining < 4)) {
        return EBADMSG;
    }

	*(uint32_t*) *pbuffer = value;
    
    return _advance((uintptr_t*) pbuffer, 4, remaining);
}
static inline int _write64(uint8_t **pbuffer, uint64_t value, size_t *remaining) {
	if ((remaining != NULL) && (*remaining < 8)) {
        return EBADMSG;
    }
    
    *(uint64_t*) *pbuffer = value;
    
    return _advance((uintptr_t*) pbuffer, 8, remaining);
}

static inline int _read8(uint8_t **pbuffer, uint8_t* value_out, size_t *remaining) {
	if ((remaining != NULL) && (*remaining < 1)) {
        return EBADMSG;
    }

    *value_out = *(uint8_t *) *pbuffer;

    return _advance((uintptr_t*) pbuffer, 1, remaining);
}
static inline int _read16(uint8_t **pbuffer, uint16_t *value_out, size_t *remaining) {
    if ((remaining != NULL) && (*remaining < 2)) {
        return EBADMSG;
    }

    *value_out = *(uint16_t *) *pbuffer;
	
    return _advance((uintptr_t*) pbuffer, 2, remaining);
}
static inline int _read32(uint8_t **pbuffer, uint32_t *value_out, size_t *remaining) {
	if ((remaining != NULL) && (*remaining < 4)) {
        return EBADMSG;
    }
    
    *value_out = *(uint32_t *) *pbuffer;

    return _advance((uintptr_t*) pbuffer, 4, remaining);
}
static inline int _read64(uint8_t **pbuffer, uint64_t *value_out, size_t *remaining) {
	if ((remaining != NULL) && (*remaining < 8)) {
        return EBADMSG;
    }
    
    *value_out = *(uint64_t *) *pbuffer;

    return _advance((uintptr_t*) pbuffer, 8, remaining);
}

static inline int _writeSize(uint8_t **pbuffer, int size, size_t *remaining) {
	int ok;

    if (size < 254) {
		return _write8(pbuffer, (uint8_t) size, remaining);
	} else if (size <= 0xFFFF) {
		ok = _write8(pbuffer, 0xFE, remaining);
        if (ok != 0) return ok;

		ok = _write16(pbuffer, (uint16_t) size, remaining);
        if (ok != 0) return ok;
	} else {
		ok = _write8(pbuffer, 0xFF, remaining);
        if (ok != 0) return ok;

		ok = _write32(pbuffer, (uint32_t) size, remaining);
        if (ok != 0) return ok;
    }

    return ok;
}
static inline int  _readSize(uint8_t **pbuffer, uint32_t *psize, size_t *remaining) {
	int ok;
    uint8_t size8;
    uint16_t size16;

	ok = _read8(pbuffer, &size8, remaining);
    if (ok != 0) return ok;
    
    if (size8 <= 253) {
        *psize = size8;

        return 0;
    } else if (size8 == 254) {
		ok = _read16(pbuffer, &size16, remaining);
        if (ok != 0) return ok;

        *psize = size16;
        return 0;
	} else if (size8 == 255) {
		return _read32(pbuffer, psize, remaining);
	}

    return 0;
}

#endif