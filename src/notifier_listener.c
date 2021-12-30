#include <stdbool.h>
#include <stdatomic.h>

#include <notifier_listener.h>

struct listener {
    listener_cb_t notify;
    void_callback_t destroy;
    void *userdata;
};

#define for_each_listener_in_notifier(notifier, listener) for_each_pointer_in_pset(&notifier->listeners, listener)

static struct listener *listener_new(listener_cb_t notify, void_callback_t destroy, void *userdata);
static void listener_destroy(struct listener *listener);
static enum listener_return listener_notify(struct listener *listener, void *arg);

int change_notifier_init(struct notifier *notifier) {
    int ok;

    ok = pthread_mutex_init(&notifier->mutex, NULL);
    if (ok != 0) {
        return ok;
    }

    ok = pset_init(&notifier->listeners, PSET_DEFAULT_MAX_SIZE);
    if (ok != 0) {
        return ok;
    }

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

    ok = pset_init(&notifier->listeners, PSET_DEFAULT_MAX_SIZE);
    if (ok != 0) {
        return ok;
    }

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
    struct listener *l;
    
    for_each_listener_in_notifier(notifier, l) {
        listener_destroy(l);
    }
    pthread_mutex_destroy(&notifier->mutex);
    pset_deinit(&notifier->listeners);
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
    int ok;
    
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

    ok = pset_put(&notifier->listeners, l);

    notifier_unlock(notifier);

    if (ok != 0) {
        listener_destroy(l);
        return NULL;
    }

    return l;
}

int notifier_unlisten(struct notifier *notifier, struct listener *listener) {
    int ok;

    notifier_lock(notifier);

    ok = pset_remove(&notifier->listeners, listener);

    notifier_unlock(notifier);

    if (ok == 0) {
        listener_destroy(listener);
    }

    return ok;
}

void notifier_notify(struct notifier *notifier, void *arg) {
    enum listener_return r;
    struct listener *l, *last_kept;
    int ok;

    notifier_lock(notifier);

    if (notifier->value_destroy_callback != NULL) {
        notifier->value_destroy_callback(notifier->state);
    }
    notifier->state = arg;

    last_kept = NULL;
    for_each_listener_in_notifier(notifier, l) {
        r = listener_notify(l, arg);
        if (r == kUnlisten) {
            ok = pset_remove(&notifier->listeners, l);
            DEBUG_ASSERT(ok == 0);

            listener_destroy(l);

            l = last_kept;
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

    listener->notify = notify;
    listener->destroy = destroy;
    listener->userdata = userdata;

    return listener;
}

static void listener_destroy(struct listener *listener) {
    if (listener->destroy != NULL) {
        listener->destroy(listener->userdata);
    }
    free(listener);
}

static enum listener_return listener_notify(struct listener *listener, void *arg) {
    return listener->notify(arg, listener->userdata);
}

