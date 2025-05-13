#include "platformchannel.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <flutter_embedder.h>

#include "flutter-pi.h"
#define JSMN_STATIC
#include "jsmn.h"
#include "util/asserts.h"

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
static int _read(const uint8_t **pbuffer, void *dest, int n_bytes, size_t *remaining) {
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

#define DEFINE_READ_WRITE_FUNC(suffix, value_type)                                                    \
    UNUSED static int _write_##suffix(uint8_t **pbuffer, value_type value, size_t *remaining) {       \
        return _write(pbuffer, &value, sizeof value, remaining);                                      \
    }                                                                                                 \
    UNUSED static int _read_##suffix(const uint8_t **pbuffer, value_type *value, size_t *remaining) { \
        return _read(pbuffer, value, sizeof *value, remaining);                                       \
    }

DEFINE_READ_WRITE_FUNC(u8, uint8_t)
DEFINE_READ_WRITE_FUNC(u16, uint16_t)
DEFINE_READ_WRITE_FUNC(u32, uint32_t)
DEFINE_READ_WRITE_FUNC(u64, uint64_t)
DEFINE_READ_WRITE_FUNC(i8, int8_t)
DEFINE_READ_WRITE_FUNC(i16, int16_t)
DEFINE_READ_WRITE_FUNC(i32, int32_t)
DEFINE_READ_WRITE_FUNC(i64, int64_t)
DEFINE_READ_WRITE_FUNC(float, float)
DEFINE_READ_WRITE_FUNC(double, double)

static int _writeSize(uint8_t **pbuffer, int size, size_t *remaining) {
    int ok;

    if (size < 254) {
        return _write_u8(pbuffer, (uint8_t) size, remaining);
    } else if (size <= 0xFFFF) {
        ok = _write_u8(pbuffer, 0xFE, remaining);
        if (ok != 0)
            return ok;

        ok = _write_u16(pbuffer, (uint16_t) size, remaining);
        if (ok != 0)
            return ok;
    } else {
        ok = _write_u8(pbuffer, 0xFF, remaining);
        if (ok != 0)
            return ok;

        ok = _write_u32(pbuffer, (uint32_t) size, remaining);
        if (ok != 0)
            return ok;
    }

    return ok;
}
static int _readSize(const uint8_t **pbuffer, uint32_t *psize, size_t *remaining) {
    int ok;
    uint8_t size8;
    uint16_t size16;

    ok = _read_u8(pbuffer, &size8, remaining);
    if (ok != 0)
        return ok;

    if (size8 <= 253) {
        *psize = size8;

        return 0;
    } else if (size8 == 254) {
        ok = _read_u16(pbuffer, &size16, remaining);
        if (ok != 0)
            return ok;

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
        case kStdString: free(value->string_value); break;
        case kStdList:
            for (int i = 0; i < value->size; i++) {
                ok = platch_free_value_std(&(value->list[i]));
                if (ok != 0)
                    return ok;
            }
            free(value->list);
            break;
        case kStdMap:
            for (int i = 0; i < value->size; i++) {
                ok = platch_free_value_std(&(value->keys[i]));
                if (ok != 0)
                    return ok;
                ok = platch_free_value_std(&(value->values[i]));
                if (ok != 0)
                    return ok;
            }
            free(value->keys);
            break;
        default: break;
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
                    if (ok != 0)
                        return ok;
                }
            }

            free(value->array);
            break;
        case kJsonObject:
            if (!shallow) {
                for (int i = 0; i < value->size; i++) {
                    ok = platch_free_json_value(&(value->values[i]), false);
                    if (ok != 0)
                        return ok;
                }
            }

            free(value->keys);
            free(value->values);
            break;
        default: break;
    }

    return 0;
}
int platch_free_obj(struct platch_obj *object) {
    switch (object->codec) {
        case kStringCodec: free(object->string_value); break;
        case kBinaryCodec: break;
        case kJSONMessageCodec: platch_free_json_value(&(object->json_value), false); break;
        case kStandardMessageCodec: platch_free_value_std(&(object->std_value)); break;
        case kStandardMethodCall:
            free(object->method);
            platch_free_value_std(&(object->std_arg));
            break;
        case kJSONMethodCall: platch_free_json_value(&(object->json_arg), false); break;
        default: break;
    }

    return 0;
}

