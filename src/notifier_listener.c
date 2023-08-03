#include "notifier_listener.h"

#include <stdatomic.h>
#include <stdbool.h>

struct listener {
    struct list_head entry;
    listener_cb_t notify;
    void_callback_t destroy;
    void *userdata;
};

#define for_each_listener_in_notifier(_notifier, _listener) \
    list_for_each_entry_safe(struct listener, _listener, &(_notifier).listeners, entry)

static struct listener *listener_new(listener_cb_t notify, void_callback_t destroy, void *userdata);
static void listener_destroy(struct listener *listener);
static enum listener_return listener_notify(struct listener *listener, void *arg);

int change_notifier_init(struct notifier *notifier) {
    int ok;

    ok = pthread_mutex_init(&notifier->mutex, NULL);
    if (ok != 0) {
        return ok;
    }

    list_inithead(&notifier->listeners);
    notifier->is_value_notifier = false;
    notifier->state = NULL;
    notifier->value_destroy_callback = NULL;
    return 0;
}

int value_notifier_init(struct notifier *notifier, void *initial_value, void_callback_t value_destroy_callback) {
    int ok;

    ok = pthread_mutex_init(&notifier->mutex, NULL);
    if (ok != 0) {
        return ok;
    }

    list_inithead(&notifier->listeners);
    notifier->is_value_notifier = true;
    notifier->state = initial_value;
    notifier->value_destroy_callback = value_destroy_callback;

    return 0;
}

struct notifier *change_notifier_new() {
    struct notifier *n;
    int ok;

    n = malloc(sizeof *n);
    if (n == NULL) {
        return NULL;
    }

    ok = change_notifier_init(n);
    if (ok != 0) {
        free(n);
        return NULL;
    }

    return n;
}

struct notifier *value_notifier_new(void *initial_value, void_callback_t value_destroy_callback) {
    struct notifier *n;
    int ok;

    n = malloc(sizeof *n);
    if (n == NULL) {
        return NULL;
    }

    ok = value_notifier_init(n, initial_value, value_destroy_callback);
    if (ok != 0) {
        free(n);
        return NULL;
    }

    return n;
}

void notifier_deinit(struct notifier *notifier) {
    for_each_listener_in_notifier(*notifier, l) {
        listener_destroy(l);
    }

    pthread_mutex_destroy(&notifier->mutex);
    ASSERT_MSG(list_is_empty(&notifier->listeners), "Listener list was not empty after removing all listeners");
    if (notifier->value_destroy_callback != NULL) {
        notifier->value_destroy_callback(notifier->state);
    }
}

void notifier_destroy(struct notifier *notifier) {
    notifier_deinit(notifier);
    free(notifier);
}

DEFINE_LOCK_OPS(notifier, mutex)

struct listener *notifier_listen(struct notifier *notifier, listener_cb_t notify, void_callback_t destroy, void *userdata) {
    enum listener_return r;
    struct listener *l;

    l = listener_new(notify, destroy, userdata);
    if (l == NULL) {
        return NULL;
    }

    r = listener_notify(l, notifier->state);
    if (r == kUnlisten) {
        listener_destroy(l);
        return NULL;
    }

    notifier_lock(notifier);

    list_add(&l->entry, &notifier->listeners);

    notifier_unlock(notifier);

    return l;
}

int notifier_unlisten(struct notifier *notifier, struct listener *listener) {
    notifier_lock(notifier);

    listener_destroy(listener);

    notifier_unlock(notifier);

    return 0;
}

void notifier_notify(struct notifier *notifier, void *arg) {
    enum listener_return r;

    notifier_lock(notifier);

    if (notifier->value_destroy_callback != NULL) {
        notifier->value_destroy_callback(notifier->state);
    }
    notifier->state = arg;

    for_each_listener_in_notifier(*notifier, l) {
        r = listener_notify(l, arg);
        if (r == kUnlisten) {
            listener_destroy(l);
        }
    }

    notifier_unlock(notifier);
}

static struct listener *listener_new(listener_cb_t notify, void_callback_t destroy, void *userdata) {
    struct listener *listener;

    listener = malloc(sizeof *listener);
    if (listener == NULL) {
        return NULL;
    }

    listener->entry = (struct list_head){ NULL, NULL };
    listener->notify = notify;
    listener->destroy = destroy;
    listener->userdata = userdata;

    return listener;
}

static void listener_destroy(struct listener *listener) {
    if (listener->destroy != NULL) {
        listener->destroy(listener->userdata);
    }

    if (list_is_linked(&listener->entry)) {
        list_del(&listener->entry);
    }

    free(listener);
}

static enum listener_return listener_notify(struct listener *listener, void *arg) {
    return listener->notify(arg, listener->userdata);
}
