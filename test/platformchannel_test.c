#define _GNU_SOURCE
#include <platformchannel.h>
#include <math.h>
#include <limits.h>

#include <unity.h>

#define RAW_STD_BUF(...) (const struct raw_std_value*) ((const uint8_t[]) {__VA_ARGS__})
#define AS_RAW_STD_VALUE(_value) ((const struct raw_std_value*) (_value))

// required by Unity.
void setUp() {}

void tearDown() {}

void test_raw_std_value_is_null() {
    TEST_ASSERT_TRUE(raw_std_value_is_null(RAW_STD_BUF(kStdNull)));
    TEST_ASSERT_FALSE(raw_std_value_is_null(RAW_STD_BUF(kStdTrue)));
}

void test_raw_std_value_is_true() {
    TEST_ASSERT_TRUE(raw_std_value_is_true(RAW_STD_BUF(kStdTrue)));
    TEST_ASSERT_FALSE(raw_std_value_is_true(RAW_STD_BUF(kStdFalse)));
}

void test_raw_std_value_is_false() {
    TEST_ASSERT_TRUE(raw_std_value_is_false(RAW_STD_BUF(kStdFalse)));
    TEST_ASSERT_FALSE(raw_std_value_is_false(RAW_STD_BUF(kStdTrue)));
}

void test_raw_std_value_is_int32() {
    TEST_ASSERT_TRUE(raw_std_value_is_int32(RAW_STD_BUF(kStdInt32)));
    TEST_ASSERT_FALSE(raw_std_value_is_int32(RAW_STD_BUF(kStdNull)));
}

void test_raw_std_value_as_int32() {
    uint8_t buffer[5] = {
        kStdInt32,
        0, 0, 0, 0
    };


    TEST_ASSERT_EQUAL_INT32(0, raw_std_value_as_int32(AS_RAW_STD_VALUE(buffer)));

    int value = -2003205;
    memcpy(buffer + 1, &value, sizeof(int));

    TEST_ASSERT_EQUAL_INT32(-2003205, raw_std_value_as_int32(AS_RAW_STD_VALUE(buffer)));
}

void test_raw_std_value_is_int64() {
    TEST_ASSERT_TRUE(raw_std_value_is_int64(RAW_STD_BUF(kStdInt64)));
    TEST_ASSERT_FALSE(raw_std_value_is_int64(RAW_STD_BUF(kStdNull)));
}

void test_raw_std_value_as_int64() {
    uint8_t buffer[9] = {
        kStdInt64,
        0, 0, 0, 0, 0, 0, 0, 0
    };

    TEST_ASSERT_EQUAL_INT64(0, raw_std_value_as_int64(AS_RAW_STD_VALUE(buffer)));

    int64_t value = -7998090352538419200;
    memcpy(buffer + 1, &value, sizeof(value));

    TEST_ASSERT_EQUAL_INT64(-7998090352538419200, raw_std_value_as_int64(AS_RAW_STD_VALUE(buffer)));
}

void test_raw_std_value_is_float64() {
    TEST_ASSERT_TRUE(raw_std_value_is_float64(RAW_STD_BUF(kStdFloat64)));
    TEST_ASSERT_FALSE(raw_std_value_is_float64(RAW_STD_BUF(kStdNull)));
}

void test_raw_std_value_as_float64() {
    uint8_t buffer[] = {
        kStdFloat64,
        0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0
    };

    double value = M_PI;
    memcpy(buffer + 8, &value, sizeof(value));

    TEST_ASSERT_EQUAL_DOUBLE(M_PI, raw_std_value_as_float64(AS_RAW_STD_VALUE(buffer)));

    value = INFINITY;
    memcpy(buffer + 8, &value, sizeof(value));

    TEST_ASSERT_EQUAL_DOUBLE(INFINITY, raw_std_value_as_float64(AS_RAW_STD_VALUE(buffer)));
}

void test_raw_std_value_is_string() {
    TEST_ASSERT_TRUE(raw_std_value_is_string(RAW_STD_BUF(kStdString)));
    TEST_ASSERT_FALSE(raw_std_value_is_string(RAW_STD_BUF(kStdNull)));
}

