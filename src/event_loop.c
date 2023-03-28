#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <semaphore.h>

#include <systemd/sd-event.h>

#include <collection.h>
#include <event_loop.h>

struct evloop {
    refcount_t n_refs;
    pthread_mutex_t mutex;
    sd_event *sdloop;
    int wakeup_fd;
    pthread_t owning_thread;
};

FILE_DESCR("event_loop.c")

DEFINE_STATIC_LOCK_OPS(evloop, mutex)

static int on_wakeup_event_loop(sd_event_source *s, int fd, uint32_t revents, void *userdata) {
    uint8_t buffer[8];
    int ok;

    (void) s;
    (void) revents;
    (void) userdata;

    ok = read(fd, buffer, 8);
    if (ok < 0) {
        ok = errno;
        LOG_ERROR("Could not read eventloop wakeup userdata. read: %s\n", strerror(ok));
        return ok;
    }

    return 0;
}

struct evloop *evloop_new() {
    struct evloop *loop;
    sd_event *sdloop;
    int ok, wakeup_fd;

    loop = malloc(sizeof *loop);
    if (loop == NULL) {
        return NULL;
    }

    ok = sd_event_new(&sdloop);
    if (ok != 0) {
        LOG_ERROR("Couldn't create libsystemd event loop. sd_event_new: %s\n", strerror(-ok));
        goto fail_free_loop;
    }

    wakeup_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (wakeup_fd < 0) {
        LOG_ERROR("Could not create fd for waking up the main loop. eventfd: %s\n", strerror(errno));
        goto fail_unref_sdloop;
    }

    ok = sd_event_add_io(
        sdloop,
        NULL,
        wakeup_fd,
        EPOLLIN,
        on_wakeup_event_loop,
        NULL
    );
    if (ok < 0) {
        LOG_ERROR("Error adding wakeup callback to main loop. sd_event_add_io: %s\n", strerror(-ok));
        goto fail_unref_sdloop;
    }

    loop->n_refs = REFCOUNT_INIT_1;
    pthread_mutex_init(&loop->mutex, get_default_mutex_attrs());
    loop->sdloop = sdloop;
    loop->wakeup_fd = wakeup_fd;
    loop->owning_thread = pthread_self();
    return loop;


    fail_unref_sdloop:
    sd_event_unref(sdloop);

    fail_free_loop:
    free(loop);
    return NULL;
}

void evloop_destroy(struct evloop *loop) {
    sd_event_unref(loop->sdloop);
    close(loop->wakeup_fd);
    pthread_mutex_destroy(&loop->mutex);
    free(loop);
}

DEFINE_REF_OPS(evloop, n_refs)

int evloop_get_fd_locked(struct evloop *loop) {
    DEBUG_ASSERT_NOT_NULL(loop);
    DEBUG_ASSERT_MUTEX_LOCKED(loop->mutex);
    return sd_event_get_fd(loop->sdloop);    
}

int evloop_get_fd(struct evloop *loop) {
    int result;

    DEBUG_ASSERT_NOT_NULL(loop);

    evloop_lock(loop);
    result = evloop_get_fd_locked(loop);
    evloop_unlock(loop);

    return result;
}

