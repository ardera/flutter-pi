// SPDX-License-Identifier: MIT
/*
 * Vsync Waiter
 *
 * Manages scheduling of frames, rendering, flutter vsync requests/replies.
 *
 * Copyright (c) 2022, Hannes Winkler <hanneswinkler2000@web.de>
 */


#ifndef _FLUTTERPI_INCLUDE_VSYNC_WAITER_H
#define _FLUTTERPI_INCLUDE_VSYNC_WAITER_H

#include <collection.h>

struct vsync_waiter;

typedef void (*fl_vsync_callback_t)(void *userdata, intptr_t vsync_baton, uint64_t frame_start_time_nanos, uint64_t next_frame_start_time_nanos);

/**
 * @brief Creates a new vsync waiter.
 * 
 * A vsync waiter manages the task of scheduling rendering, frames, and handling & responding to
 * flutter vsync requests, depending on the chosen present mode.
 * 
 * The vsync callback is the function that will be called when a flutter vsync request should be responded to.
 * (In practice, that callback should rethread to the platform task thread and then call FlutterEngineOnVsync with the argument
 * vsync baton.)
 * 
 * @param uses_frame_requests Whether @ref vsync_waiter_on_fl_vsync_request will be called at all. For example, this might be false
 *                            when there was no `vsync_callback` specified in the FlutterProjectArgs.
 * @param present_mode        Which present mode to use. (Always wait for the next vsync before starting a frame? Always start
 *                            the next frame when rendering is complete)
 * @param vsync_cb            The function that will be called when a flutter vsync request should be responded to.
 *                            (In practice, that callback should rethread to the platform task thread and then call
 *                            @ref FlutterEngineOnVsync with the argument vsync baton.) 
 * @param userdata            userdata that will be passed to vsync_cb.
 * @return ATTR_MALLOC struct* The new vsync waiter.
 */
ATTR_MALLOC struct vsync_waiter *vsync_waiter_new(bool uses_frame_requests, enum present_mode present_mode, fl_vsync_callback_t vsync_cb, void *userdata);

void vsync_waiter_destroy(struct vsync_waiter *waiter);

DECLARE_REF_OPS(vsync_waiter)

void vsync_waiter_on_fl_vsync_request(struct vsync_waiter *waiter, intptr_t vsync_baton);

void vsync_waiter_on_rendering_complete(struct vsync_waiter *waiter);

void vsync_waiter_on_fb_released(struct vsync_waiter *waiter, bool has_timestamp, uint64_t timestamp_ns);

#endif // _FLUTTERPI_INCLUDE_VSYNC_WAITER_H
