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


struct platch_msg_resp_handler_data {
	enum platch_codec codec;
	platch_msg_resp_callback on_response;
	void *userdata;
};


static int _check_remaining(size_t *remaining, int min_remaining) {
	if (remaining == NULL) {
		return 0;
	}
	if (*remaining < min_remaining) {
		return EBADMSG;
	}
	return 0;
}
static int _read(uint8_t **pbuffer, void *dest, int n_bytes, size_t *remaining) {
	int ok;

	ok = _check_remaining(remaining, n_bytes);
	if (ok != 0) {
		return ok;
	}

	memcpy(dest, *pbuffer, (size_t) n_bytes);
	if (remaining != NULL) {
		*remaining -= n_bytes;
	}
	*pbuffer += n_bytes;
	return 0;
}
static int _write(uint8_t **pbuffer, void *src, int n_bytes, size_t *remaining) {
	int ok;

	ok = _check_remaining(remaining, n_bytes);
	if (ok != 0) {
		return ok;
	}

	memcpy(*pbuffer, src, (size_t) n_bytes);
	if (remaining != NULL) {
		*remaining -= n_bytes;
	}
	*pbuffer += n_bytes;
	return 0;
}

static int _advance(uintptr_t *value, int n_bytes, size_t *remaining) {
	int ok;

	ok = _check_remaining(remaining, n_bytes);
	if (ok != 0) {
		return ok;
	}

	if (remaining != NULL) {
    	*remaining -= n_bytes;
	}
    *value += n_bytes;
    return 0;
}
static int _align(uintptr_t *value, int alignment, size_t *remaining) {
    int diff;

    alignment--;
	diff = ((((*value) + alignment) | alignment) - alignment) - *value;

    return _advance(value, diff, remaining);
}
static int _advance_size_bytes(uintptr_t *value, size_t size, size_t *remaining) {
    if (size < 254) {
		return _advance(value, 1, remaining);
	} else if (size <= 0xFFFF) {
		return _advance(value, 3, remaining);
	} else {
		return _advance(value, 5, remaining);
    }
}

#define DEFINE_READ_WRITE_FUNC(suffix, value_type) \
MAYBE_UNUSED static int _write_##suffix(uint8_t **pbuffer, value_type value, size_t *remaining) { \
	return _write(pbuffer, &value, sizeof value, remaining); \
} \
MAYBE_UNUSED static int _read_##suffix(uint8_t **pbuffer, value_type* value, size_t *remaining) { \
	return _read(pbuffer, value, sizeof *value, remaining); \
}

DEFINE_READ_WRITE_FUNC(    u8,  uint8_t)
DEFINE_READ_WRITE_FUNC(   u16, uint16_t)
DEFINE_READ_WRITE_FUNC(   u32, uint32_t)
DEFINE_READ_WRITE_FUNC(   u64, uint64_t)
DEFINE_READ_WRITE_FUNC(    i8,   int8_t)
DEFINE_READ_WRITE_FUNC(   i16,  int16_t)
DEFINE_READ_WRITE_FUNC(   i32,  int32_t)
DEFINE_READ_WRITE_FUNC(   i64,  int64_t)
DEFINE_READ_WRITE_FUNC( float,    float)
DEFINE_READ_WRITE_FUNC(double,   double)

static int _writeSize(uint8_t **pbuffer, int size, size_t *remaining) {
	int ok;

    if (size < 254) {
		return _write_u8(pbuffer, (uint8_t) size, remaining);
	} else if (size <= 0xFFFF) {
		ok = _write_u8(pbuffer, 0xFE, remaining);
        if (ok != 0) return ok;

		ok = _write_u16(pbuffer, (uint16_t) size, remaining);
        if (ok != 0) return ok;
	} else {
		ok = _write_u8(pbuffer, 0xFF, remaining);
        if (ok != 0) return ok;

		ok = _write_u32(pbuffer, (uint32_t) size, remaining);
        if (ok != 0) return ok;
    }

    return ok;
}
static int _readSize(uint8_t **pbuffer, uint32_t *psize, size_t *remaining) {
	int ok;
    uint8_t size8;
    uint16_t size16;

	ok = _read_u8(pbuffer, &size8, remaining);
    if (ok != 0) return ok;
    
    if (size8 <= 253) {
        *psize = size8;

        return 0;
    } else if (size8 == 254) {
		ok = _read_u16(pbuffer, &size16, remaining);
        if (ok != 0) return ok;

        *psize = size16;
        return 0;
	} else if (size8 == 255) {
		return _read_u32(pbuffer, psize, remaining);
	}

    return 0;
}