int evloop_run(struct evloop *loop) {
    int evloop_fd;
    int ok;

    DEBUG_ASSERT_NOT_NULL(loop);

    evloop_fd = evloop_get_fd(loop);

    {
        fd_set rfds, wfds, xfds;
        int state;
        FD_ZERO(&rfds);
        FD_ZERO(&wfds);
        FD_ZERO(&xfds);
        FD_SET(evloop_fd, &rfds);
        FD_SET(evloop_fd, &wfds);
        FD_SET(evloop_fd, &xfds);

        const fd_set const_fds = rfds;

        evloop_lock(loop);

        do {
            state = sd_event_get_state(loop->sdloop);
            switch (state) {
                case SD_EVENT_INITIAL:
                    ok = sd_event_prepare(loop->sdloop);
                    if (ok < 0) {
                        ok = -ok;
                        LOG_ERROR("Could not prepare event loop. sd_event_prepare: %s\n", strerror(ok));
                        goto fail_unlock;
                    }

                    break;
                case SD_EVENT_ARMED:
                    evloop_unlock(loop);

                    do {
                        rfds = const_fds;
                        wfds = const_fds;
                        xfds = const_fds;
                        ok = select(evloop_fd + 1, &rfds, &wfds, &xfds, NULL);
                        if ((ok < 0) && (errno != EINTR)) {
                            ok = errno;
                            LOG_ERROR("Could not wait for event loop events. select: %s\n", strerror(ok));
                            goto fail;
                        }
                    } while ((ok < 0) && (errno == EINTR));

                    evloop_lock(loop);

                    ok = sd_event_wait(loop->sdloop, 0);
                    if (ok < 0) {
                        ok = -ok;
                        LOG_ERROR("Could not check for event loop events. sd_event_wait: %s\n", strerror(ok));
                        goto fail_unlock;
                    }

                    break;
                case SD_EVENT_PENDING:
                    ok = sd_event_dispatch(loop->sdloop);
                    if (ok < 0) {
                        ok = -ok;
                        LOG_ERROR("Could not dispatch event loop events. sd_event_dispatch: %s\n", strerror(ok));
                        goto fail_unlock;
                    }

                    break;
                case SD_EVENT_FINISHED:
                    break;
                default:
                    UNREACHABLE();
            }
        } while (state != SD_EVENT_FINISHED);

        evloop_unlock(loop);
    }

    return 0;


    fail_unlock:
    evloop_unlock(loop);

    fail:
    return ok;
}

static int wakeup_sdloop(struct evloop *loop) {
    int ok;

    ok = write(loop->wakeup_fd, (uint8_t[8]) {0, 0, 0, 0, 0, 0, 0, 1}, 8);
    if (ok < 0) {
        ok = errno;
        LOG_ERROR("Error waking up event loop. write: %s\n", strerror(ok));
        return ok;
    }

    return 0;
}

int evloop_schedule_exit_locked(struct evloop *loop) {
    int ok;

    DEBUG_ASSERT_NOT_NULL(loop);
    DEBUG_ASSERT_MUTEX_LOCKED(loop->mutex);

    ok = sd_event_exit(loop->sdloop, 0);
    if (ok != 0) {
        LOG_ERROR("Couldn't schedule event loop exit. sd_event_exit: %s\n", strerror(-ok));
        return -ok;
    }

    return 0;
}

int evloop_schedule_exit(struct evloop *loop) {
    int ok;

    DEBUG_ASSERT_NOT_NULL(loop);

    evloop_lock(loop);
    ok = evloop_schedule_exit_locked(loop);
    evloop_unlock(loop);

    wakeup_sdloop(loop);

    return ok;
}

struct task {
    void_callback_t callback;
    void *userdata;
};

static int on_execute_task(
    sd_event_source *s,
    void *userdata
) {
    struct task *task;
    int ok;

    DEBUG_ASSERT_NOT_NULL(userdata);
    task = userdata;

    task->callback(task->userdata);
    free(task);

    sd_event_source_set_enabled(s, SD_EVENT_OFF);
    sd_event_source_unref(s);
    return 0;
}

int evloop_post_task_locked(struct evloop *loop, void_callback_t callback, void *userdata) {
    sd_event_source *src;
    struct task *task;
    int ok;

    DEBUG_ASSERT_NOT_NULL(loop);
    DEBUG_ASSERT_NOT_NULL(callback);
    DEBUG_ASSERT_MUTEX_LOCKED(loop->mutex);

    task = malloc(sizeof *task);
    if (task == NULL) {
        return ENOMEM;
    }

    task->callback = callback;
    task->userdata = userdata;

    ok = sd_event_add_defer(
        loop->sdloop,
        &src,
        on_execute_task,
        task
    );
    if (ok < 0) {
        LOG_ERROR("Error adding task to event loop. sd_event_add_defer: %s\n", strerror(-ok));
        ok = -ok;
        goto fail_free_task;
    }

    return 0;


    fail_remove_src:
    sd_event_source_disable_unref(src);

    fail_free_task:
    free(task);
    return ok;
}

