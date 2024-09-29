#define _GNU_SOURCE
#include "platformchannel.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdalign.h>

#include <unity.h>

#define RAW_STD_BUF(...) (const struct raw_std_value *) ((const uint8_t[]){ __VA_ARGS__ })
#define AS_RAW_STD_VALUE(_value) ((const struct raw_std_value *) (_value))

#define DBL_INFINITY ((double) INFINITY)
#define DBL_NAN ((double) NAN)

// required by Unity.
void setUp(void) {
}

void tearDown(void) {
}

void test_raw_std_value_is_null(void) {
    TEST_ASSERT_TRUE(raw_std_value_is_null(RAW_STD_BUF(kStdNull)));
    TEST_ASSERT_FALSE(raw_std_value_is_null(RAW_STD_BUF(kStdTrue)));
}

void test_raw_std_value_is_true(void) {
    TEST_ASSERT_TRUE(raw_std_value_is_true(RAW_STD_BUF(kStdTrue)));
    TEST_ASSERT_FALSE(raw_std_value_is_true(RAW_STD_BUF(kStdFalse)));
}

void test_raw_std_value_is_false(void) {
    TEST_ASSERT_TRUE(raw_std_value_is_false(RAW_STD_BUF(kStdFalse)));
    TEST_ASSERT_FALSE(raw_std_value_is_false(RAW_STD_BUF(kStdTrue)));
}

void test_raw_std_value_is_int32(void) {
    TEST_ASSERT_TRUE(raw_std_value_is_int32(RAW_STD_BUF(kStdInt32)));
    TEST_ASSERT_FALSE(raw_std_value_is_int32(RAW_STD_BUF(kStdNull)));
}

void test_raw_std_value_as_int32(void) {
    // clang-format off
    alignas(16) uint8_t buffer[5] = {
        kStdInt32,
        0, 0, 0, 0
    };
    // clang-format on

    TEST_ASSERT_EQUAL_INT32(0, raw_std_value_as_int32(AS_RAW_STD_VALUE(buffer)));

    int value = -2003205;
    memcpy(buffer + 1, &value, sizeof(int));

    TEST_ASSERT_EQUAL_INT32(-2003205, raw_std_value_as_int32(AS_RAW_STD_VALUE(buffer)));
}

void test_raw_std_value_is_int64(void) {
    TEST_ASSERT_TRUE(raw_std_value_is_int64(RAW_STD_BUF(kStdInt64)));
    TEST_ASSERT_FALSE(raw_std_value_is_int64(RAW_STD_BUF(kStdNull)));
}

void test_raw_std_value_as_int64(void) {
    // clang-format off
    alignas(16) uint8_t buffer[9] = {
        kStdInt64,
        0, 0, 0, 0, 0, 0, 0, 0
    };
    // clang-format on

    TEST_ASSERT_EQUAL_INT64(0, raw_std_value_as_int64(AS_RAW_STD_VALUE(buffer)));

    int64_t value = -7998090352538419200;
    memcpy(buffer + 1, &value, sizeof(value));

    TEST_ASSERT_EQUAL_INT64(-7998090352538419200, raw_std_value_as_int64(AS_RAW_STD_VALUE(buffer)));
}

void test_raw_std_value_is_float64(void) {
    TEST_ASSERT_TRUE(raw_std_value_is_float64(RAW_STD_BUF(kStdFloat64)));
    TEST_ASSERT_FALSE(raw_std_value_is_float64(RAW_STD_BUF(kStdNull)));
}

void test_raw_std_value_as_float64(void) {
    // clang-format off
    alignas(16) uint8_t buffer[] = {
        kStdFloat64,
        0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0
    };
    // clang-format on

    double value = M_PI;
    memcpy(buffer + 8, &value, sizeof(value));

    TEST_ASSERT_EQUAL_DOUBLE(M_PI, raw_std_value_as_float64(AS_RAW_STD_VALUE(buffer)));

    value = DBL_INFINITY;
    memcpy(buffer + 8, &value, sizeof(value));

    TEST_ASSERT_EQUAL_DOUBLE(DBL_INFINITY, raw_std_value_as_float64(AS_RAW_STD_VALUE(buffer)));
}

void test_raw_std_value_is_string(void) {
    TEST_ASSERT_TRUE(raw_std_value_is_string(RAW_STD_BUF(kStdString)));
    TEST_ASSERT_FALSE(raw_std_value_is_string(RAW_STD_BUF(kStdNull)));
}

void test_raw_std_string_dup(void) {
    const char *str = "The quick brown fox jumps over the lazy dog.";

    // clang-format off
    alignas(16) uint8_t buffer[1 + 1 + 45] = {
        kStdString, 45, 0
    };
    // clang-format on

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

void test_raw_std_string_equals(void) {
    const char *str = "The quick brown fox jumps over the lazy dog.";

    alignas(16) uint8_t buffer[1 + 1 + strlen(str)];

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

void test_raw_std_value_is_uint8array(void) {
    TEST_ASSERT_TRUE(raw_std_value_is_uint8array(RAW_STD_BUF(kStdUInt8Array)));
    TEST_ASSERT_FALSE(raw_std_value_is_uint8array(RAW_STD_BUF(kStdNull)));
}

void test_raw_std_value_as_uint8array(void) {
    // clang-format off
    alignas(16) uint8_t buffer[] = {
        kStdUInt8Array,
        4,
        1, 2, 3, 4
    };
    // clang-format on

    // clang-format off
    alignas(16) uint8_t expected[] = {
        1, 2, 3, 4
    };
    // clang-format on

    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, raw_std_value_as_uint8array(AS_RAW_STD_VALUE(buffer)), 4);

    buffer[2] = 0;
    expected[0] = 0;

    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, raw_std_value_as_uint8array(AS_RAW_STD_VALUE(buffer)), 4);
}