int platch_free_value_std(struct std_value *value) {
	int ok;

	switch (value->type) {
		case kStdString:
			free(value->string_value);
			break;
		case kStdList:
			for (int i=0; i < value->size; i++) {
				ok = platch_free_value_std(&(value->list[i]));
				if (ok != 0) return ok;
			}
			free(value->list);
			break;
		case kStdMap:
			for (int i=0; i < value->size; i++) {
				ok = platch_free_value_std(&(value->keys[i]));
				if (ok != 0) return ok;
				ok = platch_free_value_std(&(value->values[i]));
				if (ok != 0) return ok;
			}
			free(value->keys);
			break;
		default:
			break;
	}

	return 0;
}
int platch_free_json_value(struct json_value *value, bool shallow) {
	int ok;
	
	switch (value->type) {
		case kJsonArray:
			if (!shallow) {
				for (int i = 0; i < value->size; i++) {
					ok = platch_free_json_value(&(value->array[i]), false);
					if (ok != 0) return ok;
				}
			}

			free(value->array);
			break;
		case kJsonObject:
			if (!shallow) {
				for (int i = 0; i < value->size; i++) {
					ok = platch_free_json_value(&(value->values[i]), false);
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
int platch_free_obj(struct platch_obj *object) {
	switch (object->codec) {
		case kStringCodec:
			free(object->string_value);
			break;
		case kBinaryCodec:
			break;
		case kJSONMessageCodec:
			platch_free_json_value(&(object->json_value), false);
			break;
		case kStandardMessageCodec:
			platch_free_value_std(&(object->std_value));
			break;
		case kStandardMethodCall:
			free(object->method);
			platch_free_value_std(&(object->std_arg));
			break;
		case kJSONMethodCall:
			platch_free_json_value(&(object->json_arg), false);
			break;
		default:
			break;
	}

	return 0;
}

int platch_calc_value_size_std(struct std_value* value, size_t* size_out) {
	enum std_value_type type = value->type;
	uintptr_t size = (uintptr_t) *size_out;
	size_t element_size, sizet_size = 0;
	int ok;

	// Type Byte
	_advance(&size, 1, NULL);
	switch (type) {
		case kStdNull:
		case kStdTrue:
		case kStdFalse:
			break;
		case kStdInt32:
			_advance(&size, 4, NULL);
			break;
		case kStdInt64:
			_advance(&size, 8, NULL);
			break;
		case kStdFloat64:
			_align  (&size, 8, NULL);
			_advance(&size, 8, NULL);
			break;
		case kStdString:
		case kStdLargeInt:
			element_size = strlen(value->string_value);
			_advance_size_bytes(&size, element_size, NULL);
			_advance(&size, element_size, NULL);
			break;
		case kStdUInt8Array:
			element_size = value->size;
			_advance_size_bytes(&size, element_size, NULL);
			_advance(&size, element_size, NULL);
			break;
		case kStdInt32Array:
			element_size = value->size;

			_advance_size_bytes(&size, element_size, NULL);
			_align  (&size, 4, NULL);
			_advance(&size, element_size*4, NULL);

			break;
		case kStdInt64Array:
			element_size = value->size;
			
			_advance_size_bytes(&size, element_size, NULL);
			_align  (&size, 8, NULL);
			_advance(&size, element_size*8, NULL);

			break;
		case kStdFloat64Array:
			element_size = value->size;
			
			_advance_size_bytes(&size, element_size, NULL);
			_align  (&size, 8, NULL);
			_advance(&size, element_size*8, NULL);

			break;
		case kStdList:
			element_size = value->size;

			_advance_size_bytes(&size, element_size, NULL);
			for (int i = 0; i<element_size; i++) {
				sizet_size = (size_t) size;

				ok = platch_calc_value_size_std(&(value->list[i]), &sizet_size);
				if (ok != 0) return ok;

				size = (uintptr_t) sizet_size;
			}

			break;
		case kStdMap:
			element_size = value->size;

			_advance_size_bytes(&size, element_size, NULL);
			for (int i = 0; i<element_size; i++) {
				sizet_size = (size_t) size;

				ok = platch_calc_value_size_std(&(value->keys[i]), &sizet_size);
				if (ok != 0) return ok;

				ok = platch_calc_value_size_std(&(value->values[i]), &sizet_size);
				if (ok != 0) return ok;

				size = (uintptr_t) sizet_size;
			}

			break;
		default:
			return EINVAL;
	}

	*size_out = (size_t) size;

	return 0;
}
int platch_write_value_to_buffer_std(struct std_value* value, uint8_t **pbuffer) {
	uint8_t* byteArray;
	size_t size;
	int ok;

	_write_u8(pbuffer, value->type, NULL);

	switch (value->type) {
		case kStdNull:
		case kStdTrue:
		case kStdFalse:
			break;
		case kStdInt32:
			_write_i32(pbuffer, value->int32_value, NULL);
			break;
		case kStdInt64:
			_write_i64(pbuffer, value->int64_value, NULL);
			break;
		case kStdFloat64:
			_align  ((uintptr_t*) pbuffer, 8, NULL);
			_write_double(pbuffer, value->float64_value, NULL);
			break;
		case kStdLargeInt:
		case kStdString:
		case kStdUInt8Array:
			if ((value->type == kStdLargeInt) || (value->type == kStdString)) {
				size = strlen(value->string_value);
				byteArray = (uint8_t*) value->string_value;
			} else {
				DEBUG_ASSERT(value->type == kStdUInt8Array);
				size = value->size;
				byteArray = value->uint8array;
			}

			_writeSize(pbuffer, size, NULL);
			for (int i=0; i<size; i++) {
				_write_u8(pbuffer, byteArray[i], NULL);
			}
			break;
		case kStdInt32Array:
			size = value->size;

			_writeSize(pbuffer, size, NULL);
			_align   ((uintptr_t*) pbuffer, 4, NULL);
			
			for (int i=0; i<size; i++) {
				_write_i32(pbuffer, value->int32array[i], NULL);
			}
			break;
		case kStdInt64Array:
			size = value->size;

			_writeSize(pbuffer, size, NULL);
			_align((uintptr_t*) pbuffer, 8, NULL);
			for (int i=0; i<size; i++) {
				_write_i64(pbuffer, value->int64array[i], NULL);
			}
			break;
		case kStdFloat64Array:
			size = value->size;

			_writeSize(pbuffer, size, NULL);
			_align((uintptr_t*) pbuffer, 8, NULL);

			for (int i=0; i<size; i++) {
				_write_double(pbuffer, value->float64array[i], NULL);
			}
			break;
		case kStdList:
			size = value->size;

			_writeSize(pbuffer, size, NULL);
			for (int i=0; i < size; i++) {
				ok = platch_write_value_to_buffer_std(&value->list[i], pbuffer);
				if (ok != 0) return ok;
			}

			break;
		case kStdMap:
			size = value->size;

			_writeSize(pbuffer, size, NULL);
			for (int i=0; i<size; i++) {
				ok = platch_write_value_to_buffer_std(&value->keys[i], pbuffer);
				if (ok != 0) return ok;

				ok = platch_write_value_to_buffer_std(&value->values[i], pbuffer);
				if (ok != 0) return ok;
			}
			break;
		default:
			return EINVAL;
	}

	return 0;
}
size_t platch_calc_value_size_json(struct json_value *value) {
	size_t size = 0;

	switch (value->type) {
		case kJsonNull:
		case kJsonTrue:
			return 4;
		case kJsonFalse:
			return 5;
		case kJsonNumber: ;
			char numBuffer[32];
			return sprintf(numBuffer, "%g", value->number_value);
		case kJsonString:
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
		case kJsonArray:
			size += 2;
			for (int i=0; i < value->size; i++) {
				size += platch_calc_value_size_json(value->array + i);
				if (i+1 != value->size) size += 1;
			}
			return size;
		case kJsonObject:
			size += 2;
			for (int i=0; i < value->size; i++) {
				size += strlen(value->keys[i]) + 3 + platch_calc_value_size_json(&(value->values[i]));
				if (i+1 != value->size) size += 1;
			}
			return size;
		default:
			return EINVAL;
	}

	return 0;
}
int platch_write_value_to_buffer_json(struct json_value* value, uint8_t **pbuffer) {
	switch (value->type) {
		case kJsonNull:
			*pbuffer += sprintf((char*) *pbuffer, "null");
			break;
		case kJsonTrue:
			*pbuffer += sprintf((char*) *pbuffer, "true");
			break;
		case kJsonFalse:
			*pbuffer += sprintf((char*) *pbuffer, "false");
			break;
		case kJsonNumber:
			*pbuffer += sprintf((char*) *pbuffer, "%g", value->number_value);
			break;
		case kJsonString:
			*((*pbuffer)++) = '\"';

			for (char *s = value->string_value; *s; s++) {
				switch (*s) {
					case '\b':
						*((*pbuffer)++) = '\\';
						*((*pbuffer)++) = 'b';
						break;
					case '\f':
						*((*pbuffer)++) = '\\';
						*((*pbuffer)++) = 'f';
						break;
					case '\n':
						*((*pbuffer)++) = '\\';
						*((*pbuffer)++) = 'n';
						break;
					case '\r':
						*((*pbuffer)++) = '\\';
						*((*pbuffer)++) = 'r';
						break;
					case '\t':
						*((*pbuffer)++) = '\\';
						*((*pbuffer)++) = 't';
						break;
					case '\"':
						*((*pbuffer)++) = '\\';
						*((*pbuffer)++) = 't';
						break;
					case '\\':
						*((*pbuffer)++) = '\\';
						*((*pbuffer)++) = '\\';
						break;
					default:
						*((*pbuffer)++) = *s;
						break;
				}
			}

			*((*pbuffer)++) = '\"';

			break;
		case kJsonArray:
			*pbuffer += sprintf((char*) *pbuffer, "[");
			for (int i=0; i < value->size; i++) {
				platch_write_value_to_buffer_json(&(value->array[i]), pbuffer);
				if (i+1 != value->size) *pbuffer += sprintf((char*) *pbuffer, ",");
			}
			*pbuffer += sprintf((char*) *pbuffer, "]");
			break;	
		case kJsonObject:
			*pbuffer += sprintf((char*) *pbuffer, "{");
			for (int i=0; i < value->size; i++) {
				*pbuffer += sprintf((char*) *pbuffer, "\"%s\":", value->keys[i]);
				platch_write_value_to_buffer_json(&(value->values[i]), pbuffer);
				if (i+1 != value->size) *pbuffer += sprintf((char*) *pbuffer, ",");
			}
			*pbuffer += sprintf((char*) *pbuffer, "}");
			break;
		default:
			return EINVAL;
	}

	return 0;
}
int platch_decode_value_std(uint8_t **pbuffer, size_t *premaining, struct std_value *value_out) {
	enum std_value_type type;
	uint8_t type_byte;
	uint32_t size;
	int ok;

	/// FIXME: Somehow, in release mode, this always reads 0.
	ok = _read_u8(pbuffer, &type_byte, premaining);
	if (ok != 0) return ok;

	type = (enum std_value_type) type_byte;
	value_out->type = (enum std_value_type) type_byte;
	switch (type) {
		case kStdNull:
		case kStdTrue:
		case kStdFalse:
			break;
		case kStdInt32:
			ok = _read_i32(pbuffer, &value_out->int32_value, premaining);
			if (ok != 0) return ok;

			break;
		case kStdInt64:
			ok = _read_i64(pbuffer, &value_out->int64_value, premaining);
			if (ok != 0) return ok;

			break;
		case kStdFloat64:
			ok = _align((uintptr_t*) pbuffer, 8, premaining);
			if (ok != 0) return ok;

			ok = _read_double(pbuffer, &value_out->float64_value, premaining);
			if (ok != 0) return ok;

			break;
		case kStdLargeInt:
		case kStdString:
			ok = _readSize(pbuffer, &size, premaining);
			if (ok != 0) return ok;

			value_out->string_value = calloc(size+1, sizeof(char));
			if (!value_out->string_value) return ENOMEM;

			ok = _read(pbuffer, value_out->string_value, size, premaining);
			if (ok != 0) {
				free(value_out->string_value);
				return ok;
			}

			break;
		case kStdUInt8Array:
			ok = _readSize(pbuffer, &size, premaining);
			if (ok != 0) return ok;
			if (*premaining < size) return EBADMSG;

			value_out->size = size;
			value_out->uint8array = *pbuffer;

			ok = _advance((uintptr_t*) pbuffer, size, premaining);
			if (ok != 0) return ok;

			break;
		case kStdInt32Array:
			ok = _readSize(pbuffer, &size, premaining);
			if (ok != 0) return ok;
			
			ok = _align((uintptr_t*) pbuffer, 4, premaining);
			if (ok != 0) return ok;

			if (*premaining < size*4) return EBADMSG;

			value_out->size = size;
			value_out->int32array = (int32_t*) *pbuffer;

			ok = _advance((uintptr_t*) pbuffer, size*4, premaining);
			if (ok != 0) return ok;

			break;
		case kStdInt64Array:
			ok = _readSize(pbuffer, &size, premaining);
			if (ok != 0) return ok;

			ok = _align((uintptr_t*) pbuffer, 8, premaining);
			if (ok != 0) return ok;

			if (*premaining < size*8) return EBADMSG;

			value_out->size = size;
			value_out->int64array = (int64_t*) *pbuffer;

			ok = _advance((uintptr_t*) pbuffer, size*8, premaining);
			if (ok != 0) return ok;

			break;
		case kStdFloat64Array:
			ok = _readSize(pbuffer, &size, premaining);
			if (ok != 0) return ok;

			ok = _align((uintptr_t*) pbuffer, 8, premaining);
			if (ok != 0) return ok;

			if (*premaining < size*8) return EBADMSG;

			value_out->size = size;
			value_out->float64array = (double*) *pbuffer;

			ok = _advance((uintptr_t*) pbuffer, size*8, premaining);
			if (ok != 0) return ok;
			
			break;
		case kStdList:
			ok = _readSize(pbuffer, &size, premaining);
			if (ok != 0) return ok;

			value_out->size = size;
			value_out->list = calloc(size, sizeof(struct std_value));

			for (int i = 0; i < size; i++) {
				ok = platch_decode_value_std(pbuffer, premaining, &value_out->list[i]);
				if (ok != 0) return ok;
			}

			break;
		case kStdMap:
			ok = _readSize(pbuffer, &size, premaining);
			if (ok != 0) return ok;

			value_out->size = size;

			value_out->keys = calloc(size*2, sizeof(struct std_value));
			if (!value_out->keys) return ENOMEM;

			value_out->values = &value_out->keys[size];

			for (int i = 0; i < size; i++) {
				ok = platch_decode_value_std(pbuffer, premaining, &(value_out->keys[i]));
				if (ok != 0) return ok;
				
				ok = platch_decode_value_std(pbuffer, premaining, &(value_out->values[i]));
				if (ok != 0) return ok;
			}

			break;
		default:
			return EBADMSG;
	}

	return 0;
}
int platch_decode_value_json(char *message, size_t size, jsmntok_t **pptoken, size_t *ptokensremaining, struct json_value *value_out) {
	jsmntok_t *ptoken;
	int result, ok;
	
	if (!pptoken) {
		// if we have no token list yet, parse the message & create one.

		jsmntok_t tokens[JSON_DECODE_TOKENLIST_SIZE];
		jsmn_parser parser;
		size_t tokensremaining;

		memset(tokens, 0, sizeof(tokens));

		jsmn_init(&parser);
		result = jsmn_parse(&parser, (const char *) message, (const size_t) size, tokens, JSON_DECODE_TOKENLIST_SIZE);
		if (result < 0) return EBADMSG;
		
		tokensremaining = (size_t) result;
		ptoken = tokens;

		ok = platch_decode_value_json(message, size, &ptoken, &tokensremaining, value_out);
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
					value_out->type = kJsonNull;
				} else if (message[ptoken->start] == 't') {
					value_out->type = kJsonTrue;
				} else if (message[ptoken->start] == 'f') {
					value_out->type = kJsonFalse;
				} else {
					value_out->type = kJsonNumber;

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

				value_out->type = kJsonString;
				value_out->string_value = string;

				break;
			case JSMN_ARRAY: ;
				struct json_value *array = calloc(ptoken->size, sizeof(struct json_value));
				if (!array) return ENOMEM;

				for (int i=0; i < ptoken->size; i++) {
					ok = platch_decode_value_json(message, size, pptoken, ptokensremaining, &array[i]);
					if (ok != 0) return ok;
				}

				value_out->type = kJsonArray;
				value_out->size = ptoken->size;
				value_out->array = array;

				break;
			case JSMN_OBJECT: ;
				struct json_value  key;
				char                    **keys = calloc(ptoken->size, sizeof(char *));
				struct json_value *values = calloc(ptoken->size, sizeof(struct json_value));
				if ((!keys) || (!values)) return ENOMEM;

				for (int i=0; i < ptoken->size; i++) {
					ok = platch_decode_value_json(message, size, pptoken, ptokensremaining, &key);
					if (ok != 0) return ok;

					if (key.type != kJsonString) return EBADMSG;
					keys[i] = key.string_value;

					ok = platch_decode_value_json(message, size, pptoken, ptokensremaining, &values[i]);
					if (ok != 0) return ok;
				}

				value_out->type = kJsonObject;
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

int platch_decode_json(char *string, struct json_value *out) {
	return platch_decode_value_json(string, strlen(string), NULL, NULL, out);
}

int platch_decode(uint8_t *buffer, size_t size, enum platch_codec codec, struct platch_obj *object_out) {
	struct json_value root_jsvalue;
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
			ok = platch_decode_value_json((char *) buffer, size, NULL, NULL, &(object_out->json_value));
			if (ok != 0) return ok;

			break;
		case kJSONMethodCall: ;
			ok = platch_decode_value_json((char *) buffer, size, NULL, NULL, &root_jsvalue);
			if (ok != 0) return ok;

			if (root_jsvalue.type != kJsonObject) return EBADMSG;
			
			for (int i=0; i < root_jsvalue.size; i++) {
				if ((strcmp(root_jsvalue.keys[i], "method") == 0) && (root_jsvalue.values[i].type == kJsonString)) {
					object_out->method = root_jsvalue.values[i].string_value;
				} else if (strcmp(root_jsvalue.keys[i], "args") == 0) {
					object_out->json_arg = root_jsvalue.values[i];
				} else return EBADMSG;
			}

			platch_free_json_value(&root_jsvalue, true);

			break;
		case kJSONMethodCallResponse: ;
			ok = platch_decode_value_json((char *) buffer, size, NULL, NULL, &root_jsvalue);
			if (ok != 0) return ok;
			if (root_jsvalue.type != kJsonArray) return EBADMSG;
			
			if (root_jsvalue.size == 1) {
				object_out->success = true;
				object_out->json_result = root_jsvalue.array[0];
				return platch_free_json_value(&root_jsvalue, true);
			} else if ((root_jsvalue.size == 3) &&
					   (root_jsvalue.array[0].type == kJsonString) &&
					   ((root_jsvalue.array[1].type == kJsonString) || (root_jsvalue.array[1].type == kJsonNull))) {
				
				
				object_out->success = false;
				object_out->error_code = root_jsvalue.array[0].string_value;
				object_out->error_msg = root_jsvalue.array[1].string_value;
				object_out->json_error_details = root_jsvalue.array[2];
				return platch_free_json_value(&root_jsvalue, true);
			} else return EBADMSG;

			break;
		case kStandardMessageCodec:
			ok = platch_decode_value_std(&buffer_cursor, &remaining, &object_out->std_value);
			if (ok != 0) return ok;
			break;
		case kStandardMethodCall: ;
			struct std_value methodname;

			ok = platch_decode_value_std(&buffer_cursor, &remaining, &methodname);
			if (ok != 0) return ok;
			if (methodname.type != kStdString) {
				platch_free_value_std(&methodname);
				return EBADMSG;
			}
			object_out->method = methodname.string_value;

			ok = platch_decode_value_std(&buffer_cursor, &remaining, &object_out->std_arg);
			if (ok != 0) return ok;

			break;
		case kStandardMethodCallResponse: ;
			ok = _read_u8(&buffer_cursor, (uint8_t*) &object_out->success, &remaining);

			if (object_out->success) {
				ok = platch_decode_value_std(&buffer_cursor, &remaining, &(object_out->std_result));
				if (ok != 0) return ok;
			} else {
				struct std_value error_code, error_msg;

				ok = platch_decode_value_std(&buffer_cursor, &remaining, &error_code);
				if (ok != 0) return ok;
				ok = platch_decode_value_std(&buffer_cursor, &remaining, &error_msg);
				if (ok != 0) return ok;
				ok = platch_decode_value_std(&buffer_cursor, &remaining, &(object_out->std_error_details));
				if (ok != 0) return ok;

				if ((error_code.type == kStdString) && ((error_msg.type == kStdString) || (error_msg.type == kStdNull))) {
					object_out->error_code = error_code.string_value;
					object_out->error_msg = (error_msg.type == kStdString) ? error_msg.string_value : NULL;
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

int platch_encode(struct platch_obj *object, uint8_t **buffer_out, size_t *size_out) {
	struct std_value stdmethod, stderrcode, stderrmessage;
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
			size = platch_calc_value_size_json(&(object->json_value));
			size += 1;  // JSONMsgCodec uses sprintf, which null-terminates strings,
						// so lets allocate one more byte for the last null-terminator.
						// this is decremented again in the second switch-case, so flutter
						// doesn't complain about a malformed message.
			break;
		case kStandardMessageCodec:
			ok = platch_calc_value_size_std(&(object->std_value), &size);
			if (ok != 0) return ok;
			break;
		case kStandardMethodCall:
			stdmethod.type = kStdString;
			stdmethod.string_value = object->method;
			
			ok = platch_calc_value_size_std(&stdmethod, &size);
			if (ok != 0) return ok;

			ok = platch_calc_value_size_std(&(object->std_arg), &size);
			if (ok != 0) return ok;

			break;
		case kStandardMethodCallResponse:
			size += 1;

			if (object->success) {
				ok = platch_calc_value_size_std(&(object->std_result), &size);
				if (ok != 0) return ok;
			} else {
				stderrcode = (struct std_value) {
					.type = kStdString,
					.string_value = object->error_code
				};
				stderrmessage = (struct std_value) {
					.type = kStdString,
					.string_value = object->error_msg
				};
				
				ok = platch_calc_value_size_std(&stderrcode, &size);
				if (ok != 0) return ok;
				ok = platch_calc_value_size_std(&stderrmessage, &size);
				if (ok != 0) return ok;
				ok = platch_calc_value_size_std(&(object->std_error_details), &size);
				if (ok != 0) return ok;
			}
			break;
		case kJSONMethodCall:
			size = platch_calc_value_size_json(
				&JSONOBJECT2(
					"method", JSONSTRING(object->method),
					"args", object->json_arg
				)
			);
			size += 1;
			break;
		case kJSONMethodCallResponse:
			if (object->success) {
				size = 1 + platch_calc_value_size_json(&JSONARRAY1(object->json_result));
			} else {
				size = 1 + platch_calc_value_size_json(
					&JSONARRAY3(
						JSONSTRING(object->error_code),
						(object->error_msg != NULL) ? JSONSTRING(object->error_msg) : JSONNULL,
						object->json_error_details
					)
				);
			}
			break;
		default:
			return EINVAL;
	}

	buffer = malloc(size);
	if (buffer == NULL) {
		return ENOMEM;
	}

	buffer_cursor = buffer;
	
	switch (object->codec) {
		case kStringCodec:
			memcpy(buffer, object->string_value, size);
			break;
		case kStandardMessageCodec:
			ok = platch_write_value_to_buffer_std(&(object->std_value), &buffer_cursor);
			if (ok != 0) goto free_buffer_and_return_ok;
			break;
		case kStandardMethodCall:
			ok = platch_write_value_to_buffer_std(&stdmethod, &buffer_cursor);
			if (ok != 0) goto free_buffer_and_return_ok;

			ok = platch_write_value_to_buffer_std(&(object->std_arg), &buffer_cursor);
			if (ok != 0) goto free_buffer_and_return_ok;

			break;
		case kStandardMethodCallResponse:
			if (object->success) {
				_write_u8(&buffer_cursor, 0x00, NULL);

				ok = platch_write_value_to_buffer_std(&(object->std_result), &buffer_cursor);
				if (ok != 0) goto free_buffer_and_return_ok;
			} else {
				_write_u8(&buffer_cursor, 0x01, NULL);

				ok = platch_write_value_to_buffer_std(&stderrcode, &buffer_cursor);
				if (ok != 0) goto free_buffer_and_return_ok;
				ok = platch_write_value_to_buffer_std(&stderrmessage, &buffer_cursor);
				if (ok != 0) goto free_buffer_and_return_ok;
				ok = platch_write_value_to_buffer_std(&(object->std_error_details), &buffer_cursor);
				if (ok != 0) goto free_buffer_and_return_ok;
			}
			
			break;
		case kJSONMessageCodec:
			size -= 1;
			ok = platch_write_value_to_buffer_json(&(object->json_value), &buffer_cursor);
			if (ok != 0) goto free_buffer_and_return_ok;
			break;
		case kJSONMethodCall:
			size -= 1;
			ok = platch_write_value_to_buffer_json(
				&JSONOBJECT2(
					"method", JSONSTRING(object->method),
					"args", object->json_arg
				),
				&buffer_cursor
			);
			if (ok != 0) {
				goto free_buffer_and_return_ok;
			}
			break;
		case kJSONMethodCallResponse: 
			if (object->success) {
				ok = platch_write_value_to_buffer_json(
					&JSONARRAY1(object->json_result),
					&buffer_cursor
				);
			} else {
				ok = platch_write_value_to_buffer_json(
					&JSONARRAY3(
						JSONSTRING(object->error_code),
						(object->error_msg != NULL) ? JSONSTRING(object->error_msg) : JSONNULL,
						object->json_error_details
					),
					&buffer_cursor
				);
			}
			size -= 1;
			if (ok != 0) {
				goto free_buffer_and_return_ok;
			}
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

void platch_on_response_internal(const uint8_t *buffer, size_t size, void *userdata) {
	struct platch_msg_resp_handler_data *handlerdata;
	struct platch_obj object;
	int ok;

	handlerdata = (struct platch_msg_resp_handler_data *) userdata;
	ok = platch_decode((uint8_t*) buffer, size, handlerdata->codec, &object);
	if (ok != 0) return;

	ok = handlerdata->on_response(&object, handlerdata->userdata);
	if (ok != 0) return;

	free(handlerdata);

	ok = platch_free_obj(&object);
	if (ok != 0) return;
}

int platch_send(char *channel, struct platch_obj *object, enum platch_codec response_codec, platch_msg_resp_callback on_response, void *userdata) {
	FlutterPlatformMessageResponseHandle *response_handle = NULL;
	struct platch_msg_resp_handler_data *handlerdata = NULL;
	FlutterEngineResult result;
	uint8_t *buffer;
	size_t   size;
	int ok;

	ok = platch_encode(object, &buffer, &size);
	if (ok != 0) return ok;

	if (on_response) {
		handlerdata = malloc(sizeof(struct platch_msg_resp_handler_data));
		if (!handlerdata) {
			return ENOMEM;
		}
		
		handlerdata->codec = response_codec;
		handlerdata->on_response = on_response;
		handlerdata->userdata = userdata;

		result = flutterpi.flutter.procs.PlatformMessageCreateResponseHandle(flutterpi.flutter.engine, platch_on_response_internal, handlerdata, &response_handle);
		if (result != kSuccess) {
			fprintf(stderr, "[flutter-pi] Error create platform message response handle. FlutterPlatformMessageCreateResponseHandle: %s\n", FLUTTER_RESULT_TO_STRING(result));
			goto fail_free_handlerdata;
		}
	}

	ok = flutterpi_send_platform_message(
		&flutterpi,
		channel,
		buffer,
		size,
		response_handle
	);
	if (ok != 0) {
		goto fail_release_handle;
	}

	// TODO: This won't work if we're not on the main thread
	if (on_response) {
		result = flutterpi.flutter.procs.PlatformMessageReleaseResponseHandle(flutterpi.flutter.engine, response_handle);
		if (result != kSuccess) {
			fprintf(stderr, "[flutter-pi] Error releasing platform message response handle. FlutterPlatformMessageReleaseResponseHandle: %s\n", FLUTTER_RESULT_TO_STRING(result));
			ok = EIO;
			goto fail_free_buffer;
		}
	}

	if (object->codec != kBinaryCodec) {
		free(buffer);
	}
	
	return 0;


	fail_release_handle:
	if (on_response) {
		flutterpi.flutter.procs.PlatformMessageReleaseResponseHandle(flutterpi.flutter.engine, response_handle);
	}

	fail_free_buffer:
	if (object->codec != kBinaryCodec) {
		free(buffer);
	}

	fail_free_handlerdata:
	if (on_response) {
		free(handlerdata);
	}

	return ok;
}

int platch_call_std(char *channel, char *method, struct std_value *argument, platch_msg_resp_callback on_response, void *userdata) {
	struct platch_obj object = {
		.codec = kStandardMethodCall,
		.method = method,
		.std_arg = *argument
	};
	
	return platch_send(channel, &object, kStandardMethodCallResponse, on_response, userdata);
}

int platch_call_json(char *channel, char *method, struct json_value *argument, platch_msg_resp_callback on_response, void *userdata) {
	return platch_send(channel,
								&(struct platch_obj) {
									.codec = kJSONMethodCall,
									.method = method,
									.json_arg = *argument
								},
								kJSONMethodCallResponse,
								on_response,
								userdata);
}

int platch_respond(FlutterPlatformMessageResponseHandle *handle, struct platch_obj *response) {
	uint8_t *buffer = NULL;
	size_t   size = 0;
	int ok;

	ok = platch_encode(response, &buffer, &size);
	if (ok != 0) return ok;

	ok = flutterpi_respond_to_platform_message(handle, buffer, size);

	if (buffer != NULL) {
		free(buffer);
	}

	return 0;
}

int platch_respond_not_implemented(FlutterPlatformMessageResponseHandle *handle) {
	return platch_respond(
		(FlutterPlatformMessageResponseHandle *) handle,
		&(struct platch_obj) {
			.codec = kNotImplemented
		});
}

/****************************
 * STANDARD METHOD CHANNELS *
 ****************************/

int platch_respond_success_std(FlutterPlatformMessageResponseHandle *handle,
							   struct std_value *return_value) {
	return platch_respond(
		handle,
		&(struct platch_obj) {
			.codec = kStandardMethodCallResponse,
			.success = true,
			.std_result = return_value? *return_value : STDNULL
		}
	);
}

int platch_respond_error_std(FlutterPlatformMessageResponseHandle *handle,
							 char *error_code,
							 char *error_msg,
							 struct std_value *error_details) {
	return platch_respond(handle, &(struct platch_obj) {
		.codec = kStandardMethodCallResponse,
		.success = false,
		.error_code = error_code,
		.error_msg = error_msg,
		.std_error_details = error_details? *error_details : STDNULL
	});
}

/// Sends a platform message to `handle` with error code "illegalargument"
/// and error message `errmsg`.
int platch_respond_illegal_arg_std(FlutterPlatformMessageResponseHandle *handle,
								   char *error_msg) {
	return platch_respond_error_std(handle, "illegalargument", error_msg, NULL);
}

/// Sends a platform message to `handle` with error code "nativeerror"
/// and error messsage `strerror(_errno)`
int platch_respond_native_error_std(FlutterPlatformMessageResponseHandle *handle,
									int _errno) {
	return platch_respond_error_std(
		handle,
		"nativeerror",
		strerror(_errno),
		&STDINT32(_errno)
	);
}


/************************
 * JSON METHOD CHANNELS *
 ************************/

int platch_respond_success_json(FlutterPlatformMessageResponseHandle *handle,
								struct json_value *return_value) {
	return platch_respond(
		handle,
		&(struct platch_obj) {
			.codec = kJSONMethodCallResponse,
			.success = true,
			.json_result = return_value
				? *return_value
				: JSONNULL
		}
	);
}

int platch_respond_error_json(FlutterPlatformMessageResponseHandle *handle,
							  char *error_code,
							  char *error_msg,
							  struct json_value *error_details) {
	return platch_respond(handle, &(struct platch_obj) {
		.codec = kJSONMethodCallResponse,
		.success = false,
		.error_code = error_code,
		.error_msg = error_msg,
		.json_error_details = (error_details) ?
			*error_details :
			(struct json_value) {.type = kJsonNull}
	});
}

int platch_respond_illegal_arg_json(FlutterPlatformMessageResponseHandle *handle,
                                    char *error_msg) {
	return platch_respond_error_json(handle, "illegalargument", error_msg, NULL);
}

int platch_respond_native_error_json(FlutterPlatformMessageResponseHandle *handle,
                                     int _errno) {
	return platch_respond_error_json(
		handle,
		"nativeerror",
		strerror(_errno),
		&(struct json_value) {.type = kJsonNumber, .number_value = _errno}
	);
}

/**************************
 * PIGEON METHOD CHANNELS *
 **************************/
int platch_respond_success_pigeon(
	FlutterPlatformMessageResponseHandle *handle,
	struct std_value *return_value
) {
	return platch_respond(
		handle,
		&PLATCH_OBJ_STD_MSG(
			STDMAP1(
				STDSTRING("result"),
				return_value != NULL
					? *return_value
					: STDNULL
			)
		)
	);
}

int platch_respond_error_pigeon(
	FlutterPlatformMessageResponseHandle *handle,
	char *error_code,
	char *error_msg,
	struct std_value *error_details
) {
	return platch_respond(
		handle,
		&PLATCH_OBJ_STD_MSG(
			STDMAP1(
				STDSTRING("error"),
				STDMAP3(
					STDSTRING("code"),    STDSTRING(error_code),
					STDSTRING("message"), STDSTRING(error_msg),
					STDSTRING("details"), error_details != NULL
											? *error_details
											: STDNULL
				)
			)
		)
	);
}

int platch_respond_illegal_arg_pigeon(
	FlutterPlatformMessageResponseHandle *handle,
	char *error_msg
) {
	return platch_respond_error_pigeon(
		handle,
		"illegalargument",
		error_msg,
		NULL
	);
}

int platch_respond_illegal_arg_ext_pigeon(
	FlutterPlatformMessageResponseHandle *handle,
	char *error_msg,
	struct std_value *error_details
) {
	return platch_respond_error_pigeon(
		handle,
		"illegalargument",
		error_msg,
		error_details
	);
}


int platch_respond_native_error_pigeon(
	FlutterPlatformMessageResponseHandle *handle,
	int _errno
) {
	return platch_respond_error_pigeon(
		handle,
		"nativeerror",
		strerror(_errno),
		&STDINT32(_errno)
	);
}

/***************************
 * STANDARD EVENT CHANNELS *
 ***************************/
int platch_send_success_event_std(char *channel, struct std_value *event_value) {
	return platch_send(
		channel,
		&(struct platch_obj) {
			.codec = kStandardMethodCallResponse,
			.success = true,
			.std_result = event_value? *event_value : STDNULL
		},
		0, NULL, NULL
	);
}

int platch_send_error_event_std(char *channel,
							 	char *error_code,
							 	char *error_msg,
							 	struct std_value *error_details) {
	return platch_send(
		channel,
		&(struct platch_obj) {
			.codec = kStandardMethodCallResponse,
			.success = false,
			.error_code = error_code,
			.error_msg = error_msg,
			.std_error_details = error_details? *error_details : STDNULL
		},
		0, NULL, NULL
	);
}

/***********************
 * JSON EVENT CHANNELS *
 ***********************/
int platch_send_success_event_json(char *channel, struct json_value *event_value) {
	return platch_send(channel,
		&(struct platch_obj) {
			.codec = kJSONMethodCallResponse,
			.success = true,
			.json_result = event_value? *event_value : (struct json_value) {.type = kJsonNull}
		},
		0, NULL, NULL
	);
}

int platch_send_error_event_json(char *channel,
								 char *error_code,
								 char *error_msg,
								 struct json_value *error_details) {
	return platch_send(
		channel,
		&(struct platch_obj) {
			.codec = kJSONMethodCallResponse,
			.success = false,
			.error_code = error_code,
			.error_msg = error_msg,
			.json_error_details = error_details?
				*error_details :
				(struct json_value) {.type = kJsonNull}
		},
		0, NULL, NULL
	);
}


bool jsvalue_equals(struct json_value *a, struct json_value *b) {
	if (a == b) return true;
	if ((a == NULL) ^ (b == NULL)) return false;
	if (a->type != b->type) return false;

	switch (a->type) {
		case kJsonNull:
		case kJsonTrue:
		case kJsonFalse:
			return true;
		case kJsonNumber:
			return a->number_value == b->number_value;
		case kJsonString:
			return strcmp(a->string_value, b->string_value) == 0;
		case kJsonArray:
			if (a->size != b->size) return false;
			if (a->array == b->array) return true;
			for (int i = 0; i < a->size; i++)
				if (!jsvalue_equals(&a->array[i], &b->array[i]))
					return false;
			return true;
		case kJsonObject: {
			if (a->size != b->size) return false;
			if ((a->keys == b->keys) && (a->values == b->values)) return true;

			bool _keyInBAlsoInA[a->size];
			memset(_keyInBAlsoInA, 0, a->size * sizeof(bool));

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
		default:
			return false;
	}

	return 0;	
}
struct json_value *jsobject_get(struct json_value *object, char *key) {
	int i;
	for (i=0; i < object->size; i++)
		if (strcmp(object->keys[i], key) == 0) break;


	if (i != object->size) return &(object->values[i]);
	return NULL;
}
bool stdvalue_equals(struct std_value *a, struct std_value *b) {
	if (a == b) return true;
	if ((a == NULL) ^ (b == NULL)) return false;

	DEBUG_ASSERT_NOT_NULL(a);
	DEBUG_ASSERT_NOT_NULL(b);

	if (a->type != b->type) return false;

	switch (a->type) {
		case kStdNull:
		case kStdTrue:
		case kStdFalse:
			return true;
		case kStdInt32:
			return a->int32_value == b->int32_value;
		case kStdInt64:
			return a->int64_value == b->int64_value;
		case kStdLargeInt:
		case kStdString:
			DEBUG_ASSERT_NOT_NULL(a->string_value);
			DEBUG_ASSERT_NOT_NULL(b->string_value);
			return strcmp(a->string_value, b->string_value) == 0;
		case kStdFloat64:
			return a->float64_value == b->float64_value;
		case kStdUInt8Array:
			if (a->size != b->size) return false;
			if (a->uint8array == b->uint8array) return true;

			DEBUG_ASSERT_NOT_NULL(a->uint8array);
			DEBUG_ASSERT_NOT_NULL(b->uint8array);
			for (int i = 0; i < a->size; i++)
				if (a->uint8array[i] != b->uint8array[i])
					return false;
			return true;
		case kStdInt32Array:
			if (a->size != b->size) return false;
			if (a->int32array == b->int32array) return true;

			DEBUG_ASSERT_NOT_NULL(a->int32array);
			DEBUG_ASSERT_NOT_NULL(b->int32array);
			for (int i = 0; i < a->size; i++)
				if (a->int32array[i] != b->int32array[i])
					return false;
			return true;
		case kStdInt64Array:
			if (a->size != b->size) return false;
			if (a->int64array == b->int64array) return true;

			DEBUG_ASSERT_NOT_NULL(a->int64array);
			DEBUG_ASSERT_NOT_NULL(b->int64array);
			for (int i = 0; i < a->size; i++)
				if (a->int64array[i] != b->int64array[i])
					return false;
			return true;
		case kStdFloat64Array:
			if (a->size != b->size) return false;
			if (a->float64array == b->float64array) return true;

			DEBUG_ASSERT_NOT_NULL(a->float64array);
			DEBUG_ASSERT_NOT_NULL(b->float64array);
			for (int i = 0; i < a->size; i++)
				if (a->float64array[i] != b->float64array[i])
					return false;
			return true;
		case kStdList:
			// the order of list elements is important
			if (a->size != b->size) return false;
			if (a->list == b->list) return true;

			DEBUG_ASSERT_NOT_NULL(a->list);
			DEBUG_ASSERT_NOT_NULL(b->list);
			for (int i = 0; i < a->size; i++)
				if (!stdvalue_equals(a->list + i, b->list + i))
					return false;
			
			return true;
		case kStdMap: {
			// the order is not important here, which makes it a bit difficult to compare
			if (a->size != b->size) return false;
			if ((a->keys == b->keys) && (a->values == b->values)) return true;

			// _keyInBAlsoInA[i] == true means that there's a key in a that matches b->keys[i]
			//   so if we're searching for a key in b, we can safely ignore / don't need to compare
			//   keys in b that have they're _keyInBAlsoInA set to true.
			bool *_keyInBAlsoInA = alloca(sizeof(bool) * a->size);
			memset(_keyInBAlsoInA, 0, sizeof(bool) * a->size);

			for (int i = 0; i < a->size; i++) {
				// The key we're searching for in b.
				struct std_value *key = a->keys + i;
				
				int j = 0;
				while (j < a->size) {
					while (_keyInBAlsoInA[j] && (j < a->size))  j++;	// skip all keys with _keyInBAlsoInA set to true.
					if (stdvalue_equals(key, b->keys + j) == false) {
						j++;	// if b->keys[j] is not equal to "key", continue searching
					} else {
						_keyInBAlsoInA[j] = true;

						// the values of "key" in a and b must (of course) also be equivalent.
						if (stdvalue_equals(a->values + i, b->values + j) == false) {
							return false;
						}
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
struct std_value *stdmap_get(struct std_value *map, struct std_value *key) {
	DEBUG_ASSERT_NOT_NULL(map);
	DEBUG_ASSERT_NOT_NULL(key);

	for (int i=0; i < map->size; i++) {
		if (stdvalue_equals(&map->keys[i], key)) {
			return &map->values[i];
		}
	}

	return NULL;
}
struct std_value *stdmap_get_str(struct std_value *map, char *key) {
	DEBUG_ASSERT_NOT_NULL(map);
	DEBUG_ASSERT_NOT_NULL(key);
	return stdmap_get(map, &STDSTRING(key));
}