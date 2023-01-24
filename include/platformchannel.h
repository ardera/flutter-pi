#ifndef _METHODCHANNEL_H
#define _METHODCHANNEL_H

#include <stdint.h>
#include <errno.h>
#include <flutter_embedder.h>
#include <collection.h>

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
    kStdMap,
	kStdFloat32Array
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


#if defined(WARN_MISSING_FIELD_INITIALIZERS) && (defined(__GNUC__) || defined(__clang__)) 
#	define STDSTRING(str) (*({ \
		struct std_value *value = alloca(sizeof(struct std_value)); \
		memset(value, 0, sizeof *value); \
		value->type = kStdString; \
		value->string_value = (str); \
		value; \
	}))
#else
// somehow GCC warns about field "values" being uninitialized, even though it isn't.
// so if we haven't enabled warning about missing field initializers, we can use this, otherwise
// we somehow need to silence it.
#	define STDSTRING(str) ((struct std_value) {.type = kStdString, .string_value = (str)})
#	ifdef WARN_MISSING_FIELD_INITIALIZERS
#		pragma warning "Warning about missing field initializers but neither clang nor gcc is used - spurious warnings will ocurr."
#	endif
#endif

#define STDVALUE_IS_LIST(value) ((value).type == kStdList)
#define STDVALUE_IS_SIZE(value, _size) ((value).size == (_size))
#define STDVALUE_IS_SIZED_LIST(value, _size) (STDVALUE_IS_LIST(value) && STDVALUE_IS_SIZE(value, _size))

#define STDVALUE_IS_INT_ARRAY(value) (((value).type == kStdInt32Array) || ((value).type == kStdInt64Array) || ((value).type == kStdUInt8Array))
#define STDVALUE_IS_FLOAT_ARRAY(value) ((value).type == kStdFloat64Array)
#define STDVALUE_IS_NUM_ARRAY(value) (STDVALUE_IS_INT_ARRAY(value) || STDVALUE_IS_FLOAT_ARRAY(value))

#define STDVALUE_IS_MAP(value) ((value).type == kStdMap)
#define STDVALUE_IS_SIZED_MAP(value, _size) ((value).size == (_size))

#define STDMAP1(key1, val1) ((struct std_value) { \
	.type = kStdMap, \
	.size = 1, \
	.keys = (struct std_value[1]) { \
		(key1) \
	}, \
	.values = (struct std_value[1]) { \
		(val1) \
	} \
})