void test_raw_std_string_dup() {
    const char *str = "The quick brown fox jumps over the lazy dog.";

    uint8_t buffer[1 + 1 + 45] = {
        kStdString, 45, 0
    };

    memcpy(buffer + 2, str, strlen(str));

    char *str_duped = raw_std_string_dup(AS_RAW_STD_VALUE(buffer));
    TEST_ASSERT_NOT_NULL(str_duped);
    TEST_ASSERT_EQUAL_STRING(str, str_duped);

    free(str_duped);

    buffer[1] = 0;
    str_duped = raw_std_string_dup(AS_RAW_STD_VALUE(buffer));
    TEST_ASSERT_NOT_NULL(str_duped);
    TEST_ASSERT_EQUAL_STRING("", str_duped);

    free(str_duped);
}

void test_raw_std_string_equals() {
    const char *str = "The quick brown fox jumps over the lazy dog.";

    uint8_t buffer[1 + 1 + strlen(str)];

    buffer[0] = kStdString;
    buffer[1] = strlen(str);

    // only string lengths less or equal 253 are actually encoded as one byte in
    // the standard message codec encoding.
    TEST_ASSERT_LESS_OR_EQUAL_size_t(253, strlen(str));

    memcpy(buffer + 2, str, strlen(str));

    TEST_ASSERT_TRUE(raw_std_string_equals(AS_RAW_STD_VALUE(buffer), "The quick brown fox jumps over the lazy dog."));
    TEST_ASSERT_FALSE(raw_std_string_equals(AS_RAW_STD_VALUE(buffer), "The quick brown fox jumps over the lazy dog"));
    
    buffer[1] = 0;
    TEST_ASSERT_TRUE(raw_std_string_equals(AS_RAW_STD_VALUE(buffer), ""));
    TEST_ASSERT_FALSE(raw_std_string_equals(AS_RAW_STD_VALUE(buffer), "anything"));
}

void test_raw_std_value_is_uint8array() {
    TEST_ASSERT_TRUE(raw_std_value_is_uint8array(RAW_STD_BUF(kStdUInt8Array)));
    TEST_ASSERT_FALSE(raw_std_value_is_uint8array(RAW_STD_BUF(kStdNull)));
}

void test_raw_std_value_as_uint8array() {
    uint8_t buffer[] = {
        kStdUInt8Array,
        4,
        1, 2, 3, 4
    };

    uint8_t expected[] = {
        1, 2, 3, 4
    };

    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, raw_std_value_as_uint8array(AS_RAW_STD_VALUE(buffer)), 4);

    buffer[2] = 0;
    expected[0] = 0;

    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, raw_std_value_as_uint8array(AS_RAW_STD_VALUE(buffer)), 4);
}

void test_raw_std_value_is_int32array() {
    TEST_ASSERT_TRUE(raw_std_value_is_int32array(RAW_STD_BUF(kStdInt32Array)));
    TEST_ASSERT_FALSE(raw_std_value_is_int32array(RAW_STD_BUF(kStdNull)));
}

void test_raw_std_value_as_int32array() {
    uint8_t buffer[] = {
        // type
        kStdInt32Array,
        // size
        2,
        // 2 alignment bytes
        0, 0,
        // space for 2 int32_t's
        0, 0, 0, 0,
        0, 0, 0, 0
    };

    int32_t expected[] = {
        INT_MIN,
        0x12345678,
    };

    memcpy(buffer + 4, expected, sizeof(expected));

    TEST_ASSERT_EQUAL_INT32_ARRAY(expected, raw_std_value_as_int32array(AS_RAW_STD_VALUE(buffer)), 2);

    expected[0] = 0;
    memcpy(buffer + 4, expected, sizeof(expected));

    TEST_ASSERT_EQUAL_INT32_ARRAY(expected, raw_std_value_as_int32array(AS_RAW_STD_VALUE(buffer)), 2);
}

void test_raw_std_value_is_int64array() {
    TEST_ASSERT_TRUE(raw_std_value_is_int64array(RAW_STD_BUF(kStdInt64Array)));
    TEST_ASSERT_FALSE(raw_std_value_is_int64array(RAW_STD_BUF(kStdNull)));
}

