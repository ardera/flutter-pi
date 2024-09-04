// SPDX-License-Identifier: MIT
/*
 * Event Loop
 *
 * - multithreaded event loop.
 * - based on sd_event by default, but with locks so tasks can be posted
 *   and event listeners added from any thread.
 *
 * Copyright (c) 2023, Hannes Winkler <hanneswinkler2000@web.de>
 */

#ifndef _FLUTTERPI_SRC_EVENT_LOOP_H
#define _FLUTTERPI_SRC_EVENT_LOOP_H

#include <pthread.h>

#include <flutter_embedder.h>

#include "util/refcounting.h"
#include "util/collection.h"

/**
 * @brief An event loop.
 * 
 */
struct evloop;

/**
 * @brief Creates a new event loop.
 *  
 * The event loop is not running yet. Any tasks added using @ref evloop_post_task,
 * @ref evloop_post_delayed_task or any fd callbacks added using @ref evloop_add_io
 * will not be executed yet.
 * 
 * Unless explicitly stated otherwise, all functions in this file are thread-safe
 * and can be called from any thread.
 * 
 * @returns A new event loop or NULL on error.
 */
struct evloop *evloop_new();

DECLARE_REF_OPS(evloop)

/**
 * @brief Run the event loop.
 * 
 * This function will actually call the (delayed) task callbacks and fd callbacks,
 * when they are ready.
 * 
 * This function will run until exit is scheduled using @ref evloop_schedule_exit.
 */
int evloop_run(struct evloop *loop);

/**
 * @brief Schedule the event loop to exit.
 * 
 */
int evloop_schedule_exit(struct evloop *loop);

/**
 * @brief Post a task to the event loop to be executed as soon as possible.
 * 
 * @param loop The event loop.
 * @param callback The task to execute.
 * @param userdata The userdata to pass to the task
 */
int evloop_post_task(struct evloop *loop, void_callback_t callback, void *userdata);

/**
 * @brief Post a task to the event loop to be executed not sooner than target_time_usec.
 * 
 * @param loop The event loop.
 * @param callback The task to execute.
 * @param userdata The userdata to pass to the task
 * @param target_time_usec The time in microseconds (of CLOCK_MONOTONIC) when the task should be executed.
 */
int evloop_post_delayed_task(struct evloop *loop, void_callback_t callback, void *userdata, uint64_t target_time_usec);


/**
 * @brief An event source that was added to the event loop,
 * and can be disabled & destroyed using @ref evsrc_destroy.
 */
struct evsrc;

/**
 * @brief Destroy an event source.
 * 
 * After this function returns, the callback registered for the event source
 * will not be called anymore.
 * 
 * @param src The event source to destroy.
 */
void evsrc_destroy(struct evsrc *src);

/**
 * @brief The return value of an event handler.
 * 
 */
enum event_handler_return {
    /**
     * @brief Continue watching the event source (No change basically)
     */
    EVENT_HANDLER_CONTINUE,

    /**
     * @brief Stop watching the event source and destroy it.
     * 
     * This can just be used as a shorthand to @ref evsrc_destroy inside an event handler callback.
     * 
     * NOTE: Calling @ref evsrc_destroy inside an fd callback AND returning this value
     * is invalid and will result in undefined behavior.
     */
    EVENT_HANDLER_CANCEL
};

/**
 * @brief A callback that is called by the event loop when a file descriptor is ready.
 * 
 * @param fd The file descriptor that is ready.
 * @param revents The events that are ready.
 * @param userdata The userdata passed to @ref evloop_add_io.
 * @returns Whether the event source should be kept or destroyed.
 */
typedef enum event_handler_return (*evloop_io_handler_t)(int fd, uint32_t revents, void *userdata);

/**
 * @brief Watch a file-descriptor and call a callback when it is ready.
 * 
 * The event loop will call the callback on the thread that it executing @ref evloop_run
 * when fd is ready to read/write (depending on @ref events).
 * 
 * To stop watching the fd, call @ref evsrc_destroy on the returned evsrc,
 * or return @ref EVENT_HANDLER_CANCEL from the callback.
 * 
 * @param loop The event loop.
 * @param fd The file descriptor to watch.
 * @param events The events to watch for (EPOLLIN, EPOLLOUT, etc).
 * @param callback The callback to call when the fd is ready.
 * @param userdata The userdata to pass to the callback.
 */
struct evsrc *evloop_add_io(struct evloop *loop, int fd, uint32_t events, evloop_io_handler_t callback, void *userdata);


struct evthread;

/**
 * @brief Start a new thread just running `evloop_run(loop)`.
 */
struct evthread *evthread_start_with_loop(struct evloop *loop);

/**
 * @brief Get the thread id of the event thread.
 */
pthread_t evthread_get_pthread(struct evthread *thread);

/**
 * @brief Stops the event loop that the thread is running,
 * and waits for the event thread to quit.
 */
void evthread_stop(struct evthread *thread);

#endif  // _FLUTTERPI_SRC_EVENT_LOOP_H