#define STDMAP2(key1, val1, key2, val2) ((struct std_value) { \
	.type = kStdMap, \
	.size = 2, \
	.keys = (struct std_value[2]) { \
		(key1), (key2) \
	}, \
	.values = (struct std_value[2]) { \
		(val1), (val2) \
	} \
})
#define STDMAP3(key1, val1, key2, val2, key3, val3) ((struct std_value) { \
	.type = kStdMap, \
	.size = 3, \
	.keys = (struct std_value[3]) { \
		(key1), (key2), (key3) \
	}, \
	.values = (struct std_value[3]) { \
		(val1), (val2), (val3) \
	} \
})
#define STDMAP4(key1, val1, key2, val2, key3, val3, key4, val4) ((struct std_value) { \
	.type = kStdMap, \
	.size = 4, \
	.keys = (struct std_value[4]) { \
		(key1), (key2), (key3), (key4) \
	}, \
	.values = (struct std_value[4]) { \
		(val1), (val2), (val3), (val4) \
	} \
})
#define STDMAP5(key1, val1, key2, val2, key3, val3, key4, val4, key5, val5) ((struct std_value) { \
	.type = kStdMap, \
	.size = 5, \
	.keys = (struct std_value[5]) { \
		(key1), (key2), (key3), (key4), (key5) \
	}, \
	.values = (struct std_value[5]) { \
		(val1), (val2), (val3), (val4), (val5) \
	} \
})
#define STDMAP6(key1, val1, key2, val2, key3, val3, key4, val4, key5, val5, key6, val6) ((struct std_value) { \
	.type = kStdMap, \
	.size = 6, \
	.keys = (struct std_value[6]) { \
		(key1), (key2), (key3), (key4), (key5), (key6) \
	}, \
	.values = (struct std_value[6]) { \
		(val1), (val2), (val3), (val4), (val5), (val6) \
	} \
})
#define STDMAP7(key1, val1, key2, val2, key3, val3, key4, val4, key5, val5, key6, val6, key7, val7) ((struct std_value) { \
	.type = kStdMap, \
	.size = 7, \
	.keys = (struct std_value[7]) { \
		(key1), (key2), (key3), (key4), (key5), (key6), (key7) \
	}, \
	.values = (struct std_value[7]) { \
		(val1), (val2), (val3), (val4), (val5), (val6), (val7) \
	} \
})
#define STDMAP8(key1, val1, key2, val2, key3, val3, key4, val4, key5, val5, key6, val6, key7, val7, key8, val8) ((struct std_value) { \
	.type = kStdMap, \
	.size = 8, \
	.keys = (struct std_value[8]) { \
		(key1), (key2), (key3), (key4), (key5), (key6), (key7), (key8) \
	}, \
	.values = (struct std_value[8]) { \
		(val1), (val2), (val3), (val4), (val5), (val6), (val7), (val8) \
	} \
})
#define STDMAP9(key1, val1, key2, val2, key3, val3, key4, val4, key5, val5, key6, val6, key7, val7, key8, val8, key9, val9) ((struct std_value) { \
	.type = kStdMap, \
	.size = 9, \
	.keys = (struct std_value[9]) { \
		(key1), (key2), (key3), (key4), (key5), (key6), (key7), (key8), (key9) \
	}, \
	.values = (struct std_value[9]) { \
		(val1), (val2), (val3), (val4), (val5), (val6), (val7), (val8), (val9) \
	} \
})
#define STDMAP10(key1, val1, key2, val2, key3, val3, key4, val4, key5, val5, key6, val6, key7, val7, key8, val8, key9, val9, key10, val10) ((struct std_value) { \
	.type = kStdMap, \
	.size = 10, \
	.keys = (struct std_value[10]) { \
		(key1), (key2), (key3), (key4), (key5), (key6), (key7), (key8), (key9), (key10) \
	}, \
	.values = (struct std_value[10]) { \
		(val1), (val2), (val3), (val4), (val5), (val6), (val7), (val8), (val9), (val10) \
	} \
})
#define STDMAP11(key1, val1, key2, val2, key3, val3, key4, val4, key5, val5, key6, val6, key7, val7, key8, val8, key9, val9, key10, val10, key11, val11) ((struct std_value) { \
	.type = kStdMap, \
	.size = 11, \
	.keys = (struct std_value[11]) { \
		(key1), (key2), (key3), (key4), (key5), (key6), (key7), (key8), (key9), (key10), (key11) \
	}, \
	.values = (struct std_value[11]) { \
		(val1), (val2), (val3), (val4), (val5), (val6), (val7), (val8), (val9), (val10), (val11) \
	} \
})
#define STDMAP12(key1, val1, key2, val2, key3, val3, key4, val4, key5, val5, key6, val6, key7, val7, key8, val8, key9, val9, key10, val10, key11, val11, key12, val12) ((struct std_value) { \
	.type = kStdMap, \
	.size = 12, \
	.keys = (struct std_value[12]) { \
		(key1), (key2), (key3), (key4), (key5), (key6), (key7), (key8), (key9), (key10), (key11), (key12) \
	}, \
	.values = (struct std_value[12]) { \
		(val1), (val2), (val3), (val4), (val5), (val6), (val7), (val8), (val9), (val10), (val11), (val12) \
	} \
})
#define STDMAP13(key1, val1, key2, val2, key3, val3, key4, val4, key5, val5, key6, val6, key7, val7, key8, val8, key9, val9, key10, val10, key11, val11, key12, val12, key13, val13) ((struct std_value) { \
	.type = kStdMap, \
	.size = 13, \
	.keys = (struct std_value[13]) { \
		(key1), (key2), (key3), (key4), (key5), (key6), (key7), (key8), (key9), (key10), (key11), (key12), (key13) \
	}, \
	.values = (struct std_value[13]) { \
		(val1), (val2), (val3), (val4), (val5), (val6), (val7), (val8), (val9), (val10), (val11), (val12), (val13) \
	} \
})
#define STDMAP14(key1, val1, key2, val2, key3, val3, key4, val4, key5, val5, key6, val6, key7, val7, key8, val8, key9, val9, key10, val10, key11, val11, key12, val12, key13, val13, key14, val14) ((struct std_value) { \
	.type = kStdMap, \
	.size = 14, \
	.keys = (struct std_value[14]) { \
		(key1), (key2), (key3), (key4), (key5), (key6), (key7), (key8), (key9), (key10), (key11), (key12), (key13), (key14) \
	}, \
	.values = (struct std_value[14]) { \
		(val1), (val2), (val3), (val4), (val5), (val6), (val7), (val8), (val9), (val10), (val11), (val12), (val13), (val14) \
	} \
})
#define STDMAP15(key1, val1, key2, val2, key3, val3, key4, val4, key5, val5, key6, val6, key7, val7, key8, val8, key9, val9, key10, val10, key11, val11, key12, val12, key13, val13, key14, val14, key15, val15) ((struct std_value) { \
	.type = kStdMap, \
	.size = 15, \
	.keys = (struct std_value[15]) { \
		(key1), (key2), (key3), (key4), (key5), (key6), (key7), (key8), (key9), (key10), (key11), (key12), (key13), (key14), (key15) \
	}, \
	.values = (struct std_value[15]) { \
		(val1), (val2), (val3), (val4), (val5), (val6), (val7), (val8), (val9), (val10), (val11), (val12), (val13), (val14), (val15) \
	} \
})
#define STDMAP16(key1, val1, key2, val2, key3, val3, key4, val4, key5, val5, key6, val6, key7, val7, key8, val8, key9, val9, key10, val10, key11, val11, key12, val12, key13, val13, key14, val14, key15, val15, key16, val16) ((struct std_value) { \
	.type = kStdMap, \
	.size = 16, \
	.keys = (struct std_value[16]) { \
		(key1), (key2), (key3), (key4), (key5), (key6), (key7), (key8), (key9), (key10), (key11), (key12), (key13), (key14), (key15), (key16) \
	}, \
	.values = (struct std_value[16]) { \
		(val1), (val2), (val3), (val4), (val5), (val6), (val7), (val8), (val9), (val10), (val11), (val12), (val13), (val14), (val15), (val16) \
	} \
})
#define STDMAP17(key1, val1, key2, val2, key3, val3, key4, val4, key5, val5, key6, val6, key7, val7, key8, val8, key9, val9, key10, val10, key11, val11, key12, val12, key13, val13, key14, val14, key15, val15, key16, val16, key17, val17) ((struct std_value) { \
	.type = kStdMap, \
	.size = 17, \
	.keys = (struct std_value[17]) { \
		(key1), (key2), (key3), (key4), (key5), (key6), (key7), (key8), (key9), (key10), (key11), (key12), (key13), (key14), (key15), (key16), (key17) \
	}, \
	.values = (struct std_value[17]) { \
		(val1), (val2), (val3), (val4), (val5), (val6), (val7), (val8), (val9), (val10), (val11), (val12), (val13), (val14), (val15), (val16), (val17) \
	} \
})
#define STDMAP18(key1, val1, key2, val2, key3, val3, key4, val4, key5, val5, key6, val6, key7, val7, key8, val8, key9, val9, key10, val10, key11, val11, key12, val12, key13, val13, key14, val14, key15, val15, key16, val16, key17, val17, key18, val18) ((struct std_value) { \
	.type = kStdMap, \
	.size = 18, \
	.keys = (struct std_value[18]) { \
		(key1), (key2), (key3), (key4), (key5), (key6), (key7), (key8), (key9), (key10), (key11), (key12), (key13), (key14), (key15), (key16), (key17), (key18) \
	}, \
	.values = (struct std_value[18]) { \
		(val1), (val2), (val3), (val4), (val5), (val6), (val7), (val8), (val9), (val10), (val11), (val12), (val13), (val14), (val15), (val16), (val17), (val18) \
	} \
})
#define STDMAP19(key1, val1, key2, val2, key3, val3, key4, val4, key5, val5, key6, val6, key7, val7, key8, val8, key9, val9, key10, val10, key11, val11, key12, val12, key13, val13, key14, val14, key15, val15, key16, val16, key17, val17, key18, val18, key19, val19) ((struct std_value) { \
	.type = kStdMap, \
	.size = 19, \
	.keys = (struct std_value[19]) { \
		(key1), (key2), (key3), (key4), (key5), (key6), (key7), (key8), (key9), (key10), (key11), (key12), (key13), (key14), (key15), (key16), (key17), (key18), (key19) \
	}, \
	.values = (struct std_value[19]) { \
		(val1), (val2), (val3), (val4), (val5), (val6), (val7), (val8), (val9), (val10), (val11), (val12), (val13), (val14), (val15), (val16), (val17), (val18), (val19) \
	} \
})
#define STDMAP20(key1, val1, key2, val2, key3, val3, key4, val4, key5, val5, key6, val6, key7, val7, key8, val8, key9, val9, key10, val10, key11, val11, key12, val12, key13, val13, key14, val14, key15, val15, key16, val16, key17, val17, key18, val18, key19, val19, key20, val20) ((struct std_value) { \
	.type = kStdMap, \
	.size = 20, \
	.keys = (struct std_value[20]) { \
		(key1), (key2), (key3), (key4), (key5), (key6), (key7), (key8), (key9), (key10), (key11), (key12), (key13), (key14), (key15), (key16), (key17), (key18), (key19), (key20) \
	}, \
	.values = (struct std_value[20]) { \
		(val1), (val2), (val3), (val4), (val5), (val6), (val7), (val8), (val9), (val10), (val11), (val12), (val13), (val14), (val15), (val16), (val17), (val18), (val19), (val20) \
	} \
})