void test_raw_std_value_as_int64array() {
    uint8_t buffer[] = {
        // type
        kStdInt64Array,
        // size
        2,
        // 6 alignment bytes
        0, 0, 0, 0, 0, 0,
        // space for 2 int64_t's
        0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0,
    };

    int64_t expected[] = {
        INT64_MIN,
        0x123456789ABCDEF,
    };

    memcpy(buffer + 8, expected, sizeof(expected));

    TEST_ASSERT_EQUAL_INT64_ARRAY(expected, raw_std_value_as_int64array(AS_RAW_STD_VALUE(buffer)), 2);

    expected[0] = 0;
    memcpy(buffer + 8, expected, sizeof(expected));

    TEST_ASSERT_EQUAL_INT64_ARRAY(expected, raw_std_value_as_int64array(AS_RAW_STD_VALUE(buffer)), 2);
}

void test_raw_std_value_is_float64array() {
    TEST_ASSERT_TRUE(raw_std_value_is_float64array(RAW_STD_BUF(kStdFloat64Array)));
    TEST_ASSERT_FALSE(raw_std_value_is_float64array(RAW_STD_BUF(kStdNull)));
}

void test_raw_std_value_as_float64array() {
    uint8_t buffer[] = {
        // type
        kStdFloat64Array,
        // size
        2,
        // 6 alignment bytes
        0, 0, 0, 0, 0, 0,
        // space for 2 doubles
        0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0,
    };

    double expected[] = {
        M_PI,
        INFINITY,
    };

    memcpy(buffer + 8, expected, sizeof(expected));

    TEST_ASSERT_EQUAL_DOUBLE_ARRAY(expected, raw_std_value_as_float64array(AS_RAW_STD_VALUE(buffer)), 2);

    expected[0] = 0.0;
    memcpy(buffer + 8, expected, sizeof(expected));

    TEST_ASSERT_EQUAL_DOUBLE_ARRAY(expected, raw_std_value_as_float64array(AS_RAW_STD_VALUE(buffer)), 2);
}

void test_raw_std_value_is_list() {
    TEST_ASSERT_TRUE(raw_std_value_is_list(RAW_STD_BUF(kStdList)));
    TEST_ASSERT_FALSE(raw_std_value_is_list(RAW_STD_BUF(kStdNull)));
}

void test_raw_std_list_get_size() {
    uint8_t buffer[] = {
        // type
        kStdList,
        // size
        2,
        // space for more size bytes
        0, 0, 0, 0,
    };

    TEST_ASSERT_EQUAL_size_t(2, raw_std_list_get_size(AS_RAW_STD_VALUE(buffer)));

    buffer[1] = 0;

    TEST_ASSERT_EQUAL_size_t(0, raw_std_list_get_size(AS_RAW_STD_VALUE(buffer)));

    uint32_t size = 0xDEAD;
    
    buffer[1] = 254;
    memcpy(buffer + 2, &size, 2);

    TEST_ASSERT_EQUAL_size_t(0xDEAD, raw_std_list_get_size(AS_RAW_STD_VALUE(buffer)));

    size = 0xDEADBEEF;
    buffer[1] = 255;
    memcpy(buffer + 2, &size, 4);

    TEST_ASSERT_EQUAL_size_t(0xDEADBEEF, raw_std_list_get_size(AS_RAW_STD_VALUE(buffer)));
}

void test_raw_std_value_is_map() {
    TEST_ASSERT_TRUE(raw_std_value_is_map(RAW_STD_BUF(kStdMap)));
    TEST_ASSERT_FALSE(raw_std_value_is_map(RAW_STD_BUF(kStdNull)));
}

void test_raw_std_map_get_size() {
    uint8_t buffer[] = {
        // type
        kStdMap,
        // size
        2,
        // space for more size bytes
        0, 0, 0, 0,
    };

    TEST_ASSERT_EQUAL_size_t(2, raw_std_map_get_size(AS_RAW_STD_VALUE(buffer)));

    buffer[1] = 0;

    TEST_ASSERT_EQUAL_size_t(0, raw_std_map_get_size(AS_RAW_STD_VALUE(buffer)));

    uint32_t size = 0xDEAD;
    
    buffer[1] = 254;
    memcpy(buffer + 2, &size, 2);

    TEST_ASSERT_EQUAL_size_t(0xDEAD, raw_std_list_get_size(AS_RAW_STD_VALUE(buffer)));

    size = 0xDEADBEEF;
    buffer[1] = 255;
    memcpy(buffer + 2, &size, 4);

    TEST_ASSERT_EQUAL_size_t(0xDEADBEEF, raw_std_map_get_size(AS_RAW_STD_VALUE(buffer)));
}

