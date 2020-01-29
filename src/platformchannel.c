#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <flutter_embedder.h>

#include <platformchannel.h>
#include <flutter-pi.h>
#include <jsmn.h>


struct ResponseHandlerData {
	enum ChannelCodec codec;
	PlatformMessageResponseCallback on_response;
	void *userdata;
};

int PlatformChannel_freeStdMsgCodecValue(struct StdMsgCodecValue *value) {
	int ok;

	switch (value->type) {
		case kString:
			free(value->string_value);
			break;
		case kList:
			for (int i=0; i < value->size; i++) {
				ok = PlatformChannel_freeStdMsgCodecValue(&(value->list[i]));
				if (ok != 0) return ok;
			}
			free(value->list);
			break;
		case kMap:
			for (int i=0; i < value->size; i++) {
				ok = PlatformChannel_freeStdMsgCodecValue(&(value->keys[i]));
				if (ok != 0) return ok;
				ok = PlatformChannel_freeStdMsgCodecValue(&(value->values[i]));
				if (ok != 0) return ok;
			}
			free(value->keys);
			break;
		default:
			break;
	}

	return 0;
}
int PlatformChannel_freeJSONMsgCodecValue(struct JSONMsgCodecValue *value, bool shallow) {
	int ok;
	
	switch (value->type) {
		case kJSArray:
			if (!shallow) {
				for (int i = 0; i < value->size; i++) {
					ok = PlatformChannel_freeJSONMsgCodecValue(&(value->array[i]), false);
					if (ok != 0) return ok;
				}
			}

			free(value->array);
			break;
		case kJSObject:
			if (!shallow) {
				for (int i = 0; i < value->size; i++) {
					ok = PlatformChannel_freeJSONMsgCodecValue(&(value->values[i]), false);
					if (ok != 0) return ok;
				}
			}

			free(value->keys);
			break;
		default:
			break;
	}

	return 0;
}
int PlatformChannel_free(struct ChannelObject *object) {
	switch (object->codec) {
		case kStringCodec:
			free(object->string_value);
			break;
		case kBinaryCodec:
			break;
		case kJSONMessageCodec:
			PlatformChannel_freeJSONMsgCodecValue(&(object->jsonmsgcodec_value), false);
			break;
		case kStandardMessageCodec:
			PlatformChannel_freeStdMsgCodecValue(&(object->stdmsgcodec_value));
			break;
		case kStandardMethodCall:
			free(object->method);
			PlatformChannel_freeStdMsgCodecValue(&(object->stdarg));
			break;
		case kJSONMethodCall:
			PlatformChannel_freeJSONMsgCodecValue(&(object->jsarg), false);
	}

	return 0;
}

