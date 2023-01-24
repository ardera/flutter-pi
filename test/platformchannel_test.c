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

    uint8_t buffer[1 + 1 + 45] = {
        kStdString, 45, 0
    };

    memcpy(buffer + 2, str, strlen(str));

    TEST_ASSERT_TRUE(raw_std_string_equals(AS_RAW_STD_VALUE(buffer), "The quick brown fox jumps over the lazy dog."));
    TEST_ASSERT_FALSE(raw_std_string_equals(AS_RAW_STD_VALUE(buffer), "The quick brown fox jumps over the lazy dog"));
    
    buffer[1] = 0;
    TEST_ASSERT_TRUE(raw_std_string_equals(AS_RAW_STD_VALUE(buffer), ""));
    TEST_ASSERT_FALSE(raw_std_string_equals(AS_RAW_STD_VALUE(buffer), "anything"));
}

void test_raw_std_value_is_uint8array() {

}

void test_raw_std_value_as_uint8array() {

}

void test_raw_std_value_is_int32array() {

}

void test_raw_std_value_as_int32array() {

}

void test_raw_std_value_is_int64array() {

}

void test_raw_std_value_as_int64array() {

}

void test_raw_std_value_is_float64array() {

}

void test_raw_std_value_as_float64array() {

}

void test_raw_std_value_is_list() {

}

void test_raw_std_list_get_size() {

}

void test_raw_std_value_is_map() {

}

void test_raw_std_map_get_size() {

}

void test_raw_std_value_is_float32array() {

}

void test_raw_std_value_as_float32array() {

}

void test_raw_std_value_equals() {

}

void test_raw_std_value_is_bool() {

}

void test_raw_std_value_as_bool() {

}

void test_raw_std_value_is_int() {

}

void test_raw_std_value_as_int() {

}

void test_raw_std_value_get_size() {

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