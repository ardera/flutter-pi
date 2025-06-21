// SPDX-License-Identifier: MIT
/*
 * Event Loop
 *
 * - multithreaded event loop
 *
 * Copyright (c) 2023, Hannes Winkler <hanneswinkler2000@web.de>
 */

#ifndef _FLUTTERPI_SRC_EVENT_LOOP_H
#define _FLUTTERPI_SRC_EVENT_LOOP_H

#include <pthread.h>

#include <flutter_embedder.h>

#include "util/refcounting.h"
#include "util/collection.h"

struct evloop;

struct evloop *evloop_new();

DECLARE_REF_OPS(evloop)

int evloop_get_fd(struct evloop *loop);

int evloop_run(struct evloop *loop);

int evloop_schedule_exit(struct evloop *loop);

int evloop_post_task(struct evloop *loop, void_callback_t callback, void *userdata);

int evloop_post_delayed_task(struct evloop *loop, void_callback_t callback, void *userdata, uint64_t target_time_usec);

struct evsrc;

void evsrc_destroy(struct evsrc *src);

enum event_handler_return { EVENT_HANDLER_CONTINUE, EVENT_HANDLER_CANCEL };

typedef enum event_handler_return (*evloop_io_handler_t)(int fd, uint32_t revents, void *userdata);

struct evsrc *evloop_add_io(struct evloop *loop, int fd, uint32_t events, evloop_io_handler_t callback, void *userdata);


struct evthread;

struct evthread *evthread_start_with_loop(struct evloop *loop);

pthread_t evthread_get_pthread(struct evthread *thread);

void evthread_stop(struct evthread *thread);

#endif  // _FLUTTERPI_SRC_EVENT_LOOP_H