int platch_calc_value_size_std(struct std_value *value, size_t *size_out) {
    enum std_value_type type = value->type;
    uintptr_t size = (uintptr_t) *size_out;
    size_t element_size, sizet_size = 0;
    int ok;

    // Type Byte
    _advance(&size, 1, NULL);
    switch (type) {
        case kStdNull:
        case kStdTrue:
        case kStdFalse: break;
        case kStdInt32: _advance(&size, 4, NULL); break;
        case kStdInt64: _advance(&size, 8, NULL); break;
        case kStdFloat64:
            _align(&size, 8, NULL);
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
            _align(&size, 4, NULL);
            _advance(&size, element_size * 4, NULL);

            break;
        case kStdInt64Array:
            element_size = value->size;

            _advance_size_bytes(&size, element_size, NULL);
            _align(&size, 8, NULL);
            _advance(&size, element_size * 8, NULL);

            break;
        case kStdFloat64Array:
            element_size = value->size;

            _advance_size_bytes(&size, element_size, NULL);
            _align(&size, 8, NULL);
            _advance(&size, element_size * 8, NULL);

            break;
        case kStdList:
            element_size = value->size;

            _advance_size_bytes(&size, element_size, NULL);
            for (int i = 0; i < element_size; i++) {
                sizet_size = (size_t) size;

                ok = platch_calc_value_size_std(&(value->list[i]), &sizet_size);
                if (ok != 0)
                    return ok;

                size = (uintptr_t) sizet_size;
            }

            break;
        case kStdMap:
            element_size = value->size;

            _advance_size_bytes(&size, element_size, NULL);
            for (int i = 0; i < element_size; i++) {
                sizet_size = (size_t) size;

                ok = platch_calc_value_size_std(&(value->keys[i]), &sizet_size);
                if (ok != 0)
                    return ok;

                ok = platch_calc_value_size_std(&(value->values[i]), &sizet_size);
                if (ok != 0)
                    return ok;

                size = (uintptr_t) sizet_size;
            }

            break;
        default: return EINVAL;
    }

    *size_out = (size_t) size;

    return 0;
}
int platch_write_value_to_buffer_std(struct std_value *value, uint8_t **pbuffer) {
    const uint8_t *byteArray;
    size_t size;
    int ok;

    _write_u8(pbuffer, value->type, NULL);

    switch (value->type) {
        case kStdNull:
        case kStdTrue:
        case kStdFalse: break;
        case kStdInt32: _write_i32(pbuffer, value->int32_value, NULL); break;
        case kStdInt64: _write_i64(pbuffer, value->int64_value, NULL); break;
        case kStdFloat64:
            _align((uintptr_t *) pbuffer, 8, NULL);
            _write_double(pbuffer, value->float64_value, NULL);
            break;
        case kStdLargeInt:
        case kStdString:
        case kStdUInt8Array:
            if ((value->type == kStdLargeInt) || (value->type == kStdString)) {
                size = strlen(value->string_value);
                byteArray = (uint8_t *) value->string_value;
            } else {
                assert(value->type == kStdUInt8Array);
                size = value->size;
                byteArray = value->uint8array;
            }

            _writeSize(pbuffer, size, NULL);
            for (int i = 0; i < size; i++) {
                _write_u8(pbuffer, byteArray[i], NULL);
            }
            break;
        case kStdInt32Array:
            size = value->size;

            _writeSize(pbuffer, size, NULL);
            _align((uintptr_t *) pbuffer, 4, NULL);

            for (int i = 0; i < size; i++) {
                _write_i32(pbuffer, value->int32array[i], NULL);
            }
            break;
        case kStdInt64Array:
            size = value->size;

            _writeSize(pbuffer, size, NULL);
            _align((uintptr_t *) pbuffer, 8, NULL);
            for (int i = 0; i < size; i++) {
                _write_i64(pbuffer, value->int64array[i], NULL);
            }
            break;
        case kStdFloat64Array:
            size = value->size;

            _writeSize(pbuffer, size, NULL);
            _align((uintptr_t *) pbuffer, 8, NULL);

            for (int i = 0; i < size; i++) {
                _write_double(pbuffer, value->float64array[i], NULL);
            }
            break;
        case kStdList:
            size = value->size;

            _writeSize(pbuffer, size, NULL);
            for (int i = 0; i < size; i++) {
                ok = platch_write_value_to_buffer_std(&value->list[i], pbuffer);
                if (ok != 0)
                    return ok;
            }

            break;
        case kStdMap:
            size = value->size;

            _writeSize(pbuffer, size, NULL);
            for (int i = 0; i < size; i++) {
                ok = platch_write_value_to_buffer_std(&value->keys[i], pbuffer);
                if (ok != 0)
                    return ok;

                ok = platch_write_value_to_buffer_std(&value->values[i], pbuffer);
                if (ok != 0)
                    return ok;
            }
            break;
        default: return EINVAL;
    }

    return 0;
}
size_t platch_calc_value_size_json(struct json_value *value) {
    size_t size = 0;

    switch (value->type) {
        case kJsonNull:
        case kJsonTrue: return 4;
        case kJsonFalse: return 5;
        case kJsonNumber:; char numBuffer[32]; return sprintf(numBuffer, "%g", value->number_value);
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
                    case '\\': size += 2; break;
                    default: size++; break;
                }
            }

            return size;
        case kJsonArray:
            size += 2;
            for (int i = 0; i < value->size; i++) {
                size += platch_calc_value_size_json(value->array + i);
                if (i + 1 != value->size)
                    size += 1;
            }
            return size;
        case kJsonObject:
            size += 2;
            for (int i = 0; i < value->size; i++) {
                size += strlen(value->keys[i]) + 3 + platch_calc_value_size_json(&(value->values[i]));
                if (i + 1 != value->size)
                    size += 1;
            }
            return size;
        default: return EINVAL;
    }

    return 0;
}
int platch_write_value_to_buffer_json(struct json_value *value, uint8_t **pbuffer) {
    switch (value->type) {
        case kJsonNull: *pbuffer += sprintf((char *) *pbuffer, "null"); break;
        case kJsonTrue: *pbuffer += sprintf((char *) *pbuffer, "true"); break;
        case kJsonFalse: *pbuffer += sprintf((char *) *pbuffer, "false"); break;
        case kJsonNumber: *pbuffer += sprintf((char *) *pbuffer, "%g", value->number_value); break;
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
                    default: *((*pbuffer)++) = *s; break;
                }
            }

            *((*pbuffer)++) = '\"';

            break;
        case kJsonArray:
            *pbuffer += sprintf((char *) *pbuffer, "[");
            for (int i = 0; i < value->size; i++) {
                platch_write_value_to_buffer_json(&(value->array[i]), pbuffer);
                if (i + 1 != value->size)
                    *pbuffer += sprintf((char *) *pbuffer, ",");
            }
            *pbuffer += sprintf((char *) *pbuffer, "]");
            break;
        case kJsonObject:
            *pbuffer += sprintf((char *) *pbuffer, "{");
            for (int i = 0; i < value->size; i++) {
                *pbuffer += sprintf((char *) *pbuffer, "\"%s\":", value->keys[i]);
                platch_write_value_to_buffer_json(&(value->values[i]), pbuffer);
                if (i + 1 != value->size)
                    *pbuffer += sprintf((char *) *pbuffer, ",");
            }
            *pbuffer += sprintf((char *) *pbuffer, "}");
            break;
        default: return EINVAL;
    }

    return 0;
}
int platch_decode_value_std(const uint8_t **pbuffer, size_t *premaining, struct std_value *value_out) {
    enum std_value_type type;
    uint8_t type_byte;
    uint32_t size;
    int ok;

    /// FIXME: Somehow, in release mode, this always reads 0.
    ok = _read_u8(pbuffer, &type_byte, premaining);
    if (ok != 0)
        return ok;

    type = (enum std_value_type) type_byte;
    value_out->type = (enum std_value_type) type_byte;
    switch (type) {
        case kStdNull:
        case kStdTrue:
        case kStdFalse: break;
        case kStdInt32:
            ok = _read_i32(pbuffer, &value_out->int32_value, premaining);
            if (ok != 0)
                return ok;

            break;
        case kStdInt64:
            ok = _read_i64(pbuffer, &value_out->int64_value, premaining);
            if (ok != 0)
                return ok;

            break;
        case kStdFloat64:
            ok = _align((uintptr_t *) pbuffer, 8, premaining);
            if (ok != 0)
                return ok;

            ok = _read_double(pbuffer, &value_out->float64_value, premaining);
            if (ok != 0)
                return ok;

            break;
        case kStdLargeInt:
        case kStdString:
            ok = _readSize(pbuffer, &size, premaining);
            if (ok != 0)
                return ok;

            value_out->string_value = calloc(size + 1, sizeof(char));
            if (!value_out->string_value)
                return ENOMEM;

            ok = _read(pbuffer, value_out->string_value, size, premaining);
            if (ok != 0) {
                free(value_out->string_value);
                return ok;
            }

            break;
        case kStdUInt8Array:
            ok = _readSize(pbuffer, &size, premaining);
            if (ok != 0)
                return ok;
            if (*premaining < size)
                return EBADMSG;

            value_out->size = size;
            value_out->uint8array = *pbuffer;

            ok = _advance((uintptr_t *) pbuffer, size, premaining);
            if (ok != 0)
                return ok;

            break;
        case kStdInt32Array:
            ok = _readSize(pbuffer, &size, premaining);
            if (ok != 0)
                return ok;

            ok = _align((uintptr_t *) pbuffer, 4, premaining);
            if (ok != 0)
                return ok;

            if (*premaining < size * 4)
                return EBADMSG;

            value_out->size = size;
            value_out->int32array = (int32_t *) *pbuffer;

            ok = _advance((uintptr_t *) pbuffer, size * 4, premaining);
            if (ok != 0)
                return ok;

            break;
        case kStdInt64Array:
            ok = _readSize(pbuffer, &size, premaining);
            if (ok != 0)
                return ok;

            ok = _align((uintptr_t *) pbuffer, 8, premaining);
            if (ok != 0)
                return ok;

            if (*premaining < size * 8)
                return EBADMSG;

            value_out->size = size;
            value_out->int64array = (int64_t *) *pbuffer;

            ok = _advance((uintptr_t *) pbuffer, size * 8, premaining);
            if (ok != 0)
                return ok;

            break;
        case kStdFloat64Array:
            ok = _readSize(pbuffer, &size, premaining);
            if (ok != 0)
                return ok;

            ok = _align((uintptr_t *) pbuffer, 8, premaining);
            if (ok != 0)
                return ok;

            if (*premaining < size * 8)
                return EBADMSG;

            value_out->size = size;
            value_out->float64array = (double *) *pbuffer;

            ok = _advance((uintptr_t *) pbuffer, size * 8, premaining);
            if (ok != 0)
                return ok;

            break;
        case kStdList:
            ok = _readSize(pbuffer, &size, premaining);
            if (ok != 0)
                return ok;

            value_out->size = size;
            value_out->list = calloc(size, sizeof(struct std_value));

            for (int i = 0; i < size; i++) {
                ok = platch_decode_value_std(pbuffer, premaining, &value_out->list[i]);
                if (ok != 0)
                    return ok;
            }

            break;
        case kStdMap:
            ok = _readSize(pbuffer, &size, premaining);
            if (ok != 0)
                return ok;

            value_out->size = size;

            value_out->keys = calloc(size * 2, sizeof(struct std_value));
            if (!value_out->keys)
                return ENOMEM;

            value_out->values = &value_out->keys[size];

            for (int i = 0; i < size; i++) {
                ok = platch_decode_value_std(pbuffer, premaining, &(value_out->keys[i]));
                if (ok != 0)
                    return ok;

                ok = platch_decode_value_std(pbuffer, premaining, &(value_out->values[i]));
                if (ok != 0)
                    return ok;
            }

            break;
        default: return EBADMSG;
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
        if (result < 0)
            return EBADMSG;

        tokensremaining = (size_t) result;
        ptoken = tokens;

        ok = platch_decode_value_json(message, size, &ptoken, &tokensremaining, value_out);
        if (ok != 0)
            return ok;
    } else {
        // message is already tokenized

        ptoken = *pptoken;

        (*pptoken) += 1;
        *ptokensremaining -= 1;

        switch (ptoken->type) {
            case JSMN_UNDEFINED: return EBADMSG;
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
            case JSMN_STRING:;
                // use zero-copy approach.

                message[ptoken->end] = '\0';
                char *string = message + ptoken->start;

                value_out->type = kJsonString;
                value_out->string_value = string;

                break;
            case JSMN_ARRAY:;
                struct json_value *array = calloc(ptoken->size, sizeof(struct json_value));
                if (!array)
                    return ENOMEM;

                for (int i = 0; i < ptoken->size; i++) {
                    ok = platch_decode_value_json(message, size, pptoken, ptokensremaining, &array[i]);
                    if (ok != 0)
                        return ok;
                }

                value_out->type = kJsonArray;
                value_out->size = ptoken->size;
                value_out->array = array;

                break;
            case JSMN_OBJECT:;
                struct json_value key;
                char **keys = calloc(ptoken->size, sizeof(char *));
                struct json_value *values = calloc(ptoken->size, sizeof(struct json_value));
                if ((!keys) || (!values))
                    return ENOMEM;

                for (int i = 0; i < ptoken->size; i++) {
                    ok = platch_decode_value_json(message, size, pptoken, ptokensremaining, &key);
                    if (ok != 0)
                        return ok;

                    if (key.type != kJsonString)
                        return EBADMSG;
                    keys[i] = key.string_value;

                    ok = platch_decode_value_json(message, size, pptoken, ptokensremaining, &values[i]);
                    if (ok != 0)
                        return ok;
                }

                value_out->type = kJsonObject;
                value_out->size = ptoken->size;
                value_out->keys = keys;
                value_out->values = values;

                break;
            default: return EBADMSG;
        }
    }

    return 0;
}

