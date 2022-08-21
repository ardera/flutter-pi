// SPDX-License-Identifier: MIT
/*
 * Vsync Waiter
 *
 * Manages scheduling of frames, rendering, flutter vsync requests/replies.
 *
 * Copyright (c) 2022, Hannes Winkler <hanneswinkler2000@web.de>
 */


#include <stdlib.h>

#include <collection.h>
#include <compositor_ng.h>
#include <vsync_waiter.h>

struct vsync_waiter {
    refcount_t n_refs;

    bool uses_frame_requests;
    enum present_mode present_mode;
    fl_vsync_callback_t vsync_cb;
    void *userdata;

    pthread_mutex_t mutex;
};

DEFINE_REF_OPS(vsync_waiter, n_refs)
DEFINE_STATIC_LOCK_OPS(vsync_waiter, mutex)

struct vsync_waiter *vsync_waiter_new(
    bool uses_frame_requests,
    enum present_mode present_mode,
    fl_vsync_callback_t vsync_cb,
    void *userdata
) {
    struct vsync_waiter *waiter;

    waiter = malloc(sizeof *waiter);
    if (waiter == NULL) {
        return NULL;
    }

    waiter->n_refs = REFCOUNT_INIT_1;
    waiter->uses_frame_requests = uses_frame_requests;
    waiter->present_mode = present_mode;
    waiter->vsync_cb = vsync_cb;
    waiter->userdata = userdata;
    return waiter;
}

void vsync_waiter_destroy(struct vsync_waiter *waiter) {
    free(waiter);
}

void vsync_waiter_on_fl_vsync_request(struct vsync_waiter *waiter, intptr_t vsync_baton) {
    DEBUG_ASSERT_NOT_NULL(waiter);
    DEBUG_ASSERT(vsync_baton != 0);
    DEBUG_ASSERT(waiter->uses_frame_requests);

    if (waiter->present_mode == kTripleBufferedVsync_PresentMode) {

    } else if (waiter->present_mode == kDoubleBufferedVsync_PresentMode) {

    }
}

void vsync_waiter_on_rendering_complete(struct vsync_waiter *waiter) {
    DEBUG_ASSERT_NOT_NULL(waiter);


}

void vsync_waiter_on_fb_released(struct vsync_waiter *waiter, bool has_timestamp, uint64_t timestamp_ns) {
    DEBUG_ASSERT_NOT_NULL(waiter);
}

void vsync_waiter_request_fb(struct vsync_waiter *waiter, uint64_t scanout_time_ns) {
    DEBUG_ASSERT_NOT_NULL(waiter);
}
