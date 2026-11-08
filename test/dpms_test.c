// SPDX-License-Identifier: MIT
#define _GNU_SOURCE

// Exercise private KMS/window state against mocked DRM ioctls. The other
// flutterpi_module sources are linked normally by this test's CMake target.
#include <unity.h>

#include "../src/modesetting.c"
#include "../src/window.c"
#include "plugins/dpms_plugin.h"

static struct drmdev device;
static struct drm_crtc crtc;
static struct drm_connector connector;
static struct window output;
static drmModeModeInfo mode;
static int commit_error, property_error, event_error;
static int atomic_commits, legacy_commits, event_reads, cursor_moves;
static uint32_t last_flags;
static uint64_t last_active, last_legacy_dpms;
static void *pending_flip;

int __real_drmModeAtomicAddProperty(drmModeAtomicReq *req, uint32_t object, uint32_t property, uint64_t value);

int __wrap_drmModeAtomicAddProperty(drmModeAtomicReq *req, uint32_t object, uint32_t property, uint64_t value) {
    if (property_error != 0) {
        return -property_error;
    }
    TEST_ASSERT_NOT_EQUAL(connector.ids.dpms, property);
    if (object == crtc.id && property == crtc.ids.active) {
        last_active = value;
    }
    return __real_drmModeAtomicAddProperty(req, object, property, value);
}

int __wrap_drmAuthMagic(int fd, drm_magic_t magic) {
    (void) fd;
    (void) magic;
    return 0;
}

int __wrap_drmModeAtomicCommit(int fd, const drmModeAtomicReq *req, uint32_t flags, void *userdata) {
    (void) fd;
    (void) req;
    atomic_commits++;
    last_flags = flags;
    if (commit_error != 0) {
        errno = commit_error;
        return -1;
    }
    if (flags & DRM_MODE_PAGE_FLIP_EVENT) {
        TEST_ASSERT_NULL(pending_flip);
        pending_flip = userdata;
    } else {
        // Power changes must follow the outstanding flip and must not leave a
        // queued frame that could enable the CRTC again.
        TEST_ASSERT_NULL(pending_flip);
        TEST_ASSERT_FALSE(device.per_crtc_state[0].flip_pending);
        TEST_ASSERT_NULL(device.per_crtc_state[0].queued);
    }
    return 0;
}

int __wrap_drmModeConnectorSetProperty(int fd, uint32_t connector_id, uint32_t property, uint64_t value) {
    (void) fd;
    TEST_ASSERT_EQUAL_UINT32(connector.id, connector_id);
    TEST_ASSERT_EQUAL_UINT32(connector.ids.dpms, property);
    legacy_commits++;
    if (commit_error != 0) {
        errno = commit_error;
        return -1;
    }
    last_legacy_dpms = value;
    return 0;
}

int __wrap_drmModeMoveCursor(int fd, uint32_t crtc_id, int x, int y) {
    (void) fd;
    (void) crtc_id;
    (void) x;
    (void) y;
    cursor_moves++;
    return 0;
}

int __wrap_drmHandleEvent(int fd, drmEventContext *context) {
    void *userdata = pending_flip;
    event_reads++;
    // No event is produced by a power-only commit. Catch an accidental read
    // here instead of hanging the test as it would hang on an actual device.
    if (event_error != 0 || userdata == NULL) {
        return -1;
    }
    pending_flip = NULL;
    context->page_flip_handler2(fd, 1, 1, 0, crtc.id, userdata);
    return 0;
}