#define STDLIST1(val1) ((struct std_value) { \
	.type = kStdList, \
	.size = 1, \
	.list = (struct std_value[1]) { \
		(val1) \
	} \
})

#define STDLIST2(val1, val2) ((struct std_value) { \
	.type = kStdList, \
	.size = 2, \
	.list = (struct std_value[2]) { \
		(val1), (val2) \
	} \
})

#define JSONVALUE_IS_NULL(value) ((value).type == kJsonNull)
#define JSONNULL ((struct json_value) {.type = kJsonNull})

#define JSONVALUE_IS_BOOL(value) (((value).type == kJsonTrue) || ((value).type == kJsonFalse))
#define JSONVALUE_AS_BOOL(value) ((value).type == kJsonTrue)
#define JSONBOOL(bool_value) ((struct json_value) {.type = (bool_value) ? kJsonTrue : kJsonFalse})

#define JSONVALUE_IS_NUM(value) ((value).type == kJsonNumber)
#define JSONVALUE_AS_NUM(value) ((value).number_value)
#define JSONNUM(_number_value) ((struct json_value) {.type = kJsonNumber, .number_value = (_number_value)})

#define JSONVALUE_IS_STRING(value) ((value).type == kJsonString)
#define JSONVALUE_AS_STRING(value) ((value).string_value)
#define JSONSTRING(str) ((struct json_value) {.type = kJsonString, .string_value = str})