int PlatformChannel_calculateStdMsgCodecValueSize(struct StdMsgCodecValue* value, size_t* psize) {
	enum StdMsgCodecValueType type = value->type;
	size_t size;
	int ok;

	// Type Byte
	advance(psize, 1);
	switch (type) {
		case kNull:
		case kTrue:
		case kFalse:
			break;
		case kInt32:
			advance(psize, 4);
			break;
		case kInt64:
			advance(psize, 8);
			break;
		case kFloat64:
			align8 (psize);
			advance(psize, 8);
			break;
		case kString:
		case kLargeInt:
			size = strlen(value->string_value);
			advance(psize, size + nSizeBytes(size));
			break;
		case kUInt8Array:
			size = value->size;
			advance(psize, size + nSizeBytes(size));
			break;
		case kInt32Array:
			size = value->size;

			advance(psize, nSizeBytes(size));
			align4 (psize);
			advance(psize, size*4);

			break;
		case kInt64Array:
			size = value->size;
			
			advance(psize, nSizeBytes(size));
			align8 (psize);
			advance(psize, size*8);

			break;
		case kFloat64Array:
			size = value->size;
			
			advance(psize, nSizeBytes(size));
			align8 (psize);
			advance(psize, size*8);

			break;
		case kList:
			size = value->size;

			advance(psize, nSizeBytes(size));
			for (int i = 0; i<size; i++)
				if ((ok = PlatformChannel_calculateStdMsgCodecValueSize(&(value->list[i]), psize))   != 0)    return ok;
			
			break;
		case kMap:
			size = value->size;

			advance(psize, nSizeBytes(size));
			for (int i = 0; i<size; i++) {
				if ((ok = PlatformChannel_calculateStdMsgCodecValueSize(&(value->keys[i]), psize))   != 0) return ok;
				if ((ok = PlatformChannel_calculateStdMsgCodecValueSize(&(value->values[i]), psize)) != 0) return ok;
			}

			break;
		default:
			return EINVAL;
	}

	return 0;
}
int PlatformChannel_writeStdMsgCodecValueToBuffer(struct StdMsgCodecValue* value, uint8_t **pbuffer) {
	uint8_t* byteArray;
	size_t size;
	int ok;

	write8(pbuffer, value->type);
	advance(pbuffer, 1);

	switch (value->type) {
		case kNull:
		case kTrue:
		case kFalse:
			break;
		case kInt32:
			write32(pbuffer, value->int32_value);
			advance(pbuffer, 4);
			break;
		case kInt64:
			write64(pbuffer, value->int64_value);
			advance(pbuffer, 8);
			break;
		case kFloat64:
			align8(pbuffer);
			write64(pbuffer, *((uint64_t*) &(value->float64_value)));
			advance(pbuffer, 8);
			break;
		case kLargeInt:
		case kString:
		case kUInt8Array:
			if ((value->type == kLargeInt) || (value->type == kString)) {
				size = strlen(value->string_value);
				byteArray = (uint8_t*) value->string_value;
			} else if (value->type == kUInt8Array) {
				size = value->size;
				byteArray = value->uint8array;
			}

			writeSize(pbuffer, size);
			for (int i=0; i<size; i++) {
				write8(pbuffer, byteArray[i]);
				advance(pbuffer, 1);
			}
			break;
		case kInt32Array:
			size = value->size;

			writeSize(pbuffer, size);
			align4(pbuffer);
			
			for (int i=0; i<size; i++) {
				write32(pbuffer, value->int32array[i]);
				advance(pbuffer, 4);
			}
			break;
		case kInt64Array:
			size = value->size;

			writeSize(pbuffer, size);
			align8(pbuffer);
			for (int i=0; i<size; i++) {
				write64(pbuffer, value->int64array[i]);
				advance(pbuffer, 8);
			}
			break;
		case kFloat64Array:
			size = value->size;

			writeSize(pbuffer, size);
			align8(pbuffer);

			for (int i=0; i<size; i++) {
				write64(pbuffer, value->float64array[i]);
				advance(pbuffer, 8);
			}
			break;
		case kList:
			size = value->size;

			writeSize(pbuffer, size);
			for (int i=0; i<size; i++)
				if ((ok = PlatformChannel_writeStdMsgCodecValueToBuffer(&(value->list[i]), pbuffer))   != 0) return ok;
			
			break;
		case kMap:
			size = value->size;

			writeSize(pbuffer, size);
			for (int i=0; i<size; i++) {
				if ((ok = PlatformChannel_writeStdMsgCodecValueToBuffer(&(value->keys[i]), pbuffer))   != 0) return ok;
				if ((ok = PlatformChannel_writeStdMsgCodecValueToBuffer(&(value->values[i]), pbuffer)) != 0) return ok;
			}
			break;
		default:
			return EINVAL;
	}

	return 0;
}
size_t PlatformChannel_calculateJSONMsgCodecValueSize(struct JSONMsgCodecValue *value) {
	size_t size = 0;

	switch (value->type) {
		case kJSNull:
		case kJSTrue:
			return 4;
		case kJSFalse:
			return 5;
		case kJSNumber: ;
			char numBuffer[32];
			return sprintf(numBuffer, "%lf", value->number_value);
		case kJSString:
			size = 2;

			// we need to count how many characters we need to escape.
			for (char *s = value->string_value; *s; s++) {
				switch (*s) {
					case '\b':
					case '\f':
					case '\n':
					case '\r':
					case '\t':
					case '\"':
					case '\\':
						size += 2;
						break;
					default:
						size++;
						break;
				}
			}

			return size;
		case kJSArray:
			size += 2;
			for (int i=0; i < value->size; i++) {
				size += PlatformChannel_calculateJSONMsgCodecValueSize(&(value->array[i]));
				if (i+1 != value->size) size += 1;
			}
			return size;
		case kJSObject:
			size += 2;
			for (int i=0; i < value->size; i++) {
				size += strlen(value->keys[i]) + 3 + PlatformChannel_calculateJSONMsgCodecValueSize(&(value->values[i]));
				if (i+1 != value->size) size += 1;
			}
			return size;
		default:
			return EINVAL;
	}

	return 0;
}
int PlatformChannel_writeJSONMsgCodecValueToBuffer(struct JSONMsgCodecValue* value, uint8_t **pbuffer) {
	switch (value->type) {
		case kJSNull:
			*pbuffer += sprintf((char*) *pbuffer, "null");
			break;
		case kJSTrue:
			*pbuffer += sprintf((char*) *pbuffer, "true");
			break;
		case kJSFalse:
			*pbuffer += sprintf((char*) *pbuffer, "false");
			break;
		case kJSNumber:
			*pbuffer += sprintf((char*) *pbuffer, "%lf", value->number_value);
			break;
		case kJSString:
			*(pbuffer++) = '\"';

			for (char *s = value->string_value; *s; s++) {
				switch (*s) {
					case '\b':
						*(pbuffer++) = '\\';
						*(pbuffer++) = 'b';
						break;
					case '\f':
						*(pbuffer++) = '\\';
						*(pbuffer++) = 'f';
						break;
					case '\n':
						*(pbuffer++) = '\\';
						*(pbuffer++) = 'n';
						break;
					case '\r':
						*(pbuffer++) = '\\';
						*(pbuffer++) = 'r';
						break;
					case '\t':
						*(pbuffer++) = '\\';
						*(pbuffer++) = 't';
						break;
					case '\"':
						*(pbuffer++) = '\\';
						*(pbuffer++) = 't';
						break;
					case '\\':
						*(pbuffer++) = '\\';
						*(pbuffer++) = '\\';
						break;
					default:
						*(pbuffer++) = *s;
						break;
				}
			}

			*(pbuffer++) = '\"';

			break;
		case kJSArray:
			*pbuffer += sprintf((char*) *pbuffer, "[");
			for (int i=0; i < value->size; i++) {
				PlatformChannel_writeJSONMsgCodecValueToBuffer(&(value->array[i]), pbuffer);
				if (i+1 != value->size) *pbuffer += sprintf((char*) *pbuffer, ",");
			}
			*pbuffer += sprintf((char*) *pbuffer, "]");
			break;	
		case kJSObject:
			*pbuffer += sprintf((char*) *pbuffer, "{");
			for (int i=0; i < value->size; i++) {
				*pbuffer += sprintf((char*) *pbuffer, "\"%s\":", value->keys[i]);
				PlatformChannel_writeJSONMsgCodecValueToBuffer(&(value->values[i]), pbuffer);
				if (i+1 != value->size) *pbuffer += sprintf((char*) *pbuffer, ",");
			}
			*pbuffer += sprintf((char*) *pbuffer, "}");
			break;
		default:
			return EINVAL;
	}

	return 0;
}
int PlatformChannel_decodeStdMsgCodecValue(uint8_t **pbuffer, size_t *premaining, struct StdMsgCodecValue *value_out) {
	int64_t *longArray = 0;
	int32_t *intArray = 0;
	uint8_t *byteArray = 0;
	char *c_string = 0; 
	size_t size = 0;
	int ok;
	
	enum StdMsgCodecValueType type = read8(pbuffer);
	advance(pbuffer, 1, premaining);

	value_out->type = type;
	switch (type) {
		case kNull:
		case kTrue:
		case kFalse:
			break;
		case kInt32:
			if (*premaining < 4) return EBADMSG;

			value_out->int32_value = (int32_t) read32(pbuffer);
			advance(pbuffer, 4, premaining);

			break;
		case kInt64:
			if (*premaining < 8) return EBADMSG;

			value_out->int64_value = (int64_t) read64(pbuffer);
			advance(pbuffer, 8, premaining);

			break;
		case kFloat64:
			if (*premaining < (8 + alignmentDiff(*pbuffer, 8))) return EBADMSG;

			align8(pbuffer, premaining);
			uint64_t temp = read64(pbuffer);
			value_out->float64_value = *((double*) (&temp));
			advance(pbuffer, 8, premaining);

			break;
		case kLargeInt:
		case kString:
			if ((ok = readSize(pbuffer, premaining, &size)) != 0) return ok;
			if (*premaining < size) return EBADMSG;

			value_out->string_value = calloc(size+1, sizeof(char));
			if (!value_out->string_value) return ENOMEM;
			memcpy(value_out->string_value, *pbuffer, size);
			advance(pbuffer, size, premaining);

			break;
		case kUInt8Array:
			if ((ok = readSize(pbuffer, premaining, &size)) != 0) return ok;
			if (*premaining < size) return EBADMSG;

			value_out->size = size;
			value_out->uint8array = *pbuffer;
			advance(pbuffer, size, premaining);

			break;
		case kInt32Array:
			if ((ok = readSize(pbuffer, premaining, &size)) != 0) return ok;
			if (*premaining < (size*4 + alignmentDiff(*pbuffer, 4))) return EBADMSG;

			align4(pbuffer, premaining);
			value_out->size = size;
			value_out->int32array = (int32_t*) *pbuffer;
			advance(pbuffer, size*4, premaining);

			break;
		case kInt64Array:
			if ((ok = readSize(pbuffer, premaining, &size)) != 0) return ok;
			if (*premaining < (size*8 + alignmentDiff(*pbuffer, 8))) return EBADMSG;

			align8(pbuffer, premaining);
			value_out->size = size;
			value_out->int64array = (int64_t*) *pbuffer;
			advance(pbuffer, size*8, premaining);

			break;
		case kFloat64Array:
			if ((ok = readSize(pbuffer, premaining, &size)) != 0) return ok;
			if (*premaining < (size*8 + alignmentDiff(*pbuffer, 8))) return EBADMSG;

			align8(pbuffer, premaining);
			value_out->size = size;
			value_out->float64array = (double*) *pbuffer;
			advance(pbuffer, size*8, premaining);

			break;
		case kList:
			if ((ok = readSize(pbuffer, premaining, &size)) != 0) return ok;

			value_out->size = size;
			value_out->list = calloc(size, sizeof(struct StdMsgCodecValue));

			for (int i = 0; i < size; i++) {
				ok = PlatformChannel_decodeStdMsgCodecValue(pbuffer, premaining, &(value_out->list[i]));
				if (ok != 0) return ok;
			}

			break;
		case kMap:
			if ((ok = readSize(pbuffer, premaining, &size)) != 0) return ok;

			value_out->size = size;
			value_out->keys = calloc(size*2, sizeof(struct StdMsgCodecValue));
			if (!value_out->keys) return ENOMEM;
			value_out->values = &(value_out->keys[size]);

			for (int i = 0; i < size; i++) {
				ok = PlatformChannel_decodeStdMsgCodecValue(pbuffer, premaining, &(value_out->keys[i]));
				if (ok != 0) return ok;
				
				ok = PlatformChannel_decodeStdMsgCodecValue(pbuffer, premaining, &(value_out->values[i]));
				if (ok != 0) return ok;
			}

			break;
		default:
			return EBADMSG;
	}

	return 0;
}
int PlatformChannel_decodeJSONMsgCodecValue(char *message, size_t size, jsmntok_t **pptoken, size_t *ptokensremaining, struct JSONMsgCodecValue *value_out) {
	jsmntok_t *ptoken;
	int result, ok;
	
	if (!pptoken) {
		// if we have no token list yet, parse the message & create one.

		jsmntok_t tokens[JSON_DECODE_TOKENLIST_SIZE];
		jsmn_parser parser;
		size_t tokensremaining;

		memset(tokens, sizeof(tokens), 0);

		jsmn_init(&parser);
		result = jsmn_parse(&parser, (const char *) message, (const size_t) size, tokens, JSON_DECODE_TOKENLIST_SIZE);
		if (result < 0) return EBADMSG;
		
		tokensremaining = (size_t) result;
		ptoken = tokens;

		ok = PlatformChannel_decodeJSONMsgCodecValue(message, size, &ptoken, &tokensremaining, value_out);
		if (ok != 0) return ok;
	} else {
		// message is already tokenized

		ptoken = *pptoken;

		(*pptoken) += 1;
		*ptokensremaining -= 1;

		switch (ptoken->type) {
			case JSMN_UNDEFINED:
				return EBADMSG;
			case JSMN_PRIMITIVE:
				if (message[ptoken->start] == 'n') {
					value_out->type = kJSNull;
				} else if (message[ptoken->start] == 't') {
					value_out->type = kJSTrue;
				} else if (message[ptoken->start] == 'f') {
					value_out->type = kJSFalse;
				} else {
					value_out->type = kJSNumber;

					// hacky, but should work in normal circumstances. If the platform message solely consists
					//   of this number and nothing else, this could fail.
					char old = message[ptoken->end];
					message[ptoken->end] = '\0';
					value_out->number_value = strtod(message + ptoken->start, NULL);
					message[ptoken->end] = old;
				}

				break;
			case JSMN_STRING: ;
				// use zero-copy approach.

				message[ptoken->end] = '\0';
				char *string = message + ptoken->start;

				value_out->type = kJSString;
				value_out->string_value = string;

				break;
			case JSMN_ARRAY: ;
				struct JSONMsgCodecValue *array = calloc(ptoken->size, sizeof(struct JSONMsgCodecValue));
				if (!array) return ENOMEM;

				for (int i=0; i < ptoken->size; i++) {
					ok = PlatformChannel_decodeJSONMsgCodecValue(message, size, pptoken, ptokensremaining, &array[i]);
					if (ok != 0) return ok;
				}

				value_out->type = kJSArray;
				value_out->size = ptoken->size;
				value_out->array = array;

				break;
			case JSMN_OBJECT: ;
				struct JSONMsgCodecValue  key;
				char                    **keys = calloc(ptoken->size, sizeof(char *));
				struct JSONMsgCodecValue *values = calloc(ptoken->size, sizeof(struct JSONMsgCodecValue));
				if ((!keys) || (!values)) return ENOMEM;

				for (int i=0; i < ptoken->size; i++) {
					ok = PlatformChannel_decodeJSONMsgCodecValue(message, size, pptoken, ptokensremaining, &key);
					if (ok != 0) return ok;

					if (key.type != kJSString) return EBADMSG;
					keys[i] = key.string_value;

					ok = PlatformChannel_decodeJSONMsgCodecValue(message, size, pptoken, ptokensremaining, &values[i]);
					if (ok != 0) return ok;
				}

				value_out->type = kJSObject;
				value_out->size = ptoken->size;
				value_out->keys = keys;
				value_out->values = values;

				break;
			default:
				return EBADMSG;
		}
	}

	return 0;
}

