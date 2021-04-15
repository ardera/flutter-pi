#define _POSIX_C_SOURCE 199309L
#include <errno.h>
#include <stdbool.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <systemd/sd-event.h>

#include <collection.h>
#include <event_loop.h>

struct event_loop {
    int epoll_fd;
    int schedule_exit_fd;
    bool should_exit;
};

struct trampoline {
    event_loop_task_callback_t task_callback;
    event_loop_timed_task_callback_t timed_task_callback;
    event_loop_io_callback_t io_callback;
    int fd;
    int events;
    void *userdata;
};

static struct trampoline *trampoline_new(int fd, void *userdata) {
    struct trampoline *trampoline;

    trampoline = malloc(sizeof *trampoline);
    if (trampoline == NULL) {
        return NULL;
    }

    trampoline->task_callback = NULL;
    trampoline->timed_task_callback = NULL;
    trampoline->io_callback = NULL;
    trampoline->fd = fd;
    trampoline->events = 0;
    trampoline->userdata = userdata;

    return trampoline;
}


struct event_loop *event_loop_create(void) {
    struct event_loop *loop;
	int epoll_fd, schedule_exit_fd;

	loop = malloc(sizeof *loop);
	if (loop == NULL) {
		goto fail_return_null;
	}

    epoll_fd = epoll_create1(EFD_CLOEXEC);
    if (epoll_fd < 0) {
        LOG_EVENT_LOOP_ERROR("Couldn't create epoll instance. epoll_create1: %s\n", strerror(errno));
        goto fail_free_loop;
    }

    schedule_exit_fd = eventfd(0, EFD_CLOEXEC);
    if (schedule_exit_fd < 0) {
        LOG_EVENT_LOOP_ERROR("Couldn't create event fd for signalling event loop exit. eventfd: %s\n", strerror(errno));
        goto fail_close_epoll_fd;
    }

	loop->epoll_fd = epoll_fd;
    loop->schedule_exit_fd = schedule_exit_fd;

	return loop;


    fail_close_epoll_fd:
    close(epoll_fd);

	fail_free_loop:
	free(loop);

	fail_return_null:
	return NULL;
}

void event_loop_destroy(struct event_loop *loop) {
    close(loop->schedule_exit_fd);
    close(loop->epoll_fd);
    free(loop);
}

int event_loop_schedule_exit(struct event_loop *loop) {
    int ok;

    ok = write(loop->schedule_exit_fd, (uint8_t[8]) {0, 0, 0, 0, 0, 0, 0, 1}, 8);
    if (ok < 0) {
        ok = errno;
        LOG_EVENT_LOOP_ERROR("Couldn't write to exit signalling fd. write: %s\n", strerror(errno));
        return ok;
    }

    return 0;
}

int event_loop_post_task(
    struct event_loop *loop,
    event_loop_task_callback_t callback,
    void *userdata
) {
    struct trampoline *trampoline;
    int ok, event_fd;

    ok = eventfd(1, EFD_CLOEXEC);
    if (ok < 0) {
        ok = errno;
        LOG_EVENT_LOOP_ERROR("Couldn't create eventfd. eventfd: %s\n", strerror(errno));
        goto fail_return_ok;
    }
    event_fd = ok;

    trampoline = trampoline_new(event_fd, userdata);
    if (trampoline == NULL) {
        ok = ENOMEM;
        goto fail_close_event_fd;
    }

    trampoline->task_callback = callback;

    ok = epoll_ctl(
        loop->epoll_fd,
        EPOLL_CTL_ADD,
        event_fd,
        &(struct epoll_event) {
            .events = (EPOLLIN | EPOLLONESHOT),
            .data.ptr = trampoline
        }
    );
    if (ok < 0) {
        ok = errno;
        LOG_EVENT_LOOP_ERROR("Couldn't add armed timer to event loop. epoll_ctl: %s\n", strerror(errno));
        goto fail_free_trampoline;
    }

    return 0;


    fail_free_trampoline:
    free(trampoline);

    fail_close_event_fd:
    close(event_fd);

    fail_return_ok:
    return ok;
}

