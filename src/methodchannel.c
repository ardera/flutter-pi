#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <flutter_embedder.h>
#include "methodchannel.h"
#include "flutter-pi.h"

// only 32bit support for now.
#define __ALIGN4_REMAINING(value, remaining) __align((uint32_t*) (value), 4, remaining)
#define __ALIGN8_REMAINING(value, remaining) __align((uint32_t*) (value), 8, remaining)
#define align4(...) _ALIGN4_REMAINING(__VA_ARGS__, NULL)
#define align8(...) _ALIGN8_REMAINING(__VA_ARGS__, NULL)

#define alignmentDiff(value, alignment) __alignmentDiff((uint64_t) value, alignment)

#define __ADVANCE_REMAINING(value, n, remaining) __advance((uint32_t*) (value), n, remaining)
#define advance(...) _ADVANCE_REMAINING(__VA_ARGS__, NULL)

#define ASSERT_RETURN_FALSE(cond, err) if (!(cond)) {fprintf(stderr, "%s\n", err); return false;}

inline int __alignmentDiff(uint64_t value, int alignment) {
	alignment--;
	return value - (((((uint64_t) value) + alignment) | alignment) - alignment);
}
inline void __align(uint32_t *value, int alignment, size_t *remaining) {
	if (remaining != NULL)
		remaining -= alignmentDiff((uint64_t) *value, alignment);	
	alignment--;

	*value = (uint32_t) ((((*value + alignment) | alignment) - alignment);
}
inline void __advance(uint32_t *value, int n_bytes) {
	*value += n_bytes;
}

inline void write8(uint8_t **pbuffer, uint8_t value) {
	*(uint8_t*) *pbuffer = value;
}
inline uint8_t read8(uint8_t **pbuffer) {
	return *(uint8_t *) *pbuffer;
}
inline void write16(uint8_t **pbuffer, uint16_t value) {
	*(uint16_t*) *pbuffer = value;
}
inline uint16_t read16(uint8_t **pbuffer) {
	return *(uint16_t *) *pbuffer;
}
inline void write32(uint8_t **pbuffer, uint32_t value) {
	*(uint32_t*) *pbuffer = value;
}
inline uint32_t read32(uint8_t **pbuffer) {
	return *(int32_t *) *pbuffer;
}
inline void write64(uint8_t **pbuffer, uint64_t value) {
	*(uint64_t*) *pbuffer = value;
}
inline uint64_t read64(uint8_t **pbuffer) {
	return *(int64_t *) *pbuffer;
}

inline int  nSizeBytes(int size) {
	return (size < 254) ? 1 : (size <= 0xFFFF) ? 3 : 5;
}
inline void writeSize(uint8_t **pbuffer, int size) {
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
inline bool readSize(uint8_t** pbuffer, size_t* remaining, uint32_t* size) {
	ASSERT_RETURN_FALSE(*remaining >= 1, "Error decoding platform message: while decoding size: message ended too soon")
	*size = read8(pbuffer);
	advance(pbuffer, 1, remaining);

	if (*size == 254) {
		ASSERT_RETURN_FALSE(*remaining >= 2, "Error decoding platform message: while decoding size: message ended too soon")

		*size = read16(pbuffer);
		advance(pbuffer, 2, remaining);
	} else if (*size == 255) {
		ASSERT_RETURN_BOOL(*remaining >= 4, "Error decoding platform message: while decoding size: message ended too soon")
		
		*size = read32(pbuffer);
		advance(pbuffer, 4, remaining);
	}

	return true;
}

bool MessageChannel_writeValueToBuffer(struct MessageChannelValue* value, uint8_t** pbuffer) {
	write8(pbuffer, value->type);
	advance(pbuffer, 1);

	size_t size; uint8_t* byteArray;
	switch (value->type) {
		case kNull:
		case kTrue:
		case kFalse:
			break;
		case kInt32:
			write32(pbuffer, value->int_value);
			advance(pbuffer, 4);
			break;
		case kInt64:
			write64(pbuffer, value->long_value);
			advance(pbuffer, 8);
			break;
		case kFloat64:
			align8(pbuffer);
			write64(pbuffer, (uint64_t) value->double_value);
			advance(pbuffer, 8);
			break;
		case kLargeInt:
		case kString:
		case kUInt8Array:
			if ((value->type == kLargeInt) || (value->type == kString)) {
				size = strlen(value->string_value);
				byteArray = (uint8_t*) value->string_value;
			} else if (value->type == kUInt8Array) {
				size = value->bytearray_value.size;
				byteArray = (uint8_t*) value->bytearray_value.array;
			}

			writeSize(size, pbuffer);
			for (int i=0; i<size; i++) {
				write8(pbuffer, byteArray[i]);
				advance(pbuffer, 1);
			}
			break;
		case kInt32Array:
			size = value->intarray_value.size;

			writeSize(pbuffer, size);
			align(pbuffer, 4);
			
			for (int i=0; i<size; i++) {
				write32(pbuffer, value->intarray_value.array[i]);
				advance(pbuffer, 4);
			}
			break;
		case kInt64Array:
		case kFloat64Array:
			size = value->longarray_value.size;

			writeSize(pbuffer, size);
			align(pbuffer, 8);
			for (int i=0; i<size; i++) {
				write64(pbuffer, value->longarray_value.array[i]);
				advance(pbuffer, 8);
			}
			break;
		/*case kFloat64Array:
			size = value->doublearray_value.size;

			writeSize(pbuffer, size);
			align(pbuffer, 8);

			for (int i=0; i<size; i++) {
				write64(pbuffer, value->doublearray_value.array[i]);
				advance(pbuffer, 8)
			}
			break;*/
		case kList:
			size = value->list_value.size;

			writeSize(pbuffer, size);
			for (int i=0; i<size; i++)
				if (!MethodChannel_writeValueToBuffer(&(value->list_value.list[i]), pbuffer)) return false;
			
			break;
		case kMap:
			size = value->map_value.size;

			writeSize(pbuffer, size);
			for (int i=0; i<size; i++) {
				if (!MethodChannel_writeValueToBuffer(&(value->map_value.map[i*2  ]), pbuffer)) return false;
				if (!MethodChannel_writeValueToBuffer(&(value->map_value.map[i*2+1]), pbuffer)) return false;
			}
			break;
		default:
			fprintf(stderr, "Error encoding Message Codec Value: Unsupported Value type: %d\n", value->type);
			return false;
	}

	return true;
}


bool MethodChannel_call(char* channel, char* method, struct MessageChannelValue* argument) {
	uint8_t *buffer, *buffer_cursor;
	size_t   buffer_size = 0;

	// the method name is encoded as a String value and is the first value written to the buffer.
	struct MessageChannelValue method_name_value = {
		.type = kTypeString,
		.string_value = method
	};

	// calculate buffer size
	if (!MessageChannel_calculateValueSizeInBuffer(&method_name_value,	&buffer_size)) return false;
	if (!MessageChannel_calculateValueSizeInBuffer(argument, 			&buffer_size)) return false;

	// allocate buffer
	buffer = (uint8_t*) malloc(buffer_size);
	buffer_cursor = buffer;

	// write buffer
	if (!MessageChannel_writeValueToBuffer(&method_name_value,	&buffer_cursor)) return false;
	if (!MessageChannel_writeValueToBuffer(argument,			&buffer_cursor)) return false;

	// send message buffer to flutter engine
	FlutterEngineResult result = FlutterEngineSendPlatformMessage(
		engine,
		& (const FlutterPlatformMessage) {
			.struct_size = sizeof(FlutterPlatformMessage),
			.channel = (const char*) channel,
			.message = (const uint8_t*) buffer,
			.message_size = (const size_t) buffer_size
		}
	);

	free(buffer);
	return result == kSuccess;
}
bool MethodChannel_respond(FlutterPlatformMessageResponseHandle* response_handle, struct MessageChannelValue* response_value) {
	uint8_t *buffer, *buffer_cursor;
	size_t   buffer_size;

	// calculate buffer size
	if (!MethodChannel_calculateValueSizeInBuffer(response_value, &buffer_size)) return false;
	
	// allocate buffer
	buffer_cursor = buffer = (uint8_t*) malloc(buffer_size);

	// write buffer
	if (!MethodChannel_writeValueToBuffer(response_value, &buffer_cursor)) return false;

	// send message buffer to flutter engine
	FlutterEngineResult result = FlutterEngineSendPlatformMessageResponse(engine, response_handle, buffer, buffer_size);
	
	free(buffer);
	return result == kSuccess;
}

bool MessageChannel_decodeValue(uint8_t** pbuffer, size_t* buffer_remaining, struct MessageChannelValue* value) {
	ASSERT_RETURN_FALSE(*buffer_remaining >= 1, "Error decoding platform message: while decoding value type: message ended to soon")
	MessageValueDiscriminator type = **pbuffer;
	advance(pbuffer, 1, buffer_remaining);

	value->type = type;

	size_t size = 0; char* c_string = 0; uint8_t* byteArray = 0; int32_t* intArray = 0; int64_t* longArray = 0;
	switch (type) {
		case kNull:
		case kTrue:
		case kFalse:
			break;
		case kInt32:
			ASSERT_RETURN_FALSE(*buffer_remaining >= 4, "Error decoding platform message: while decoding kTypeInt: message ended to soon")

			value->int_value = (int32_t) read32(pbuffer);
			advance(pbuffer, 4, buffer_remaining);

			break;
		case kInt64:
			ASSERT_RETURN_FALSE(*buffer_remaining >= 8, "Error decoding platform message: while decoding kTypeLong: message ended too soon")

			value->long_value = (int64_t) read64(pbuffer);
			advance(pbuffer, 8, buffer_remaining);

			break;
		case kFloat64:
			ASSERT_RETURN_FALSE(*buffer_remaining >= (8 + alignmentDiff(*pbuffer, 8)), "Error decoding platform message: while decoding kTypeDouble: message ended too soon")
			
			align(pbuffer, 8, buffer_remaining);
			value->double_value = (double) read64(pbuffer);
			advance(pbuffer, 8, buffer_remaining);

			break;
		case kLargeInt:
		case kString:
			if (!readSize(pbuffer, buffer_remaining, &size)) return false;

			ASSERT_RETURN_FALSE(*buffer_remaining >= size, "Error decoding platform message: while decoding kTypeString: message ended too soon")
			char* c_string = calloc(size+1, sizeof(char));

			for (int i = 0; i < size; i++) {
				c_string[i] = read8(pbuffer);
				advance(pbuffer, 1, buffer_remaining);
			}
			value->string_value = c_string;

			break;
		case kUInt8Array:
			if (!readSize(pbuffer, buffer_remaining, &size)) return false;

			ASSERT_RETURN_FALSE(*buffer_remaining >= size, "Error decoding platform message: while decoding kTypeByteArray: message ended too soon")
			value->bytearray_value.size = size;
			value->bytearray_value.array = *pbuffer;
			align(pbuffer, size, buffer_remaining);

			break;
		case kInt32Array:
			if (!readSize(pbuffer, buffer_remaining, &size)) return false;

			ASSERT_RETURN_FALSE(*buffer_remaining >= size*4 + alignmentDiff(*pbuffer, 4), "Error decoding platform message: while decoding kTypeIntArray: message ended too soon")
			align(pbuffer, 4, buffer_remaining);

			value->intarray_value.size = size;
			value->intarray_value.array = (int32_t*) *pbuffer;

			advance(pbuffer, size*4, buffer_remaining);

			break;
		case kInt64Array:
			if (!readSize(pbuffer, buffer_remaining, &size)) return false;

			ASSERT_RETURN_FALSE(*buffer_remaining >= size*8 + alignmentDiff(*pbuffer, 8), "Error decoding platform message: while decoding kTypeLongArray: message ended too soon")
			align(pbuffer, 8, buffer_remaining);

			value->longarray_value.size = size;
			value->longarray_value.array = (int64_t*) *pbuffer;

			advance(pbuffer, size*8, buffer_remaining);

			break;
		case kFloat64Array:
			if (!readSize(pbuffer, buffer_remaining, &size)) return false;

			ASSERT_RETURN_FALSE(*buffer_remaining >= size*8 + alignmentDiff(*pbuffer, 8), "Error decoding platform message: while decoding kTypeIntArray: message ended too soon")
			align(pbuffer, 8, buffer_remaining);

			value->doublearray_value.size = size;
			value->doublearray_value.array = (double*) *pbuffer;

			advance(pbuffer, size*8, buffer_remaining);

			break;
		case kList:
			if (!readSize(pbuffer, buffer_remaining, &size)) return false;

			value->list_value.size = size;
			value->list_value.list = calloc(size, sizeof(struct MessageChannelValue));

			for (int i = 0; i < size; i++) {
				if (!MethodChannel_decodeValue(pbuffer, buffer_remaining, &(value->list_value.list[i]))) return false;
			}

			break;
		case kMap:
			if (!readSize(pbuffer, buffer_remaining, &size)) return false;

			value->map_value.size = size;
			value->map_value.map = calloc(size*2, sizeof(struct MessageChannelValue));

			for (int i = 0; i < size; i++) {
				if (!MethodChannel_decodeValue(pbuffer, buffer_remaining, &(value->list_value.list[i*2  ]))) return false;
				if (!MethodChannel_decodeValue(pbuffer, buffer_remaining, &(value->list_value.list[i*2+1]))) return false;
			}

			break;
		default:
			fprintf(stderr, "Error decoding platform message: unknown value type: 0x%02X\n", type);
			return false;
	}

	return true;
}
bool MessageChannel_freeValue(struct MessageChannelValue* p_value) {
	switch (p_value->type) {
		case kTypeString:
			free(p_value->string_value);
			break;
		case kTypeList:
			for (int i=0; i < p_value->list_value.size; i++)
				if (!MethodChannel_freeValue(&(p_value->list_value.list[i]))) return false;
			
			free(p_value->list_value.list);
			break;
		case kTypeMap:
			for (int i=0; i< p_value->map_value.size; i++) {
				if (!MethodChannel_freeValue(&(p_value->map_value.map[i*2  ]))) return false;
				if (!MethodChannel_freeValue(&(p_value->map_value.map[i*2+1]))) return false;
			}

			free(p_value->map_value.map);
		default:
			break;
	}

	return true;
}

bool MethodChannel_decode(size_t buffer_size, uint8_t* buffer, struct MethodCall** presult) {
	*presult = malloc(sizeof(struct MethodCall));
	struct MethodCall* result = *presult;
	
	uint8_t* buffer_cursor = buffer;
	size_t  buffer_remaining = buffer_size;
	
	if (*buffer == (char) 123) {
		result->protocol = kJSONProtocol;
		fprintf(stderr, "Error decoding Method Call: JSON Protocol not supported yet.\n");
		return false;
	} else {
		result->protocol = kStandardProtocol;
	}
	
	struct MessageChannelValue method_name;
	if (!MessageChannel_decodeValue(&buffer_cursor, &buffer_remaining, &method_name)) return false;
	if (method_name.type != kString) {
		fprintf(stderr, "Error decoding Method Call: expected type of first value in buffer to be string (i.e. method name), got %d\n", method_name.type);
		return false;
	}
	result->method = method_name.string_value;

	if (!MessageChannel_decodeValue(&buffer_cursor, &buffer_remaining, &(result->argument))) return false;

	return true;
}
bool MethodChannel_freeMethodCall(struct MethodCall **pmethodcall) {
	struct MethodCall* methodcall = *pmethodcall;

	free(methodcall->method);
	if (!MessageChannel_freeValue(&(methodcall->argument))) return false;
	free(methodcall);

	*pmethodcall = NULL;

	return true;
}


#undef __ALIGN4_REMAINING
#undef __ALIGN8_REMAINING
#undef align4
#undef align8
#undef alignmentDiff
#undef __ADVANCE_REMAINING
#undef advance
#undef ASSERT_RETURN_FALSE