int PlatformChannel_decodeJSON(char *string, struct JSONMsgCodecValue *out) {
	return PlatformChannel_decodeJSONMsgCodecValue(string, strlen(string), NULL, NULL, out);
}

int PlatformChannel_decode(uint8_t *buffer, size_t size, enum ChannelCodec codec, struct ChannelObject *object_out) {
	struct JSONMsgCodecValue root_jsvalue;
	uint8_t *buffer_cursor = buffer;
	size_t   remaining = size;
	int      ok;

	if ((size == 0) && (buffer == NULL)) {
		object_out->codec = kNotImplemented;
		return 0;
	}
	
	object_out->codec = codec;
	switch (codec) {
		case kStringCodec: ;
			/// buffer is a non-null-terminated, UTF8-encoded string.
			/// it's really sad we have to allocate a new memory block for this, but we have to since string codec buffers are not null-terminated.

			char *string;
			if (!(string = malloc(size +1))) return ENOMEM;
			memcpy(string, buffer, size);
			string[size] = '\0';

			object_out->string_value = string;

			break;
		case kBinaryCodec:
			object_out->binarydata = buffer;
			object_out->binarydata_size = size;

			break;
		case kJSONMessageCodec:
			ok = PlatformChannel_decodeJSONMsgCodecValue((char *) buffer, size, NULL, NULL, &(object_out->jsonmsgcodec_value));
			if (ok != 0) return ok;

			break;
		case kJSONMethodCall: ;
			ok = PlatformChannel_decodeJSONMsgCodecValue((char *) buffer, size, NULL, NULL, &root_jsvalue);
			if (ok != 0) return ok;

			if (root_jsvalue.type != kJSObject) return EBADMSG;
			
			for (int i=0; i < root_jsvalue.size; i++) {
				if ((strcmp(root_jsvalue.keys[i], "method") == 0) && (root_jsvalue.values[i].type == kJSString)) {
					object_out->method = root_jsvalue.values[i].string_value;
				} else if (strcmp(root_jsvalue.keys[i], "args") == 0) {
					object_out->jsarg = root_jsvalue.values[i];
				} else return EBADMSG;
			}

			PlatformChannel_freeJSONMsgCodecValue(&root_jsvalue, true);

			break;
		case kJSONMethodCallResponse: ;
			ok = PlatformChannel_decodeJSONMsgCodecValue((char *) buffer, size, NULL, NULL, &root_jsvalue);
			if (ok != 0) return ok;
			if (root_jsvalue.type != kJSArray) return EBADMSG;
			
			if (root_jsvalue.size == 1) {
				object_out->success = true;
				object_out->jsresult = root_jsvalue.array[0];
				return PlatformChannel_freeJSONMsgCodecValue(&root_jsvalue, true);
			} else if ((root_jsvalue.size == 3) &&
					   (root_jsvalue.array[0].type == kJSString) &&
					   ((root_jsvalue.array[1].type == kJSString) || (root_jsvalue.array[1].type == kJSNull))) {
				
				
				object_out->success = false;
				object_out->errorcode = root_jsvalue.array[0].string_value;
				object_out->errormessage = root_jsvalue.array[1].string_value;
				object_out->jserrordetails = root_jsvalue.array[2];
				return PlatformChannel_freeJSONMsgCodecValue(&root_jsvalue, true);
			} else return EBADMSG;

			break;
		case kStandardMessageCodec:
			ok = PlatformChannel_decodeStdMsgCodecValue(&buffer_cursor, &remaining, &(object_out->stdmsgcodec_value));
			if (ok != 0) return ok;
			break;
		case kStandardMethodCall: ;
			struct StdMsgCodecValue methodname;

			ok = PlatformChannel_decodeStdMsgCodecValue(&buffer_cursor, &remaining, &methodname);
			if (ok != 0) return ok;
			if (methodname.type != kString) {
				PlatformChannel_freeStdMsgCodecValue(&methodname);
				return EPROTO;
			}
			object_out->method = methodname.string_value;

			ok = PlatformChannel_decodeStdMsgCodecValue(&buffer_cursor, &remaining, &(object_out->stdarg));
			if (ok != 0) return ok;

			break;
		case kStandardMethodCallResponse: ;
			object_out->success = read8(&buffer_cursor) == 0;
			advance(&buffer_cursor, 1, &remaining);

			if (object_out->success) {
				struct StdMsgCodecValue result;

				ok = PlatformChannel_decodeStdMsgCodecValue(&buffer_cursor, &remaining, &(object_out->stdresult));
				if (ok != 0) return ok;
			} else {
				struct StdMsgCodecValue errorcode, errormessage;

				ok = PlatformChannel_decodeStdMsgCodecValue(&buffer_cursor, &remaining, &errorcode);
				if (ok != 0) return ok;
				ok = PlatformChannel_decodeStdMsgCodecValue(&buffer_cursor, &remaining, &errormessage);
				if (ok != 0) return ok;
				ok = PlatformChannel_decodeStdMsgCodecValue(&buffer_cursor, &remaining, &(object_out->stderrordetails));
				if (ok != 0) return ok;

				if ((errorcode.type == kString) && ((errormessage.type == kString) || (errormessage.type == kNull))) {
					object_out->errorcode = errorcode.string_value;
					object_out->errormessage = (errormessage.type == kString) ? errormessage.string_value : NULL;
				} else {
					return EBADMSG;
				}
			}
			break;
		default:
			return EINVAL;
	}

	return 0;
}
int PlatformChannel_encode(struct ChannelObject *object, uint8_t **buffer_out, size_t *size_out) {
	struct StdMsgCodecValue stdmethod, stderrcode, stderrmessage;
	struct JSONMsgCodecValue jsmethod, jserrcode, jserrmessage, jsroot;
	uint8_t *buffer, *buffer_cursor;
	size_t   size = 0;
	int		 ok = 0;

	*size_out = 0;
	*buffer_out = NULL;

	switch (object->codec) {
		case kNotImplemented:
			*size_out = 0;
			*buffer_out = NULL;
			return 0;
		case kStringCodec:
			size = strlen(object->string_value);
			break;
		case kBinaryCodec:
			*buffer_out = object->binarydata;
			*size_out = object->binarydata_size;
			return 0;
		case kJSONMessageCodec:
			size = PlatformChannel_calculateJSONMsgCodecValueSize(&(object->jsonmsgcodec_value));
			size += 1;  // JSONMsgCodec uses sprintf, which null-terminates strings,
						// so lets allocate one more byte for the last null-terminator.
						// this is decremented again in the second switch-case, so flutter
						// doesn't complain about a malformed message.
			break;
		case kStandardMessageCodec:
			ok = PlatformChannel_calculateStdMsgCodecValueSize(&(object->stdmsgcodec_value), &size);
			if (ok != 0) return ok;
			break;
		case kStandardMethodCall:
			stdmethod.type = kString;
			stdmethod.string_value = object->method;
			
			ok = PlatformChannel_calculateStdMsgCodecValueSize(&stdmethod, &size);
			if (ok != 0) return ok;

			ok = PlatformChannel_calculateStdMsgCodecValueSize(&(object->stdarg), &size);
			if (ok != 0) return ok;

			break;
		case kStandardMethodCallResponse:
			size += 1;

			if (object->success) {
				ok = PlatformChannel_calculateStdMsgCodecValueSize(&(object->stdresult), &size);
				if (ok != 0) return ok;
			} else {
				stderrcode = (struct StdMsgCodecValue) {
					.type = kString,
					.string_value = object->errorcode
				};
				stderrmessage = (struct StdMsgCodecValue) {
					.type = kString,
					.string_value = object->errormessage
				};
				
				ok = PlatformChannel_calculateStdMsgCodecValueSize(&stderrcode, &size);
				if (ok != 0) return ok;
				ok = PlatformChannel_calculateStdMsgCodecValueSize(&stderrmessage, &size);
				if (ok != 0) return ok;
				ok = PlatformChannel_calculateStdMsgCodecValueSize(&(object->stderrordetails), &size);
				if (ok != 0) return ok;
			}
			break;
		case kJSONMethodCall:
			jsroot.type = kJSObject;
			jsroot.size = 2;
			jsroot.keys = (char*[]) {"method", "args"};
			jsroot.values = (struct JSONMsgCodecValue[]) {
				{.type = kJSString, .string_value = object->method},
				object->jsarg
			};

			size = PlatformChannel_calculateJSONMsgCodecValueSize(&jsroot);
			size += 1;
			break;
		case kJSONMethodCallResponse:
			jsroot.type = kJSArray;
			if (object->success) {
				jsroot.size = 1;
				jsroot.array = (struct JSONMsgCodecValue[]) {
					object->jsresult
				};
			} else {
				jsroot.size = 3;
				jsroot.array = (struct JSONMsgCodecValue[]) {
					{.type = kJSString, .string_value = object->errorcode},
					{.type = (object->errormessage != NULL) ? kJSString : kJSNull, .string_value = object->errormessage},
					object->jserrordetails
				};
			}

			size = PlatformChannel_calculateJSONMsgCodecValueSize(&jsroot);
			size += 1;
			break;
		default:
			return EINVAL;
	}

	if (!(buffer = malloc(size))) return ENOMEM;
	buffer_cursor = buffer;
	
	switch (object->codec) {
		case kStringCodec:
			memcpy(buffer, object->string_value, size);
			break;
		case kStandardMessageCodec:
			ok = PlatformChannel_writeStdMsgCodecValueToBuffer(&(object->stdmsgcodec_value), &buffer_cursor);
			if (ok != 0) goto free_buffer_and_return_ok;
			break;
		case kStandardMethodCall:
			ok = PlatformChannel_writeStdMsgCodecValueToBuffer(&stdmethod, &buffer_cursor);
			if (ok != 0) goto free_buffer_and_return_ok;

			ok = PlatformChannel_writeStdMsgCodecValueToBuffer(&(object->stdarg), &buffer_cursor);
			if (ok != 0) goto free_buffer_and_return_ok;

			break;
		case kStandardMethodCallResponse:
			if (object->success) {
				write8(&buffer_cursor, 0x00);
				advance(&buffer_cursor, 1);

				ok = PlatformChannel_writeStdMsgCodecValueToBuffer(&(object->stdresult), &buffer_cursor);
				if (ok != 0) goto free_buffer_and_return_ok;
			} else {
				write8(&buffer_cursor, 0x01);
				advance(&buffer_cursor, 1);
				
				ok = PlatformChannel_writeStdMsgCodecValueToBuffer(&stderrcode, &buffer_cursor);
				if (ok != 0) goto free_buffer_and_return_ok;
				ok = PlatformChannel_writeStdMsgCodecValueToBuffer(&stderrmessage, &buffer_cursor);
				if (ok != 0) goto free_buffer_and_return_ok;
				ok = PlatformChannel_writeStdMsgCodecValueToBuffer(&(object->stderrordetails), &buffer_cursor);
				if (ok != 0) goto free_buffer_and_return_ok;
			}
			
			break;
		case kJSONMessageCodec:
			size -= 1;
			ok = PlatformChannel_writeJSONMsgCodecValueToBuffer(&(object->jsonmsgcodec_value), &buffer_cursor);
			if (ok != 0) goto free_buffer_and_return_ok;
			break;
		case kJSONMethodCall: ;
			size -= 1;
			ok = PlatformChannel_writeJSONMsgCodecValueToBuffer(&jsroot, &buffer_cursor);
			if (ok != 0) goto free_buffer_and_return_ok;
			break;
		default:
			return EINVAL;
	}

	*buffer_out = buffer;
	*size_out = size;
	return 0;

	free_buffer_and_return_ok:
	free(buffer);
	return ok;
}