void test_raw_std_value_is_int32array(void) {
    TEST_ASSERT_TRUE(raw_std_value_is_int32array(RAW_STD_BUF(kStdInt32Array)));
    TEST_ASSERT_FALSE(raw_std_value_is_int32array(RAW_STD_BUF(kStdNull)));
}

void test_raw_std_value_as_int32array(void) {
    // clang-format off
    alignas(16) uint8_t buffer[] = {
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
    // clang-format on

    // clang-format off
    int32_t expected[] = {
        INT_MIN,
        0x12345678,
    };
    // clang-format on

    memcpy(buffer + 4, expected, sizeof(expected));

    TEST_ASSERT_EQUAL_INT32_ARRAY(expected, raw_std_value_as_int32array(AS_RAW_STD_VALUE(buffer)), 2);

    expected[0] = 0;
    memcpy(buffer + 4, expected, sizeof(expected));

    TEST_ASSERT_EQUAL_INT32_ARRAY(expected, raw_std_value_as_int32array(AS_RAW_STD_VALUE(buffer)), 2);
}

void test_raw_std_value_is_int64array(void) {
    TEST_ASSERT_TRUE(raw_std_value_is_int64array(RAW_STD_BUF(kStdInt64Array)));
    TEST_ASSERT_FALSE(raw_std_value_is_int64array(RAW_STD_BUF(kStdNull)));
}

void test_raw_std_value_as_int64array(void) {
    // clang-format off
    alignas(16) uint8_t buffer[] = {
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
    // clang-format on

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

void test_raw_std_value_is_float64array(void) {
    TEST_ASSERT_TRUE(raw_std_value_is_float64array(RAW_STD_BUF(kStdFloat64Array)));
    TEST_ASSERT_FALSE(raw_std_value_is_float64array(RAW_STD_BUF(kStdNull)));
}

void test_raw_std_value_as_float64array(void) {
    // clang-format off
    alignas(16) uint8_t buffer[] = {
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
    // clang-format on

    // clang-format off
    double expected[] = {
        M_PI,
        DBL_INFINITY,
    };
    // clang-format on

    memcpy(buffer + 8, expected, sizeof(expected));

    TEST_ASSERT_EQUAL_DOUBLE_ARRAY(expected, raw_std_value_as_float64array(AS_RAW_STD_VALUE(buffer)), 2);

    expected[0] = 0.0;
    memcpy(buffer + 8, expected, sizeof(expected));

    TEST_ASSERT_EQUAL_DOUBLE_ARRAY(expected, raw_std_value_as_float64array(AS_RAW_STD_VALUE(buffer)), 2);
}

void test_raw_std_value_is_list(void) {
    TEST_ASSERT_TRUE(raw_std_value_is_list(RAW_STD_BUF(kStdList)));
    TEST_ASSERT_FALSE(raw_std_value_is_list(RAW_STD_BUF(kStdNull)));
}

void test_raw_std_list_get_size(void) {
    // clang-format off
    alignas(16) uint8_t buffer[] = {
        // type
        kStdList,
        // size
        2,
        // space for more size bytes
        0, 0, 0, 0,
    };
    // clang-format on

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

void test_raw_std_value_is_map(void) {
    TEST_ASSERT_TRUE(raw_std_value_is_map(RAW_STD_BUF(kStdMap)));
    TEST_ASSERT_FALSE(raw_std_value_is_map(RAW_STD_BUF(kStdNull)));
}

void test_raw_std_map_get_size(void) {
    // clang-format off
    alignas(16) uint8_t buffer[] = {
        // type
        kStdMap,
        // size
        2,
        // space for more size bytes
        0, 0, 0, 0,
    };
    // clang-format on

    TEST_ASSERT_EQUAL_size_t(2, raw_std_map_get_size(AS_RAW_STD_VALUE(buffer)));

    buffer[1] = 0;

    TEST_ASSERT_EQUAL_size_t(0, raw_std_map_get_size(AS_RAW_STD_VALUE(buffer)));

    uint32_t size = 0xDEAD;

    buffer[1] = 254;
    memcpy(buffer + 2, &size, 2);

    TEST_ASSERT_EQUAL_size_t(0xDEAD, raw_std_map_get_size(AS_RAW_STD_VALUE(buffer)));

    size = 0xDEADBEEF;
    buffer[1] = 255;
    memcpy(buffer + 2, &size, 4);

    TEST_ASSERT_EQUAL_size_t(0xDEADBEEF, raw_std_map_get_size(AS_RAW_STD_VALUE(buffer)));
}

void test_raw_std_value_is_float32array(void) {
    TEST_ASSERT_TRUE(raw_std_value_is_float32array(RAW_STD_BUF(kStdFloat32Array)));
    TEST_ASSERT_FALSE(raw_std_value_is_float32array(RAW_STD_BUF(kStdNull)));
}

void test_raw_std_value_as_float32array(void) {
    // clang-format off
    alignas(16) uint8_t buffer[] = {
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
    // clang-format on

    // clang-format off
    float expected[] = {
        M_PI,
        DBL_INFINITY,
    };
    // clang-format on

    memcpy(buffer + 4, expected, sizeof(expected));

    TEST_ASSERT_EQUAL_FLOAT_ARRAY(expected, raw_std_value_as_float32array(AS_RAW_STD_VALUE(buffer)), 2);

    expected[0] = 0.0;
    memcpy(buffer + 4, expected, sizeof(expected));

    TEST_ASSERT_EQUAL_FLOAT_ARRAY(expected, raw_std_value_as_float32array(AS_RAW_STD_VALUE(buffer)), 2);
}

void test_raw_std_value_equals(void) {
    TEST_ASSERT_TRUE(raw_std_value_equals(RAW_STD_BUF(kStdNull), RAW_STD_BUF(kStdNull)));
    TEST_ASSERT_FALSE(raw_std_value_equals(RAW_STD_BUF(kStdNull), RAW_STD_BUF(kStdTrue)));
    TEST_ASSERT_FALSE(raw_std_value_equals(RAW_STD_BUF(kStdTrue), RAW_STD_BUF(kStdFalse)));

    // int32
    {
        // clang-format off
        alignas(16) uint8_t lhs[] = {
            kStdInt32,
            1, 2, 3, 4,
        };
        // clang-format on

        // clang-format off
        alignas(16) uint8_t rhs[] = {
            kStdInt32,
            1, 2, 3, 4,
        };
        // clang-format on

        TEST_ASSERT_TRUE(raw_std_value_equals(AS_RAW_STD_VALUE(lhs), AS_RAW_STD_VALUE(rhs)));

        rhs[4] = 0;
        TEST_ASSERT_FALSE(raw_std_value_equals(AS_RAW_STD_VALUE(lhs), AS_RAW_STD_VALUE(rhs)));
    }

    // int64
    {
        // clang-format off
        alignas(16) uint8_t lhs[] = {
            kStdInt64,
            1, 2, 3, 4, 5, 6, 7, 8
        };
        // clang-format on

        // clang-format off
        alignas(16) uint8_t rhs[] = {
            kStdInt64,
            1, 2, 3, 4, 5, 6, 7, 8
        };
        // clang-format on

        TEST_ASSERT_TRUE(raw_std_value_equals(AS_RAW_STD_VALUE(lhs), AS_RAW_STD_VALUE(rhs)));

        rhs[8] = 0;
        TEST_ASSERT_FALSE(raw_std_value_equals(AS_RAW_STD_VALUE(lhs), AS_RAW_STD_VALUE(rhs)));
    }

    // float64
    {
        // clang-format off
        alignas(16) uint8_t lhs[] = {
            // type byte
            kStdFloat64,
            // 7 alignment bytes
            0, 0, 0, 0, 0, 0, 0,
            // bytes for 1 float64
            0, 0, 0, 0, 0, 0, 0, 0,
        };
        // clang-format on

        // clang-format off
        alignas(16) uint8_t rhs[] = {
            // type byte
            kStdFloat64,
            // 7 alignment bytes
            0, 0, 0, 0, 0, 0, 0,
            // bytes for 1 float64
            0, 0, 0, 0, 0, 0, 0, 0,
        };
        // clang-format on

        double f = M_PI;

        memcpy(lhs + 8, &f, sizeof(f));
        memcpy(rhs + 8, &f, sizeof(f));

        TEST_ASSERT_TRUE(raw_std_value_equals(AS_RAW_STD_VALUE(lhs), AS_RAW_STD_VALUE(rhs)));

        f = DBL_NAN;
        memcpy(rhs + 8, &f, sizeof(f));

        TEST_ASSERT_FALSE(raw_std_value_equals(AS_RAW_STD_VALUE(lhs), AS_RAW_STD_VALUE(rhs)));
    }

    // string
    {
        const char *str = "The quick brown fox jumps over the lazy dog.";

        alignas(16) uint8_t lhs[1 + 1 + strlen(str)];
        lhs[0] = kStdString;
        lhs[1] = strlen(str);

        alignas(16) uint8_t rhs[1 + 1 + strlen(str)];
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
        // clang-format off
        alignas(16) uint8_t lhs[] = {
            kStdUInt8Array,
            4,
            1, 2, 3, 4
        };
        // clang-format on

        // clang-format off
        alignas(16) uint8_t rhs[] = {
            kStdUInt8Array,
            4,
            1, 2, 3, 4
        };
        // clang-format on

        TEST_ASSERT_TRUE(raw_std_value_equals(AS_RAW_STD_VALUE(lhs), AS_RAW_STD_VALUE(rhs)));

        rhs[1] = 3;

        TEST_ASSERT_FALSE(raw_std_value_equals(AS_RAW_STD_VALUE(lhs), AS_RAW_STD_VALUE(rhs)));

        rhs[1] = 4;
        rhs[5] = 0;

        TEST_ASSERT_FALSE(raw_std_value_equals(AS_RAW_STD_VALUE(lhs), AS_RAW_STD_VALUE(rhs)));
    }

    // int32array
    {
        // clang-format off
        alignas(16) uint8_t lhs[] = {
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
        // clang-format on

        // clang-format off
        alignas(16) uint8_t rhs[] = {
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
        // clang-format on

        // clang-format off
        int32_t array[] = {
            INT_MIN,
            0x12345678,
        };
        // clang-format on

        memcpy(lhs + 4, array, sizeof(array));
        memcpy(rhs + 4, array, sizeof(array));

        TEST_ASSERT_TRUE(raw_std_value_equals(AS_RAW_STD_VALUE(lhs), AS_RAW_STD_VALUE(rhs)));

        rhs[1] = 0;

        TEST_ASSERT_FALSE(raw_std_value_equals(AS_RAW_STD_VALUE(lhs), AS_RAW_STD_VALUE(rhs)));

        rhs[1] = 2;
        // clang-format off
        int32_t array2[] = {
            INT_MAX,
            0x12345678,
        };
        // clang-format on
        memcpy(rhs + 4, array2, sizeof(array2));

        TEST_ASSERT_FALSE(raw_std_value_equals(AS_RAW_STD_VALUE(lhs), AS_RAW_STD_VALUE(rhs)));
    }

    // int64array
    {
        // clang-format off
        alignas(16) uint8_t lhs[] = {
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

        alignas(16) uint8_t rhs[] = {
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
        // clang-format on

        memcpy(lhs + 8, array, sizeof(array));
        memcpy(rhs + 8, array, sizeof(array));

        TEST_ASSERT_TRUE(raw_std_value_equals(AS_RAW_STD_VALUE(lhs), AS_RAW_STD_VALUE(rhs)));

        rhs[1] = 0;

        TEST_ASSERT_FALSE(raw_std_value_equals(AS_RAW_STD_VALUE(lhs), AS_RAW_STD_VALUE(rhs)));

        rhs[1] = 2;
        // clang-format off
        int64_t array2[] = {
            INT64_MAX,
            0x123456789ABCDEF,
        };
        // clang-format on
        memcpy(rhs + 8, array2, sizeof(array2));

        TEST_ASSERT_FALSE(raw_std_value_equals(AS_RAW_STD_VALUE(lhs), AS_RAW_STD_VALUE(rhs)));
    }

    // float64array
    {
        // clang-format off
        alignas(16) uint8_t lhs[] = {
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

        alignas(16) uint8_t rhs[] = {
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
            DBL_INFINITY,
        };
        // clang-format on

        memcpy(lhs + 8, array, sizeof(array));
        memcpy(rhs + 8, array, sizeof(array));

        TEST_ASSERT_TRUE(raw_std_value_equals(AS_RAW_STD_VALUE(lhs), AS_RAW_STD_VALUE(rhs)));

        rhs[1] = 0;

        TEST_ASSERT_FALSE(raw_std_value_equals(AS_RAW_STD_VALUE(lhs), AS_RAW_STD_VALUE(rhs)));

        rhs[1] = 2;
        double array2[] = {
            0.0,
            DBL_INFINITY,
        };
        memcpy(rhs + 8, array2, sizeof(array2));

        TEST_ASSERT_FALSE(raw_std_value_equals(AS_RAW_STD_VALUE(lhs), AS_RAW_STD_VALUE(rhs)));
    }

    // list
    {
        const char *str = "The quick brown fox jumps over the lazy dog.";

        alignas(16) uint8_t lhs[1 + 1 + 1 + 1 + strlen(str) + 1];
        lhs[0] = kStdList;
        lhs[1] = 2;
        lhs[2] = kStdString;
        lhs[3] = strlen(str);
        lhs[4 + strlen(str)] = kStdTrue;

        alignas(16) uint8_t rhs[1 + 1 + 1 + 1 + strlen(str) + 1];
        rhs[0] = kStdList;
        rhs[1] = 2;
        rhs[2] = kStdString;
        rhs[3] = strlen(str);
        rhs[4 + strlen(str)] = kStdTrue;

        // only string lengths less or equal 253 are actually encoded as one byte in
        // the standard message codec encoding.
        TEST_ASSERT_LESS_OR_EQUAL_size_t(253, strlen(str));

        memcpy(lhs + 4, str, strlen(str));
        memcpy(rhs + 4, str, strlen(str));

        TEST_ASSERT_TRUE(raw_std_value_equals(AS_RAW_STD_VALUE(lhs), AS_RAW_STD_VALUE(rhs)));

        rhs[1] = 0;
        TEST_ASSERT_FALSE(raw_std_value_equals(AS_RAW_STD_VALUE(lhs), AS_RAW_STD_VALUE(rhs)));

        rhs[1] = 2;
        rhs[3] = strlen(str) - 1;

        TEST_ASSERT_FALSE(raw_std_value_equals(AS_RAW_STD_VALUE(lhs), AS_RAW_STD_VALUE(rhs)));

        rhs[3] = strlen(str);
        rhs[3 + strlen(str)] = kStdFalse;

        TEST_ASSERT_FALSE(raw_std_value_equals(AS_RAW_STD_VALUE(lhs), AS_RAW_STD_VALUE(rhs)));
    }

    // map
    {
        // clang-format off
        alignas(16) uint8_t lhs[] = {
            [0] = kStdMap,
            [1] = 2,
            [2] = kStdNull,
            [3] = kStdInt64,
            [4] = 0, 0, 0, 0, 0, 0, 0, 0,
            [12] = kStdFloat32Array,
            [13] = 2,
            [16] = 0, 0, 0, 0, 0, 0, 0, 0,
            [24] = kStdTrue,
        };

        alignas(16) uint8_t rhs[] = {
            [0] = kStdMap,
            [1] = 2,
            [2] = kStdFloat32Array,
            [3] = 2,
            [4] = 0, 0, 0, 0, 0, 0, 0, 0,
            [12] = kStdTrue,
            [13] = kStdNull,
            [14] = kStdInt64,
            [15] = 0, 0, 0, 0, 0, 0, 0, 0,
        };

        int64_t int64 = (int64_t) INT64_MIN;
        float floats[] = {
            M_PI,
            DBL_INFINITY,
        };
        // clang-format on

        memcpy(lhs + 4, &int64, sizeof(int64));
        memcpy(rhs + 15, &int64, sizeof(int64));
        memcpy(lhs + 16, floats, sizeof(floats));
        memcpy(rhs + 4, floats, sizeof(floats));

        TEST_ASSERT_TRUE(raw_std_value_equals(AS_RAW_STD_VALUE(lhs), AS_RAW_STD_VALUE(rhs)));

        rhs[1] = 0;
        TEST_ASSERT_FALSE(raw_std_value_equals(AS_RAW_STD_VALUE(lhs), AS_RAW_STD_VALUE(rhs)));

        rhs[1] = 2;
        rhs[13] = kStdTrue;
        TEST_ASSERT_FALSE(raw_std_value_equals(AS_RAW_STD_VALUE(lhs), AS_RAW_STD_VALUE(rhs)));

        rhs[13] = kStdNull;
        rhs[3] = 1;
        TEST_ASSERT_FALSE(raw_std_value_equals(AS_RAW_STD_VALUE(lhs), AS_RAW_STD_VALUE(rhs)));
    }

    // float32array
    {
        // clang-format off
        alignas(16) uint8_t lhs[] = {
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

        alignas(16) uint8_t rhs[] = {
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

        float array[] = {
            M_PI,
            DBL_INFINITY,
        };
        // clang-format on

        memcpy(lhs + 4, array, sizeof(array));
        memcpy(rhs + 4, array, sizeof(array));

        TEST_ASSERT_TRUE(raw_std_value_equals(AS_RAW_STD_VALUE(lhs), AS_RAW_STD_VALUE(rhs)));

        rhs[1] = 0;

        TEST_ASSERT_FALSE(raw_std_value_equals(AS_RAW_STD_VALUE(lhs), AS_RAW_STD_VALUE(rhs)));

        rhs[1] = 2;
        // clang-format off
        float array2[] = {
            0.0,
            DBL_INFINITY,
        };
        // clang-format on
        memcpy(rhs + 4, array2, sizeof(array2));

        TEST_ASSERT_FALSE(raw_std_value_equals(AS_RAW_STD_VALUE(lhs), AS_RAW_STD_VALUE(rhs)));
    }
}

void test_raw_std_value_is_bool(void) {
    TEST_ASSERT_FALSE(raw_std_value_is_bool(RAW_STD_BUF(kStdNull)));
    TEST_ASSERT_TRUE(raw_std_value_is_bool(RAW_STD_BUF(kStdTrue)));
    TEST_ASSERT_TRUE(raw_std_value_is_bool(RAW_STD_BUF(kStdFalse)));
}

void test_raw_std_value_as_bool(void) {
    TEST_ASSERT_TRUE(raw_std_value_as_bool(RAW_STD_BUF(kStdTrue)));
    TEST_ASSERT_FALSE(raw_std_value_as_bool(RAW_STD_BUF(kStdFalse)));
}

void test_raw_std_value_is_int(void) {
    TEST_ASSERT_FALSE(raw_std_value_is_int(RAW_STD_BUF(kStdNull)));
    TEST_ASSERT_FALSE(raw_std_value_is_int(RAW_STD_BUF(kStdTrue)));
    TEST_ASSERT_FALSE(raw_std_value_is_int(RAW_STD_BUF(kStdFalse)));
    TEST_ASSERT_TRUE(raw_std_value_is_int(RAW_STD_BUF(kStdInt32)));
    TEST_ASSERT_TRUE(raw_std_value_is_int(RAW_STD_BUF(kStdInt64)));
    TEST_ASSERT_FALSE(raw_std_value_is_int(RAW_STD_BUF(kStdFloat64)));
}

void test_raw_std_value_as_int(void) {
    // clang-format off
    alignas(16) uint8_t buffer[9] = {
        kStdInt32,
        0, 0, 0, 0, 0, 0, 0, 0
    };
    // clang-format on

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

void test_raw_std_value_get_size(void) {
    // clang-format off
    alignas(16) uint8_t buffer[] = {
        // type
        kStdList,
        // size
        2,
        // space for more size bytes
        0, 0, 0, 0,
    };
    // clang-format on

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

void test_raw_std_value_after(void) {
    // null
    {
        // clang-format off
        alignas(16) uint8_t buffer[] = {
            kStdNull,
            0,
        };
        // clang-format on

        TEST_ASSERT_EQUAL_PTR(buffer + 1, raw_std_value_after(AS_RAW_STD_VALUE(buffer)));
    }

    // true
    {
        // clang-format off
        alignas(16) uint8_t buffer[] = {
            kStdTrue,
            0,
        };
        // clang-format on

        TEST_ASSERT_EQUAL_PTR(buffer + 1, raw_std_value_after(AS_RAW_STD_VALUE(buffer)));
    }

    // true
    {
        // clang-format off
        alignas(16) uint8_t buffer[] = {
            kStdFalse,
            0,
        };
        // clang-format on

        TEST_ASSERT_EQUAL_PTR(buffer + 1, raw_std_value_after(AS_RAW_STD_VALUE(buffer)));
    }

    // int32
    {
        // clang-format off
        alignas(16) uint8_t buffer[] = {
            kStdInt32,
            1, 2, 3, 4,
        };
        // clang-format on

        TEST_ASSERT_EQUAL_PTR(buffer + 5, raw_std_value_after(AS_RAW_STD_VALUE(buffer)));
    }

    // int64
    {
        // clang-format off
        alignas(16) uint8_t buffer[] = {
            kStdInt64,
            1, 2, 3, 4, 5, 6, 7, 8
        };
        // clang-format on

        TEST_ASSERT_EQUAL_PTR(buffer + 9, raw_std_value_after(AS_RAW_STD_VALUE(buffer)));
    }

    // float64
    {
        // clang-format off
        alignas(16) uint8_t buffer[] = {
            // type byte
            kStdFloat64,
            // 7 alignment bytes
            0, 0, 0, 0, 0, 0, 0,
            // bytes for 1 float64
            0, 0, 0, 0, 0, 0, 0, 0,
        };
        // clang-format on

        TEST_ASSERT_EQUAL_PTR(buffer + 1 + 7 + 8, raw_std_value_after(AS_RAW_STD_VALUE(buffer)));
    }

    // string
    {
        const char *str = "The quick brown fox jumps over the lazy dog.";

        // clang-format off
        alignas(16) uint8_t buffer[1 + 1 + 4] = {
            kStdString,
            strlen(str),
            0
        };
        // clang-format on

        // only string lengths less or equal 253 are actually encoded as one byte in
        // the standard message codec encoding.
        TEST_ASSERT_LESS_OR_EQUAL_size_t(253, strlen(str));

        TEST_ASSERT_EQUAL_PTR(buffer + 1 + 1 + strlen(str), raw_std_value_after(AS_RAW_STD_VALUE(buffer)));

        buffer[1] = 0;
        TEST_ASSERT_EQUAL_PTR(buffer + 1 + 1, raw_std_value_after(AS_RAW_STD_VALUE(buffer)));

        buffer[1] = 254;
        buffer[2] = 254;
        buffer[3] = 0;
        TEST_ASSERT_EQUAL_PTR(buffer + 1 + 1 + 2 + 254, raw_std_value_after(AS_RAW_STD_VALUE(buffer)));

        buffer[1] = 255;
        buffer[2] = 0;
        buffer[3] = 0;
        buffer[4] = 1;
        buffer[5] = 0;
        TEST_ASSERT_EQUAL_PTR(buffer + 1 + 1 + 4 + 0x00010000, raw_std_value_after(AS_RAW_STD_VALUE(buffer)));
    }

    // uint8array
    {
        // clang-format off
        alignas(16) uint8_t buffer[1 + 1 + 4 + 0x00010000] = {
            kStdUInt8Array,
            4,
            1, 2, 3, 4
        };
        // clang-format on

        TEST_ASSERT_EQUAL_PTR(buffer + 1 + 1 + 4, raw_std_value_after(AS_RAW_STD_VALUE(buffer)));

        buffer[1] = 0;
        TEST_ASSERT_EQUAL_PTR(buffer + 1 + 1, raw_std_value_after(AS_RAW_STD_VALUE(buffer)));

        buffer[1] = 254;
        buffer[2] = 254;
        buffer[3] = 0;
        TEST_ASSERT_EQUAL_PTR(buffer + 1 + 1 + 2 + 254, raw_std_value_after(AS_RAW_STD_VALUE(buffer)));

        buffer[1] = 255;
        buffer[2] = 0;
        buffer[3] = 0;
        buffer[4] = 1;
        buffer[5] = 0;
        TEST_ASSERT_EQUAL_PTR(buffer + 1 + 1 + 4 + 0x00010000, raw_std_value_after(AS_RAW_STD_VALUE(buffer)));
    }

    // int32array
    {
        // clang-format off
        alignas(16) uint8_t buffer[1 + 1 + 4 + 2 + 0x010000*4] = {
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
        // clang-format on

        TEST_ASSERT_EQUAL_PTR(buffer + 1 + 1 + 2 + 8, raw_std_value_after(AS_RAW_STD_VALUE(buffer)));

        buffer[1] = 0;
        TEST_ASSERT_EQUAL_PTR(buffer + 1 + 1 + 2, raw_std_value_after(AS_RAW_STD_VALUE(buffer)));

        buffer[1] = 254;
        buffer[2] = 254;
        buffer[3] = 0;
        TEST_ASSERT_EQUAL_PTR(buffer + 1 + 1 + 2 + 254 * 4, raw_std_value_after(AS_RAW_STD_VALUE(buffer)));

        buffer[1] = 255;
        buffer[2] = 0;
        buffer[3] = 0;
        buffer[4] = 1;
        buffer[5] = 0;
        TEST_ASSERT_EQUAL_PTR(buffer + 1 + 1 + 4 + 2 + 0x010000 * 4, raw_std_value_after(AS_RAW_STD_VALUE(buffer)));
    }

    // int64array
    {
        // clang-format off
        alignas(16) uint8_t buffer[1 + 1 + 4 + 2 + 0x010000*8] = {
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
        // clang-format on

        TEST_ASSERT_EQUAL_PTR(buffer + 1 + 1 + 6 + 2 * 8, raw_std_value_after(AS_RAW_STD_VALUE(buffer)));

        buffer[1] = 0;
        TEST_ASSERT_EQUAL_PTR(buffer + 1 + 1 + 6, raw_std_value_after(AS_RAW_STD_VALUE(buffer)));

        buffer[1] = 254;
        buffer[2] = 254;
        buffer[3] = 0;
        TEST_ASSERT_EQUAL_PTR(buffer + 1 + 1 + 4 + 2 + 254 * 8, raw_std_value_after(AS_RAW_STD_VALUE(buffer)));

        buffer[1] = 255;
        buffer[2] = 0;
        buffer[3] = 0;
        buffer[4] = 1;
        buffer[5] = 0;
        TEST_ASSERT_EQUAL_PTR(buffer + 1 + 1 + 4 + 2 + 0x010000 * 8, raw_std_value_after(AS_RAW_STD_VALUE(buffer)));
    }

    // float64array
    {
        // clang-format off
        alignas(16) uint8_t buffer[1 + 1 + 4 + 2 + 0x010000*8] = {
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
        // clang-format on

        TEST_ASSERT_EQUAL_PTR(buffer + 1 + 1 + 6 + 2 * 8, raw_std_value_after(AS_RAW_STD_VALUE(buffer)));

        buffer[1] = 0;
        TEST_ASSERT_EQUAL_PTR(buffer + 1 + 1 + 6, raw_std_value_after(AS_RAW_STD_VALUE(buffer)));

        buffer[1] = 254;
        buffer[2] = 254;
        buffer[3] = 0;
        TEST_ASSERT_EQUAL_PTR(buffer + 1 + 1 + 4 + 2 + 254 * 8, raw_std_value_after(AS_RAW_STD_VALUE(buffer)));

        buffer[1] = 255;
        buffer[2] = 0;
        buffer[3] = 0;
        buffer[4] = 1;
        buffer[5] = 0;
        TEST_ASSERT_EQUAL_PTR(buffer + 1 + 1 + 4 + 2 + 0x010000 * 8, raw_std_value_after(AS_RAW_STD_VALUE(buffer)));
    }

    // list
    {
        const char *str = "The quick brown fox jumps over the lazy dog.";

        alignas(16) uint8_t buffer[1 + 1 + 4 + 1 + 1 + 4 + strlen(str) + 1];
        buffer[0] = kStdList;
        buffer[1] = 2;
        buffer[2] = kStdString;
        buffer[3] = strlen(str);
        buffer[4 + strlen(str)] = kStdTrue;

        // only string lengths less or equal 253 are actually encoded as one byte in
        // the standard message codec encoding.
        TEST_ASSERT_LESS_OR_EQUAL_size_t(253, strlen(str));

        TEST_ASSERT_EQUAL_PTR(buffer + 1 + 1 + 1 + 1 + strlen(str) + 1, raw_std_value_after(AS_RAW_STD_VALUE(buffer)));

        buffer[1] = 0;
        TEST_ASSERT_EQUAL_PTR(buffer + 1 + 1, raw_std_value_after(AS_RAW_STD_VALUE(buffer)));

        buffer[1] = 1;
        TEST_ASSERT_EQUAL_PTR(buffer + 1 + 1 + 1 + 1 + strlen(str), raw_std_value_after(AS_RAW_STD_VALUE(buffer)));
    }

    // map
    {
        // clang-format off
        alignas(16) uint8_t buffer[] = {
            [0] = kStdMap,
            [1] = 2,
            [2] = kStdNull,
            [3] = kStdInt64,
            [4] = 0, 0, 0, 0, 0, 0, 0, 0,
            [12] = kStdFloat32Array,
            [13] = 2,
            [16] = 0, 0, 0, 0, 0, 0, 0, 0,
            [24] = kStdTrue,
        };
        // clang-format on

        TEST_ASSERT_EQUAL_PTR(buffer + 25, raw_std_value_after(AS_RAW_STD_VALUE(buffer)));

        buffer[1] = 0;
        TEST_ASSERT_EQUAL_PTR(buffer + 2, raw_std_value_after(AS_RAW_STD_VALUE(buffer)));

        buffer[1] = 1;
        TEST_ASSERT_EQUAL_PTR(buffer + 12, raw_std_value_after(AS_RAW_STD_VALUE(buffer)));
    }

    // float32array
    {
        // clang-format off
        alignas(16) uint8_t buffer[1 + 1 + 4 + 2 + 0x040000] = {
            // type
            kStdFloat32Array,
            // size
            2,
            // 2 alignment bytes
            0, 0,
            // space for 2 int32_t's
            0, 0, 0, 0,
            0, 0, 0, 0
        };
        // clang-format on

        TEST_ASSERT_EQUAL_PTR(buffer + 1 + 1 + 2 + 8, raw_std_value_after(AS_RAW_STD_VALUE(buffer)));

        buffer[1] = 0;
        TEST_ASSERT_EQUAL_PTR(buffer + 1 + 1 + 2, raw_std_value_after(AS_RAW_STD_VALUE(buffer)));

        buffer[1] = 254;
        buffer[2] = 254;
        buffer[3] = 0;
        TEST_ASSERT_EQUAL_PTR(buffer + 1 + 1 + 2 + 254 * 4, raw_std_value_after(AS_RAW_STD_VALUE(buffer)));

        buffer[1] = 255;
        buffer[2] = 0;
        buffer[3] = 0;
        buffer[4] = 1;
        buffer[5] = 0;
        TEST_ASSERT_EQUAL_PTR(buffer + 1 + 1 + 4 + 2 + 0x010000 * 4, raw_std_value_after(AS_RAW_STD_VALUE(buffer)));
    }
}

void test_raw_std_list_get_first_element(void) {
    // list
    const char *str = "The quick brown fox jumps over the lazy dog.";

    alignas(16) uint8_t buffer[1 + 1 + 4 + 1 + 1 + 4 + strlen(str) + 1];
    buffer[0] = kStdList;
    buffer[1] = 2;
    buffer[2] = kStdString;
    buffer[3] = strlen(str);
    buffer[4 + strlen(str)] = kStdTrue;

    // only string lengths less or equal 253 are actually encoded as one byte in
    // the standard message codec encoding.
    TEST_ASSERT_LESS_OR_EQUAL_size_t(253, strlen(str));

    TEST_ASSERT_EQUAL_PTR(buffer + 1 + 1, raw_std_list_get_first_element(AS_RAW_STD_VALUE(buffer)));

    TEST_ASSERT_EQUAL_PTR(
        buffer + 1 + 1 + 1 + 1 + strlen(str),
        raw_std_value_after(raw_std_list_get_first_element(AS_RAW_STD_VALUE(buffer)))
    );
}

void test_raw_std_list_get_nth_element(void) {
    // list
    const char *str = "The quick brown fox jumps over the lazy dog.";

    alignas(16) uint8_t buffer[1 + 1 + 4 + 1 + 1 + 4 + strlen(str) + 1];
    buffer[0] = kStdList;
    buffer[1] = 2;
    buffer[2] = kStdString;
    buffer[3] = strlen(str);
    buffer[4 + strlen(str)] = kStdTrue;

    // only string lengths less or equal 253 are actually encoded as one byte in
    // the standard message codec encoding.
    TEST_ASSERT_LESS_OR_EQUAL_size_t(253, strlen(str));

    TEST_ASSERT_EQUAL_PTR(buffer + 1 + 1, raw_std_list_get_nth_element(AS_RAW_STD_VALUE(buffer), 0));

    TEST_ASSERT_EQUAL_PTR(
        buffer + 1 + 1 + 1 + 1 + strlen(str),
        raw_std_value_after(raw_std_list_get_first_element(AS_RAW_STD_VALUE(buffer)))
    );
}

void test_raw_std_map_get_first_key(void) {
    // map
    // clang-format off
    alignas(16) uint8_t buffer[] = {
        [0] = kStdMap,
        [1] = 2,
        [2] = kStdNull,
        [3] = kStdInt64,
        [4] = 0, 0, 0, 0, 0, 0, 0, 0,
        [12] = kStdFloat32Array,
        [13] = 2,
        [16] = 0, 0, 0, 0, 0, 0, 0, 0,
        [24] = kStdTrue,
    };
    // clang-format on

    TEST_ASSERT_EQUAL_PTR(buffer + 1 + 1, raw_std_map_get_first_key(AS_RAW_STD_VALUE(buffer)));

    buffer[1] = 254;
    buffer[2] = 254;
    buffer[3] = 0;
    TEST_ASSERT_EQUAL_PTR(buffer + 1 + 1 + 2, raw_std_map_get_first_key(AS_RAW_STD_VALUE(buffer)));

    buffer[1] = 255;
    buffer[2] = 0x00;
    buffer[3] = 0x00;
    buffer[4] = 0x01;
    buffer[5] = 0x00;
    TEST_ASSERT_EQUAL_PTR(buffer + 1 + 1 + 4, raw_std_map_get_first_key(AS_RAW_STD_VALUE(buffer)));
}

void test_raw_std_map_find(void) {
}

void test_raw_std_map_find_str(void) {
}

void test_raw_std_value_check(void) {
}

void test_raw_std_method_call_check(void) {
}

void test_raw_std_method_call_response_check(void) {
}

void test_raw_std_event_check(void) {
}

void test_raw_std_method_call_get_method(void) {
}

void test_raw_std_method_call_get_method_dup(void) {
}

void test_raw_std_method_call_get_arg(void) {
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