void test_raw_std_value_is_float32array() {
    TEST_ASSERT_TRUE(raw_std_value_is_float32array(RAW_STD_BUF(kStdFloat32Array)));
    TEST_ASSERT_FALSE(raw_std_value_is_float32array(RAW_STD_BUF(kStdNull)));
}

void test_raw_std_value_as_float32array() {
    uint8_t buffer[] = {
        // type
        kStdFloat32Array,
        // size
        2,
        // 2 alignment bytes
        0, 0,
        // space for 2 floats
        0, 0, 0, 0,
        0, 0, 0, 0,
    };

    float expected[] = {
        M_PI,
        INFINITY,
    };

    memcpy(buffer + 4, expected, sizeof(expected));

    TEST_ASSERT_EQUAL_FLOAT_ARRAY(expected, raw_std_value_as_float32array(AS_RAW_STD_VALUE(buffer)), 2);

    expected[0] = 0.0;
    memcpy(buffer + 4, expected, sizeof(expected));

    TEST_ASSERT_EQUAL_FLOAT_ARRAY(expected, raw_std_value_as_float32array(AS_RAW_STD_VALUE(buffer)), 2);
}

void test_raw_std_value_equals() {
    TEST_ASSERT_TRUE(raw_std_value_equals(RAW_STD_BUF(kStdNull), RAW_STD_BUF(kStdNull)));
    TEST_ASSERT_FALSE(raw_std_value_equals(RAW_STD_BUF(kStdNull), RAW_STD_BUF(kStdTrue)));
    TEST_ASSERT_FALSE(raw_std_value_equals(RAW_STD_BUF(kStdTrue), RAW_STD_BUF(kStdFalse)));

    // int32
    {
        uint8_t lhs[] = {
            kStdInt32,
            1, 2, 3, 4,
        };

        uint8_t rhs[] = {
            kStdInt32,
            1, 2, 3, 4,
        };

        TEST_ASSERT_TRUE(raw_std_value_equals(AS_RAW_STD_VALUE(lhs), AS_RAW_STD_VALUE(rhs)));
        
        rhs[4] = 0;
        TEST_ASSERT_FALSE(raw_std_value_equals(AS_RAW_STD_VALUE(lhs), AS_RAW_STD_VALUE(rhs)));
    }

    // int64
    {
        uint8_t lhs[] = {
            kStdInt64,
            1, 2, 3, 4, 5, 6, 7, 8
        };

        uint8_t rhs[] = {
            kStdInt64,
            1, 2, 3, 4, 5, 6, 7, 8
        };

        TEST_ASSERT_TRUE(raw_std_value_equals(AS_RAW_STD_VALUE(lhs), AS_RAW_STD_VALUE(rhs)));
        
        rhs[8] = 0;
        TEST_ASSERT_FALSE(raw_std_value_equals(AS_RAW_STD_VALUE(lhs), AS_RAW_STD_VALUE(rhs)));
    }

    // float64
    {
        uint8_t lhs[] = {
            // type byte
            kStdFloat64,
            // 7 alignment bytes
            0, 0, 0, 0, 0, 0, 0,
            // bytes for 1 float64
            0, 0, 0, 0, 0, 0, 0, 0,
        };

        uint8_t rhs[] = {
            // type byte
            kStdFloat64,
            // 7 alignment bytes
            0, 0, 0, 0, 0, 0, 0,
            // bytes for 1 float64
            0, 0, 0, 0, 0, 0, 0, 0,
        };

        double f = M_PI;

        memcpy(lhs + 8, &f, sizeof(f));
        memcpy(rhs + 8, &f, sizeof(f));

        TEST_ASSERT_TRUE(raw_std_value_equals(AS_RAW_STD_VALUE(lhs), AS_RAW_STD_VALUE(rhs)));
        
        f = NAN;
        memcpy(rhs + 8, &f, sizeof(f));

        TEST_ASSERT_FALSE(raw_std_value_equals(AS_RAW_STD_VALUE(lhs), AS_RAW_STD_VALUE(rhs)));
    }

    // string
    {
        const char *str = "The quick brown fox jumps over the lazy dog.";

        uint8_t lhs[1 + 1 + strlen(str)];
        lhs[0] = kStdString;
        lhs[1] = strlen(str);

        uint8_t rhs[1 + 1 + strlen(str)];
        rhs[0] = kStdString;
        rhs[1] = strlen(str);

        // only string lengths less or equal 253 are actually encoded as one byte in
        // the standard message codec encoding.
        TEST_ASSERT_LESS_OR_EQUAL_size_t(253, strlen(str));

        memcpy(lhs + 2, str, strlen(str));
        memcpy(rhs + 2, str, strlen(str));

        TEST_ASSERT_TRUE(raw_std_value_equals(AS_RAW_STD_VALUE(lhs), AS_RAW_STD_VALUE(rhs)));

        rhs[1] = strlen(str) - 1;

        TEST_ASSERT_FALSE(raw_std_value_equals(AS_RAW_STD_VALUE(lhs), AS_RAW_STD_VALUE(rhs)));
        
        const char *str2 = "The quick brown fox jumps over the lazy DOG ";
        TEST_ASSERT_EQUAL_size_t(strlen(str), strlen(str2));
        rhs[1] = strlen(str2);
        memcpy(rhs + 2, str2, strlen(str2));

        TEST_ASSERT_FALSE(raw_std_value_equals(AS_RAW_STD_VALUE(lhs), AS_RAW_STD_VALUE(rhs)));
    }

    // uint8array
    {
        uint8_t lhs[] = {
            kStdUInt8Array,
            4,
            1, 2, 3, 4
        };

        uint8_t rhs[] = {
            kStdUInt8Array,
            4,
            1, 2, 3, 4
        };

        TEST_ASSERT_TRUE(raw_std_value_equals(AS_RAW_STD_VALUE(lhs), AS_RAW_STD_VALUE(rhs)));

        rhs[1] = 3;

        TEST_ASSERT_FALSE(raw_std_value_equals(AS_RAW_STD_VALUE(lhs), AS_RAW_STD_VALUE(rhs)));

        rhs[1] = 4;
        rhs[5] = 0;

        TEST_ASSERT_FALSE(raw_std_value_equals(AS_RAW_STD_VALUE(lhs), AS_RAW_STD_VALUE(rhs)));
    }

    // int32array
    {
        uint8_t lhs[] = {
            // type
            kStdInt32Array,
            // size
            2,
            // 2 alignment bytes
            0, 0,
            // space for 2 int32_t's
            0, 0, 0, 0,
            0, 0, 0, 0
        };

        uint8_t rhs[] = {
            // type
            kStdInt32Array,
            // size
            2,
            // 2 alignment bytes
            0, 0,
            // space for 2 int32_t's
            0, 0, 0, 0,
            0, 0, 0, 0
        };

        int32_t array[] = {
            INT_MIN,
            0x12345678,
        };

        memcpy(lhs + 4, array, sizeof(array));
        memcpy(rhs + 4, array, sizeof(array));

        TEST_ASSERT_TRUE(raw_std_value_equals(AS_RAW_STD_VALUE(lhs), AS_RAW_STD_VALUE(rhs)));

        rhs[1] = 0;

        TEST_ASSERT_FALSE(raw_std_value_equals(AS_RAW_STD_VALUE(lhs), AS_RAW_STD_VALUE(rhs)));

        rhs[1] = 2;
        int32_t array2[] = {
            INT_MAX,
            0x12345678,
        };
        memcpy(rhs + 4, array2, sizeof(array2));

        TEST_ASSERT_FALSE(raw_std_value_equals(AS_RAW_STD_VALUE(lhs), AS_RAW_STD_VALUE(rhs)));
    }

    // int64array
    {
        uint8_t lhs[] = {
            // type
            kStdInt64Array,
            // size
            2,
            // 6 alignment bytes
            0, 0, 0, 0, 0, 0,
            // space for 2 int64_t's
            0, 0, 0, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0,
        };

        uint8_t rhs[] = {
            // type
            kStdInt64Array,
            // size
            2,
            // 6 alignment bytes
            0, 0, 0, 0, 0, 0,
            // space for 2 int64_t's
            0, 0, 0, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0,
        };

        int64_t array[] = {
            INT64_MIN,
            0x123456789ABCDEF,
        };

        memcpy(lhs + 8, array, sizeof(array));
        memcpy(rhs + 8, array, sizeof(array));

        TEST_ASSERT_TRUE(raw_std_value_equals(AS_RAW_STD_VALUE(lhs), AS_RAW_STD_VALUE(rhs)));

        rhs[1] = 0;

        TEST_ASSERT_FALSE(raw_std_value_equals(AS_RAW_STD_VALUE(lhs), AS_RAW_STD_VALUE(rhs)));

        rhs[1] = 2;
        int64_t array2[] = {
            INT64_MAX,
            0x123456789ABCDEF,
        };
        memcpy(rhs + 8, array2, sizeof(array2));

        TEST_ASSERT_FALSE(raw_std_value_equals(AS_RAW_STD_VALUE(lhs), AS_RAW_STD_VALUE(rhs)));
    }

    // float64array
    {
        uint8_t lhs[] = {
            // type
            kStdFloat64Array,
            // size
            2,
            // 6 alignment bytes
            0, 0, 0, 0, 0, 0,
            // space for 2 doubles
            0, 0, 0, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0,
        };

        uint8_t rhs[] = {
            // type
            kStdFloat64Array,
            // size
            2,
            // 6 alignment bytes
            0, 0, 0, 0, 0, 0,
            // space for 2 doubles
            0, 0, 0, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0,
        };

        double array[] = {
            M_PI,
            INFINITY,
        };

        memcpy(lhs + 8, array, sizeof(array));
        memcpy(rhs + 8, array, sizeof(array));

        TEST_ASSERT_TRUE(raw_std_value_equals(AS_RAW_STD_VALUE(lhs), AS_RAW_STD_VALUE(rhs)));

        rhs[1] = 0;

        TEST_ASSERT_FALSE(raw_std_value_equals(AS_RAW_STD_VALUE(lhs), AS_RAW_STD_VALUE(rhs)));

        rhs[1] = 2;
        double array2[] = {
            0.0,
            INFINITY,
        };
        memcpy(rhs + 8, array2, sizeof(array2));

        TEST_ASSERT_FALSE(raw_std_value_equals(AS_RAW_STD_VALUE(lhs), AS_RAW_STD_VALUE(rhs)));
    }

    /// TODO: Test list
    /// TODO: Test map
    /// TODO: Test float32array
}