void PlatformChannel_internalOnResponse(const uint8_t *buffer, size_t size, void *userdata) {
	struct ResponseHandlerData *handlerdata;
	struct ChannelObject object;
	int ok;

	handlerdata = (struct ResponseHandlerData *) userdata;
	ok = PlatformChannel_decode((uint8_t*) buffer, size, handlerdata->codec, &object);
	if (ok != 0) return;

	ok = handlerdata->on_response(&object, handlerdata->userdata);
	if (ok != 0) return;

	free(handlerdata);

	ok = PlatformChannel_free(&object);
	if (ok != 0) return;
}
int PlatformChannel_send(char *channel, struct ChannelObject *object, enum ChannelCodec response_codec, PlatformMessageResponseCallback on_response, void *userdata) {
	struct ResponseHandlerData *handlerdata = NULL;
	FlutterPlatformMessageResponseHandle *response_handle = NULL;
	FlutterEngineResult result;
	uint8_t *buffer;
	size_t   size;
	int ok;

	ok = PlatformChannel_encode(object, &buffer, &size);
	if (ok != 0) return ok;

	if (on_response) {
		handlerdata = malloc(sizeof(struct ResponseHandlerData));
		if (!handlerdata) return ENOMEM;
		
		handlerdata->codec = response_codec;
		handlerdata->on_response = on_response;
		handlerdata->userdata = userdata;

		result = FlutterPlatformMessageCreateResponseHandle(engine, PlatformChannel_internalOnResponse, handlerdata, &response_handle);
		if (result != kSuccess) return EINVAL;
	}

	//printf("[platformchannel] sending platform message to flutter on channel \"%s\". message_size: %d, has response_handle? %s\n", channel, size, response_handle ? "yes" : "no");
	//printf("  message buffer: \"");
	//for (int i = 0; i < size; i++)
	//	if (isprint(buffer[i])) printf("%c", buffer[i]);
	//	else printf("\\x%02X", buffer[i]);
	//printf("\"\n");
	
	result = FlutterEngineSendPlatformMessage(
		engine,
		& (const FlutterPlatformMessage) {
			.struct_size = sizeof(FlutterPlatformMessage),
			.channel = (const char*) channel,
			.message = (const uint8_t*) buffer,
			.message_size = (const size_t) size,
			.response_handle = response_handle
		}
	);

	if (on_response) {
		result = FlutterPlatformMessageReleaseResponseHandle(engine, response_handle);
		if (result != kSuccess) return EINVAL;
	}

	if (object->codec != kBinaryCodec)
		free(buffer);
	
	return (result == kSuccess) ? 0 : EINVAL;
}
int PlatformChannel_stdcall(char *channel, char *method, struct StdMsgCodecValue *argument, PlatformMessageResponseCallback on_response, void *userdata) {
	struct ChannelObject object = {
		.codec = kStandardMethodCall,
		.method = method,
		.stdarg = *argument
	};
	
	return PlatformChannel_send(channel, &object, kStandardMethodCallResponse, on_response, userdata);
}
int PlatformChannel_jsoncall(char *channel, char *method, struct JSONMsgCodecValue *argument, PlatformMessageResponseCallback on_response, void *userdata) {
	return PlatformChannel_send(channel,
								&(struct ChannelObject) {
									.codec = kJSONMethodCall,
									.method = method,
									.jsarg = *argument
								},
								kJSONMethodCallResponse,
								on_response,
								userdata);
}
int PlatformChannel_respond(FlutterPlatformMessageResponseHandle *handle, struct ChannelObject *response) {
	FlutterEngineResult result;
	uint8_t *buffer = NULL;
	size_t   size = 0;
	int ok;

	ok = PlatformChannel_encode(response, &buffer, &size);
	if (ok != 0) return ok;

	result = FlutterEngineSendPlatformMessageResponse(engine, (const FlutterPlatformMessageResponseHandle*) handle, (const uint8_t*) buffer, size);
	
	if (buffer != NULL) free(buffer);
	
	return (result == kSuccess) ? 0 : EINVAL;
}
int PlatformChannel_respondNotImplemented(FlutterPlatformMessageResponseHandle *handle) {
	return PlatformChannel_respond(
		(FlutterPlatformMessageResponseHandle *) handle,
		&(struct ChannelObject) {
			.codec = kNotImplemented
		});
}
int PlatformChannel_respondError(FlutterPlatformMessageResponseHandle *handle, enum ChannelCodec codec, char *errorcode, char *errormessage, void *errordetails) {
	if ((codec == kStandardMessageCodec) || (codec == kStandardMethodCall) || (codec == kStandardMethodCallResponse)) {
		if (errordetails == NULL)
			errordetails = &(struct StdMsgCodecValue) {.type = kNull};
		
		return PlatformChannel_respond(handle, &(struct ChannelObject) {
			.codec = kStandardMethodCallResponse,
			.success = false,
			.errorcode = errorcode,
			.errormessage = errormessage,
			.stderrordetails = *((struct StdMsgCodecValue *) errordetails)
		});
	} else if ((codec == kJSONMessageCodec) || (codec == kJSONMethodCall) || (codec == kJSONMethodCallResponse)) {
		if (errordetails == NULL)
			errordetails = &(struct JSONMsgCodecValue) {.type = kJSNull};
		
		return PlatformChannel_respond(handle, &(struct ChannelObject) {
			.codec = kJSONMethodCallResponse,
			.success = false,
			.errorcode = errorcode,
			.errormessage = errormessage,
			.jserrordetails = *((struct JSONMsgCodecValue *) errordetails)
		});
	} else return EINVAL;
}