int platch_decode_json(char *string, struct json_value *out) {
    return platch_decode_value_json(string, strlen(string), NULL, NULL, out);
}

int platch_decode(const uint8_t *buffer, size_t size, enum platch_codec codec, struct platch_obj *object_out) {
    struct json_value root_jsvalue;
    const uint8_t *buffer_cursor = buffer;
    size_t remaining = size;
    int ok;

    if ((size == 0) && (buffer == NULL)) {
        object_out->codec = kNotImplemented;
        return 0;
    }

    object_out->codec = codec;
    switch (codec) {
        case kStringCodec:;
            /// buffer is a non-null-terminated, UTF8-encoded string.
            /// it's really sad we have to allocate a new memory block for this, but we have to since string codec buffers are not null-terminated.

            char *string;
            if (!(string = malloc(size + 1)))
                return ENOMEM;
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
            if (ok != 0)
                return ok;

            break;
        case kJSONMethodCall:;
            ok = platch_decode_value_json((char *) buffer, size, NULL, NULL, &root_jsvalue);
            if (ok != 0)
                return ok;

            if (root_jsvalue.type != kJsonObject)
                return EBADMSG;

            for (int i = 0; i < root_jsvalue.size; i++) {
                if ((streq(root_jsvalue.keys[i], "method")) && (root_jsvalue.values[i].type == kJsonString)) {
                    object_out->method = root_jsvalue.values[i].string_value;
                } else if (streq(root_jsvalue.keys[i], "args")) {
                    object_out->json_arg = root_jsvalue.values[i];
                } else
                    return EBADMSG;
            }

            platch_free_json_value(&root_jsvalue, true);

            break;
        case kJSONMethodCallResponse:;
            ok = platch_decode_value_json((char *) buffer, size, NULL, NULL, &root_jsvalue);
            if (ok != 0)
                return ok;
            if (root_jsvalue.type != kJsonArray)
                return EBADMSG;

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
            } else
                return EBADMSG;

            break;
        case kStandardMessageCodec:
            ok = platch_decode_value_std(&buffer_cursor, &remaining, &object_out->std_value);
            if (ok != 0)
                return ok;
            break;
        case kStandardMethodCall:;
            struct std_value methodname;

            ok = platch_decode_value_std(&buffer_cursor, &remaining, &methodname);
            if (ok != 0)
                return ok;
            if (methodname.type != kStdString) {
                platch_free_value_std(&methodname);
                return EBADMSG;
            }
            object_out->method = methodname.string_value;

            ok = platch_decode_value_std(&buffer_cursor, &remaining, &object_out->std_arg);
            if (ok != 0)
                return ok;

            break;
        case kStandardMethodCallResponse:;
            ok = _read_u8(&buffer_cursor, (uint8_t *) &object_out->success, &remaining);

            if (object_out->success) {
                ok = platch_decode_value_std(&buffer_cursor, &remaining, &(object_out->std_result));
                if (ok != 0)
                    return ok;
            } else {
                struct std_value error_code, error_msg;

                ok = platch_decode_value_std(&buffer_cursor, &remaining, &error_code);
                if (ok != 0)
                    return ok;
                ok = platch_decode_value_std(&buffer_cursor, &remaining, &error_msg);
                if (ok != 0)
                    return ok;
                ok = platch_decode_value_std(&buffer_cursor, &remaining, &(object_out->std_error_details));
                if (ok != 0)
                    return ok;

                if ((error_code.type == kStdString) && ((error_msg.type == kStdString) || (error_msg.type == kStdNull))) {
                    object_out->error_code = error_code.string_value;
                    object_out->error_msg = (error_msg.type == kStdString) ? error_msg.string_value : NULL;
                } else {
                    return EBADMSG;
                }
            }
            break;
        default: return EINVAL;
    }

    return 0;
}

