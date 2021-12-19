#include <unistd.h>
#include <errno.h>
#include <sys/eventfd.h>

#include <event_loop.h>
#include <collection.h>

#include <systemd/sd-event.h>

struct sd_event_source_generic {
    sd_event *event;
    sd_event_source *real_source;
    refcount_t n_refs;
    int event_fd;
    sd_event_generic_handler_t handler;
    void *userdata;
    void *argument;
};


static int on_eventfd_ready(sd_event_source *s, int fd, uint32_t revents, void *userdata) {
    sd_event_source_generic *t;
    uint8_t buf[8];
    int ok;
    
    (void) s;
    (void) fd;
    (void) revents;

    t = userdata;

    ok = read(fd, buf, 8);
    if (ok < 0) {
        fprintf(stderr, "[event loop] Couldn't clear generic event source.\n");
    }

    /// TODO: What to do here with our allocated memory if this returns sub-zero?
    return t->handler(t, t->argument, t->userdata);
}

static sd_event_source_generic *sd_event_source_generic_new(sd_event *event, sd_event_generic_handler_t handler, void *userdata) {
    sd_event_source_generic *source;
    sd_event_source *s;
    int fd, ok;

    source = malloc(sizeof *source);
    if (source == NULL) {
        return NULL;
    }

    fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (fd < 0) {
        free(source);
        return NULL;
    }

    ok = sd_event_add_io(
        event,
        &s,
        fd,
        EPOLLIN,
        on_eventfd_ready,
        source
    );
    if (ok < 0) {
        close(fd);
        free(source);
        return NULL;
    }
    
    source->event = sd_event_ref(event);
    source->real_source = s;
    source->n_refs = REFCOUNT_INIT_0;
    source->event_fd = fd;
    source->handler = handler;
    source->userdata = userdata;
    return source;
}

static void sd_event_source_generic_destroy(sd_event_source_generic *source) {
    sd_event_unref(source->event);
    close(source->event_fd);
    free(source);
}

sd_event_source_generic *sd_event_add_generic(struct sd_event *event, sd_event_generic_handler_t handler, void *userdata) {
    sd_event_source_generic *source;
    source = sd_event_source_generic_new(event, handler, userdata);
    return source;
}

DEFINE_REF_OPS(sd_event_source_generic, n_refs)

void sd_event_source_generic_signal(sd_event_source_generic *source, void *argument) {
    source->argument = argument;
    write(source->event_fd, (uint8_t[8]) {0, 0, 0, 0, 0, 0, 0, 1}, 8);
}

void *sd_event_source_generic_set_userdata(sd_event_source_generic *source, void *userdata) {
    void *before = source->userdata;
    source->userdata = userdata;
    return before;
}

void *sd_event_source_generic_get_userdata(sd_event_source_generic *source) {
    return source->userdata;
}