bool jsvalue_equals(struct JSONMsgCodecValue *a, struct JSONMsgCodecValue *b) {
	if (a == b) return true;
	if ((a == NULL) ^ (b == NULL)) return false;
	if (a->type != b->type) return false;

	switch (a->type) {
		case kJSNull:
		case kJSTrue:
		case kJSFalse:
			return true;
		case kJSNumber:
			return a->number_value == b->number_value;
		case kJSString:
			return strcmp(a->string_value, b->string_value) == 0;
		case kJSArray:
			if (a->size != b->size) return false;
			if (a->array == b->array) return true;
			for (int i = 0; i < a->size; i++)
				if (!jsvalue_equals(&a->array[i], &b->array[i]))
					return false;
			return true;
		case kJSObject:
			if (a->size != b->size) return false;
			if ((a->keys == b->keys) && (a->values == b->values)) return true;

			bool _keyInBAlsoInA[a->size];
			memset(_keyInBAlsoInA, false, a->size * sizeof(bool));

			for (int i = 0; i < a->size; i++) {
				// The key we're searching for in b.
				char *key = a->keys[i];
				
				int j = 0;
				while (j < a->size) {
					while (_keyInBAlsoInA[j] && (j < a->size))  j++;	// skip all keys with _keyInBAlsoInA set to true.
					if (strcmp(key, b->keys[j]) != 0)   		j++;	// if b->keys[j] is not equal to "key", continue searching
					else {
						_keyInBAlsoInA[j] = true;

						// the values of "key" in a and b must (of course) also be equivalent.
						if (!jsvalue_equals(&a->values[i], &b->values[j])) return false;
						break;
					}
				}

				// we did not find a->keys[i] in b.
				if (j + 1 >= a->size) return false;
			}

			return true;
	}
}
struct JSONMsgCodecValue *jsobject_get(struct JSONMsgCodecValue *object, char *key) {
	int i;
	for (i=0; i < object->size; i++)
		if (strcmp(object->keys[i], key) == 0) break;


