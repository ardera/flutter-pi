#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <flutter_embedder.h>
#include "methodchannel.h"
#include "flutter-pi.h"

#define _NEXTN(buffer,n) ((buffer) = &((buffer)[n]));
#define _NEXTN_REMAINING(buffer,remaining,n) do {_NEXTN(buffer,n) (remaining)-=(n);} while (false);
#define _NEXT(buffer) _NEXTN(buffer, 1)
#define _NEXT_REMAINING(buffer,remaining) _NEXTN_REMAINING(buffer,remaining,1)

#define _GET_MACRO(_1,_2,_3,NAME,...) NAME
#define NEXT(...) _GET_MACRO(__VA_ARGS__, _NEXT_REMAINING, _NEXT_REMAINING, _NEXT)(__VA_ARGS__)
#define NEXTN(...) _GET_MACRO(__VA_ARGS__, _NEXTN_REMAINING, _NEXTN)(__VA_ARGS__)

#define ALIGNMENT_DIFF(buffer, n) (((uint64_t) (buffer) - (((uint64_t) (buffer) + (n)-1) | (n)-1) - (n)-1))
#define _ALIGN(buffer, n) do {(buffer) = (uint8_t*) ((((uint64_t) (buffer) + (n)-1) | (n)-1) - (n-1));} while (false);
#define _ALIGN_REMAINING(buffer, remaining, n) do {(buffer) = (uint8_t*) ((((uint64_t) (buffer) + (n)-1) | (n)-1) - (n-1));} while (false);
#define ALIGN(...) _GET_MACRO(__VA_ARGS__, _ALIGN_REMAINING, _ALIGN)(__VA_ARGS__)


bool MethodChannel_calculateValueSizeInBuffer(struct MethodChannelValue* value, size_t* p_buffer_size) {
	MessageValueDiscriminator type = value->type;

	// Type Byte
	*p_buffer_size += 1;

	size_t size;
	switch (type) {
		case kNull:
		case kTrue:
		case kFalse:
			break;
		case kTypeInt:
			*p_buffer_size += 4;
			break;
		case kTypeLong:
			*p_buffer_size += 8;
			break;
		case kTypeDouble:
			*p_buffer_size = (((*p_buffer_size) + 7) | 7) - 7;	// 8-byte aligned
			*p_buffer_size += 8;

			break;
		case kTypeString:
			size = strlen(value->value.string_value);

			*p_buffer_size += (size < 254) ? 1 : (size <= 0xFFFF) ? 3 : 5;	// write array size
			*p_buffer_size += size;

			break;
		case kTypeByteArray:
			size = value->value.bytearray_value.size;

			*p_buffer_size += (size < 254) ? 1 : (size <= 0xFFFF) ? 3 : 5;	// write array size
			*p_buffer_size += size;

			break;
		case kTypeIntArray:
			size = value->value.intarray_value.size;

			*p_buffer_size += (size < 254) ? 1 : (size <= 0xFFFF) ? 3 : 5;	// write array size
			*p_buffer_size = (((*p_buffer_size) + 3) | 3) - 3;				// 4-byte aligned
			*p_buffer_size += size*4;

			break;
		case kTypeLongArray:
			size = value->value.longarray_value.size;
			
			*p_buffer_size += (size < 254) ? 1 : (size <= 0xFFFF) ? 3 : 5;	// write array size
			*p_buffer_size = (((*p_buffer_size) + 7) | 7) - 7;				// 8-byte aligned
			*p_buffer_size += size*8;

			break;
		case kTypeDoubleArray:
			size = value->value.doublearray_value.size;
			
			*p_buffer_size += (size < 254) ? 1 : (size <= 0xFFFF) ? 3 : 5;	// write array size
			*p_buffer_size = (((*p_buffer_size) + 7) | 7) - 7;				// 8-byte aligned
			*p_buffer_size += size*8;

			break;
		case kTypeList:
			size = value->value.list_value.size;

			*p_buffer_size += (size < 254) ? 1 : (size <= 0xFFFF) ? 3 : 5;	// write list size
			for (int i = 0; i<size; i++)
				if (!MethodChannel_calculateValueSizeInBuffer(&(value->value.list_value.list[i]), p_buffer_size))    return false;
			
			break;
		case kTypeMap:
			size = value->value.map_value.size;

			*p_buffer_size += (size < 254) ? 1 : (size <= 0xFFFF) ? 3 : 5;	// write map size
			for (int i = 0; i<size; i++) {
				if (!MethodChannel_calculateValueSizeInBuffer(&(value->value.list_value.list[i*2  ]), p_buffer_size)) return false;
				if (!MethodChannel_calculateValueSizeInBuffer(&(value->value.list_value.list[i*2+1]), p_buffer_size)) return false;
			}

			break;
		default:
			fprintf(stderr, "Error calculating Message Codec Value size: Unsupported Value type: %d\n", type);
			return false;
	}

	return true;
}

