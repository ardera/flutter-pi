#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <flutter_embedder.h>
#include "methodchannel.h"

#define NEXT(a) (a = &((a)[1]));
#define NEXTN(a, n) (a = &((a)[(n)]));

bool calcBufferSize(uint64_t** argument, int* buffer_size) {
	uint64_t type = **argument;
	NEXT(*argument)

	if (type == kNoValue) return false;
	
	int size; uint8_t* byteArray; uint32_t* intArray; uint64_t* longArray;
	switch (type) {
		case kNull:
		case kTrue:
		case kFalse:
			*buffer_size += 1;
			break;
		case kTypeInt:
			NEXT(*argument)
			*buffer_size += 1;
			*buffer_size += 4;
			break;
		case kTypeLong:
			NEXT(*argument)
			*buffer_size += 1;
			*buffer_size += 8;
			break;
		case kTypeDouble:
			NEXT(*argument)
			*buffer_size += 1;
			*buffer_size = (((*buffer_size) + 7) | 7) - 7;	// 8-byte aligned
			*buffer_size += 8;
			break;
		case kTypeString:
		case kTypeByteArray:
			size = **argument;
			NEXT(*argument)
			byteArray = (uint8_t*) **argument;
			NEXT(*argument)

			*buffer_size += 1;
			*buffer_size += (size < 254) ? 1 : (size <= 0xFFFF) ? 3 : 5;
			*buffer_size += size;
			break;
		case kTypeIntArray:
			size = **argument;
			NEXT(*argument)
			intArray = (uint32_t*) **argument;
			NEXT(*argument)

			*buffer_size += 1;
			*buffer_size += (size < 254) ? 1 : (size <= 0xFFFF) ? 3 : 5;
			*buffer_size = (((*buffer_size) + 3) | 3) - 3;
			*buffer_size += size*4;
			break;
		case kTypeLongArray:
		case kTypeDoubleArray:
			size = **argument;
			NEXT(*argument)
			longArray = (uint64_t*) **argument;
			NEXT(*argument)

			*buffer_size += 1;
			*buffer_size += (size < 254) ? 1 : (size <= 0xFFFF) ? 3 : 5;		// write size byte
			*buffer_size = (((*buffer_size) + 7) | 7) - 7;						// 8-byte aligned
			*buffer_size += size*8;
			break;
		case kTypeList:
		case kTypeMap:
		case kTypeBigInt:
			fprintf(stderr, "message arguments type BigInt, list and map not supported\n");
			exit(1);
			break;
	}

	return true;
}

void writeSize(int size, uint8_t** buffer) {
	if (size < 254) {
		**buffer = size;
		NEXT(*buffer)
	} else if (size <= 0xFFFF) {
		**buffer = 254;
		NEXT(*buffer)
		*((uint16_t*) *buffer) = size;
		NEXTN(*buffer, 2)
	} else {
		**buffer = 255;
		NEXT(*buffer)
		*((uint32_t*) *buffer) = size;
		NEXTN(*buffer, 4)
	}
}
bool convert(uint64_t** argument, uint8_t** buffer) {
	uint64_t type = **argument;
	NEXT(*argument)

	if (type == kNoValue) return false;

	int size; uint8_t* byteArray; uint32_t* intArray; uint64_t* longArray;
	switch (type) {
		case kNull:
		case kTrue:
		case kFalse:
			**buffer = type;
			NEXT(*buffer)
			break;
		case kTypeInt:
			*((uint32_t*) *buffer) = **argument;
			NEXT(*argument)
			NEXTN(*buffer, 4)
			break;

		case kTypeDouble:
			*buffer = (uint8_t*) (((((uint64_t) *buffer) + 7) | 7) - 7);	// 8-byte aligned
		case kTypeLong:
			*((uint64_t*) *buffer) = **argument;
			NEXT(*argument)
			NEXTN(*buffer, 8)
			break;
		
		case kTypeString:
		case kTypeByteArray:
			if (type == kTypeString) {
				byteArray = (uint8_t*) **argument;
				NEXT(*argument)

				size = strlen((char*) byteArray);
			} else {
				size = **argument;
				NEXT(*argument)

				byteArray = (uint8_t*) **argument;
				NEXT(*argument)
			}		

			writeSize(size, buffer);
			
			for (int i = 0; i<size; i++) {
				**buffer = byteArray[i];
				NEXT(*buffer)
			}

			break;
		case kTypeIntArray:
			size = **argument;
			NEXT(*argument)
			intArray = (uint32_t*) **argument;
			NEXT(*argument)

			writeSize(size, buffer);

			*buffer = (uint8_t*) ((((uint64_t) *buffer + 3) | 3) - 3);
			
			for (int i = 0; i<size; i++) {
				*((uint32_t*) *buffer) = intArray[i];
				NEXTN(*buffer, 4)
			}

			break;
		case kTypeLongArray:
		case kTypeDoubleArray:
			size = **argument;
			NEXT(*argument)
			longArray = (uint64_t*) **argument;
			NEXT(*argument)

			writeSize(size, buffer);

			*buffer = (uint8_t*) ((((uint64_t) *buffer + 7) | 7) - 7);						// 8-byte aligned
			
			for (int i = 0; i<size; i++) {
				*((uint64_t*) *buffer) = longArray[i];
				NEXTN(*buffer, 8)
			}

			break;
		case kTypeList:
		case kTypeMap:
		case kTypeBigInt:
			fprintf(stderr, "message arguments type BigInt, list and map not supported\n");
			exit(1);
			break;
	}

	return true;
}

FlutterPlatformMessage* buildMethodChannelMessage(const char* channel, const FlutterPlatformMessageResponseHandle* response_handle, uint64_t* arguments) {
	uint64_t* arguments_copy = arguments;
	uint8_t* buffer_copy;

	int buffer_size = 0;
	while (calcBufferSize(&arguments_copy, &buffer_size));

	uint8_t* buffer = malloc(buffer_size);
	buffer_copy = buffer;
	arguments_copy = arguments;
	while (convert(&arguments_copy, &buffer_copy));

	FlutterPlatformMessage* result = malloc(sizeof(FlutterPlatformMessage));
	result->struct_size = sizeof(FlutterPlatformMessage);
	result->channel = channel;
	result->response_handle = response_handle;
	result->message = (const uint8_t*) buffer;
	*((size_t*) &(result->message_size)) = buffer_size;

	return result;
}