#define JSONVALUE_IS_ARRAY(value) ((value).type == kJsonArray)
#define JSONVALUE_IS_SIZE(value, _size) ((value).size == (_size))
#define JSONVALUE_IS_SIZED_ARRAY(value, _size) (JSONVALUE_IS_ARRAY(value) && JSONVALUE_IS_SIZE(value, _size))

#define JSONVALUE_IS_OBJECT(value) ((value).type == kJsonObject)
#define JSONVALUE_IS_SIZED_OBJECT(value, _size) (JSONVALUE_IS_OBJECT(value) && JSONVALUE_IS_SIZE(value, _size))

#define JSONARRAY1(val1) ((struct json_value) { \
	.type = kJsonArray, \
	.size = 1, \
	.array = (struct json_value[1]) { \
		(val1) \
	} \
})
#define JSONARRAY2(val1, val2) ((struct json_value) { \
	.type = kJsonArray, \
	.size = 2, \
	.array = (struct json_value[2]) { \
		(val1), (val2) \
	} \
})
#define JSONARRAY3(val1, val2, val3) ((struct json_value) { \
	.type = kJsonArray, \
	.size = 3, \
	.array = (struct json_value[3]) { \
		(val1), (val2), (val3) \
	} \
})

#define JSONOBJECT1(key1, val1) ((struct json_value) { \
	.type = kJsonObject, \
	.size = 1, \
	.keys = (char *[1]) { \
		(key1) \
	}, \
	.values = (struct json_value[1]) { \
		(val1) \
	} \
})

