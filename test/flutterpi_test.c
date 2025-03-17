#include <flutter-pi.h>
#include <unity.h>

void setUp() {
}

void tearDown() {
}

#define TEST_ASSERT_EQUAL_BOOL(expected, actual)                                \
    do {                                                                        \
        if (expected) {                                                         \
            UNITY_TEST_ASSERT((actual), __LINE__, " Expected TRUE Was FALSE");  \
        } else {                                                                \
            UNITY_TEST_ASSERT(!(actual), __LINE__, " Expected FALSE Was TRUE"); \
        }                                                                       \
    } while (0)

#define BUNDLE_PATH "/path/to/bundle"

void expect_parsed_cmdline_args_matches(int argc, char **argv, bool expected_result, const struct flutterpi_cmdline_args expected) {
    struct flutterpi_cmdline_args actual;
    bool result;

    result = flutterpi_parse_cmdline_args(argc, argv, &actual);
    TEST_ASSERT_EQUAL_BOOL(expected_result, result);
    TEST_ASSERT_EQUAL_BOOL(expected.has_orientation, actual.has_orientation);
    TEST_ASSERT_EQUAL(expected.orientation, actual.orientation);
    TEST_ASSERT_EQUAL_BOOL(expected.has_rotation, actual.has_rotation);
    TEST_ASSERT_EQUAL_INT(expected.rotation, actual.rotation);
    TEST_ASSERT_EQUAL_BOOL(expected.has_physical_dimensions, actual.has_physical_dimensions);
    TEST_ASSERT_EQUAL_INT(expected.physical_dimensions.x, actual.physical_dimensions.x);
    TEST_ASSERT_EQUAL_INT(expected.physical_dimensions.y, actual.physical_dimensions.y);
    TEST_ASSERT_EQUAL_BOOL(expected.has_pixel_format, actual.has_pixel_format);
    TEST_ASSERT_EQUAL(expected.pixel_format, actual.pixel_format);
    TEST_ASSERT_EQUAL_BOOL(expected.has_runtime_mode, actual.has_runtime_mode);
    TEST_ASSERT_EQUAL(expected.runtime_mode, actual.runtime_mode);
    TEST_ASSERT_EQUAL_STRING(expected.bundle_path, actual.bundle_path);
    TEST_ASSERT_EQUAL_INT(expected.engine_argc, actual.engine_argc);
    if (expected.engine_argc != 0) {
        TEST_ASSERT_NOT_NULL(actual.engine_argv);
        TEST_ASSERT_EQUAL_STRING_ARRAY(expected.engine_argv, actual.engine_argv, expected.engine_argc);
    } else {
        TEST_ASSERT_NULL(actual.engine_argv);
    }
    TEST_ASSERT_EQUAL_BOOL(expected.use_vulkan, actual.use_vulkan);
    TEST_ASSERT_EQUAL_STRING(expected.desired_videomode, actual.desired_videomode);
    TEST_ASSERT_EQUAL_BOOL(expected.dummy_display, actual.dummy_display);
    TEST_ASSERT_EQUAL_INT(expected.dummy_display_size.x, actual.dummy_display_size.x);
    TEST_ASSERT_EQUAL_INT(expected.dummy_display_size.y, actual.dummy_display_size.y);
}

static struct flutterpi_cmdline_args get_default_args() {
    static char *engine_argv[1] = { "flutter-pi" };

    return (struct flutterpi_cmdline_args){
        .has_orientation = false,
        .orientation = kPortraitUp,
        .has_rotation = false,
        .rotation = 0,
        .has_physical_dimensions = false,
        .physical_dimensions = { .x = 0, .y = 0 },
        .has_pixel_format = false,
        .pixel_format = PIXFMT_RGB565,
        .has_runtime_mode = false,
        .runtime_mode = FLUTTER_RUNTIME_MODE_DEBUG,
        .bundle_path = BUNDLE_PATH,
        .engine_argc = 1,
        .engine_argv = engine_argv,
        .use_vulkan = false,
        .desired_videomode = NULL,
        .dummy_display = false,
        .dummy_display_size = { .x = 0, .y = 0 },
    };
}

void test_parse_orientation_arg() {
    struct flutterpi_cmdline_args expected = get_default_args();

    // test --orientation
    expected.has_orientation = true;
    expected.orientation = kPortraitUp;
    expect_parsed_cmdline_args_matches(
        4,
        (char *[]){
            "flutter-pi",
            "--orientation",
            "portrait_up",
            BUNDLE_PATH,
        },
        true,
        expected
    );

    expected.orientation = kLandscapeLeft;
    expect_parsed_cmdline_args_matches(
        4,
        (char *[]){
            "flutter-pi",
            "--orientation",
            "landscape_left",
            BUNDLE_PATH,
        },
        true,
        expected
    );

    expected.orientation = kPortraitDown;
    expect_parsed_cmdline_args_matches(
        4,
        (char *[]){
            "flutter-pi",
            "--orientation",
            "portrait_down",
            BUNDLE_PATH,
        },
        true,
        expected
    );

    expected.orientation = kLandscapeRight;
    expect_parsed_cmdline_args_matches(
        4,
        (char *[]){
            "flutter-pi",
            "--orientation",
            "landscape_right",
            BUNDLE_PATH,
        },
        true,
        expected
    );
}

