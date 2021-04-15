#ifndef _EVENT_LOOP_H
#define _EVENT_LOOP_H

#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>

#define LOG_EVENT_LOOP_ERROR(format_str, ...) fprintf(stderr, "[event_loop] %s: " format_str, __func__, ##__VA_ARGS__)

typedef void (*event_loop_task_callback_t)(void *userdata);

typedef void (*event_loop_timed_task_callback_t)(void *userdata);

typedef bool (*event_loop_io_callback_t)(int fd, uint32_t events, void *userdata);

struct event_loop;

/**
 * @brief Create a new multi-producer, single-consumer event loop. The sd_event loop
 * is single-producer, single-consumer, so only the thread that is processing the events
 * can modify the event loop (add new event sources).
 * 
 * But we want to be able to add events to the event loop from multiple threads
 * (even though we're still only processing on one thread), so we use this instead.
 */
struct event_loop *event_loop_create(void);

/**
 * @brief Destroy the event loop, freeing all allocated resources. Should not be called
 * inside a event loop callback.
 */
void event_loop_destroy(struct event_loop *loop);

/**
 * @brief Schedule the exit of this event loop, possibly causing @ref event_loop_process
 * to return and @ref event_loop_process_pending to set its should_exit argument to true.
 * 
 * After both of these functions have finished, you can destroy the exit loop.
 */
int event_loop_schedule_exit(struct event_loop *loop);

/**
 * @brief Post a generic task to the event loop which will be executed
 * when @ref event_loop_process or @ref event_loop_process_pending is called.
 */
int event_loop_post_task(
    struct event_loop *loop,
    event_loop_task_callback_t callback,
    void *userdata
);

/**
 * @brief Post a task that is executed not before the absolute timestamp in @param target_time_usec.
 * The reference clock used is CLOCK_MONOTONIC.
 */
int event_loop_post_task_with_time(
    struct event_loop *loop,
    uint64_t target_time_usec,
    event_loop_timed_task_callback_t callback,
    void *userdata
);

/**
 * @brief Post a callback to be called on the thread processing the events when the fd becomes ready.
 * The return value of the callback determines whether the callback should stay active. If the callback
 * returns false, this event source is deleted and all associated data is freed (but the file descriptor
 * won't be closed. That's the job of the callback)
 */
int event_loop_add_io(
    struct event_loop *event_loop,
    int fd,
    int events,
    event_loop_io_callback_t callback,
    void *userdata
);

/**
 * @brief Repeatedly process events in this loop until @ref event_loop_schedule_exit is called.
 */
int event_loop_process(
    struct event_loop *loop
);

/**
 * @brief Process all the events that are currently pending and set the should_exit param to whether
 * the event loop should exit.
 */
int event_loop_process_pending(
    struct event_loop *loop,
    bool *should_exit
);

#endif