int evloop_post_task(struct evloop *loop, void_callback_t callback, void *userdata) {
    int ok;

    DEBUG_ASSERT_NOT_NULL(loop);
    DEBUG_ASSERT_NOT_NULL(callback);

    evloop_lock(loop);
    ok = evloop_post_task_locked(loop, callback, userdata);
    evloop_unlock(loop);

    wakeup_sdloop(loop);

    return ok;
}

static int on_execute_delayed_task(
    sd_event_source *s,
    uint64_t usec,
    void *userdata
) {
    struct task *task;

    DEBUG_ASSERT_NOT_NULL(userdata);
    task = userdata;
    (void) usec;

    task->callback(task->userdata);
    free(task);

    sd_event_source_disable_unref(s);
    return 0;
}

int evloop_post_delayed_task_locked(
    struct evloop *loop,
    void_callback_t callback,
    void *userdata,
    uint64_t target_time_usec
) {
    sd_event_source *src;
    struct task *task;
    int ok;

    DEBUG_ASSERT_NOT_NULL(loop);
    DEBUG_ASSERT_NOT_NULL(callback);
    DEBUG_ASSERT_MUTEX_LOCKED(loop->mutex);

    task = malloc(sizeof *task);
    if (task == NULL) {
        return ENOMEM;
    }

    task->callback = callback;
    task->userdata = userdata;

    ok = sd_event_add_time(
        loop->sdloop,
        &src,
        CLOCK_MONOTONIC,
        target_time_usec,
        1,
        on_execute_delayed_task,
        task
    );
    if (ok < 0) {
        LOG_ERROR("Error posting platform task to main loop. sd_event_add_time: %s\n", strerror(-ok));
        ok = -ok;
        goto fail_free_task;
    }

    return 0;

    fail_remove_src:
    sd_event_source_disable_unref(src);

    fail_free_task:
    free(task);
    return ok;
}

int evloop_post_delayed_task(struct evloop *loop, void_callback_t callback, void *userdata, uint64_t target_time_usec) {
    int ok;

    DEBUG_ASSERT_NOT_NULL(loop);
    DEBUG_ASSERT_NOT_NULL(callback);

    evloop_lock(loop);
    ok = evloop_post_delayed_task_locked(loop, callback, userdata, target_time_usec);
    evloop_unlock(loop);

    wakeup_sdloop(loop);

    return ok;
}


struct evsrc {
    struct evloop *loop;
    sd_event_source *sdsrc;

    evloop_io_handler_t io_callback;
    void *userdata;
};

void evsrc_destroy_locked(struct evsrc *src) {
    DEBUG_ASSERT_MUTEX_LOCKED(src->loop->mutex);
    sd_event_source_disable_unref(src->sdsrc);
    evloop_unref(src->loop);
    free(src);
}

void evsrc_destroy(struct evsrc *src) {
    evloop_lock(src->loop);
    sd_event_source_disable_unref(src->sdsrc);
    evloop_unlock(src->loop);

    evloop_unref(src->loop);
    free(src);
}

int on_io_src_ready(sd_event_source *s, int fd, uint32_t revents, void *userdata) {
    enum event_handler_return handler_return;
    struct evsrc *evsrc;

    DEBUG_ASSERT_NOT_NULL(s);
    DEBUG_ASSERT_NOT_NULL(userdata);
    evsrc = userdata;

    handler_return = evsrc->io_callback(fd, revents, evsrc->userdata);
    if (handler_return == kRemoveSrc_EventHandlerReturn) {
        evsrc_destroy_locked(evsrc);
        return -1;
    }

    return 0;
}

struct evsrc *evloop_add_io_locked(
    struct evloop *loop,
    int fd,
    uint32_t events,
    evloop_io_handler_t callback,
    void *userdata
) {
    sd_event_source *src;
    struct evsrc *evsrc;
    int ok;

    evsrc = malloc(sizeof *evsrc);
    if (evsrc == NULL) {
        return NULL;
    }

    DEBUG_ASSERT_NOT_NULL(loop);
    DEBUG_ASSERT_MUTEX_LOCKED(loop->mutex);

    evsrc->io_callback = callback;
    evsrc->userdata = userdata;

    ok = sd_event_add_io(
        loop->sdloop,
        &src,
        fd,
        events,
        on_io_src_ready,
        evsrc
    );
    if (ok < 0) {
        LOG_ERROR("Could not add IO callback to event loop. sd_event_add_io: %s\n", strerror(-ok));
        free(evsrc);
        return NULL;
    }

    evsrc->loop = evloop_ref(loop);
    evsrc->sdsrc = sd_event_source_ref(src);
    return evsrc;
}