void test_parse_rotation_arg() {
    struct flutterpi_cmdline_args expected = get_default_args();

    expected.has_rotation = true;
    expected.rotation = 0;
    expect_parsed_cmdline_args_matches(4, (char *[]){ "flutter-pi", "--rotation", "0", BUNDLE_PATH }, true, expected);

    expected.rotation = 90;
    expect_parsed_cmdline_args_matches(4, (char *[]){ "flutter-pi", "--rotation", "90", BUNDLE_PATH }, true, expected);

    expected.rotation = 180;
    expect_parsed_cmdline_args_matches(4, (char *[]){ "flutter-pi", "--rotation", "180", BUNDLE_PATH }, true, expected);

    expected.rotation = 270;
    expect_parsed_cmdline_args_matches(4, (char *[]){ "flutter-pi", "--rotation", "270", BUNDLE_PATH }, true, expected);
}

void test_parse_physical_dimensions_arg() {
    struct flutterpi_cmdline_args expected = get_default_args();

    expected.bundle_path = NULL;
    expected.engine_argc = 0;
    expected.engine_argv = NULL;
    expect_parsed_cmdline_args_matches(4, (char *[]){ "flutter-pi", "--dimensions", "-10,-10", BUNDLE_PATH }, false, expected);
    expect_parsed_cmdline_args_matches(4, (char *[]){ "flutter-pi", "--dimensions", "xyz", BUNDLE_PATH }, false, expected);

    expected = get_default_args();
    expected.has_physical_dimensions = true;
    expected.physical_dimensions = (struct vec2i){ .x = 10, .y = 10 };
    expect_parsed_cmdline_args_matches(4, (char *[]){ "flutter-pi", "--dimensions", "10,10", BUNDLE_PATH }, true, expected);
}

void test_parse_pixel_format_arg() {
    struct flutterpi_cmdline_args expected = get_default_args();

    expected.has_pixel_format = true;
    expected.pixel_format = PIXFMT_RGB565;

    expect_parsed_cmdline_args_matches(4, (char *[]){ "flutter-pi", "--pixelformat", "RGB565", BUNDLE_PATH }, true, expected);

    expected.pixel_format = PIXFMT_RGBA8888;
    expect_parsed_cmdline_args_matches(4, (char *[]){ "flutter-pi", "--pixelformat", "RGBA8888", BUNDLE_PATH }, true, expected);
}

void test_parse_runtime_mode_arg() {
    struct flutterpi_cmdline_args expected = get_default_args();

    // test --debug, --profile, --release
    expected.bundle_path = NULL;
    expected.engine_argc = 0;
    expected.engine_argv = NULL;
    expect_parsed_cmdline_args_matches(3, (char *[]){ "flutter-pi", "--debug", BUNDLE_PATH }, false, expected);

    expected = get_default_args();
    expected.has_runtime_mode = true;
    expected.runtime_mode = FLUTTER_RUNTIME_MODE_PROFILE;
    expect_parsed_cmdline_args_matches(3, (char *[]){ "flutter-pi", "--profile", BUNDLE_PATH }, true, expected);

    expected.runtime_mode = FLUTTER_RUNTIME_MODE_RELEASE;
    expect_parsed_cmdline_args_matches(3, (char *[]){ "flutter-pi", "--release", BUNDLE_PATH }, true, expected);
}

void test_parse_bundle_path_arg() {
    struct flutterpi_cmdline_args expected = get_default_args();

    expected.bundle_path = "/path/to/bundle/test";
    expect_parsed_cmdline_args_matches(2, (char *[]){ "flutter-pi", "/path/to/bundle/test" }, true, expected);
}

void test_parse_engine_arg() {
    struct flutterpi_cmdline_args expected = get_default_args();

    expected.engine_argc = 2;
    expected.engine_argv = (char *[]){ "flutter-pi", "engine-arg" };

    expect_parsed_cmdline_args_matches(3, (char *[]){ "flutter-pi", BUNDLE_PATH, "engine-arg" }, true, expected);
}

void test_parse_vulkan_arg() {
    struct flutterpi_cmdline_args expected = get_default_args();

    expected.use_vulkan = true;
    expect_parsed_cmdline_args_matches(3, (char *[]){ "flutter-pi", "--vulkan", BUNDLE_PATH }, true, expected);
}

void test_parse_desired_videomode_arg() {
    struct flutterpi_cmdline_args expected = get_default_args();

    expected.desired_videomode = "1920x1080";
    expect_parsed_cmdline_args_matches(4, (char *[]){ "flutter-pi", "--videomode", "1920x1080", BUNDLE_PATH }, true, expected);

    expected.desired_videomode = "1920x1080@60";
    expect_parsed_cmdline_args_matches(4, (char *[]){ "flutter-pi", "--videomode", "1920x1080@60", BUNDLE_PATH }, true, expected);
}

int main() {
    UNITY_BEGIN();

    RUN_TEST(test_parse_runtime_mode_arg);
    RUN_TEST(test_parse_orientation_arg);
    RUN_TEST(test_parse_rotation_arg);
    RUN_TEST(test_parse_physical_dimensions_arg);
    RUN_TEST(test_parse_pixel_format_arg);
    RUN_TEST(test_parse_bundle_path_arg);
    RUN_TEST(test_parse_engine_arg);
    RUN_TEST(test_parse_vulkan_arg);
    RUN_TEST(test_parse_desired_videomode_arg);

    UNITY_END();
}
