// SPDX-License-Identifier: MIT
#define _GNU_SOURCE
#include <unity.h>

#include "../src/plugins/gstreamer_video_player/plugin.c"

struct fake_player {
    void *meta;
    struct notifier info, buffering;
};
static platch_obj_recv_callback receiver;
static int successes, unimplemented, events, destroyed_players;
static char registered_channel[128];

void *__wrap_gstplayer_get_userdata_locked(struct gstplayer *player) {
    return ((struct fake_player *) player)->meta;
}
struct notifier *__wrap_gstplayer_get_video_info_notifier(struct gstplayer *player) {
    return &((struct fake_player *) player)->info;
}
struct notifier *__wrap_gstplayer_get_buffering_state_notifier(struct gstplayer *player) {
    return &((struct fake_player *) player)->buffering;
}
void __wrap_gstplayer_destroy(struct gstplayer *player) {
    struct fake_player *fake = (struct fake_player *) player;
    notifier_deinit(&fake->info);
    notifier_deinit(&fake->buffering);
    destroyed_players++;
    free(fake);
}
int __wrap_plugin_registry_set_receiver(const char *channel, enum platch_codec codec, platch_obj_recv_callback cb) {
    (void) codec;
    snprintf(registered_channel, sizeof registered_channel, "%s", channel);
    receiver = cb;
    return 0;
}
int __wrap_plugin_registry_set_receiver_locked(const char *channel, enum platch_codec codec, platch_obj_recv_callback cb) {
    return __wrap_plugin_registry_set_receiver(channel, codec, cb);
}
int __wrap_plugin_registry_remove_receiver(const char *channel) {
    (void) channel;
    receiver = NULL;
    return 0;
}
int __wrap_plugin_registry_remove_receiver_locked(const char *channel) {
    return __wrap_plugin_registry_remove_receiver(channel);
}
int __wrap_platch_respond_success_std(const FlutterPlatformMessageResponseHandle *handle, struct std_value *value) {
    (void) handle;
    (void) value;
    successes++;
    return 0;
}
int __wrap_platch_respond_not_implemented(const FlutterPlatformMessageResponseHandle *handle) {
    (void) handle;
    unimplemented++;
    return 0;
}
int __wrap_platch_send_success_event_std(char *channel, struct std_value *value) {
    (void) channel;
    (void) value;
    events++;
    return 0;
}

void setUp(void) {
    memset(&plugin, 0, sizeof plugin);
    TEST_ASSERT_EQUAL_INT(PLUGIN_INIT_RESULT_INITIALIZED, gstplayer_plugin_init(NULL, NULL));
    receiver = NULL;
    successes = unimplemented = events = destroyed_players = 0;
}
void tearDown(void) {
    gstplayer_plugin_deinit(NULL, NULL);
}

static struct gstplayer *make_player(void) {
    struct fake_player *fake = calloc(1, sizeof *fake);
    value_notifier_init(&fake->info, NULL, NULL);
    value_notifier_init(&fake->buffering, NULL, NULL);
    struct gstplayer_meta *meta = create_meta(7, (struct gstplayer *) fake);
    fake->meta = meta;
    add_player(meta);
    plugin_registry_set_receiver(meta->event_channel_name, kStandardMethodCall, on_receive_evch);
    return (struct gstplayer *) fake;
}
static void method(const char *name) {
    TEST_ASSERT_NOT_NULL(receiver);
    struct platch_obj object = { .codec = kStandardMethodCall, .method = (char *) name };
    TEST_ASSERT_EQUAL_INT(0, receiver(registered_channel, &object, NULL));
}
void test_cancel_before_dispose_releases_receiver(void) {
    struct gstplayer *player = make_player();
    method("listen");
    method("cancel");
    dispose_player(player, false, false);
    TEST_ASSERT_EQUAL_INT(2, successes);
    TEST_ASSERT_NULL(receiver);
    TEST_ASSERT_EQUAL_INT(1, destroyed_players);
}
void test_dispose_before_cancel_releases_player_and_acknowledges_pending_cancel(void) {
    struct gstplayer *player = make_player();
    method("listen");
    dispose_player(player, false, false);
    TEST_ASSERT_EQUAL_INT(1, destroyed_players);
    method("cancel");
    TEST_ASSERT_EQUAL_INT(2, successes);
    TEST_ASSERT_EQUAL_INT(0, unimplemented);
    TEST_ASSERT_NULL(receiver);
}
void test_dispose_without_listen_has_no_uninitialized_listeners_or_retired_channel(void) {
    struct gstplayer *player = make_player();
    struct gstplayer_meta *meta = get_meta(player);
    TEST_ASSERT_NULL(meta->video_info_listener);
    TEST_ASSERT_NULL(meta->buffering_state_listener);
    dispose_player(player, false, false);
    TEST_ASSERT_NULL(receiver);
    TEST_ASSERT_EQUAL_INT(1, destroyed_players);
}
void test_repeated_listen_and_notify_keep_one_subscription_and_one_initialization(void) {
    struct gstplayer *player = make_player();
    method("listen");
    method("listen");
    struct notifier *notifier = gstplayer_get_video_info_notifier(player);
    TEST_ASSERT_EQUAL_INT(1, list_length(&notifier->listeners));
    struct video_info info = { .width = 4, .height = 4 };
    notifier_notify(notifier, &info);
    notifier_notify(notifier, &info);
    TEST_ASSERT_EQUAL_INT(1, events);
    method("cancel");
    TEST_ASSERT_TRUE(list_is_empty(&notifier->listeners));
    dispose_player(player, false, false);
}
void test_retired_channel_rejects_listen_then_accepts_cancel(void) {
    struct gstplayer *player = make_player();
    method("listen");
    dispose_player(player, false, false);
    method("listen");
    TEST_ASSERT_EQUAL_INT(1, unimplemented);
    struct platch_obj malformed = PLATCH_OBJ_NOT_IMPLEMENTED;
    receiver(registered_channel, &malformed, NULL);
    TEST_ASSERT_EQUAL_INT(2, unimplemented);
    method("cancel");
    TEST_ASSERT_NULL(receiver);
}

