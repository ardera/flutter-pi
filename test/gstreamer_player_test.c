// SPDX-License-Identifier: MIT
#define _GNU_SOURCE
#include <unity.h>

#include "../src/plugins/gstreamer_video_player/player.c"

static struct gstplayer *destroying;
static int destroyed_notifiers, state_changes, released_interfaces, released_textures;
static int seek_calls;
static gboolean seek_succeeds;
GstStateChangeReturn __real_gst_element_set_state(GstElement *element, GstState state);
void __real_notifier_deinit(struct notifier *notifier);

gboolean __wrap_gst_element_seek(
    GstElement *element,
    gdouble rate,
    GstFormat format,
    GstSeekFlags flags,
    GstSeekType start_type,
    gint64 start,
    GstSeekType stop_type,
    gint64 stop
) {
    TEST_ASSERT_EQUAL_PTR(destroying->pipeline, element);
    TEST_ASSERT_EQUAL_DOUBLE(1.0, rate);
    TEST_ASSERT_EQUAL_INT(GST_FORMAT_TIME, format);
    TEST_ASSERT_TRUE(flags & GST_SEEK_FLAG_FLUSH);
    TEST_ASSERT_EQUAL_INT(GST_SEEK_TYPE_SET, start_type);
    TEST_ASSERT_EQUAL_INT64(0, start);
    TEST_ASSERT_EQUAL_INT(GST_SEEK_TYPE_SET, stop_type);
    TEST_ASSERT_EQUAL_INT64(GST_CLOCK_TIME_NONE, stop);
    seek_calls++;
    return seek_succeeds;
}

GstStateChangeReturn __wrap_gst_element_set_state(GstElement *element, GstState state) {
    TEST_ASSERT_EQUAL_INT(0, destroyed_notifiers);
    TEST_ASSERT_NOT_NULL(destroying->sink);
    TEST_ASSERT_NOT_NULL(destroying->bus);
    state_changes++;
    return __real_gst_element_set_state(element, state);
}
void __wrap_notifier_deinit(struct notifier *notifier) {
    if (destroying->pipeline != NULL)
        TEST_ASSERT_EQUAL_INT(2, state_changes);
    destroyed_notifiers++;
    __real_notifier_deinit(notifier);
}
void __wrap_frame_interface_unref(struct frame_interface *interface) {
    (void) interface;
    released_interfaces++;
}
void __wrap_texture_destroy(struct texture *texture) {
    (void) texture;
    released_textures++;
}

void setUp(void) {
    destroyed_notifiers = state_changes = released_interfaces = released_textures = seek_calls = 0;
    seek_succeeds = TRUE;
    destroying = calloc(1, sizeof *destroying);
    destroying->headers = gst_structure_new_empty("http-headers");
    destroying->playback_rate_forward = destroying->current_playback_rate = 1.0;
    pthread_mutex_init(&destroying->lock, NULL);
    value_notifier_init(&destroying->video_info_notifier, NULL, NULL);
    value_notifier_init(&destroying->buffering_state_notifier, NULL, NULL);
    change_notifier_init(&destroying->error_notifier);
}
void tearDown(void) {
}

static void attach_test_pipeline(void) {
    destroying->pipeline = gst_pipeline_new(NULL);
    destroying->sink = gst_element_factory_make("fakesink", NULL);
    TEST_ASSERT_NOT_NULL(destroying->sink);
    TEST_ASSERT_TRUE(gst_bin_add(GST_BIN(destroying->pipeline), destroying->sink));
    gst_object_ref(destroying->sink);
    destroying->bus = gst_element_get_bus(destroying->pipeline);
}