void test_raw_std_value_is_bool() {
    TEST_ASSERT_FALSE(raw_std_value_is_bool(RAW_STD_BUF(kStdNull)));
    TEST_ASSERT_TRUE(raw_std_value_is_bool(RAW_STD_BUF(kStdTrue)));
    TEST_ASSERT_TRUE(raw_std_value_is_bool(RAW_STD_BUF(kStdFalse)));
}

void test_raw_std_value_as_bool() {
    TEST_ASSERT_TRUE(raw_std_value_as_bool(RAW_STD_BUF(kStdTrue)));
    TEST_ASSERT_FALSE(raw_std_value_as_bool(RAW_STD_BUF(kStdFalse)));
}

void test_raw_std_value_is_int() {
    TEST_ASSERT_FALSE(raw_std_value_is_int(RAW_STD_BUF(kStdNull)));
    TEST_ASSERT_FALSE(raw_std_value_is_int(RAW_STD_BUF(kStdTrue)));
    TEST_ASSERT_FALSE(raw_std_value_is_int(RAW_STD_BUF(kStdFalse)));
    TEST_ASSERT_TRUE(raw_std_value_is_int(RAW_STD_BUF(kStdInt32)));
    TEST_ASSERT_TRUE(raw_std_value_is_int(RAW_STD_BUF(kStdInt64)));
    TEST_ASSERT_FALSE(raw_std_value_is_int(RAW_STD_BUF(kStdFloat64)));
}

