#include "pluginregistry.h"
#include <unity.h>

#undef FLUTTERPI_PLUGIN
#define FLUTTERPI_PLUGIN(...)
#define charset_converter_init test_charset_converter_init
#define charset_converter_deinit test_charset_converter_deinit
#define platch_respond capture_response
#define platch_respond_success_std capture_success

static uint8_t *response_data;
static size_t response_size;
static int capture_response(const FlutterPlatformMessageResponseHandle *handle, struct platch_obj *response);
static int capture_success(FlutterPlatformMessageResponseHandle *handle, struct std_value *value);

#include "plugins/charset_converter.c"

static int capture_response(const FlutterPlatformMessageResponseHandle *handle, struct platch_obj *response) {
    (void) handle;
    response_size = response->binarydata_size;
    response_data = malloc(response_size);
    TEST_ASSERT_NOT_NULL(response_data);
    memcpy(response_data, response->binarydata, response_size);
    return 0;
}

static int capture_success(FlutterPlatformMessageResponseHandle *handle, struct std_value *value) {
    (void) handle;
    if (value->type == kStdList) {
        response_size = value->size;
        TEST_ASSERT_GREATER_THAN(0, response_size);
        bool found_utf8 = false;
        for (size_t i = 0; i < value->size; i++) {
            TEST_ASSERT_EQUAL(kStdString, value->list[i].type);
            const char *name = value->list[i].string_value;
            if (name == NULL) {
                TEST_FAIL_MESSAGE("Charset name is NULL");
                return EINVAL;
            }
            TEST_ASSERT_GREATER_THAN(0, strlen(name));
            TEST_ASSERT_NULL(strpbrk(name, " /\t\r\n"));
            found_utf8 |= strcmp(name, "UTF-8") == 0;
        }
        TEST_ASSERT_TRUE(found_utf8);
        return 0;
    }
    TEST_ASSERT_EQUAL(kStdUInt8Array, value->type);
    response_size = value->size;
    response_data = malloc(response_size + 1);
    TEST_ASSERT_NOT_NULL(response_data);
    memcpy(response_data, value->uint8array, response_size);
    return 0;
}

void setUp(void) {
    response_data = NULL;
    response_size = 0;
}

void tearDown(void) {
    free(response_data);
}

static void check_conversion(const char *from, const char *to, const char *input, size_t input_size, const char *expected, size_t expected_size) {
    char *output = NULL;
    size_t size = 0;
    TEST_ASSERT_EQUAL_INT(0, convert(from, to, input, input_size, &output, &size));
    TEST_ASSERT_EQUAL_size_t(expected_size, size);
    if (size > 0) {
        TEST_ASSERT_EQUAL_MEMORY(expected, output, size);
    }
    TEST_ASSERT_EQUAL_CHAR(0, output[size]);
    free(output);
}

static void test_conversion(void) {
    check_conversion("UTF-8", "ISO-8859-1", "hello", 5, "hello", 5);
    check_conversion("ISO-8859-1", "UTF-8", "\xe4", 1, "\xc3\xa4", 2);
    check_conversion("UTF-8", "UTF-16LE", "ab", 2, "a\0b\0", 4);
    check_conversion("UTF-8", "UTF-8", "a\0b", 3, "a\0b", 3);
    check_conversion("UTF-8", "UTF-8", NULL, 0, "", 0);
    check_conversion("UTF-8", "ISO-2022-JP", "\xe3\x81\x82", 3, "\x1b$B$\"\x1b(B", 8);
}

static void test_growth(void) {
    char input[4096], expected[8192];
    memset(input, 'a', sizeof(input));
    for (size_t i = 0; i < sizeof(input); i++) {
        expected[2 * i] = 'a';
        expected[2 * i + 1] = 0;
    }
    check_conversion("UTF-8", "UTF-16LE", input, sizeof(input), expected, sizeof(expected));
}