void test_shutdown_cleans_channels_whose_cancel_never_arrived(void) {
    struct gstplayer *player = make_player();
    method("listen");
    dispose_player(player, false, false);
    gstplayer_plugin_deinit(NULL, NULL);
    TEST_ASSERT_NULL(receiver);
    TEST_ASSERT_EQUAL_INT(1, destroyed_players);
    TEST_ASSERT_EQUAL_INT(PLUGIN_INIT_RESULT_INITIALIZED, gstplayer_plugin_init(NULL, NULL));
}
void test_rapid_creation_and_late_cancel_release_every_player(void) {
    for (int i = 0; i < 100; i++) {
        struct gstplayer *player = make_player();
        method("listen");
        dispose_player(player, false, false);
        method("cancel");
        TEST_ASSERT_NULL(receiver);
    }
    TEST_ASSERT_EQUAL_INT(100, destroyed_players);
}
void test_unknown_channel_is_not_silently_acknowledged(void) {
    struct platch_obj object = PLATCH_OBJ_STD_CALL("cancel", STDNULL);
    on_receive_evch("flutter.io/videoPlayer/videoEvents999", &object, NULL);
    TEST_ASSERT_EQUAL_INT(1, unimplemented);
    TEST_ASSERT_EQUAL_INT(0, successes);
}

void test_relisten_reports_cached_initialization_and_buffering_to_the_new_subscription(void) {
    struct gstplayer *player = make_player();
    method("listen");
    struct video_info info = { .width = 4, .height = 4 };
    struct buffering_state buffering = { .percent = 25, .n_ranges = 0 };
    notifier_notify(gstplayer_get_video_info_notifier(player), &info);
    notifier_notify(gstplayer_get_buffering_state_notifier(player), &buffering);
    TEST_ASSERT_EQUAL_INT(3, events);
    method("cancel");
    method("listen");
    TEST_ASSERT_EQUAL_INT(6, events);
    method("cancel");
    dispose_player(player, false, false);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_cancel_before_dispose_releases_receiver);
    RUN_TEST(test_dispose_before_cancel_releases_player_and_acknowledges_pending_cancel);
    RUN_TEST(test_dispose_without_listen_has_no_uninitialized_listeners_or_retired_channel);
    RUN_TEST(test_repeated_listen_and_notify_keep_one_subscription_and_one_initialization);
    RUN_TEST(test_retired_channel_rejects_listen_then_accepts_cancel);
    RUN_TEST(test_shutdown_cleans_channels_whose_cancel_never_arrived);
    RUN_TEST(test_rapid_creation_and_late_cancel_release_every_player);
    RUN_TEST(test_unknown_channel_is_not_silently_acknowledged);
    RUN_TEST(test_relisten_reports_cached_initialization_and_buffering_to_the_new_subscription);
    return UNITY_END();
}