struct evsrc *evloop_add_io(
    struct evloop *loop,
    int fd,
    uint32_t events,
    evloop_io_handler_t callback,
    void *userdata
) {
    struct evsrc *src;

    DEBUG_ASSERT_NOT_NULL(loop);
    DEBUG_ASSERT_NOT_NULL(callback);

    evloop_lock(loop);
    src = evloop_add_io_locked(loop, fd, events, callback, userdata);
    evloop_unlock(loop);

    wakeup_sdloop(loop);

    return src;
}

struct evthread {
    struct evloop *loop;
    pthread_t thread;
};

struct evthread_startup_args {
    struct evthread *evthread;
    sem_t initialization_done;
    bool initialization_success;
};

static void *evthread_entry(void *userdata) {
    struct evthread *evthread;
    struct evloop *evloop;
    int ok;

    // initialization.
    {
        struct evthread_startup_args *args;
        DEBUG_ASSERT_NOT_NULL(userdata);
        args = userdata;

        evthread = malloc(sizeof *evthread);
        if (evthread == NULL) {
            goto fail_post_semaphore;
        }

        evloop = evloop_new();
        if (evloop == NULL) {
            goto fail_free_evthread;
        }

        evthread->loop = evloop;
        evthread->thread = pthread_self();
        
        args->initialization_success = true;
        sem_post(&args->initialization_done);
        goto init_done;
        
        // error handling
        fail_free_evthread:
        free(evthread);

        fail_post_semaphore:
        args->initialization_success = false;
        sem_post(&args->initialization_done);
        return NULL;
    }

    init_done:
    evloop_run(evloop);
    sd_event_unrefp(&evthread->loop);
    return NULL;
}

struct evthread *evthread_start() {
    struct evthread_startup_args *args;
    struct evthread *evthread;
    pthread_t tid;
    int ok;

    args = malloc(sizeof *args);
    if (args == NULL) {
        return NULL;
    }

    args->evthread = NULL;
    args->initialization_success = false;

    ok = sem_init(&args->initialization_done, 0, 0);
    if (ok != 0) {
        goto fail_free_args;
    }

    ok = pthread_create(&tid, NULL, evthread_entry, args);
    if (ok != 0) {
        LOG_ERROR("Could not create new event thread. pthread_create: %s\n", strerror(-ok));
        goto fail_deinit_semaphore;
    }

    ok = sem_wait(&args->initialization_done);
    if (ok != 0) {
        LOG_ERROR("Couldn't wait for event thread initialization finish. sem_wait: %s\n", strerror(-ok));
        goto fail_cancel_thread;
    }

    if (!args->initialization_success) {
        ok = pthread_join(tid, NULL);
        if (ok != 0) {
            LOG_ERROR("Couldn't wait for event thread to finish. pthread_join: %s\n", strerror(-ok));
            goto fail_cancel_thread;
        }

        sem_destroy(&args->initialization_done);
        free(args);
        return NULL;
    }

    sem_destroy(&args->initialization_done);
    
    DEBUG_ASSERT_NOT_NULL(args->evthread);
    evthread = args->evthread;
    free(args);
    
    return evthread;


    fail_cancel_thread:
    pthread_cancel(tid);

    fail_deinit_semaphore:
    sem_destroy(&args->initialization_done);

    fail_free_args:
    free(args);
    return NULL;
}

struct evloop *evthread_get_evloop(struct evthread *thread) {
    return thread->loop;
}

void evthread_stop(struct evthread *thread) {
    evloop_schedule_exit(thread->loop);
    pthread_join(thread->thread, NULL);
    free(thread);
}