void setUp(void) {
    memset(&device, 0, sizeof device);
    memset(&crtc, 0, sizeof crtc);
    memset(&connector, 0, sizeof connector);
    memset(&output, 0, sizeof output);
    memset(&mode, 0, sizeof mode);
    // Match the debug DRM device mutex so an extra unlock fails immediately.
    pthread_mutexattr_t mutex_attrs;
    TEST_ASSERT_EQUAL_INT(0, pthread_mutexattr_init(&mutex_attrs));
    TEST_ASSERT_EQUAL_INT(0, pthread_mutexattr_settype(&mutex_attrs, PTHREAD_MUTEX_ERRORCHECK));
    TEST_ASSERT_EQUAL_INT(0, pthread_mutex_init(&device.mutex, &mutex_attrs));
    TEST_ASSERT_EQUAL_INT(0, pthread_mutexattr_destroy(&mutex_attrs));
    pthread_mutex_init(&output.lock, NULL);
    device.n_refs = REFCOUNT_INIT_1;
    device.fd = 42;
    device.master_fd = 42;
    device.supports_atomic_modesetting = true;
    device.n_crtcs = 1;
    device.crtcs = &crtc;
    device.n_connectors = 1;
    device.connectors = &connector;
    crtc.id = 12;
    crtc.bitmask = 1;
    crtc.ids.active = 101;
    crtc.ids.mode_id = 102;
    crtc.committed_state.active = true;
    crtc.committed_state.has_mode = true;
    connector.id = 15;
    connector.ids.crtc_id = 103;
    connector.ids.dpms = 104;
    output.active = true;
    output.set_dpms_locked = kms_window_set_dpms_locked;
    output.kms.drmdev = &device;
    output.kms.connector = &connector;
    output.kms.crtc = &crtc;
    output.kms.mode = &mode;
    output.kms.should_apply_mode = true;
    atomic_commits = legacy_commits = event_reads = cursor_moves = 0;
    commit_error = property_error = event_error = 0;
    pending_flip = NULL;
    last_flags = 0;
    last_active = last_legacy_dpms = UINT64_MAX;
}

void tearDown(void) {
    if (output.composition != NULL) {
        fl_layer_composition_unref(output.composition);
    }
    if (device.per_crtc_state[0].last_flipped != NULL) {
        kms_req_unref(device.per_crtc_state[0].last_flipped);
    }
    TEST_ASSERT_EQUAL_INT(1, refcount_get_for_debug(&device.n_refs));
    pthread_mutex_destroy(&output.lock);
    pthread_mutex_destroy(&device.mutex);
}

static void test_power_commit_keeps_scanout_buffers_and_has_no_flip_event(void) {
    struct kms_req_builder *builder = drmdev_create_request_builder(&device, crtc.id);
    struct kms_req *previous = kms_req_builder_build(builder);
    kms_req_builder_unref(builder);
    device.per_crtc_state[0].last_flipped = previous;

    struct kms_req *req = drmdev_create_dpms_req(&device, crtc.id, connector.id, false);
    TEST_ASSERT_NOT_NULL(req);
    uint64_t timestamp = UINT64_MAX;
    TEST_ASSERT_EQUAL_INT(0, kms_req_commit_blocking(req, &timestamp));
    TEST_ASSERT_EQUAL_UINT64(0, timestamp);
    TEST_ASSERT_EQUAL_UINT64(0, last_active);
    TEST_ASSERT_EQUAL_UINT32(DRM_MODE_ATOMIC_ALLOW_MODESET, last_flags);
    TEST_ASSERT_EQUAL_INT(0, event_reads);
    TEST_ASSERT_EQUAL_PTR(previous, device.per_crtc_state[0].last_flipped);
    TEST_ASSERT_TRUE(crtc.committed_state.has_mode);
    TEST_ASSERT_FALSE(crtc.committed_state.active);
    kms_req_unref(req);
}

static void test_off_buffers_newest_composition_and_on_replays_it(void) {
    TEST_ASSERT_EQUAL_INT(0, window_set_dpms(&output, false));
    struct fl_layer_composition *first = fl_layer_composition_new(0);
    struct fl_layer_composition *latest = fl_layer_composition_new(0);
    TEST_ASSERT_EQUAL_INT(0, kms_window_push_composition(&output, first));
    TEST_ASSERT_EQUAL_INT(0, kms_window_push_composition(&output, latest));
    TEST_ASSERT_EQUAL_INT(1, atomic_commits);
    TEST_ASSERT_EQUAL_PTR(latest, output.composition);
    TEST_ASSERT_EQUAL_INT(1, refcount_get_for_debug(&first->n_refs));
    TEST_ASSERT_EQUAL_INT(2, refcount_get_for_debug(&latest->n_refs));
    TEST_ASSERT_EQUAL_INT(0, window_get_dpms(&output));

    TEST_ASSERT_EQUAL_INT(0, window_set_dpms(&output, true));
    TEST_ASSERT_EQUAL_INT(1, window_get_dpms(&output));
    TEST_ASSERT_EQUAL_INT(2, atomic_commits);
    TEST_ASSERT_EQUAL_UINT64(1, last_active);
    TEST_ASSERT_TRUE(last_flags & DRM_MODE_ATOMIC_ALLOW_MODESET);
    TEST_ASSERT_EQUAL_INT(1, event_reads);
    TEST_ASSERT_EQUAL_PTR(latest, output.composition);
    fl_layer_composition_unref(first);
    fl_layer_composition_unref(latest);
}