#define JSONOBJECT2(key1, val1, key2, val2) ((struct json_value) { \
	.type = kJsonObject, \
	.size = 2, \
	.keys = (char *[2]) { \
		(key1), (key2) \
	}, \
	.values = (struct json_value[2]) { \
		(val1), (val2) \
	} \
})
#define JSONOBJECT3(key1, val1, key2, val2, key3, val3) ((struct json_value) { \
	.type = kJsonObject, \
	.size = 3, \
	.keys = (char *[3]) { \
		(key1), (key2), (key3) \
	}, \
	.values = (struct json_value[3]) { \
		(val1), (val2), (val3) \
	} \
})
#define JSONOBJECT4(key1, val1, key2, val2, key3, val3, key4, val4) ((struct json_value) { \
	.type = kJsonObject, \
	.size = 4, \
	.keys = (char *[4]) { \
		(key1), (key2), (key3), (key4) \
	}, \
	.values = (struct json_value[4]) { \
		(val1), (val2), (val3), (val4) \
	} \
})
#define JSONOBJECT5(key1, val1, key2, val2, key3, val3, key4, val4, key5, val5) ((struct json_value) { \
	.type = kJsonObject, \
	.size = 5, \
	.keys = (char *[5]) { \
		(key1), (key2), (key3), (key4), (key5) \
	}, \
	.values = (struct json_value[5]) { \
		(val1), (val2), (val3), (val4), (val5) \
	} \
})
#define JSONOBJECT6(key1, val1, key2, val2, key3, val3, key4, val4, key5, val5, key6, val6) ((struct json_value) { \
	.type = kJsonObject, \
	.size = 6, \
	.keys = (char *[6]) { \
		(key1), (key2), (key3), (key4), (key5), (key6) \
	}, \
	.values = (struct json_value[6]) { \
		(val1), (val2), (val3), (val4), (val5), (val6) \
	} \
})
#define JSONOBJECT7(key1, val1, key2, val2, key3, val3, key4, val4, key5, val5, key6, val6, key7, val7) ((struct json_value) { \
	.type = kJsonObject, \
	.size = 7, \
	.keys = (char *[7]) { \
		(key1), (key2), (key3), (key4), (key5), (key6), (key7) \
	}, \
	.values = (struct json_value[7]) { \
		(val1), (val2), (val3), (val4), (val5), (val6), (val7) \
	} \
})
#define JSONOBJECT8(key1, val1, key2, val2, key3, val3, key4, val4, key5, val5, key6, val6, key7, val7, key8, val8) ((struct json_value) { \
	.type = kJsonObject, \
	.size = 8, \
	.keys = (char *[8]) { \
		(key1), (key2), (key3), (key4), (key5), (key6), (key7), (key8) \
	}, \
	.values = (struct json_value[8]) { \
		(val1), (val2), (val3), (val4), (val5), (val6), (val7), (val8) \
	} \
})
#define JSONOBJECT9(key1, val1, key2, val2, key3, val3, key4, val4, key5, val5, key6, val6, key7, val7, key8, val8, key9, val9) ((struct json_value) { \
	.type = kJsonObject, \
	.size = 9, \
	.keys = (char *[9]) { \
		(key1), (key2), (key3), (key4), (key5), (key6), (key7), (key8), (key9) \
	}, \
	.values = (struct json_value[9]) { \
		(val1), (val2), (val3), (val4), (val5), (val6), (val7), (val8), (val9) \
	} \
})
#define JSONOBJECT10(key1, val1, key2, val2, key3, val3, key4, val4, key5, val5, key6, val6, key7, val7, key8, val8, key9, val9, key10, val10) ((struct json_value) { \
	.type = kJsonObject, \
	.size = 10, \
	.keys = (char *[10]) { \
		(key1), (key2), (key3), (key4), (key5), (key6), (key7), (key8), (key9), (key10) \
	}, \
	.values = (struct json_value[10]) { \
		(val1), (val2), (val3), (val4), (val5), (val6), (val7), (val8), (val9), (val10) \
	} \
})
#define JSONOBJECT11(key1, val1, key2, val2, key3, val3, key4, val4, key5, val5, key6, val6, key7, val7, key8, val8, key9, val9, key10, val10, key11, val11) ((struct json_value) { \
	.type = kJsonObject, \
	.size = 11, \
	.keys = (char *[11]) { \
		(key1), (key2), (key3), (key4), (key5), (key6), (key7), (key8), (key9), (key10), (key11) \
	}, \
	.values = (struct json_value[11]) { \
		(val1), (val2), (val3), (val4), (val5), (val6), (val7), (val8), (val9), (val10), (val11) \
	} \
})
#define JSONOBJECT12(key1, val1, key2, val2, key3, val3, key4, val4, key5, val5, key6, val6, key7, val7, key8, val8, key9, val9, key10, val10, key11, val11, key12, val12) ((struct json_value) { \
	.type = kJsonObject, \
	.size = 12, \
	.keys = (char *[12]) { \
		(key1), (key2), (key3), (key4), (key5), (key6), (key7), (key8), (key9), (key10), (key11), (key12) \
	}, \
	.values = (struct json_value[12]) { \
		(val1), (val2), (val3), (val4), (val5), (val6), (val7), (val8), (val9), (val10), (val11), (val12) \
	} \
})
#define JSONOBJECT13(key1, val1, key2, val2, key3, val3, key4, val4, key5, val5, key6, val6, key7, val7, key8, val8, key9, val9, key10, val10, key11, val11, key12, val12, key13, val13) ((struct json_value) { \
	.type = kJsonObject, \
	.size = 13, \
	.keys = (char *[13]) { \
		(key1), (key2), (key3), (key4), (key5), (key6), (key7), (key8), (key9), (key10), (key11), (key12), (key13) \
	}, \
	.values = (struct json_value[13]) { \
		(val1), (val2), (val3), (val4), (val5), (val6), (val7), (val8), (val9), (val10), (val11), (val12), (val13) \
	} \
})
#define JSONOBJECT14(key1, val1, key2, val2, key3, val3, key4, val4, key5, val5, key6, val6, key7, val7, key8, val8, key9, val9, key10, val10, key11, val11, key12, val12, key13, val13, key14, val14) ((struct json_value) { \
	.type = kJsonObject, \
	.size = 14, \
	.keys = (char *[14]) { \
		(key1), (key2), (key3), (key4), (key5), (key6), (key7), (key8), (key9), (key10), (key11), (key12), (key13), (key14) \
	}, \
	.values = (struct json_value[14]) { \
		(val1), (val2), (val3), (val4), (val5), (val6), (val7), (val8), (val9), (val10), (val11), (val12), (val13), (val14) \
	} \
})
#define JSONOBJECT15(key1, val1, key2, val2, key3, val3, key4, val4, key5, val5, key6, val6, key7, val7, key8, val8, key9, val9, key10, val10, key11, val11, key12, val12, key13, val13, key14, val14, key15, val15) ((struct json_value) { \
	.type = kJsonObject, \
	.size = 15, \
	.keys = (char *[15]) { \
		(key1), (key2), (key3), (key4), (key5), (key6), (key7), (key8), (key9), (key10), (key11), (key12), (key13), (key14), (key15) \
	}, \
	.values = (struct json_value[15]) { \
		(val1), (val2), (val3), (val4), (val5), (val6), (val7), (val8), (val9), (val10), (val11), (val12), (val13), (val14), (val15) \
	} \
})
#define JSONOBJECT16(key1, val1, key2, val2, key3, val3, key4, val4, key5, val5, key6, val6, key7, val7, key8, val8, key9, val9, key10, val10, key11, val11, key12, val12, key13, val13, key14, val14, key15, val15, key16, val16) ((struct json_value) { \
	.type = kJsonObject, \
	.size = 16, \
	.keys = (char *[16]) { \
		(key1), (key2), (key3), (key4), (key5), (key6), (key7), (key8), (key9), (key10), (key11), (key12), (key13), (key14), (key15), (key16) \
	}, \
	.values = (struct json_value[16]) { \
		(val1), (val2), (val3), (val4), (val5), (val6), (val7), (val8), (val9), (val10), (val11), (val12), (val13), (val14), (val15), (val16) \
	} \
})
#define JSONOBJECT17(key1, val1, key2, val2, key3, val3, key4, val4, key5, val5, key6, val6, key7, val7, key8, val8, key9, val9, key10, val10, key11, val11, key12, val12, key13, val13, key14, val14, key15, val15, key16, val16, key17, val17) ((struct json_value) { \
	.type = kJsonObject, \
	.size = 17, \
	.keys = (char *[17]) { \
		(key1), (key2), (key3), (key4), (key5), (key6), (key7), (key8), (key9), (key10), (key11), (key12), (key13), (key14), (key15), (key16), (key17) \
	}, \
	.values = (struct json_value[17]) { \
		(val1), (val2), (val3), (val4), (val5), (val6), (val7), (val8), (val9), (val10), (val11), (val12), (val13), (val14), (val15), (val16), (val17) \
	} \
})
#define JSONOBJECT18(key1, val1, key2, val2, key3, val3, key4, val4, key5, val5, key6, val6, key7, val7, key8, val8, key9, val9, key10, val10, key11, val11, key12, val12, key13, val13, key14, val14, key15, val15, key16, val16, key17, val17, key18, val18) ((struct json_value) { \
	.type = kJsonObject, \
	.size = 18, \
	.keys = (char *[18]) { \
		(key1), (key2), (key3), (key4), (key5), (key6), (key7), (key8), (key9), (key10), (key11), (key12), (key13), (key14), (key15), (key16), (key17), (key18) \
	}, \
	.values = (struct json_value[18]) { \
		(val1), (val2), (val3), (val4), (val5), (val6), (val7), (val8), (val9), (val10), (val11), (val12), (val13), (val14), (val15), (val16), (val17), (val18) \
	} \
})
#define JSONOBJECT19(key1, val1, key2, val2, key3, val3, key4, val4, key5, val5, key6, val6, key7, val7, key8, val8, key9, val9, key10, val10, key11, val11, key12, val12, key13, val13, key14, val14, key15, val15, key16, val16, key17, val17, key18, val18, key19, val19) ((struct json_value) { \
	.type = kJsonObject, \
	.size = 19, \
	.keys = (char *[19]) { \
		(key1), (key2), (key3), (key4), (key5), (key6), (key7), (key8), (key9), (key10), (key11), (key12), (key13), (key14), (key15), (key16), (key17), (key18), (key19) \
	}, \
	.values = (struct json_value[19]) { \
		(val1), (val2), (val3), (val4), (val5), (val6), (val7), (val8), (val9), (val10), (val11), (val12), (val13), (val14), (val15), (val16), (val17), (val18), (val19) \
	} \
})
#define JSONOBJECT20(key1, val1, key2, val2, key3, val3, key4, val4, key5, val5, key6, val6, key7, val7, key8, val8, key9, val9, key10, val10, key11, val11, key12, val12, key13, val13, key14, val14, key15, val15, key16, val16, key17, val17, key18, val18, key19, val19, key20, val20) ((struct json_value) { \
	.type = kJsonObject, \
	.size = 20, \
	.keys = (char *[20]) { \
		(key1), (key2), (key3), (key4), (key5), (key6), (key7), (key8), (key9), (key10), (key11), (key12), (key13), (key14), (key15), (key16), (key17), (key18), (key19), (key20) \
	}, \
	.values = (struct json_value[20]) { \
		(val1), (val2), (val3), (val4), (val5), (val6), (val7), (val8), (val9), (val10), (val11), (val12), (val13), (val14), (val15), (val16), (val17), (val18), (val19), (val20) \
	} \
})