	if (i != object->size) return &(object->values[i]);
	return NULL;
}
bool stdvalue_equals(struct StdMsgCodecValue *a, struct StdMsgCodecValue *b) {
	if (a == b) return true;
	if ((a == NULL) ^  (b == NULL)) return false;
	if (a->type != b->type) return false;

	switch (a->type) {
		case kNull:
		case kTrue:
		case kFalse:
			return true;
		case kInt32:
			return a->int32_value == b->int32_value;
		case kInt64:
			return a->int64_value == b->int64_value;
		case kLargeInt:
		case kString:
			return strcmp(a->string_value, b->string_value) == 0;
		case kFloat64:
			return a->float64_value == b->float64_value;
		case kUInt8Array:
			if (a->size != b->size) return false;
			if (a->uint8array == b->uint8array) return true;
			for (int i = 0; i < a->size; i++)
				if (a->uint8array[i] != b->uint8array[i])
					return false;
			return true;
		case kInt32Array:
			if (a->size != b->size) return false;
			if (a->int32array == b->int32array) return true;
			for (int i = 0; i < a->size; i++)
				if (a->int32array[i] != b->int32array[i])
					return false;
			return true;
		case kInt64Array:
			if (a->size != b->size) return false;
			if (a->int64array == b->int64array) return true;
			for (int i = 0; i < a->size; i++)
				if (a->int64array[i] != b->int64array[i])
					return false;
			return true;
		case kFloat64Array:
			if (a->size != b->size) return false;
			if (a->float64array == b->float64array) return true;
			for (int i = 0; i < a->size; i++)
				if (a->float64array[i] != b->float64array[i])
					return false;
			return true;
		case kList:
			// the order of list elements is important
			if (a->size != b->size) return false;
			if (a->list == b->list) return true;

			for (int i = 0; i < a->size; i++)
				if (!stdvalue_equals(&(a->list[i]), &(b->list[i])));
					return false;
			
			return true;
		case kMap: {
			// the order is not important here, which makes it a bit difficult to compare
			if (a->size != b->size) return false;
			if ((a->keys == b->keys) && (a->values == b->values)) return true;

			// _keyInBAlsoInA[i] == true means that there's a key in a that matches b->keys[i]
			//   so if we're searching for a key in b, we can safely ignore / don't need to compare
			//   keys in b that have they're _keyInBAlsoInA set to true.
			bool _keyInBAlsoInA[a->size];
			memset(_keyInBAlsoInA, false, a->size * sizeof(bool));

			for (int i = 0; i < a->size; i++) {
				// The key we're searching for in b.
				struct StdMsgCodecValue *key = &(a->keys[i]);
				
				int j = 0;
				while (j < a->size) {
					while (_keyInBAlsoInA[j] && (j < a->size))  j++;	// skip all keys with _keyInBAlsoInA set to true.
					if (!stdvalue_equals(key, &(b->keys[j])))   j++;	// if b->keys[j] is not equal to "key", continue searching
					else {
						_keyInBAlsoInA[j] = true;

						// the values of "key" in a and b must (of course) also be equivalent.
						if (!stdvalue_equals(&(a->values[i]), &(b->values[j]))) return false;
						break;
					}
				}

				// we did not find a->keys[i] in b.
				if (j + 1 >= a->size) return false;
			}

			return true;
		}
		default: return false;
	}

	return false;
}
struct StdMsgCodecValue *stdmap_get(struct StdMsgCodecValue *map, struct StdMsgCodecValue *key) {
	for (int i=0; i < map->size; i++)
		if (stdvalue_equals(&map->keys[i], key))
			return &map->values[i];

	return NULL;
}