void test_raw_std_value_as_int() {
    uint8_t buffer[9] = {
        kStdInt32,
        0, 0, 0, 0, 0, 0, 0, 0
    };

    int64_t int64 = INT64_MAX;
    buffer[0] = kStdInt64;
    memcpy(buffer + 1, &int64, sizeof(int64));

    TEST_ASSERT_EQUAL_INT64(INT64_MAX, raw_std_value_as_int(AS_RAW_STD_VALUE(buffer)));

    buffer[0] = kStdInt32;
    TEST_ASSERT_NOT_EQUAL_INT64(INT64_MAX, raw_std_value_as_int(AS_RAW_STD_VALUE(buffer)));    

    int32_t int32 = INT32_MIN;
    buffer[0] = kStdInt32;
    memcpy(buffer + 1, &int32, sizeof(int32));

    TEST_ASSERT_EQUAL_INT64(INT32_MIN, raw_std_value_as_int(AS_RAW_STD_VALUE(buffer)));
}

void test_raw_std_value_get_size() {
    uint8_t buffer[] = {
        // type
        kStdList,
        // size
        2,
        // space for more size bytes
        0, 0, 0, 0,
    };

    TEST_ASSERT_EQUAL_size_t(2, raw_std_value_get_size(AS_RAW_STD_VALUE(buffer)));

    buffer[1] = 0;

    TEST_ASSERT_EQUAL_size_t(0, raw_std_value_get_size(AS_RAW_STD_VALUE(buffer)));

    uint32_t size = 0xDEAD;
    
    buffer[1] = 254;
    memcpy(buffer + 2, &size, 2);
    memcpy(buffer + 4, &size, 2);

    TEST_ASSERT_EQUAL_size_t(0xDEAD, raw_std_value_get_size(AS_RAW_STD_VALUE(buffer)));

    size = 0xDEADBEEF;
    buffer[1] = 255;
    memcpy(buffer + 2, &size, 4);

    TEST_ASSERT_EQUAL_size_t(0xDEADBEEF, raw_std_value_get_size(AS_RAW_STD_VALUE(buffer)));
}