static void test_conversion_errors(void) {
    char *output = NULL;
    size_t size = 0;
    TEST_ASSERT_EQUAL_INT(ENOENT, convert("UTF-8", "NOT-A-CHARSET", "", 0, &output, &size));
    TEST_ASSERT_EQUAL_INT(EILSEQ, convert("UTF-8", "ASCII", "\xff", 1, &output, &size));
    TEST_ASSERT_EQUAL_INT(EINVAL, convert("UTF-8", "ASCII", "\xc3", 1, &output, &size));
    TEST_ASSERT_EQUAL_INT(EILSEQ, convert("UTF-8", "ASCII", "\xc3\xa4", 2, &output, &size));
    TEST_ASSERT_EQUAL_INT(EOVERFLOW, convert("UTF-8", "ASCII", "", SIZE_MAX, &output, &size));
    TEST_ASSERT_NULL(output);
}

static struct platch_obj call_args(struct std_value *keys, struct std_value *values) {
    keys[0] = STDSTRING("charset");
    keys[1] = STDSTRING("data");
    return (struct platch_obj) {
        .std_arg = { .type = kStdMap, .size = 2, .keys = keys, .values = values }
    };
}

static void test_linux_encode(void) {
    struct std_value keys[2], values[2] = {
        STDSTRING("UTF-16LE"),
        { .type = kStdUInt8Array, .size = 4, .uint8array = (uint8_t *) "a\0b" }
    };
    struct platch_obj call = call_args(keys, values);
    TEST_ASSERT_EQUAL_INT(0, on_encode(&call, NULL));
    TEST_ASSERT_EQUAL_size_t(6, response_size);
    TEST_ASSERT_EQUAL_MEMORY("a\0\0\0b\0", response_data, response_size);
}

static void test_decode_response(void) {
    struct std_value keys[2], values[2] = {
        STDSTRING("UTF-16LE"),
        { .type = kStdUInt8Array, .size = 6, .uint8array = (uint8_t *) "a\0\0\0b\0" }
    };
    struct platch_obj call = call_args(keys, values);
    TEST_ASSERT_EQUAL_INT(0, on_decode(&call, NULL));
    TEST_ASSERT_EQUAL_size_t(6, response_size);
    TEST_ASSERT_EQUAL_MEMORY("\0\7\3a\0b", response_data, response_size);
}

static void test_decode_size_prefixes(void) {
    const size_t sizes[] = { 0, 253, 254, 65535, 65536 };
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        size_t size = sizes[i];
        uint8_t *input = calloc(size + 1, 1);
        TEST_ASSERT_NOT_NULL(input);
        struct std_value keys[2], values[2] = {
            STDSTRING("UTF-8"),
            { .type = kStdUInt8Array, .size = size, .uint8array = input }
        };
        struct platch_obj call = call_args(keys, values);
        TEST_ASSERT_EQUAL_INT(0, on_decode(&call, NULL));
        size_t prefix = size < 254 ? 3 : size <= UINT16_MAX ? 5 : 7;
        TEST_ASSERT_EQUAL_size_t(size + prefix, response_size);
        TEST_ASSERT_EQUAL_UINT8(0, response_data[0]);
        TEST_ASSERT_EQUAL_UINT8(kStdString, response_data[1]);
        if (size < 254) {
            TEST_ASSERT_EQUAL_UINT8(size, response_data[2]);
        } else if (size <= UINT16_MAX) {
            uint16_t encoded;
            memcpy(&encoded, response_data + 3, sizeof(encoded));
            TEST_ASSERT_EQUAL_UINT8(254, response_data[2]);
            TEST_ASSERT_EQUAL_UINT16(size, encoded);
        } else {
            uint32_t encoded;
            memcpy(&encoded, response_data + 3, sizeof(encoded));
            TEST_ASSERT_EQUAL_UINT8(255, response_data[2]);
            TEST_ASSERT_EQUAL_UINT32(size, encoded);
        }
        free(input);
        free(response_data);
        response_data = NULL;
    }
}

static void test_available_charsets(void) {
    TEST_ASSERT_EQUAL_INT(0, on_available_charsets(NULL, NULL));
    TEST_ASSERT_GREATER_THAN(0, response_size);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_conversion);
    RUN_TEST(test_growth);
    RUN_TEST(test_conversion_errors);
    RUN_TEST(test_linux_encode);
    RUN_TEST(test_decode_response);
    RUN_TEST(test_decode_size_prefixes);
    RUN_TEST(test_available_charsets);
    return UNITY_END();
}
