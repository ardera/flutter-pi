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

typedef void (*fl_vsync_callback_t)(void *userdata, intptr_t vsync_baton);

ATTR_MALLOC struct vsync_waiter *vsync_waiter_new(bool uses_frame_requests, enum present_mode present_mode, fl_vsync_callback_t vsync_cb, void *userdata);

void vsync_waiter_destroy(struct vsync_waiter *waiter);

DECLARE_REF_OPS(vsync_waiter)

void vsync_waiter_on_fl_vsync_request(struct vsync_waiter *waiter, intptr_t vsync_baton);

void vsync_waiter_on_rendering_complete(struct vsync_waiter *waiter);

void vsync_waiter_on_fb_released(struct vsync_waiter *waiter, bool has_timestamp, uint64_t timestamp_ns);

#endif // _FLUTTERPI_INCLUDE_VSYNC_WAITER_H