void test_raw_std_value_after() {

}

void test_raw_std_list_get_first_element() {

}

void test_raw_std_list_get_nth_element() {

}

void test_raw_std_map_get_first_key() {

}

void test_raw_std_map_find() {

}

void test_raw_std_map_find_str() {

}

void test_raw_std_value_check() {

}

void test_raw_std_method_call_check() {

}

void test_raw_std_method_call_response_check() {

}

void test_raw_std_event_check() {

}

void test_raw_std_method_call_get_method() {

}

void test_raw_std_method_call_get_method_dup() {

}

void test_raw_std_method_call_get_arg() {

}

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_raw_std_value_is_null);
    RUN_TEST(test_raw_std_value_is_true);
    RUN_TEST(test_raw_std_value_is_false);
    RUN_TEST(test_raw_std_value_is_int32);
    RUN_TEST(test_raw_std_value_as_int32);
    RUN_TEST(test_raw_std_value_is_int64);
    RUN_TEST(test_raw_std_value_as_int64);
    RUN_TEST(test_raw_std_value_is_float64);
    RUN_TEST(test_raw_std_value_as_float64);
    RUN_TEST(test_raw_std_value_is_string);
    RUN_TEST(test_raw_std_string_dup);
    RUN_TEST(test_raw_std_string_equals);
    RUN_TEST(test_raw_std_value_is_uint8array);
    RUN_TEST(test_raw_std_value_as_uint8array);
    RUN_TEST(test_raw_std_value_is_int32array);
    RUN_TEST(test_raw_std_value_as_int32array);
    RUN_TEST(test_raw_std_value_is_int64array);
    RUN_TEST(test_raw_std_value_as_int64array);
    RUN_TEST(test_raw_std_value_is_float64array);
    RUN_TEST(test_raw_std_value_as_float64array);
    RUN_TEST(test_raw_std_value_is_list);
    RUN_TEST(test_raw_std_list_get_size);
    RUN_TEST(test_raw_std_value_is_map);
    RUN_TEST(test_raw_std_map_get_size);
    RUN_TEST(test_raw_std_value_is_float32array);
    RUN_TEST(test_raw_std_value_as_float32array);
    RUN_TEST(test_raw_std_value_equals);
    RUN_TEST(test_raw_std_value_is_bool);
    RUN_TEST(test_raw_std_value_as_bool);
    RUN_TEST(test_raw_std_value_is_int);
    RUN_TEST(test_raw_std_value_as_int);
    RUN_TEST(test_raw_std_value_get_size);
    RUN_TEST(test_raw_std_value_after);
    RUN_TEST(test_raw_std_list_get_first_element);
    RUN_TEST(test_raw_std_list_get_nth_element);
    RUN_TEST(test_raw_std_map_get_first_key);
    RUN_TEST(test_raw_std_map_find);
    RUN_TEST(test_raw_std_map_find_str);
    RUN_TEST(test_raw_std_value_check);
    RUN_TEST(test_raw_std_method_call_check);
    RUN_TEST(test_raw_std_method_call_response_check);
    RUN_TEST(test_raw_std_event_check);
    RUN_TEST(test_raw_std_method_call_get_method);
    RUN_TEST(test_raw_std_method_call_get_method_dup);
    RUN_TEST(test_raw_std_method_call_get_arg);

    return UNITY_END();
}