#pragma GCC diagnostic pop

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
///   kStandardMessageCodec:
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

#define PLATCH_OBJ_NOT_IMPLEMENTED ((struct platch_obj) {.codec = kNotImplemented})
#define PLATCH_OBJ_STRING(string) ((struct platch_obj) {.codec = kStringCodec, .string_value = (string)})
#define PLATCH_OBJ_BINARY_DATA(data, data_size) ((struct platch_obj) {.codec = kBinaryCodec, .binarydata_size = (data_size), .binarydata = (data)})
#define PLATCH_OBJ_JSON_MSG(__json_value) ((struct platch_obj) {.codec = kJSONMessageCodec, .json_value = (__json_value)})
#define PLATCH_OBJ_STD_MSG(__std_value) ((struct platch_obj) {.codec = kStandardMessageCodec, .std_value = (__std_value)})
#define PLATCH_OBJ_STD_CALL(method_name, arg) ((struct platch_obj) {.codec = kStandardMethodCall, .method = (char*) (method_name), .std_arg = (arg)})
#define PLATCH_OBJ_JSON_CALL(method_name, arg) ((struct platch_obj) {.codec = kJSONMethodCall, .method = (char*) (method_name), .json_arg = (arg)})
#define PLATCH_OBJ_STD_CALL_SUCCESS_RESPONSE(result) ((struct platch_obj) {.codec = kStandardMethodCallResponse, .success = true, .std_result = (result)})
#define PLATCH_OBJ_STD_CALL_ERROR_RESPONSE(code, msg, details) ((struct platch_obj) {.codec = kStandardMethodCallResponse, .success = false, .error_code = (char*) (code), .error_msg = (char*) (msg), .std_error_details = (details)})
#define PLATCH_OBJ_JSON_CALL_SUCCESS_RESPONSE(result) ((struct platch_obj) {.codec = kJSONMethodCallResponse, .success = true, .json_result = (result)})
#define PLATCH_OBJ_JSON_CALL_ERROR_RESPONSE(code, msg, details) ((struct platch_obj) {.codec = kStandardMethodCallResponse, .success = false, .error_code = (char*) (code), .error_msg = (char*) (msg), .json_error_details = (details)})
#define PLATCH_OBJ_STD_SUCCESS_EVENT(value) PLATCH_OBJ_STD_CALL_SUCCESS_RESPONSE(value)
#define PLATCH_OBJ_STD_ERROR_EVENT(code, msg, details) PLATCH_OBJ_STD_CALL_ERROR_RESPONSE(code, msg, details)
#define PLATCH_OBJ_JSON_SUCCESS_EVENT(value) PLATCH_OBJ_JSON_CALL_SUCCESS_RESPONSE(value)
#define PLATCH_OBJ_JSON_ERROR_EVENT(code, msg, details) PLATCH_OBJ_JSON_CALL_ERROR_RESPONSE(code, msg, details)

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

