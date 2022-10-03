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
    
    // uses_frame_requests? => vsync_cb != NULL
    DEBUG_ASSERT(!uses_frame_requests || vsync_cb != NULL);

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

/**
 * @brief Called when flutter calls the embedder supplied vsync_callback.
 * Embedder should reply on the platform task thread with the timestamp
 * of the next vsync request. Engine will wait till that time and then begin
 * rendering the next frame.
 * 
 * @param waiter 
 * @param vsync_baton 
 */
void vsync_waiter_on_fl_vsync_request(struct vsync_waiter *waiter, intptr_t vsync_baton) {
    DEBUG_ASSERT_NOT_NULL(waiter);
    DEBUG_ASSERT(vsync_baton != 0);
    DEBUG_ASSERT(waiter->uses_frame_requests);

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
    if (waiter->present_mode == kTripleBufferedVsync_PresentMode) {
        waiter->vsync_cb(waiter->userdata, vsync_baton, 0, 0);
    } else if (waiter->present_mode == kDoubleBufferedVsync_PresentMode) {
        waiter->vsync_cb(waiter->userdata, vsync_baton, 0, 0);
    }
}

void vsync_waiter_on_rendering_complete(struct vsync_waiter *waiter) {
    DEBUG_ASSERT_NOT_NULL(waiter);

    /// TODO: Implement
    UNIMPLEMENTED();
}

void vsync_waiter_on_fb_released(struct vsync_waiter *waiter, bool has_timestamp, uint64_t timestamp_ns) {
    DEBUG_ASSERT_NOT_NULL(waiter);
    (void) has_timestamp;
    (void) timestamp_ns;

    /// TODO: Implement
    UNIMPLEMENTED();
}

void vsync_waiter_request_fb(struct vsync_waiter *waiter, uint64_t scanout_time_ns) {
    DEBUG_ASSERT_NOT_NULL(waiter);
    (void) scanout_time_ns;

    /// TODO: Implement
    UNIMPLEMENTED();
}