bool MethodChannel_writeSizeValueToBuffer(size_t size, uint8_t** p_buffer) {
	if (size < 254) {
		**p_buffer = size;
		NEXT(*p_buffer)
	} else if (size <= 0xFFFF) {
		**p_buffer = 254;
		NEXT(*p_buffer)
		*(uint16_t*) *p_buffer = size;
		NEXTN(*p_buffer, 2)
	} else {
		**p_buffer = 255;
		NEXT(*p_buffer)
		*(uint32_t*) *p_buffer = size;
		NEXTN(*p_buffer, 4)
	}

	return true;
}
bool MethodChannel_alignBuffer(unsigned int alignment, uint8_t** p_buffer) {
	alignment--;
	*p_buffer = (uint8_t*) ((((uint64_t) *p_buffer + alignment) | alignment) - alignment);
	return true;
}
bool MethodChannel_writeValueToBuffer(struct MethodChannelValue* value, uint8_t** p_buffer) {
	**p_buffer = (uint8_t) value->type;
	NEXT(*p_buffer)

	size_t size; uint8_t* byteArray;
	switch (value->type) {
		case kNull:
		case kTrue:
		case kFalse:
			break;
		case kTypeInt:
			*(int32_t*) *p_buffer = value->value.int_value;
			NEXTN(*p_buffer, 4)
			break;
		case kTypeLong:
			*(int64_t*) *p_buffer = value->value.long_value;
			NEXTN(*p_buffer, 8)
			break;
		case kTypeDouble:
			MethodChannel_alignBuffer(8, p_buffer);
			
			*(double*) *p_buffer = value->value.double_value;
			NEXTN(*p_buffer, 8)
			break;
		case kTypeBigInt:
		case kTypeString:
		case kTypeByteArray:
			if (value->type == kTypeBigInt) {
				size = strlen(value->value.bigint_value);
				byteArray = (uint8_t*) value->value.bigint_value;
			} else if (value->type == kTypeString) {
				size = strlen(value->value.string_value);
				byteArray = (uint8_t*) value->value.string_value;
			} else if (value->type == kTypeByteArray) {
				size = value->value.bytearray_value.size;
				byteArray = (uint8_t*) value->value.bytearray_value.array;
			}

			MethodChannel_writeSizeValueToBuffer(size, p_buffer);
			for (int i=0; i<size; i++) {
				**p_buffer = byteArray[i];
				NEXT(*p_buffer)
			}
			break;
		case kTypeIntArray:
			size = value->value.intarray_value.size;

			MethodChannel_writeSizeValueToBuffer(size, p_buffer);
			MethodChannel_alignBuffer(4, p_buffer);
			
			for (int i=0; i<size; i++) {
				*(int32_t*) *p_buffer = value->value.intarray_value.array[i];
				NEXTN(*p_buffer, 4)
			}
			break;
		case kTypeLongArray:
			size = value->value.longarray_value.size;

			MethodChannel_writeSizeValueToBuffer(size, p_buffer);
			MethodChannel_alignBuffer(8, p_buffer);

			for (int i=0; i<size; i++) {
				*(int64_t*) *p_buffer = value->value.longarray_value.array[i];
				NEXTN(*p_buffer, 8)
			}
			break;
		case kTypeDoubleArray:
			size = value->value.doublearray_value.size;

			MethodChannel_writeSizeValueToBuffer(size, p_buffer);
			MethodChannel_alignBuffer(8, p_buffer);

			for (int i=0; i<size; i++) {
				*(double*) *p_buffer = value->value.doublearray_value.array[i];
				NEXTN(*p_buffer, 8)
			}
			break;
		case kTypeList:
			size = value->value.list_value.size;

			MethodChannel_writeSizeValueToBuffer(size, p_buffer);

			for (int i=0; i<size; i++)
				if (!MethodChannel_writeValueToBuffer(&(value->value.list_value.list[i]), p_buffer)) return false;
			
			break;
		case kTypeMap:
			size = value->value.map_value.size;

			MethodChannel_writeSizeValueToBuffer(size, p_buffer);

			for (int i=0; i<size; i++) {
				if (!MethodChannel_writeValueToBuffer(&(value->value.map_value.map[i*2  ]), p_buffer)) return false;
				if (!MethodChannel_writeValueToBuffer(&(value->value.map_value.map[i*2+1]), p_buffer)) return false;
			}
			break;
		default:
			fprintf(stderr, "Error encoding Message Codec Value: Unsupported Value type: %d\n", value->type);
			return false;
	}

	return true;
}