static struct kms_req *present_frame(void) {
    struct kms_req_builder *builder = drmdev_create_request_builder(&device, crtc.id);
    TEST_ASSERT_NOT_NULL(builder);
    TEST_ASSERT_EQUAL_INT(0, kms_req_builder_set_connector(builder, connector.id));
    struct kms_req *req = kms_req_builder_build(builder);
    kms_req_builder_unref(builder);
    TEST_ASSERT_EQUAL_INT(0, kms_req_present(req));
    return req;
}

static void test_disable_drains_flip_discards_queue_and_can_replay(void) {
    struct kms_req *pending = present_frame();
    struct kms_req *queued = present_frame();
    TEST_ASSERT_EQUAL_INT(1, atomic_commits);
    TEST_ASSERT_TRUE(device.per_crtc_state[0].flip_pending);
    TEST_ASSERT_EQUAL_PTR(queued, device.per_crtc_state[0].queued);

    TEST_ASSERT_EQUAL_INT(0, window_set_dpms(&output, false));
    TEST_ASSERT_EQUAL_INT(0, window_get_dpms(&output));
    TEST_ASSERT_FALSE(crtc.committed_state.active);
    TEST_ASSERT_EQUAL_INT(2, atomic_commits);
    TEST_ASSERT_EQUAL_INT(1, event_reads);
    TEST_ASSERT_EQUAL_UINT64(0, last_active);
    TEST_ASSERT_EQUAL_UINT32(DRM_MODE_ATOMIC_ALLOW_MODESET, last_flags);
    TEST_ASSERT_NULL(pending_flip);
    TEST_ASSERT_FALSE(device.per_crtc_state[0].flip_pending);
    TEST_ASSERT_NULL(device.per_crtc_state[0].queued);
    TEST_ASSERT_EQUAL_PTR(pending, device.per_crtc_state[0].last_flipped);
    TEST_ASSERT_EQUAL_INT(2, refcount_get_for_debug(&((struct kms_req_builder *) pending)->n_refs));
    TEST_ASSERT_EQUAL_INT(1, refcount_get_for_debug(&((struct kms_req_builder *) queued)->n_refs));

    struct fl_layer_composition *latest = fl_layer_composition_new(0);
    TEST_ASSERT_EQUAL_INT(0, kms_window_push_composition(&output, latest));
    TEST_ASSERT_EQUAL_INT(2, atomic_commits);
    TEST_ASSERT_EQUAL_INT(0, window_set_dpms(&output, true));
    TEST_ASSERT_EQUAL_INT(1, window_get_dpms(&output));
    TEST_ASSERT_TRUE(crtc.committed_state.active);
    TEST_ASSERT_EQUAL_INT(3, atomic_commits);
    TEST_ASSERT_EQUAL_INT(2, event_reads);
    TEST_ASSERT_EQUAL_UINT64(1, last_active);
    TEST_ASSERT_EQUAL_PTR(latest, output.composition);
    TEST_ASSERT_EQUAL_INT(1, refcount_get_for_debug(&((struct kms_req_builder *) pending)->n_refs));
    fl_layer_composition_unref(latest);
    kms_req_unref(queued);
    kms_req_unref(pending);
}

