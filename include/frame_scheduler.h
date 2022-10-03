// SPDX-License-Identifier: MIT
/*
 * Vsync Waiter
 *
 * Manages scheduling of frames, rendering, flutter vsync requests/replies.
 *
 * Copyright (c) 2022, Hannes Winkler <hanneswinkler2000@web.de>
 */


#ifndef _FLUTTERPI_INCLUDE_FRAME_SCHEDULER_H
#define _FLUTTERPI_INCLUDE_FRAME_SCHEDULER_H

#include <collection.h>

struct frame_scheduler;

typedef void (*fl_vsync_callback_t)(void *userdata, intptr_t vsync_baton, uint64_t frame_start_time_nanos, uint64_t next_frame_start_time_nanos);

enum present_mode {
    kDoubleBufferedVsync_PresentMode,
    kTripleBufferedVsync_PresentMode
};

/**
 * @brief Creates a new frame scheduler.
 * 
 * A frame scheduler manages the task of scheduling rendering, frames, and handling & responding to
 * flutter vsync requests, depending on the chosen present mode.
 * 
 * The vsync callback is the function that will be called when a flutter vsync request should be responded to.
 * (In practice, that callback should rethread to the platform task thread and then call FlutterEngineOnVsync with the argument
 * vsync baton.)
 * 
 * @param uses_frame_requests Whether @ref frame_scheduler_on_fl_vsync_request will be called at all. For example, this might be false
 *                            when there was no `vsync_callback` specified in the FlutterProjectArgs.
 * @param present_mode        Which present mode to use. (Always wait for the next vsync before starting a frame? Always start
 *                            the next frame when rendering is complete)
 * @param vsync_cb            The function that will be called when a flutter vsync request should be responded to.
 *                            (In practice, that callback should rethread to the platform task thread and then call
 *                            @ref FlutterEngineOnVsync with the argument vsync baton.) 
 * @param userdata            userdata that will be passed to vsync_cb.
 * @return struct frame_scheduler* The new frame scheduler.
 */
ATTR_MALLOC struct frame_scheduler *frame_scheduler_new(bool uses_frame_requests, enum present_mode present_mode, fl_vsync_callback_t vsync_cb, void *userdata);

void frame_scheduler_destroy(struct frame_scheduler *scheduler);

DECLARE_REF_OPS(frame_scheduler)

/**
 * @brief Called when flutter calls the embedder supplied vsync_callback.
 * Embedder should reply on the platform task thread with the timestamp
 * of the next vsync request. Engine will wait till that time and then begin
 * rendering the next frame.
 * 
 * @param scheduler    The frame scheduler instance. 
 * @param vsync_baton  The vsync baton that the flutter engine specified.
 */
void frame_scheduler_on_fl_vsync_request(struct frame_scheduler *scheduler, intptr_t vsync_baton);

void frame_scheduler_on_rendering_complete(struct frame_scheduler *scheduler);

void frame_scheduler_on_fb_released(struct frame_scheduler *scheduler, bool has_timestamp, uint64_t timestamp_ns);

/**
 * @brief Will call present_cb when the next frame is ready to be presented.
 * 
 * If the frame_scheduler is destroyed before the present_cb is called, or if the frame is displaced by another frame, cancel_cb will be called.
 * 
 * @param scheduler  The frame scheduler instance.
 * @param present_cb Called when the frame should be presented.
 * @param userdata   Userdata that's passed to the present and cancel callback.
 * @param cancel_cb  Called when the frame is not going to be presented, and all associated resources should be freed.
 */
void frame_scheduler_present_frame(struct frame_scheduler *scheduler, void_callback_t present_cb, void *userdata, void_callback_t cancel_cb);

#endif // _FLUTTERPI_INCLUDE_FRAME_SCHEDULER_H