int platch_encode(struct platch_obj *object, uint8_t **buffer_out, size_t *size_out) {
    struct std_value stdmethod, stderrcode, stderrmessage;
    uint8_t *buffer, *buffer_cursor;
    size_t size = 0;
    int ok = 0;

    *size_out = 0;
    *buffer_out = NULL;

    switch (object->codec) {
        case kNotImplemented:
            *size_out = 0;
            *buffer_out = NULL;
            return 0;
        case kStringCodec: size = strlen(object->string_value); break;
        case kBinaryCodec:
            /// FIXME: Copy buffer instead
            *buffer_out = (uint8_t *) object->binarydata;
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
            if (ok != 0)
                return ok;
            break;
        case kStandardMethodCall:
            stdmethod.type = kStdString;
            stdmethod.string_value = object->method;

            ok = platch_calc_value_size_std(&stdmethod, &size);
            if (ok != 0)
                return ok;

            ok = platch_calc_value_size_std(&(object->std_arg), &size);
            if (ok != 0)
                return ok;

            break;
        case kStandardMethodCallResponse:
            size += 1;

            if (object->success) {
                ok = platch_calc_value_size_std(&(object->std_result), &size);
                if (ok != 0)
                    return ok;
            } else {
                stderrcode = (struct std_value){ .type = kStdString, .string_value = object->error_code };
                stderrmessage = (struct std_value){ .type = kStdString, .string_value = object->error_msg };

                ok = platch_calc_value_size_std(&stderrcode, &size);
                if (ok != 0)
                    return ok;
                ok = platch_calc_value_size_std(&stderrmessage, &size);
                if (ok != 0)
                    return ok;
                ok = platch_calc_value_size_std(&(object->std_error_details), &size);
                if (ok != 0)
                    return ok;
            }
            break;
        case kJSONMethodCall:
            size = platch_calc_value_size_json(&JSONOBJECT2("method", JSONSTRING(object->method), "args", object->json_arg));
            size += 1;
            break;
        case kJSONMethodCallResponse:
            if (object->success) {
                size = 1 + platch_calc_value_size_json(&JSONARRAY1(object->json_result));
            } else {
                size = 1 + platch_calc_value_size_json(&JSONARRAY3(
                               JSONSTRING(object->error_code),
                               (object->error_msg != NULL) ? JSONSTRING(object->error_msg) : JSONNULL,
                               object->json_error_details
                           ));
            }
            break;
        default: return EINVAL;
    }

    buffer = malloc(size);
    if (buffer == NULL) {
        return ENOMEM;
    }

    buffer_cursor = buffer;

    switch (object->codec) {
        case kStringCodec: memcpy(buffer, object->string_value, size); break;
        case kStandardMessageCodec:
            ok = platch_write_value_to_buffer_std(&(object->std_value), &buffer_cursor);
            if (ok != 0)
                goto free_buffer_and_return_ok;
            break;
        case kStandardMethodCall:
            ok = platch_write_value_to_buffer_std(&stdmethod, &buffer_cursor);
            if (ok != 0)
                goto free_buffer_and_return_ok;

            ok = platch_write_value_to_buffer_std(&(object->std_arg), &buffer_cursor);
            if (ok != 0)
                goto free_buffer_and_return_ok;

            break;
        case kStandardMethodCallResponse:
            if (object->success) {
                _write_u8(&buffer_cursor, 0x00, NULL);

                ok = platch_write_value_to_buffer_std(&(object->std_result), &buffer_cursor);
                if (ok != 0)
                    goto free_buffer_and_return_ok;
            } else {
                _write_u8(&buffer_cursor, 0x01, NULL);

                ok = platch_write_value_to_buffer_std(&stderrcode, &buffer_cursor);
                if (ok != 0)
                    goto free_buffer_and_return_ok;
                ok = platch_write_value_to_buffer_std(&stderrmessage, &buffer_cursor);
                if (ok != 0)
                    goto free_buffer_and_return_ok;
                ok = platch_write_value_to_buffer_std(&(object->std_error_details), &buffer_cursor);
                if (ok != 0)
                    goto free_buffer_and_return_ok;
            }

            break;
        case kJSONMessageCodec:
            size -= 1;
            ok = platch_write_value_to_buffer_json(&(object->json_value), &buffer_cursor);
            if (ok != 0)
                goto free_buffer_and_return_ok;
            break;
        case kJSONMethodCall:
            size -= 1;
            ok = platch_write_value_to_buffer_json(
                &JSONOBJECT2("method", JSONSTRING(object->method), "args", object->json_arg),
                &buffer_cursor
            );
            if (ok != 0) {
                goto free_buffer_and_return_ok;
            }
            break;
        case kJSONMethodCallResponse:
            if (object->success) {
                ok = platch_write_value_to_buffer_json(&JSONARRAY1(object->json_result), &buffer_cursor);
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
        default: return EINVAL;
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
    ok = platch_decode((uint8_t *) buffer, size, handlerdata->codec, &object);
    if (ok != 0)
        return;

    ok = handlerdata->on_response(&object, handlerdata->userdata);
    if (ok != 0)
        return;

    free(handlerdata);

    ok = platch_free_obj(&object);
    if (ok != 0)
        return;
}

int platch_send(
    char *channel,
    struct platch_obj *object,
    enum platch_codec response_codec,
    platch_msg_resp_callback on_response,
    void *userdata
) {
    FlutterPlatformMessageResponseHandle *response_handle = NULL;
    struct platch_msg_resp_handler_data *handlerdata = NULL;
    uint8_t *buffer;
    size_t size;
    int ok;

    ok = platch_encode(object, &buffer, &size);
    if (ok != 0)
        return ok;

    if (on_response) {
        handlerdata = malloc(sizeof(struct platch_msg_resp_handler_data));
        if (!handlerdata) {
            return ENOMEM;
        }

        handlerdata->codec = response_codec;
        handlerdata->on_response = on_response;
        handlerdata->userdata = userdata;

        response_handle = flutterpi_create_platform_message_response_handle(flutterpi, platch_on_response_internal, handlerdata);
        if (response_handle == NULL) {
            goto fail_free_handlerdata;
        }
    }

    ok = flutterpi_send_platform_message(flutterpi, channel, buffer, size, response_handle);
    if (ok != 0) {
        goto fail_release_handle;
    }

    /// TODO: This won't work if we're not on the main thread
    if (on_response) {
        flutterpi_release_platform_message_response_handle(flutterpi, response_handle);
    }

    if (object->codec != kBinaryCodec) {
        free(buffer);
    }

    return 0;

fail_release_handle:
    if (on_response) {
        flutterpi_release_platform_message_response_handle(flutterpi, response_handle);
    }

fail_free_handlerdata:
    if (on_response) {
        free(handlerdata);
    }

    return ok;
}

int platch_call_std(char *channel, char *method, struct std_value *argument, platch_msg_resp_callback on_response, void *userdata) {
    struct platch_obj object = { .codec = kStandardMethodCall, .method = method, .std_arg = *argument };

    return platch_send(channel, &object, kStandardMethodCallResponse, on_response, userdata);
}

int platch_call_json(char *channel, char *method, struct json_value *argument, platch_msg_resp_callback on_response, void *userdata) {
    return platch_send(
        channel,
        &(struct platch_obj){ .codec = kJSONMethodCall, .method = method, .json_arg = *argument },
        kJSONMethodCallResponse,
        on_response,
        userdata
    );
}

int platch_respond(const FlutterPlatformMessageResponseHandle *handle, struct platch_obj *response) {
    uint8_t *buffer = NULL;
    size_t size = 0;
    int ok;

    ok = platch_encode(response, &buffer, &size);
    if (ok != 0)
        return ok;

    ok = flutterpi_respond_to_platform_message(handle, buffer, size);

    if (buffer != NULL) {
        free(buffer);
    }

    return 0;
}

int platch_respond_not_implemented(const FlutterPlatformMessageResponseHandle *handle) {
    return platch_respond((FlutterPlatformMessageResponseHandle *) handle, &(struct platch_obj){ .codec = kNotImplemented });
}

/****************************
 * STANDARD METHOD CHANNELS *
 ****************************/

int platch_respond_success_std(const FlutterPlatformMessageResponseHandle *handle, struct std_value *return_value) {
    return platch_respond(
        handle,
        &(struct platch_obj){ .codec = kStandardMethodCallResponse, .success = true, .std_result = return_value ? *return_value : STDNULL }
    );
}

int platch_respond_error_std(
    const FlutterPlatformMessageResponseHandle *handle,
    char *error_code,
    char *error_msg,
    struct std_value *error_details
) {
    return platch_respond(
        handle,
        &(struct platch_obj){
            .codec = kStandardMethodCallResponse,
            .success = false,
            .error_code = error_code,
            .error_msg = error_msg,
            .std_error_details = error_details ? *error_details : STDNULL,
        }
    );
}

/// Sends a platform message to `handle` with error code "illegalargument"
/// and error message `errmsg`.
int platch_respond_illegal_arg_std(const FlutterPlatformMessageResponseHandle *handle, char *error_msg) {
    return platch_respond_error_std(handle, "illegalargument", error_msg, NULL);
}

/// Sends a platform message to `handle` with error code "illegalargument"
/// and error message `errmsg`.
int platch_respond_illegal_arg_ext_std(const FlutterPlatformMessageResponseHandle *handle, char *error_msg, struct std_value *error_details) {
    return platch_respond_error_std(handle, "illegalargument", error_msg, error_details);
}

/// Sends a platform message to `handle` with error code "nativeerror"
/// and error messsage `strerror(_errno)`
int platch_respond_native_error_std(const FlutterPlatformMessageResponseHandle *handle, int _errno) {
    return platch_respond_error_std(handle, "nativeerror", strerror(_errno), &STDINT32(_errno));
}

/************************
 * JSON METHOD CHANNELS *
 ************************/

int platch_respond_success_json(const FlutterPlatformMessageResponseHandle *handle, struct json_value *return_value) {
    return platch_respond(
        handle,
        &(struct platch_obj){
            .codec = kJSONMethodCallResponse,
            .success = true,
            .json_result = return_value ? *return_value : JSONNULL,
        }
    );
}

int platch_respond_error_json(
    const FlutterPlatformMessageResponseHandle *handle,
    char *error_code,
    char *error_msg,
    struct json_value *error_details
) {
    return platch_respond(
        handle,
        &(struct platch_obj){
            .codec = kJSONMethodCallResponse,
            .success = false,
            .error_code = error_code,
            .error_msg = error_msg,
            .json_error_details = (error_details) ? *error_details : (struct json_value){ .type = kJsonNull },
        }
    );
}

int platch_respond_illegal_arg_json(const FlutterPlatformMessageResponseHandle *handle, char *error_msg) {
    return platch_respond_error_json(handle, "illegalargument", error_msg, NULL);
}

int platch_respond_native_error_json(const FlutterPlatformMessageResponseHandle *handle, int _errno) {
    return platch_respond_error_json(
        handle,
        "nativeerror",
        strerror(_errno),
        &(struct json_value){ .type = kJsonNumber, .number_value = _errno }
    );
}

/**************************
 * PIGEON METHOD CHANNELS *
 **************************/
int platch_respond_success_pigeon(const FlutterPlatformMessageResponseHandle *handle, struct std_value *return_value) {
    return platch_respond(handle, &PLATCH_OBJ_STD_MSG(STDMAP1(STDSTRING("result"), return_value != NULL ? *return_value : STDNULL)));
}

int platch_respond_error_pigeon(
    const FlutterPlatformMessageResponseHandle *handle,
    char *error_code,
    char *error_msg,
    struct std_value *error_details
) {
    return platch_respond(
        handle,
        &PLATCH_OBJ_STD_MSG(STDMAP1(
            STDSTRING("error"),
            STDMAP3(
                STDSTRING("code"),
                STDSTRING(error_code),
                STDSTRING("message"),
                STDSTRING(error_msg),
                STDSTRING("details"),
                error_details != NULL ? *error_details : STDNULL
            )
        ))
    );
}

int platch_respond_illegal_arg_pigeon(const FlutterPlatformMessageResponseHandle *handle, char *error_msg) {
    return platch_respond_error_pigeon(handle, "illegalargument", error_msg, NULL);
}

int platch_respond_illegal_arg_ext_pigeon(
    const FlutterPlatformMessageResponseHandle *handle,
    char *error_msg,
    struct std_value *error_details
) {
    return platch_respond_error_pigeon(handle, "illegalargument", error_msg, error_details);
}

int platch_respond_native_error_pigeon(const FlutterPlatformMessageResponseHandle *handle, int _errno) {
    return platch_respond_error_pigeon(handle, "nativeerror", strerror(_errno), &STDINT32(_errno));
}

/***************************
 * STANDARD EVENT CHANNELS *
 ***************************/
int platch_send_success_event_std(char *channel, struct std_value *event_value) {
    return platch_send(
        channel,
        &(struct platch_obj){
            .codec = kStandardMethodCallResponse,
            .success = true,
            .std_result = event_value ? *event_value : STDNULL,
        },
        0,
        NULL,
        NULL
    );
}

int platch_send_error_event_std(char *channel, char *error_code, char *error_msg, struct std_value *error_details) {
    return platch_send(
        channel,
        &(struct platch_obj){
            .codec = kStandardMethodCallResponse,
            .success = false,
            .error_code = error_code,
            .error_msg = error_msg,
            .std_error_details = error_details ? *error_details : STDNULL,
        },
        0,
        NULL,
        NULL
    );
}

/***********************
 * JSON EVENT CHANNELS *
 ***********************/
int platch_send_success_event_json(char *channel, struct json_value *event_value) {
    return platch_send(
        channel,
        &(struct platch_obj){
            .codec = kJSONMethodCallResponse,
            .success = true,
            .json_result = event_value ? *event_value : (struct json_value){ .type = kJsonNull },
        },
        0,
        NULL,
        NULL
    );
}

int platch_send_error_event_json(char *channel, char *error_code, char *error_msg, struct json_value *error_details) {
    return platch_send(
        channel,
        &(struct platch_obj){ .codec = kJSONMethodCallResponse,
                              .success = false,
                              .error_code = error_code,
                              .error_msg = error_msg,
                              .json_error_details = error_details ? *error_details : (struct json_value){ .type = kJsonNull } },
        0,
        NULL,
        NULL
    );
}

bool jsvalue_equals(struct json_value *a, struct json_value *b) {
    if (a == b)
        return true;
    if ((a == NULL) ^ (b == NULL))
        return false;
    if (a->type != b->type)
        return false;

    switch (a->type) {
        case kJsonNull:
        case kJsonTrue:
        case kJsonFalse: return true;
        case kJsonNumber: return a->number_value == b->number_value;
        case kJsonString: return streq(a->string_value, b->string_value);
        case kJsonArray:
            if (a->size != b->size)
                return false;
            if (a->array == b->array)
                return true;
            for (int i = 0; i < a->size; i++)
                if (!jsvalue_equals(&a->array[i], &b->array[i]))
                    return false;
            return true;
        case kJsonObject: {
            if (a->size != b->size)
                return false;
            if ((a->keys == b->keys) && (a->values == b->values))
                return true;

            bool _keyInBAlsoInA[a->size];
            memset(_keyInBAlsoInA, 0, a->size * sizeof(bool));

            for (int i = 0; i < a->size; i++) {
                // The key we're searching for in b.
                char *key = a->keys[i];

                int j = 0;
                while (j < a->size) {
                    while ((j < a->size) && _keyInBAlsoInA[j])
                        j++;  // skip all keys with _keyInBAlsoInA set to true.
                    if (j >= a->size)
                        break;
                    if (!streq(key, b->keys[j]))
                        j++;  // if b->keys[j] is not equal to "key", continue searching
                    else {
                        _keyInBAlsoInA[j] = true;

                        // the values of "key" in a and b must (of course) also be equivalent.
                        if (!jsvalue_equals(&a->values[i], &b->values[j]))
                            return false;
                        break;
                    }
                }

                // we did not find a->keys[i] in b.
                if (j + 1 >= a->size)
                    return false;
            }

            return true;
        }
        default: return false;
    }

    return 0;
}
struct json_value *jsobject_get(struct json_value *object, char *key) {
    int i;
    for (i = 0; i < object->size; i++)
        if (streq(object->keys[i], key))
            break;

    if (i != object->size)
        return &(object->values[i]);
    return NULL;
}
bool stdvalue_equals(struct std_value *a, struct std_value *b) {
    if (a == b)
        return true;
    if ((a == NULL) ^ (b == NULL))
        return false;

    ASSERT_NOT_NULL(a);
    ASSERT_NOT_NULL(b);

    if (a->type != b->type)
        return false;

    switch (a->type) {
        case kStdNull:
        case kStdTrue:
        case kStdFalse: return true;
        case kStdInt32: return a->int32_value == b->int32_value;
        case kStdInt64: return a->int64_value == b->int64_value;
        case kStdLargeInt:
        case kStdString:
            ASSERT_NOT_NULL(a->string_value);
            ASSERT_NOT_NULL(b->string_value);
            return streq(a->string_value, b->string_value);
        case kStdFloat64: return a->float64_value == b->float64_value;
        case kStdUInt8Array:
            if (a->size != b->size)
                return false;
            if (a->uint8array == b->uint8array)
                return true;

            ASSERT_NOT_NULL(a->uint8array);
            ASSERT_NOT_NULL(b->uint8array);
            for (int i = 0; i < a->size; i++)
                if (a->uint8array[i] != b->uint8array[i])
                    return false;
            return true;
        case kStdInt32Array:
            if (a->size != b->size)
                return false;
            if (a->int32array == b->int32array)
                return true;

            ASSERT_NOT_NULL(a->int32array);
            ASSERT_NOT_NULL(b->int32array);
            for (int i = 0; i < a->size; i++)
                if (a->int32array[i] != b->int32array[i])
                    return false;
            return true;
        case kStdInt64Array:
            if (a->size != b->size)
                return false;
            if (a->int64array == b->int64array)
                return true;

            ASSERT_NOT_NULL(a->int64array);
            ASSERT_NOT_NULL(b->int64array);
            for (int i = 0; i < a->size; i++)
                if (a->int64array[i] != b->int64array[i])
                    return false;
            return true;
        case kStdFloat64Array:
            if (a->size != b->size)
                return false;
            if (a->float64array == b->float64array)
                return true;

            ASSERT_NOT_NULL(a->float64array);
            ASSERT_NOT_NULL(b->float64array);
            for (int i = 0; i < a->size; i++)
                if (a->float64array[i] != b->float64array[i])
                    return false;
            return true;
        case kStdList:
            // the order of list elements is important
            if (a->size != b->size)
                return false;
            if (a->list == b->list)
                return true;

            ASSERT_NOT_NULL(a->list);
            ASSERT_NOT_NULL(b->list);
            for (int i = 0; i < a->size; i++)
                if (!stdvalue_equals(a->list + i, b->list + i))
                    return false;

            return true;
        case kStdMap: {
            // the order is not important here, which makes it a bit difficult to compare
            if (a->size != b->size)
                return false;
            if ((a->keys == b->keys) && (a->values == b->values))
                return true;

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
                    while ((j < a->size) && _keyInBAlsoInA[j])
                        j++;  // skip all keys with _keyInBAlsoInA set to true.
                    if (j >= a->size)
                        break;
                    if (stdvalue_equals(key, b->keys + j) == false) {
                        j++;  // if b->keys[j] is not equal to "key", continue searching
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
                if (j + 1 >= a->size)
                    return false;
            }

            return true;
        }
        default: return false;
    }

    return false;
}
struct std_value *stdmap_get(struct std_value *map, struct std_value *key) {
    ASSERT_NOT_NULL(map);
    ASSERT_NOT_NULL(key);

    for (int i = 0; i < map->size; i++) {
        if (stdvalue_equals(&map->keys[i], key)) {
            return &map->values[i];
        }
    }

    return NULL;
}

struct std_value *stdmap_get_str(struct std_value *map, char *key) {
    ASSERT_NOT_NULL(map);
    ASSERT_NOT_NULL(key);
    return stdmap_get(map, &STDSTRING(key));
}

/**
 * BEGIN Raw Standard Message Codec Value API.
 *
 * New API, for using standard-message-codec encoded buffers directly, without parsing them first.
 *
 */

ATTR_PURE enum std_value_type raw_std_value_get_type(const struct raw_std_value *value) {
    return (enum std_value_type) * ((const uint8_t *) value);
}

ATTR_CONST static const void *get_value_ptr(const struct raw_std_value *value, int alignment) {
    uintptr_t addr = (uintptr_t) value;

    // skip type byte
    addr++;

    // make sure we're aligned
    if (alignment == 4) {
        if (addr & 3) {
            addr = (addr | 3) + 1;
        }
    } else if (alignment == 8) {
        if (addr & 7) {
            addr = (addr | 7) + 1;
        }
    }

    return (const void *) addr;
}

ATTR_CONST UNUSED static const void *get_after_ptr(const struct raw_std_value *value, int alignment, int value_size) {
    return ((const uint8_t *) get_value_ptr(value, alignment)) + value_size;
}

ATTR_CONST static const void *get_array_value_ptr(const struct raw_std_value *value, int alignment, size_t size) {
    uintptr_t addr = (uintptr_t) value;

    // skip type byte
    addr++;

    // skip initial size byte
    addr++;
    if (size >= 254 && size < 0x00010000) {
        // skip two additional size bytes
        addr += 2;
    } else if (size >= 0x00010000) {
        // skip four additional size bytes
        addr += 4;
    }

    // make sure we're aligned
    if (alignment == 4) {
        if (addr & 3) {
            addr = (addr | 3) + 1;
        }
    } else if (alignment == 8) {
        if (addr & 7) {
            addr = (addr | 7) + 1;
        }
    }

    return (const void *) addr;
}

ATTR_CONST static const void *get_array_after_ptr(const struct raw_std_value *value, int alignment, size_t size, int element_size) {
    uintptr_t addr = (uintptr_t) get_array_value_ptr(value, alignment, size);

    // skip all the array elements
    addr += size * element_size;

    return (const void *) addr;
}

ATTR_PURE bool raw_std_value_is_null(const struct raw_std_value *value) {
    return raw_std_value_get_type(value) == kStdNull;
}

ATTR_PURE bool raw_std_value_is_true(const struct raw_std_value *value) {
    return raw_std_value_get_type(value) == kStdTrue;
}

ATTR_PURE bool raw_std_value_is_false(const struct raw_std_value *value) {
    return raw_std_value_get_type(value) == kStdFalse;
}

ATTR_PURE bool raw_std_value_is_int32(const struct raw_std_value *value) {
    return raw_std_value_get_type(value) == kStdInt32;
}

ATTR_PURE int32_t raw_std_value_as_int32(const struct raw_std_value *value) {
    assert(raw_std_value_is_int32(value));
    int32_t result;
    memcpy(&result, get_value_ptr(value, 0), sizeof(result));

    return result;
}

ATTR_PURE bool raw_std_value_is_int64(const struct raw_std_value *value) {
    return raw_std_value_get_type(value) == kStdInt64;
}

ATTR_PURE int64_t raw_std_value_as_int64(const struct raw_std_value *value) {
    assert(raw_std_value_is_int64(value));

    int64_t result;
    memcpy(&result, get_value_ptr(value, 0), sizeof(result));

    return result;
}

ATTR_PURE bool raw_std_value_is_float64(const struct raw_std_value *value) {
    return raw_std_value_get_type(value) == kStdFloat64;
}

ATTR_PURE double raw_std_value_as_float64(const struct raw_std_value *value) {
    assert(raw_std_value_is_float64(value));
    return *(double *) get_value_ptr(value, 8);
}

ATTR_PURE size_t raw_std_string_get_length(const struct raw_std_value *value) {
    assert(raw_std_value_is_string(value));
    return raw_std_value_get_size(value);
}

ATTR_PURE bool raw_std_value_is_string(const struct raw_std_value *value) {
    return raw_std_value_get_type(value) == kStdString;
}

MALLOCLIKE MUST_CHECK char *raw_std_string_dup(const struct raw_std_value *value) {
    assert(raw_std_value_is_string(value));

    size_t size = raw_std_value_get_size(value);

    char *str = malloc(size + 1);
    if (str == NULL) {
        return NULL;
    }

    memcpy(str, get_array_value_ptr(value, 0, size), size);
    str[size] = '\0';

    return str;
}

ATTR_PURE const char *raw_std_string_get_nonzero_terminated(const struct raw_std_value *value) {
    assert(raw_std_value_is_string(value));
    return get_array_value_ptr(value, 0, raw_std_value_get_size(value));
}

ATTR_PURE bool raw_std_string_equals(const struct raw_std_value *value, const char *str) {
    size_t length = raw_std_value_get_size(value);
    const char *as_unterminated_string = get_array_value_ptr(value, 0, length);
    return strncmp(as_unterminated_string, str, length) == 0 && str[length] == '\0';
}

ATTR_PURE bool raw_std_value_is_uint8array(const struct raw_std_value *value) {
    return raw_std_value_get_type(value) == kStdUInt8Array;
}

ATTR_PURE const uint8_t *raw_std_value_as_uint8array(const struct raw_std_value *value) {
    assert(raw_std_value_is_uint8array(value));
    return get_array_value_ptr(value, 0, raw_std_value_get_size(value));
}

ATTR_PURE bool raw_std_value_is_int32array(const struct raw_std_value *value) {
    return raw_std_value_get_type(value) == kStdInt32Array;
}

ATTR_PURE const int32_t *raw_std_value_as_int32array(const struct raw_std_value *value) {
    assert(raw_std_value_is_int32array(value));
    return get_array_value_ptr(value, 4, raw_std_value_get_size(value));
}

ATTR_PURE bool raw_std_value_is_int64array(const struct raw_std_value *value) {
    return raw_std_value_get_type(value) == kStdInt64Array;
}

ATTR_PURE const int64_t *raw_std_value_as_int64array(const struct raw_std_value *value) {
    assert(raw_std_value_is_int64array(value));
    return get_array_value_ptr(value, 8, raw_std_value_get_size(value));
}

ATTR_PURE bool raw_std_value_is_float64array(const struct raw_std_value *value) {
    return raw_std_value_get_type(value) == kStdFloat64Array;
}

ATTR_PURE const double *raw_std_value_as_float64array(const struct raw_std_value *value) {
    assert(raw_std_value_is_float64array(value));
    return get_array_value_ptr(value, 8, raw_std_value_get_size(value));
}

ATTR_PURE bool raw_std_value_is_list(const struct raw_std_value *value) {
    return raw_std_value_get_type(value) == kStdList;
}

ATTR_PURE size_t raw_std_list_get_size(const struct raw_std_value *list) {
    assert(raw_std_value_is_list(list));
    return raw_std_value_get_size(list);
}

ATTR_PURE bool raw_std_value_is_map(const struct raw_std_value *value) {
    return raw_std_value_get_type(value) == kStdMap;
}

ATTR_PURE size_t raw_std_map_get_size(const struct raw_std_value *map) {
    assert(raw_std_value_is_map(map));
    return raw_std_value_get_size(map);
}

ATTR_PURE bool raw_std_value_is_float32array(const struct raw_std_value *value) {
    return raw_std_value_get_type(value) == kStdFloat32Array;
}

ATTR_PURE const float *raw_std_value_as_float32array(const struct raw_std_value *value) {
    assert(raw_std_value_is_float32array(value));
    return get_array_value_ptr(value, 4, raw_std_value_get_size(value));
}

ATTR_PURE bool raw_std_value_equals(const struct raw_std_value *a, const struct raw_std_value *b) {
    size_t length;
    int alignment, element_size;

    ASSERT_NOT_NULL(a);
    ASSERT_NOT_NULL(b);

    if (a == b) {
        return true;
    }

    if (raw_std_value_get_type(a) != raw_std_value_get_type(b)) {
        return false;
    }

    switch (raw_std_value_get_type(a)) {
        case kStdNull:
        case kStdTrue:
        case kStdFalse: return true;
        case kStdInt32: return raw_std_value_as_int32(a) == raw_std_value_as_int32(b);
        case kStdInt64: return raw_std_value_as_int64(a) == raw_std_value_as_int64(b);
        case kStdFloat64: return raw_std_value_as_float64(a) == raw_std_value_as_float64(b);
        case kStdString:
            alignment = 0;
            element_size = 1;
            goto memcmp_arrays;
        case kStdUInt8Array:
            alignment = 0;
            element_size = 1;
            goto memcmp_arrays;
        case kStdInt32Array:
            alignment = 4;
            element_size = 4;
            goto memcmp_arrays;
        case kStdInt64Array:
            alignment = 8;
            element_size = 8;

memcmp_arrays:
            if (raw_std_value_get_size(a) != raw_std_value_get_size(b)) {
                return false;
            }

            length = raw_std_value_get_size(a);
            const void *values_a = get_array_value_ptr(a, alignment, length);
            const void *values_b = get_array_value_ptr(b, alignment, length);
            return memcmp(values_a, values_b, length * element_size) == 0;
        case kStdFloat64Array:
            if (raw_std_value_get_size(a) != raw_std_value_get_size(b)) {
                return false;
            }

            length = raw_std_value_get_size(a);
            const double *a_doubles = raw_std_value_as_float64array(a);
            const double *b_doubles = raw_std_value_as_float64array(b);
            for (int i = 0; i < length; i++) {
                if (a_doubles[i] != b_doubles[i]) {
                    return false;
                }
            }

            return true;
        case kStdList:
            if (raw_std_value_get_size(a) != raw_std_value_get_size(b)) {
                return false;
            }

            const struct raw_std_value *cursor_a = raw_std_list_get_first_element(a);
            const struct raw_std_value *cursor_b = raw_std_list_get_first_element(b);
            for (int i = 0; i < raw_std_value_get_size(a);
                 i++, cursor_a = raw_std_value_after(cursor_a), cursor_b = raw_std_value_after(cursor_b)) {
                if (!raw_std_value_equals(cursor_a, cursor_b)) {
                    return false;
                }
            }

            return true;
        case kStdMap:
            if (raw_std_value_get_size(a) != raw_std_value_get_size(b)) {
                return false;
            }

            size_t size = raw_std_value_get_size(a);

            // key_from_b_also_in_a[i] == true means that there's a key in a that matches
            // the i-th key in b. So if we're searching for a key from a in b, we can safely ignore / don't need to compare
            // keys in b that have they're key_from_b_also_in_a set to true.
            bool *key_from_b_found_in_a = calloc(size, sizeof(bool));

            // for each key in a, look for a matching key in b.
            // once it's found, mark it as used, so we don't try matching it again.
            // then, check if the values associated with both keys are equal.
            for_each_entry_in_raw_std_map(key_a, value_a, a) {
                for_each_entry_in_raw_std_map_indexed(index_b, key_b, value_b, b) {
                    // we've already linked this entry from b to an entry from a.
                    // skip to the next entry.
                    if (key_from_b_found_in_a[index_b]) {
                        continue;
                    }

                    // Keys don't match. Skip to the next entry.
                    if (!raw_std_value_equals(key_a, key_b)) {
                        continue;
                    }

                    key_from_b_found_in_a[index_b] = true;

                    // the values associated with both keys must be equal.
                    if (raw_std_value_equals(value_a, value_b) == false) {
                        free(key_from_b_found_in_a);
                        return false;
                    }

                    goto found;
                }

                // we didn't find key_a in b.
                free(key_from_b_found_in_a);
                return false;

found:
                continue;
            }

            // each key, value pair in a was found in b.
            // because a and b have the same number of entries,
            // this must mean that every key, value pair in b must also be present in a.
            // (a subset of b) and (b subset of a) ==> a equals b.

            free(key_from_b_found_in_a);
            return true;
        case kStdFloat32Array:
            if (raw_std_value_get_size(a) != raw_std_value_get_size(b)) {
                return false;
            }

            length = raw_std_value_get_size(a);
            const float *a_floats = raw_std_value_as_float32array(a);
            const float *b_floats = raw_std_value_as_float32array(b);
            for (int i = 0; i < length; i++) {
                if (a_floats[i] != b_floats[i]) {
                    return false;
                }
            }

            return true;
        default: assert(false); return false;
    }
}

ATTR_PURE bool raw_std_value_is_bool(const struct raw_std_value *value) {
    return raw_std_value_is_true(value) || raw_std_value_is_false(value);
}

ATTR_PURE bool raw_std_value_as_bool(const struct raw_std_value *value) {
    assert(raw_std_value_is_bool(value));
    return raw_std_value_is_true(value);
}

ATTR_PURE bool raw_std_value_is_int(const struct raw_std_value *value) {
    return raw_std_value_is_int32(value) || raw_std_value_is_int64(value);
}

ATTR_PURE int64_t raw_std_value_as_int(const struct raw_std_value *value) {
    assert(raw_std_value_is_int(value));
    if (raw_std_value_is_int32(value)) {
        return raw_std_value_as_int32(value);
    } else {
        return raw_std_value_as_int64(value);
    }
}

ATTR_PURE size_t raw_std_value_get_size(const struct raw_std_value *value) {
    const uint8_t *byteptr;
    size_t size;

    assert(
        raw_std_value_is_uint8array(value) || raw_std_value_is_int32array(value) || raw_std_value_is_int64array(value) ||
        raw_std_value_is_float64array(value) || raw_std_value_is_string(value) || raw_std_value_is_list(value) ||
        raw_std_value_is_map(value) || raw_std_value_is_float32array(value)
    );

    byteptr = (const uint8_t *) value;

    // skip type byte
    byteptr++;

    size = *byteptr;
    if (size <= 253) {
        return size;
    } else if (size == 254) {
        size = 0;
        memcpy(&size, byteptr + 1, 2);
        assert(size >= 254);
        return size;
    } else if (size == 255) {
        size = 0;
        memcpy(&size, byteptr + 1, 4);
        assert(size >= 0x10000);
        return size;
    }

    UNREACHABLE();
}

ATTR_PURE const struct raw_std_value *raw_std_value_after(const struct raw_std_value *value) {
    size_t size;

    switch (raw_std_value_get_type(value)) {
        case kStdNull: return get_after_ptr(value, 0, 0);
        case kStdTrue: return get_after_ptr(value, 0, 0);
        case kStdFalse: return get_after_ptr(value, 0, 0);
        case kStdInt32: return get_after_ptr(value, 0, 4);
        case kStdInt64: return get_after_ptr(value, 0, 8);
        case kStdLargeInt:
        case kStdString: return get_array_after_ptr(value, 0, raw_std_value_get_size(value), 1);
        case kStdFloat64: return get_after_ptr(value, 8, 8);
        case kStdUInt8Array: return get_array_after_ptr(value, 0, raw_std_value_get_size(value), 1);
        case kStdInt32Array: return get_array_after_ptr(value, 4, raw_std_value_get_size(value), 4);
        case kStdInt64Array: return get_array_after_ptr(value, 8, raw_std_value_get_size(value), 8);
        case kStdFloat64Array: return get_array_after_ptr(value, 8, raw_std_value_get_size(value), 8);
        case kStdList:;
            size = raw_std_value_get_size(value);

            value = get_array_value_ptr(value, 0, size);
            for (; size > 0; size--) {
                value = raw_std_value_after(value);
            }

            return value;
        case kStdMap:
            size = raw_std_value_get_size(value);

            value = get_array_value_ptr(value, 0, size);
            for (; size > 0; size--) {
                value = raw_std_value_after(value);
                value = raw_std_value_after(value);
            }

            return value;
        case kStdFloat32Array: return get_array_after_ptr(value, 4, raw_std_value_get_size(value), 4);
        default: assert(false); return value;
    }
}

ATTR_PURE const struct raw_std_value *raw_std_list_get_first_element(const struct raw_std_value *list) {
    assert(raw_std_value_is_list(list));
    assert(raw_std_value_get_size(list) > 0);
    return get_array_value_ptr(list, 0, raw_std_value_get_size(list));
}

ATTR_PURE const struct raw_std_value *raw_std_list_get_nth_element(const struct raw_std_value *list, size_t index) {
    const struct raw_std_value *element;

    assert(raw_std_value_is_list(list));
    assert(raw_std_value_get_size(list) > index);

    element = raw_std_list_get_first_element(list);
    while (index != 0) {
        element = raw_std_value_after(element);
        index--;
    }

    return element;
}

ATTR_PURE const struct raw_std_value *raw_std_map_get_first_key(const struct raw_std_value *map) {
    assert(raw_std_value_is_map(map));

    return get_array_value_ptr(map, 0, raw_std_value_get_size(map));
}

ATTR_PURE const struct raw_std_value *raw_std_map_find(const struct raw_std_value *map, const struct raw_std_value *key) {
    assert(raw_std_value_is_map(map));

    for_each_entry_in_raw_std_map(key_iter, value_iter, map) {
        if (raw_std_value_equals(key_iter, key)) {
            return value_iter;
        }
    }

    return NULL;
}

ATTR_PURE const struct raw_std_value *raw_std_map_find_str(const struct raw_std_value *map, const char *str) {
    assert(raw_std_value_is_map(map));

    for_each_entry_in_raw_std_map(key_iter, value_iter, map) {
        if (raw_std_value_is_string(key_iter) && raw_std_string_equals(key_iter, str)) {
            return value_iter;
        }
    }

    return NULL;
}

ATTR_PURE static bool check_size(const struct raw_std_value *value, size_t buffer_size) {
    size_t size;

    const uint8_t *byteptr = (const uint8_t *) value;

    if (buffer_size < 2) {
        return false;
    }

    // skip type byte
    byteptr++;

    size = *byteptr;
    buffer_size -= 2;

    if (size == 254) {
        if (buffer_size < 2) {
            return false;
        }

        // Calculation in @ref get_array_value_ptr assumes an array size 254 <= s < 0x10000 uses 3 size bytes
        // If we allow a size smaller than 254 here, it would break that calculation.
        size = 0;
        memcpy(&size, byteptr + 1, 2);
        if (size < 254) {
            return false;
        }
    } else if (size == 255) {
        if (buffer_size < 4) {
            return false;
        }

        // See above.
        size = 0;
        memcpy(&size, byteptr + 1, 4);
        if (size < 0x10000) {
            return false;
        }
    }

    return true;
}

ATTR_PURE bool raw_std_value_check(const struct raw_std_value *value, size_t buffer_size) {
    size_t size;
    int alignment, element_size;

    if (buffer_size < 1) {
        return false;
    }

    switch (raw_std_value_get_type(value)) {
        case kStdNull:
        case kStdTrue:
        case kStdFalse: return true;
        case kStdInt32: return buffer_size >= 5;
        case kStdInt64: return buffer_size >= 9;
        case kStdFloat64: return buffer_size >= 9;
        case kStdString:
            alignment = 0;
            element_size = 1;
            goto check_arrays;
        case kStdUInt8Array:
            alignment = 0;
            element_size = 1;
            goto check_arrays;
        case kStdInt32Array:
            alignment = 4;
            element_size = 4;
            goto check_arrays;
        case kStdInt64Array:
            alignment = 8;
            element_size = 8;
            goto check_arrays;
        case kStdFloat64Array:
            alignment = 8;
            element_size = 8;
            goto check_arrays;
        case kStdFloat32Array:
            alignment = 4;
            element_size = 4;

// common code for checking if the buffer is large enough to contain
// a fixed size array.
check_arrays:

            // check if buffer is large enough to contain the value size.
            if (!check_size(value, buffer_size)) {
                return false;
            }

            // get the value size.
            size = raw_std_value_get_size(value);

            // get the offset of the actual array values.
            int diff = (intptr_t) get_array_value_ptr(value, alignment, size) - (intptr_t) value;
            assert(diff >= 0);

            if (buffer_size < diff) {
                return false;
            }
            buffer_size -= diff;

            // check if the buffer is large enough to contain all the the array values.
            if (buffer_size < size * element_size) {
                return false;
            }

            return true;
        case kStdList:
            // check if buffer is large enough to contain the value size.
            if (!check_size(value, buffer_size)) {
                return false;
            }

            // get the value size.
            size = raw_std_value_get_size(value);

            for_each_element_in_raw_std_list(element, value) {
                int diff = (intptr_t) element - (intptr_t) value;
                if (buffer_size < diff) {
                    return false;
                }

                if (!raw_std_value_check(element, buffer_size - diff)) {
                    return false;
                }
            }

            return true;
        case kStdMap:
            // check if buffer is large enough to contain the value size.
            if (!check_size(value, buffer_size)) {
                return false;
            }

            // get the value size.
            size = raw_std_value_get_size(value);

            const struct raw_std_value *key = NULL, *map_value;
            for (int diff, i = 0; i < size; i++) {
                if (key == NULL) {
                    key = raw_std_map_get_first_key(value);
                } else {
                    key = raw_std_value_after(map_value);
                }

                diff = (intptr_t) key - (intptr_t) value;
                if (buffer_size < diff) {
                    return false;
                }

                if (!raw_std_value_check(key, buffer_size - diff)) {
                    return false;
                }

                map_value = raw_std_value_after(key);

                diff = (intptr_t) map_value - (intptr_t) value;
                if (buffer_size < diff) {
                    return false;
                }

                if (!raw_std_value_check(map_value, buffer_size - diff)) {
                    return false;
                }
            }

            return true;
        default: return false;
    }
}

ATTR_PURE bool raw_std_method_call_check(const struct raw_std_value *value, size_t buffer_size) {
    if (!raw_std_value_check(value, buffer_size)) {
        return false;
    }

    // first value must be a string. (method name)
    if (!raw_std_value_is_string(value)) {
        return false;
    }

    const struct raw_std_value *after = raw_std_value_after(value);
    int diff = (intptr_t) after - (intptr_t) value;
    assert(diff <= buffer_size);

    if (!raw_std_value_check(after, buffer_size - diff)) {
        return false;
    }

    return true;
}

ATTR_PURE bool raw_std_method_call_response_check(const struct raw_std_value *value, size_t buffer_size) {
    // method call responses have non-standard encoding for the first byte.
    // first byte is zero for failure, non-zero for success.
    if (buffer_size < 1) {
        return false;
    }

    bool successful = (*(const uint8_t *) (value)) != 0;

    value = (void *) ((intptr_t) value + (intptr_t) 1);
    buffer_size -= 1;

    if (successful) {
        if (!raw_std_value_check(value, buffer_size)) {
            return false;
        }
    } else {
        // first value should be the error code (string).
        if (!raw_std_value_check(value, buffer_size) || !raw_std_value_is_string(value)) {
            return false;
        }

        const struct raw_std_value *second = raw_std_value_after(value);
        int diff = (intptr_t) second - (intptr_t) value;
        assert(diff <= buffer_size);

        // second value should be the error message. (string or null)
        if (!raw_std_value_check(second, buffer_size - diff)) {
            return false;
        }

        if (!raw_std_value_is_string(second) && !raw_std_value_is_null(second)) {
            return false;
        }

        const struct raw_std_value *third = raw_std_value_after(value);
        diff = (intptr_t) third - (intptr_t) value;
        assert(diff <= buffer_size);

        // third value is the error detail. Can be anything.
        if (!raw_std_value_check(third, buffer_size - diff)) {
            return false;
        }
    }

    return true;
}

ATTR_PURE bool raw_std_event_check(const struct raw_std_value *value, size_t buffer_size) {
    return raw_std_method_call_response_check(value, buffer_size);
}

ATTR_PURE const struct raw_std_value *raw_std_method_call_get_method(const struct raw_std_value *value) {
    assert(raw_std_value_is_string(value));
    return value;
}

ATTR_PURE bool raw_std_method_call_is_method(const struct raw_std_value *value, const char *method_name) {
    assert(raw_std_value_is_string(value));
    return raw_std_string_equals(value, method_name);
}

MALLOCLIKE MUST_CHECK char *raw_std_method_call_get_method_dup(const struct raw_std_value *value) {
    assert(raw_std_value_is_string(value));
    return raw_std_string_dup(value);
}

ATTR_PURE const struct raw_std_value *raw_std_method_call_get_arg(const struct raw_std_value *value) {
    return raw_std_value_after(value);
}
