// SPDX-License-Identifier: MIT
/*
 * Frame scheduler
 *
 * Manages scheduling of frames, rendering, flutter vsync requests/replies.
 *
 * Copyright (c) 2022, Hannes Winkler <hanneswinkler2000@web.de>
 */

#include "frame_scheduler.h"

#include <stdlib.h>

#include "compositor_ng.h"
#include "util/collection.h"
#include "util/lock_ops.h"
#include "util/refcounting.h"

struct frame_scheduler {
    refcount_t n_refs;
    mutex_t mutex;

    bool uses_frame_requests;
    enum present_mode present_mode;
    fl_vsync_callback_t vsync_cb;
    void *userdata;

    bool waiting_for_scanout;

    bool has_scheduled_frame;
    struct {
        void_callback_t present_cb;
        void_callback_t cancel_cb;
        void *userdata;
    } scheduled_frame;
};

DEFINE_REF_OPS(frame_scheduler, n_refs)
DEFINE_STATIC_LOCK_OPS(frame_scheduler, mutex)

struct frame_scheduler *
frame_scheduler_new(bool uses_frame_requests, enum present_mode present_mode, fl_vsync_callback_t vsync_cb, void *userdata) {
    struct frame_scheduler *scheduler;

    // uses_frame_requests? => vsync_cb != NULL
    assert(!uses_frame_requests || vsync_cb != NULL);

    scheduler = malloc(sizeof *scheduler);
    if (scheduler == NULL) {
        return NULL;
    }

    scheduler->n_refs = REFCOUNT_INIT_1;
    mutex_init(&scheduler->mutex);

    scheduler->uses_frame_requests = uses_frame_requests;
    scheduler->present_mode = present_mode;
    scheduler->vsync_cb = vsync_cb;
    scheduler->userdata = userdata;

    scheduler->waiting_for_scanout = false;
    scheduler->has_scheduled_frame = false;
    return scheduler;
}

void frame_scheduler_destroy(struct frame_scheduler *scheduler) {
    free(scheduler);
}

void frame_scheduler_on_fl_vsync_request(struct frame_scheduler *scheduler, intptr_t vsync_baton) {
    ASSERT_NOT_NULL(scheduler);
    assert(vsync_baton != 0);
    assert(scheduler->uses_frame_requests);

    // flutter called the vsync callback.
    //  - when do we reply to it?
    //  - what timestamps do we send as a reply?
    //
    // Some things to keep in mind:
    //  - GPU rendering is a big pipeline:
    //      uploading -> vertex shading -> tesselation -> geometry shading -> rasterization -> fragment shading
    //  - Some parts of the pipeline might execute on different parts of the GPU, maybe there's specific
    //    fixed-function hardware for different steps of the pipeline. For example, on PowerVR, there's a tiler (vertex)
    //    stage and a separate renderer (fragment) stage.
    //  - So it might not be smart to render just one frame at a time.
    //  - On PowerVR, it's best to have a 3-frame pipeline:
    //
    //                               Frame 0       Frame 1       Frame 2
    //      Geometry Submission  | In Progress |      .      |      .      |
    //      Vertex Processing    |             | In Progress |      .      |
    //      Fragment Processing  |             |             | In Progress |
    //
    //  - That way, occupancy & throughput is optimal, and rendering can be at 60FPS even though maybe fragment processing takes > 16ms.
    //  - On the other hand, normally a mesa EGL surface only has 4 buffers available, so we could run out of framebuffers for surfaces
    //    as well if we draw too many frames at once. (Especially considering one framebuffer is probably busy with scanout right now)
    //

    /// TODO: Implement
    /// For now, just unconditionally reply
    if (scheduler->present_mode == kTripleBufferedVsync_PresentMode) {
        scheduler->vsync_cb(scheduler->userdata, vsync_baton, 0, 0);
    } else if (scheduler->present_mode == kDoubleBufferedVsync_PresentMode) {
        scheduler->vsync_cb(scheduler->userdata, vsync_baton, 0, 0);
    }
}

void frame_scheduler_on_rendering_complete(struct frame_scheduler *scheduler) {
    ASSERT_NOT_NULL(scheduler);
    (void) scheduler;

    /// TODO: Implement
    UNIMPLEMENTED();
}

void frame_scheduler_on_fb_released(struct frame_scheduler *scheduler, bool has_timestamp, uint64_t timestamp_ns) {
    ASSERT_NOT_NULL(scheduler);
    (void) scheduler;
    (void) has_timestamp;
    (void) timestamp_ns;

    /// TODO: Implement
    UNIMPLEMENTED();
}

void frame_scheduler_request_fb(struct frame_scheduler *scheduler, uint64_t scanout_time_ns) {
    ASSERT_NOT_NULL(scheduler);
    (void) scheduler;
    (void) scanout_time_ns;

    /// TODO: Implement
    UNIMPLEMENTED();
}

void frame_scheduler_present_frame(struct frame_scheduler *scheduler, void_callback_t present_cb, void *userdata, void_callback_t cancel_cb) {   
    ASSERT_NOT_NULL(scheduler);
    ASSERT_NOT_NULL(present_cb);
    
    frame_scheduler_lock(scheduler);

    if (scheduler->waiting_for_scanout) {
        void_callback_t cancel_prev_sched_frame = NULL;
        void *prev_sched_frame_userdata = NULL;

        // We're already waiting for a scanout, so we can't present a frame right now.
        // Wait till the previous frame is scanned out, and then present.

        if (scheduler->has_scheduled_frame) {
            // If we already have a frame scheduled, cancel it.
            cancel_prev_sched_frame = scheduler->scheduled_frame.cancel_cb;
            prev_sched_frame_userdata = scheduler->scheduled_frame.userdata;

            memset(&scheduler->scheduled_frame, 0, sizeof scheduler->scheduled_frame);
        }

        scheduler->has_scheduled_frame = true;
        scheduler->scheduled_frame.present_cb = present_cb;
        scheduler->scheduled_frame.cancel_cb = cancel_cb;
        scheduler->scheduled_frame.userdata = userdata;

        frame_scheduler_unlock(scheduler);

        if (cancel_prev_sched_frame != NULL) {
            cancel_prev_sched_frame(prev_sched_frame_userdata);
        }
    } else {
        // We're not waiting for a scanout right now.
        scheduler->waiting_for_scanout = true;
        frame_scheduler_unlock(scheduler);

        present_cb(userdata);
    }
}

void frame_scheduler_on_scanout(struct frame_scheduler *scheduler, bool has_timestamp, uint64_t timestamp_ns) {
    ASSERT_NOT_NULL(scheduler);
    assert(!has_timestamp || timestamp_ns != 0);
    (void) scheduler;
    (void) has_timestamp;
    (void) timestamp_ns;

    void_callback_t present_cb = NULL;
    void *userdata = NULL;

    frame_scheduler_lock(scheduler);

    if (scheduler->waiting_for_scanout) {
        scheduler->waiting_for_scanout = false;

        if (scheduler->has_scheduled_frame) {
            present_cb = scheduler->scheduled_frame.present_cb;
            userdata = scheduler->scheduled_frame.userdata;

            memset(&scheduler->scheduled_frame, 0, sizeof scheduler->scheduled_frame);

            scheduler->has_scheduled_frame = false;
        }
    }
    
    frame_scheduler_unlock(scheduler);

    if (present_cb != NULL) {
        present_cb(userdata);
    }
}