static void test_failed_flip_drain_preserves_mode_and_can_be_retried(void) {
    struct kms_req *pending = present_frame();
    struct kms_req *queued = present_frame();
    event_error = EIO;
    TEST_ASSERT_EQUAL_INT(EIO, window_set_dpms(&output, false));
    TEST_ASSERT_EQUAL_INT(1, window_get_dpms(&output));
    TEST_ASSERT_TRUE(crtc.committed_state.active);
    TEST_ASSERT_EQUAL_INT(1, atomic_commits);
    TEST_ASSERT_EQUAL_INT(1, event_reads);
    TEST_ASSERT_EQUAL_PTR(pending, pending_flip);
    TEST_ASSERT_TRUE(device.per_crtc_state[0].flip_pending);
    TEST_ASSERT_NULL(device.per_crtc_state[0].queued);
    TEST_ASSERT_EQUAL_INT(1, refcount_get_for_debug(&((struct kms_req_builder *) queued)->n_refs));

    event_error = 0;
    TEST_ASSERT_EQUAL_INT(0, window_set_dpms(&output, false));
    TEST_ASSERT_EQUAL_INT(0, window_get_dpms(&output));
    TEST_ASSERT_EQUAL_INT(2, atomic_commits);
    TEST_ASSERT_EQUAL_INT(2, event_reads);
    TEST_ASSERT_NULL(pending_flip);
    kms_req_unref(queued);
    kms_req_unref(pending);
}

static void test_failed_disable_after_flip_drain_preserves_mode(void) {
    struct kms_req *pending = present_frame();
    struct kms_req *queued = present_frame();
    commit_error = EIO;
    TEST_ASSERT_EQUAL_INT(EIO, window_set_dpms(&output, false));
    TEST_ASSERT_EQUAL_INT(1, window_get_dpms(&output));
    TEST_ASSERT_TRUE(crtc.committed_state.active);
    TEST_ASSERT_EQUAL_INT(2, atomic_commits);
    TEST_ASSERT_EQUAL_INT(1, event_reads);
    TEST_ASSERT_NULL(pending_flip);
    TEST_ASSERT_FALSE(device.per_crtc_state[0].flip_pending);
    TEST_ASSERT_NULL(device.per_crtc_state[0].queued);
    TEST_ASSERT_EQUAL_INT(1, refcount_get_for_debug(&((struct kms_req_builder *) queued)->n_refs));

    commit_error = 0;
    TEST_ASSERT_EQUAL_INT(0, window_set_dpms(&output, false));
    TEST_ASSERT_EQUAL_INT(0, window_get_dpms(&output));
    TEST_ASSERT_EQUAL_INT(3, atomic_commits);
    TEST_ASSERT_EQUAL_INT(1, event_reads);
    kms_req_unref(queued);
    kms_req_unref(pending);
}

static void test_blocking_frame_supersedes_queued_frame(void) {
    struct kms_req *pending = present_frame();
    struct kms_req *queued = present_frame();
    struct kms_req_builder *builder = drmdev_create_request_builder(&device, crtc.id);
    TEST_ASSERT_NOT_NULL(builder);
    TEST_ASSERT_EQUAL_INT(0, kms_req_builder_set_connector(builder, connector.id));
    struct kms_req *replacement = kms_req_builder_build(builder);
    kms_req_builder_unref(builder);

    uint64_t timestamp = 0;
    TEST_ASSERT_EQUAL_INT(0, kms_req_commit_blocking(replacement, &timestamp));
    TEST_ASSERT_EQUAL_UINT64(1000000000, timestamp);
    TEST_ASSERT_EQUAL_INT(2, atomic_commits);
    TEST_ASSERT_EQUAL_INT(2, event_reads);
    TEST_ASSERT_EQUAL_PTR(replacement, device.per_crtc_state[0].last_flipped);
    TEST_ASSERT_FALSE(device.per_crtc_state[0].flip_pending);
    TEST_ASSERT_NULL(device.per_crtc_state[0].queued);
    TEST_ASSERT_EQUAL_INT(1, refcount_get_for_debug(&((struct kms_req_builder *) pending)->n_refs));
    TEST_ASSERT_EQUAL_INT(1, refcount_get_for_debug(&((struct kms_req_builder *) queued)->n_refs));
    kms_req_unref(replacement);
    kms_req_unref(queued);
    kms_req_unref(pending);
}