void test_destroy_stops_streaming_before_releasing_callback_dependencies(void) {
    attach_test_pipeline();
    gstplayer_destroy(destroying);
    TEST_ASSERT_EQUAL_INT(2, state_changes);
    TEST_ASSERT_EQUAL_INT(3, destroyed_notifiers);
    TEST_ASSERT_EQUAL_INT(1, released_interfaces);
    TEST_ASSERT_EQUAL_INT(1, released_textures);
}
void test_destroy_before_pipeline_initialization_releases_owned_resources(void) {
    gstplayer_destroy(destroying);
    TEST_ASSERT_EQUAL_INT(0, state_changes);
    TEST_ASSERT_EQUAL_INT(3, destroyed_notifiers);
    TEST_ASSERT_EQUAL_INT(1, released_interfaces);
    TEST_ASSERT_EQUAL_INT(1, released_textures);
}
void test_http_header_values_are_initialized_and_can_be_replaced(void) {
    gstplayer_put_http_header(destroying, "Authorization", "first");
    TEST_ASSERT_EQUAL_STRING("first", gst_structure_get_string(destroying->headers, "Authorization"));
    gstplayer_put_http_header(destroying, "Authorization", "second");
    TEST_ASSERT_EQUAL_STRING("second", gst_structure_get_string(destroying->headers, "Authorization"));
    gstplayer_destroy(destroying);
}
static int source_setup_message_size(GstStructure *headers) {
    int descriptors[2];
    TEST_ASSERT_EQUAL_INT(0, pipe(descriptors));
    int saved_stderr = dup(STDERR_FILENO);
    fflush(stderr);
    dup2(descriptors[1], STDERR_FILENO);
    close(descriptors[1]);
    GstElement *source = gst_element_factory_make("fakesrc", NULL);
    on_source_setup(NULL, source, headers);
    gst_object_unref(source);
    fflush(stderr);
    dup2(saved_stderr, STDERR_FILENO);
    close(saved_stderr);
    char message[512];
    int size = read(descriptors[0], message, sizeof message);
    close(descriptors[0]);
    return size;
}
void test_only_configured_headers_report_unsupported_source_capability(void) {
    TEST_ASSERT_EQUAL_INT(0, source_setup_message_size(destroying->headers));
    gstplayer_put_http_header(destroying, "X-Test", "present");
    TEST_ASSERT_GREATER_THAN_INT(0, source_setup_message_size(destroying->headers));
    gstplayer_destroy(destroying);
}

static void message(GstMessage *msg) {
    on_bus_message(destroying, msg);
    gst_message_unref(msg);
}
void test_loop_waits_for_pipeline_eos_and_restarts_through_the_seek_path(void) {
    attach_test_pipeline();
    message(gst_message_new_eos(GST_OBJECT(destroying->pipeline)));
    TEST_ASSERT_EQUAL_INT(0, seek_calls);
    destroying->looping = true;
    message(gst_message_new_application(GST_OBJECT(destroying->sink), gst_structure_new_empty("appsink-eos")));
    message(gst_message_new_eos(GST_OBJECT(destroying->sink)));
    TEST_ASSERT_EQUAL_INT(0, seek_calls);
    message(gst_message_new_eos(GST_OBJECT(destroying->pipeline)));
    TEST_ASSERT_EQUAL_INT(1, seek_calls);
    TEST_ASSERT_FALSE(destroying->has_desired_position);
    TEST_ASSERT_EQUAL_INT64(0, destroying->fallback_position_ms);
    gstplayer_destroy(destroying);
}
void test_failed_loop_seek_preserves_the_request_and_does_not_change_state(void) {
    attach_test_pipeline();
    destroying->looping = true;
    seek_succeeds = FALSE;
    message(gst_message_new_eos(GST_OBJECT(destroying->pipeline)));
    TEST_ASSERT_EQUAL_INT(1, seek_calls);
    TEST_ASSERT_TRUE(destroying->has_desired_position);
    TEST_ASSERT_EQUAL_INT(0, state_changes);
    gstplayer_destroy(destroying);
}

int main(void) {
    gst_init(NULL, NULL);
    UNITY_BEGIN();
    RUN_TEST(test_destroy_stops_streaming_before_releasing_callback_dependencies);
    RUN_TEST(test_destroy_before_pipeline_initialization_releases_owned_resources);
    RUN_TEST(test_http_header_values_are_initialized_and_can_be_replaced);
    RUN_TEST(test_only_configured_headers_report_unsupported_source_capability);
    RUN_TEST(test_loop_waits_for_pipeline_eos_and_restarts_through_the_seek_path);
    RUN_TEST(test_failed_loop_seek_preserves_the_request_and_does_not_change_state);
    int result = UNITY_END();
    gst_deinit();
    return result;
}