int platch_respond_illegal_arg_ext_std(
	FlutterPlatformMessageResponseHandle *handle,
	char *error_msg,
	struct std_value *error_details
);

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

int platch_respond_illegal_arg_ext_pigeon(
	FlutterPlatformMessageResponseHandle *handle,
	char *error_msg,
	struct std_value *error_details
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

struct raw_std_value;

ATTR_PURE bool raw_std_value_is_null(const struct raw_std_value *value);
ATTR_PURE bool raw_std_value_is_true(const struct raw_std_value *value);
ATTR_PURE bool raw_std_value_is_false(const struct raw_std_value *value);
ATTR_PURE bool raw_std_value_is_int32(const struct raw_std_value *value);
ATTR_PURE int32_t raw_std_value_as_int32(const struct raw_std_value *value);
ATTR_PURE bool raw_std_value_is_int64(const struct raw_std_value *value);
ATTR_PURE int64_t raw_std_value_as_int64(const struct raw_std_value *value);
ATTR_PURE bool raw_std_value_is_float64(const struct raw_std_value *value);
ATTR_PURE double raw_std_value_as_float64(const struct raw_std_value *value);
ATTR_PURE bool raw_std_value_is_string(const struct raw_std_value *value);
ATTR_PURE ATTR_MALLOC char *raw_std_string_dup(const struct raw_std_value *value);
ATTR_PURE bool raw_std_string_equals(const struct raw_std_value *value, const char *str);
ATTR_PURE bool raw_std_value_is_uint8array(const struct raw_std_value *value);
ATTR_PURE const uint8_t *raw_std_value_as_uint8array(const struct raw_std_value *value);
ATTR_PURE bool raw_std_value_is_int32array(const struct raw_std_value *value);
ATTR_PURE const int32_t *raw_std_value_as_int32array(const struct raw_std_value *value);
ATTR_PURE bool raw_std_value_is_int64array(const struct raw_std_value *value);
ATTR_PURE const int64_t *raw_std_value_as_int64array(const struct raw_std_value *value);
ATTR_PURE bool raw_std_value_is_float64array(const struct raw_std_value *value);
ATTR_PURE const double *raw_std_value_as_float64array(const struct raw_std_value *value);
ATTR_PURE bool raw_std_value_is_list(const struct raw_std_value *value);
ATTR_PURE size_t raw_std_list_get_size(const struct raw_std_value *list);
ATTR_PURE bool raw_std_value_is_map(const struct raw_std_value *value);
ATTR_PURE size_t raw_std_map_get_size(const struct raw_std_value *map);
ATTR_PURE bool raw_std_value_is_float32array(const struct raw_std_value *value);
ATTR_PURE const float *raw_std_value_as_float32array(const struct raw_std_value *value);

ATTR_PURE bool raw_std_value_equals(const struct raw_std_value *a, const struct raw_std_value *b);
ATTR_PURE bool raw_std_value_is_bool(const struct raw_std_value *value);
ATTR_PURE bool raw_std_value_as_bool(const struct raw_std_value *value);
ATTR_PURE bool raw_std_value_is_int(const struct raw_std_value *value);
ATTR_PURE int64_t raw_std_value_as_int(const struct raw_std_value *value);
ATTR_PURE size_t raw_std_value_get_size(const struct raw_std_value *value);
ATTR_PURE const struct raw_std_value *raw_std_value_after(const struct raw_std_value *value);
ATTR_PURE const struct raw_std_value *raw_std_list_get_first_element(const struct raw_std_value *list);
ATTR_PURE const struct raw_std_value *raw_std_list_get_nth_element(const struct raw_std_value *list, size_t index);
ATTR_PURE const struct raw_std_value *raw_std_map_get_first_key(const struct raw_std_value *map);
ATTR_PURE const struct raw_std_value *raw_std_map_find(const struct raw_std_value *map, const struct raw_std_value *key);
ATTR_PURE const struct raw_std_value *raw_std_map_find_str(const struct raw_std_value *map, const char *str);

ATTR_PURE bool raw_std_value_check(const struct raw_std_value *value, size_t buffer_size);
ATTR_PURE bool raw_std_method_call_check(const struct raw_std_value *value, size_t buffer_size);
ATTR_PURE bool raw_std_method_call_response_check(const struct raw_std_value *value, size_t buffer_size);
ATTR_PURE bool raw_std_event_check(const struct raw_std_value *value, size_t buffer_size);

ATTR_PURE const struct raw_std_value *raw_std_method_call_get_method(const struct raw_std_value *value);
ATTR_PURE ATTR_MALLOC char *raw_std_method_call_get_method_dup(const struct raw_std_value *value);
ATTR_PURE const struct raw_std_value *raw_std_method_call_get_arg(const struct raw_std_value *value);

#define CONCAT(a, b) CONCAT_INNER(a, b)
#define CONCAT_INNER(a, b) a ## b

#define UNIQUE_NAME(base) CONCAT(base, __COUNTER__)

#define for_each_entry_in_raw_std_map_indexed(index, key, value, map) \
	for ( \
		const struct raw_std_value *key = raw_std_map_get_first_key(map), *value = raw_std_value_after(key), *guard = NULL; \
		guard == NULL; \
		guard = (void*) 1 \
	) \
		for ( \
			size_t index = 0; \
			index < raw_std_map_get_size(map); \
			index++, \
				key = raw_std_value_after(value), \
				value = raw_std_value_after(key) \
		)

#define for_each_entry_in_raw_std_map(key, value, map) \
	for_each_entry_in_raw_std_map_indexed(UNIQUE_NAME(__raw_std_map_entry_index), key, value, map)

#define for_each_element_in_raw_std_list_indexed(index, element, list) \
	for ( \
		const struct raw_std_value *element = raw_std_list_get_first_element(list), *guard = NULL; \
		guard == NULL; \
		guard = (void*) 1 \
	) \
		for ( \
			size_t index = 0; \
			index < raw_std_list_get_size(list); \
			index++, \
				element = raw_std_value_after(element) \
		)

#define for_each_element_in_raw_std_list(value, list) \
	for_each_element_in_raw_std_list_indexed(UNIQUE_NAME(__raw_std_list_element_index), value, list)


#endif