static void test_nonblocking_power_request_does_not_drain_frames(void) {
    struct kms_req *pending = present_frame();
    struct kms_req *queued = present_frame();
    struct kms_req *off = drmdev_create_dpms_req(&device, crtc.id, connector.id, false);
    TEST_ASSERT_NOT_NULL(off);
    TEST_ASSERT_EQUAL_INT(ENOTSUP, kms_req_commit_nonblocking(off, NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(0, event_reads);
    TEST_ASSERT_EQUAL_INT(1, atomic_commits);
    TEST_ASSERT_EQUAL_PTR(pending, pending_flip);
    TEST_ASSERT_EQUAL_PTR(queued, device.per_crtc_state[0].queued);
    TEST_ASSERT_EQUAL_INT(0, window_set_dpms(&output, false));
    kms_req_unref(off);
    kms_req_unref(queued);
    kms_req_unref(pending);
}

static void test_failed_disable_preserves_mode(void) {
    commit_error = EIO;
    TEST_ASSERT_EQUAL_INT(EIO, window_set_dpms(&output, false));
    TEST_ASSERT_EQUAL_INT(1, window_get_dpms(&output));
    TEST_ASSERT_TRUE(crtc.committed_state.active);
    TEST_ASSERT_EQUAL_INT(0, event_reads);
}

static void test_failed_replay_stays_off_and_can_be_retried(void) {
    TEST_ASSERT_EQUAL_INT(0, window_set_dpms(&output, false));
    struct fl_layer_composition *composition = fl_layer_composition_new(0);
    TEST_ASSERT_EQUAL_INT(0, kms_window_push_composition(&output, composition));
    commit_error = EIO;
    TEST_ASSERT_EQUAL_INT(EIO, window_set_dpms(&output, true));
    TEST_ASSERT_EQUAL_INT(0, window_get_dpms(&output));
    TEST_ASSERT_FALSE(crtc.committed_state.active);
    commit_error = 0;
    TEST_ASSERT_EQUAL_INT(0, window_set_dpms(&output, true));
    TEST_ASSERT_EQUAL_INT(1, window_get_dpms(&output));
    fl_layer_composition_unref(composition);
}

static void test_legacy_power_uses_selected_connector(void) {
    device.supports_atomic_modesetting = false;
    TEST_ASSERT_EQUAL_INT(0, window_set_dpms(&output, false));
    TEST_ASSERT_EQUAL_UINT64(DRM_MODE_DPMS_OFF, last_legacy_dpms);
    TEST_ASSERT_EQUAL_INT(0, window_set_dpms(&output, true));
    TEST_ASSERT_EQUAL_UINT64(DRM_MODE_DPMS_ON, last_legacy_dpms);
    TEST_ASSERT_EQUAL_INT(2, legacy_commits);
    TEST_ASSERT_EQUAL_INT(0, atomic_commits);
    TEST_ASSERT_EQUAL_INT(0, event_reads);
}

static void test_repeated_modes_and_toggle_before_first_frame(void) {
    TEST_ASSERT_EQUAL_INT(0, window_set_dpms(&output, true));
    TEST_ASSERT_EQUAL_INT(0, atomic_commits);
    TEST_ASSERT_EQUAL_INT(0, window_set_dpms(&output, false));
    TEST_ASSERT_EQUAL_INT(0, window_set_dpms(&output, false));
    TEST_ASSERT_EQUAL_INT(1, atomic_commits);
    TEST_ASSERT_EQUAL_INT(0, window_set_dpms(&output, true));
    TEST_ASSERT_EQUAL_INT(2, atomic_commits);
    TEST_ASSERT_EQUAL_UINT64(1, last_active);
    TEST_ASSERT_EQUAL_INT(0, event_reads);
}

static void test_dummy_and_missing_properties_are_unsupported(void) {
    output.set_dpms_locked = NULL;
    TEST_ASSERT_FALSE(window_supports_dpms(&output));
    TEST_ASSERT_EQUAL_INT(-ENOTSUP, window_get_dpms(&output));
    TEST_ASSERT_EQUAL_INT(ENOTSUP, window_set_dpms(&output, false));
    crtc.ids.active = 0;
    TEST_ASSERT_FALSE(drmdev_supports_dpms(&device, &crtc, &connector));
    errno = 0;
    TEST_ASSERT_NULL(drmdev_create_dpms_req(&device, crtc.id, connector.id, false));
    TEST_ASSERT_EQUAL_INT(ENOTSUP, errno);
}

static void test_lost_drm_master_propagates_error_without_changing_mode(void) {
    device.master_fd = -1;
    TEST_ASSERT_EQUAL_INT(EBUSY, window_set_dpms(&output, false));
    TEST_ASSERT_EQUAL_INT(1, window_get_dpms(&output));
    TEST_ASSERT_EQUAL_INT(0, atomic_commits);
}

static void test_property_allocation_failure_propagates_errno(void) {
    property_error = ENOMEM;
    errno = 0;
    TEST_ASSERT_EQUAL_INT(ENOMEM, window_set_dpms(&output, false));
    TEST_ASSERT_EQUAL_INT(1, window_get_dpms(&output));
    TEST_ASSERT_EQUAL_INT(0, atomic_commits);
}

static void test_ffi_without_an_embedder_returns_errors(void) {
    TEST_ASSERT_NULL(flutterpi);
    TEST_ASSERT_EQUAL_INT(0, flutterpi_dpms_is_available());
    TEST_ASSERT_EQUAL_INT(-ENODEV, flutterpi_dpms_get());
    TEST_ASSERT_EQUAL_INT(ENODEV, flutterpi_dpms_set(false));
}

static void test_cursor_movement_while_off_does_not_touch_hardware(void) {
    struct cursor_buffer cursor = { 0 };
    output.set_cursor_locked = kms_window_set_cursor_locked;
    output.cursor_enabled = true;
    output.kms.has_cursor_plane = true;
    output.kms.pointer_icon = pointer_icon_for_details(POINTER_KIND_BASIC, 1.0);
    cursor.icon = output.kms.pointer_icon;
    output.kms.cursor = &cursor;
    TEST_ASSERT_EQUAL_INT(0, window_set_dpms(&output, false));
    TEST_ASSERT_EQUAL_INT(0, window_set_cursor(&output, false, false, false, POINTER_KIND_NONE, true, VEC2I(30, 40)));
    TEST_ASSERT_EQUAL_INT(30, output.cursor_pos.x);
    TEST_ASSERT_EQUAL_INT(40, output.cursor_pos.y);
    TEST_ASSERT_EQUAL_INT(0, cursor_moves);
    TEST_ASSERT_EQUAL_INT(1, atomic_commits);
    output.kms.cursor = NULL;
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_power_commit_keeps_scanout_buffers_and_has_no_flip_event);
    RUN_TEST(test_off_buffers_newest_composition_and_on_replays_it);
    RUN_TEST(test_disable_drains_flip_discards_queue_and_can_replay);
    RUN_TEST(test_failed_flip_drain_preserves_mode_and_can_be_retried);
    RUN_TEST(test_failed_disable_after_flip_drain_preserves_mode);
    RUN_TEST(test_blocking_frame_supersedes_queued_frame);
    RUN_TEST(test_nonblocking_power_request_does_not_drain_frames);
    RUN_TEST(test_failed_disable_preserves_mode);
    RUN_TEST(test_failed_replay_stays_off_and_can_be_retried);
    RUN_TEST(test_legacy_power_uses_selected_connector);
    RUN_TEST(test_repeated_modes_and_toggle_before_first_frame);
    RUN_TEST(test_dummy_and_missing_properties_are_unsupported);
    RUN_TEST(test_lost_drm_master_propagates_error_without_changing_mode);
    RUN_TEST(test_property_allocation_failure_propagates_errno);
    RUN_TEST(test_ffi_without_an_embedder_returns_errors);
    RUN_TEST(test_cursor_movement_while_off_does_not_touch_hardware);
    return UNITY_END();
}
