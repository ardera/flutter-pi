#include <collection.h>

struct evloop;

struct evloop *evloop_new();

void evloop_destroy(struct evloop *loop);

DECLARE_REF_OPS(evloop)

int evloop_get_fd_locked(struct evloop *loop);

int evloop_get_fd(struct evloop *loop);

int evloop_run(struct evloop *loop);

int evloop_schedule_exit_locked(struct evloop *loop);

int evloop_schedule_exit(struct evloop *loop);

int evloop_post_task_locked(struct evloop *loop, void_callback_t callback, void *userdata);

int evloop_post_task(struct evloop *loop, void_callback_t callback, void *userdata);

int evloop_post_delayed_task_locked(
    struct evloop *loop,
    void_callback_t callback,
    void *userdata,
    uint64_t target_time_usec
);

int evloop_post_delayed_task(struct evloop *loop, void_callback_t callback, void *userdata, uint64_t target_time_usec);

struct evsrc;
void evsrc_destroy_locked(struct evsrc *src);
void evsrc_destroy(struct evsrc *src);

enum event_handler_return {
    kNoAction_EventHandlerReturn,
    kRemoveSrc_EventHandlerReturn
};

typedef enum event_handler_return (*evloop_io_handler_t)(int fd, uint32_t revents, void *userdata);

struct evsrc *evloop_add_io_locked(
    struct evloop *loop,
    int fd,
    uint32_t events,
    evloop_io_handler_t callback,
    void *userdata
);

struct evsrc *evloop_add_io(
    struct evloop *loop,
    int fd,
    uint32_t events,
    evloop_io_handler_t callback,
    void *userdata
);

struct evthread;

struct evthread *evthread_start();

struct evloop *evthread_get_evloop(struct evthread *thread);

void evthread_join(struct evthread *thread);