int event_loop_post_task_with_time(
    struct event_loop *loop,
    uint64_t target_time_usec,
    event_loop_timed_task_callback_t callback,
    void *userdata
) {
    struct trampoline *trampoline;
    int ok, timer_fd;

    ok = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (ok < 0) {
        ok = errno;
        LOG_EVENT_LOOP_ERROR("Couldn't create timer. timerfd_create: %s\n", strerror(errno));
        goto fail_return_ok;
    }
    timer_fd = ok;

    trampoline = trampoline_new(timer_fd, userdata);
    if (trampoline == NULL) {
        ok = ENOMEM;
        goto fail_close_timer_fd;
    }

    trampoline->timed_task_callback = callback;

    ok = timerfd_settime(
        timer_fd,
        TFD_TIMER_ABSTIME,
        &(const struct itimerspec) {
            .it_value = {
                .tv_sec = target_time_usec / 1000000,
                .tv_nsec = (target_time_usec % 1000000) * 1000
            },
            .it_interval = {
                .tv_sec = 0,
                .tv_nsec = 0
            }
        },
        NULL
    );
    if (ok < 0) {
        ok = errno;
        LOG_EVENT_LOOP_ERROR("Couldn't arm timer. timerfd_settime: %s\n", strerror(errno));
        goto fail_free_trampoline;
    }

    ok = epoll_ctl(
        loop->epoll_fd,
        EPOLL_CTL_ADD,
        timer_fd,
        &(struct epoll_event) {
            .events = EPOLLIN | EPOLLONESHOT,
            .data.ptr = trampoline
        }
    );
    if (ok < 0) {
        ok = errno;
        LOG_EVENT_LOOP_ERROR("Couldn't add armed timer to event loop. epoll_ctl: %s\n", strerror(errno));
        goto fail_free_trampoline;
    }

    return 0;


    fail_free_trampoline:
    free(trampoline);

    fail_close_timer_fd:
    close(timer_fd);

    fail_return_ok:
    return ok;
}

int event_loop_add_io(
    struct event_loop *event_loop,
    int fd,
    int events,
    event_loop_io_callback_t callback,
    void *userdata
) {
    struct trampoline *trampoline;
    int ok;

    trampoline = trampoline_new(fd, userdata);
    if (trampoline == NULL) {
        ok = ENOMEM;
        goto fail_return_ok;
    }
    trampoline->events = events;
    trampoline->io_callback = callback;

    ok = epoll_ctl(
        event_loop->epoll_fd,
        EPOLL_CTL_ADD,
        fd,
        &(struct epoll_event) {
            .events = events,
            .data.ptr = userdata
        }
    );
    if (ok < 0) {
        ok = errno;
        LOG_EVENT_LOOP_ERROR("Couldn't add fd to epoll instance. epoll_ctl: %s\n", strerror(errno));
        goto fail_free_trampoline;
    }

    return 0;


    fail_free_trampoline:
    free(trampoline);

    fail_return_ok:
    return ok;
}

static int process(
    struct event_loop *loop,
    int timeout
) {
    struct epoll_event events[64];
    struct trampoline *trampoline;
    size_t size_events = 64;
    int ok, n_events;

    if (loop->should_exit) {
        return EINVAL;
    }

    while (loop->should_exit == false) {
        ok = epoll_wait(loop->epoll_fd, events, size_events, timeout);
        if ((ok < 0) && (errno == EINTR)) {
            // just a signal, retry
            continue;
        }

        assert(ok >= 0); // likely a programming error
        n_events = ok;

        for (int i = 0; i < n_events; i++) {
            trampoline = events[i].data.ptr;

            DEBUG_ASSERT(trampoline != NULL);

            bool close_fd;
            bool unlisten;
            if (trampoline->io_callback != NULL) {
                if (trampoline->events & events[i].events) {
                    bool keep = trampoline->io_callback(trampoline->fd, events[i].events, trampoline->userdata);
                    if (keep == false) {
                        close_fd = false;
                        unlisten = true;
                    } else {
                        close_fd = false;
                        unlisten = false;
                    }
                }
            } else if (events[i].events & EPOLLIN) {
                close_fd = true;
                unlisten = true;
                
                if (trampoline->task_callback != NULL) {
                    trampoline->task_callback(trampoline->userdata);
                } else if (trampoline->timed_task_callback != NULL) {
                    trampoline->timed_task_callback(trampoline->userdata);
                } else if (trampoline->fd == loop->schedule_exit_fd) {
                    loop->should_exit = true;
                } else {
                    assert(false);
                }
            }

            if (unlisten) {
                ok = epoll_ctl(loop->epoll_fd, EPOLL_CTL_DEL, trampoline->fd, NULL);
                assert(ok >= 0);
            }

            if (close_fd) {
                ok = close(trampoline->fd);
                assert(ok >= 0);
            }

            if (unlisten) {
                free(trampoline);
            }
        }
    }

    return 0;
}

int event_loop_process(
    struct event_loop *loop
) {
    return process(loop, -1);
}

int event_loop_process_pending(
    struct event_loop *loop,
    bool *should_exit
) {
    int ok = process(loop, 0);
    
    if (should_exit != NULL) {
        *should_exit = loop->should_exit;
    }

    return ok;
}