bool MethodChannel_call(char* channel, char* method, struct MethodChannelValue* argument) {
	uint8_t *buffer, *buffer_cursor;
	size_t   buffer_size = 0;

	// the method name is encoded as a String value and is the first value written to the buffer.
	struct MethodChannelValue method_name_value = {
		.type = kTypeString,
		.value = {
			.string_value = method
		}
	};

	// calculate buffer size
	if (!MethodChannel_calculateValueSizeInBuffer(&method_name_value,	&buffer_size)) return false;
	if (!MethodChannel_calculateValueSizeInBuffer(argument, 			&buffer_size)) return false;

	// allocate buffer
	buffer = (uint8_t*) malloc(buffer_size);
	buffer_cursor = buffer;

	// write buffer
	if (!MethodChannel_writeValueToBuffer(&method_name_value,	&buffer_cursor)) return false;
	if (!MethodChannel_writeValueToBuffer(argument,				&buffer_cursor)) return false;

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
bool MethodChannel_respond(FlutterPlatformMessageResponseHandle* response_handle, struct MethodChannelValue* response_value) {
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


bool MethodChannel_decodeSize(uint8_t** p_buffer, size_t* buffer_remaining, size_t* size) {
	assert((*buffer_remaining >= 1) && "Error decoding platform message: while decoding size: message ended too soon");
	*size = **p_buffer;
	NEXT(*p_buffer, *buffer_remaining)

	if (*size == 254) {
		assert((*buffer_remaining >= 2) && "Error decoding platform message: while decoding size: message ended too soon");
		*size = *(uint16_t*) *p_buffer;
		NEXTN(*p_buffer, *buffer_remaining, 2)

		return true;
	} else {
		assert((*buffer_remaining >= 4) && "Error decoding platform message: while decoding size: message ended too soon");
		*size = *(uint32_t*) *p_buffer;
		NEXTN(*p_buffer, *buffer_remaining, 4)

		return true;
	}

	return true;
}
bool MethodChannel_decodeValue(uint8_t** p_buffer, size_t* buffer_remaining, struct MethodChannelValue* value) {
	assert((*buffer_remaining >= 1) && "Error decoding platform message: while decoding value type: message ended to soon");

	MessageValueDiscriminator type = **p_buffer;
	NEXT(*p_buffer, *buffer_remaining)

	value->type = type;

	size_t size; char* c_string; uint8_t* byteArray; int32_t* intArray; int64_t* longArray;
	switch (type) {
		case kNull:
		case kTrue:
		case kFalse:
			break;
		case kTypeInt:
			assert((*buffer_remaining >= 4) && "Error decoding platform message: while decoding kTypeInt: message ended to soon");

			value->value.int_value = *(int32_t*) *p_buffer;
			NEXTN(*p_buffer, *buffer_remaining, 4)

			break;
		case kTypeLong:
			assert((*buffer_remaining >= 8) && "Error decoding platform message: while decoding kTypeLong: message ended too soon");

			value->value.long_value = *(int64_t*) *p_buffer;
			NEXTN(*p_buffer, *buffer_remaining, 8)

			break;
		case kTypeDouble:
			assert((*buffer_remaining >= 8 + ALIGNMENT_DIFF(*p_buffer, 8)) && "Error decoding platform message: while decoding kTypeDouble: message ended too soon");
			ALIGN(*p_buffer, *buffer_remaining, 8)

			value->value.double_value = *(double*) *p_buffer;
			NEXTN(*p_buffer, *buffer_remaining, 8)

			break;
		case kTypeString:
			if (!MethodChannel_decodeSize(p_buffer, buffer_remaining, &size)) return false;

			assert((*buffer_remaining >= size) && "Error decoding platform message: while decoding kTypeString: message ended too soon");
			char* c_string = calloc(size+1, sizeof(char));

			for (int i = 0; i < size; i++) {
				c_string[i] = **p_buffer;
				NEXT(*p_buffer, *buffer_remaining)
			}
			value->value.string_value = c_string;

			break;
		case kTypeByteArray:
			if (!MethodChannel_decodeSize(p_buffer, buffer_remaining, &size)) return false;

			assert((*buffer_remaining >= size) && "Error decoding platform message: while decoding kTypeByteArray: message ended too soon");
			value->value.bytearray_value.size = size;
			value->value.bytearray_value.array = *p_buffer;
			
			NEXTN(*p_buffer, *buffer_remaining, size);

			break;
		case kTypeIntArray:
			if (!MethodChannel_decodeSize(p_buffer, buffer_remaining, &size)) return false;

			assert((*buffer_remaining >= size*4 + ALIGNMENT_DIFF(*p_buffer, 4)) && "Error decoding platform message: while decoding kTypeIntArray: message ended too soon");
			ALIGN(*p_buffer, *buffer_remaining, 4)

			value->value.intarray_value.size = size;
			value->value.intarray_value.array = (int32_t*) *p_buffer;

			NEXTN(*p_buffer, *buffer_remaining, size*4)

			break;
		case kTypeLongArray:
			if (!MethodChannel_decodeSize(p_buffer, buffer_remaining, &size)) return false;

			assert((*buffer_remaining >= size*8 + ALIGNMENT_DIFF(*p_buffer, 8)) && "Error decoding platform message: while decoding kTypeLongArray: message ended too soon");
			ALIGN(*p_buffer, *buffer_remaining, 8)

			value->value.longarray_value.size = size;
			value->value.longarray_value.array = (int64_t*) *p_buffer;

			NEXTN(*p_buffer, *buffer_remaining, size*8)

			break;
		case kTypeDoubleArray:
			if (!MethodChannel_decodeSize(p_buffer, buffer_remaining, &size)) return false;

			assert((*buffer_remaining >= size*8 + ALIGNMENT_DIFF(*p_buffer, 8)) && "Error decoding platform message: while decoding kTypeIntArray: message ended too soon");
			ALIGN(*p_buffer, *buffer_remaining, 8)

			value->value.doublearray_value.size = size;
			value->value.doublearray_value.array = (double*) *p_buffer;

			NEXTN(*p_buffer, *buffer_remaining, size*8)

			break;
		case kTypeList:
			if (!MethodChannel_decodeSize(p_buffer, buffer_remaining, &size)) return false;

			value->value.list_value.size = size;
			value->value.list_value.list = calloc(size, sizeof(struct MethodChannelValue));

			for (int i = 0; i < size; i++) {
				if (!MethodChannel_decodeValue(p_buffer, buffer_remaining, &(value->value.list_value.list[i]))) return false;
			}

			break;
		case kTypeMap:
			if (!MethodChannel_decodeSize(p_buffer, buffer_remaining, &size)) return false;

			value->value.map_value.size = size;
			value->value.map_value.map = calloc(size*2, sizeof(struct MethodChannelValue));

			for (int i = 0; i < size; i++) {
				if (!MethodChannel_decodeValue(p_buffer, buffer_remaining, &(value->value.list_value.list[i*2  ]))) return false;
				if (!MethodChannel_decodeValue(p_buffer, buffer_remaining, &(value->value.list_value.list[i*2+1]))) return false;
			}

			break;
		default:
			fprintf(stderr, "Error decoding platform message: unknown value type: %d\n", type);
			return false;
	}

	return true;
}
bool MethodChannel_decode(size_t buffer_size, uint8_t* buffer, struct MethodCall* result) {
	uint8_t* buffer_cursor = buffer;
	size_t buffer_remaining = buffer_size;
	
	struct MethodChannelValue method_name;
	if (!MethodChannel_decodeValue(&buffer_cursor, &buffer_remaining, &method_name)) return false;
	if (method_name.type != kTypeString) {
		fprintf(stderr, "Error decoding Method Call: expected type of first value in buffer to be string (i.e. method name), got %d\n", method_name.type);
		return false;
	}
	result->method = method_name.value.string_value;

	if (!MethodChannel_decodeValue(&buffer_cursor, &buffer_remaining, &(result->argument))) return false;

	return true;
}


bool MethodChannel_freeValue(struct MethodChannelValue* p_value) {
	switch (p_value->type) {
		case kTypeString:
			free(p_value->value.string_value);
			break;
		case kTypeList:
			for (int i=0; i < p_value->value.list_value.size; i++)
				if (!MethodChannel_freeValue(&(p_value->value.list_value.list[i]))) return false;
			
			free(p_value->value.list_value.list);
			break;
		case kTypeMap:
			for (int i=0; i< p_value->value.map_value.size; i++) {
				if (!MethodChannel_freeValue(&(p_value->value.map_value.map[i*2  ]))) return false;
				if (!MethodChannel_freeValue(&(p_value->value.map_value.map[i*2+1]))) return false;
			}

			free(p_value->value.map_value.map);
		default:
			break;
	}

	return true;
}
bool MethodChannel_freeMethodCall(struct MethodCall* methodcall) {
	free(methodcall->method);
	if (!MethodChannel_freeValue(&(methodcall->argument))) return false;

	return true;
}


#undef _NEXTN
#undef _NEXTN_REMAINING
#undef _NEXT
#undef _NEXT_REMAINING
#undef _GET_MACRO
#undef NEXT
#undef NEXTN
#undef ALIGNMENT_DIFF
#undef _ALIGN
#undef _ALIGN_REMAINING
#undef ALIGN