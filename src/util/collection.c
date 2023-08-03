#include "util/collection.h"

static pthread_mutexattr_t default_mutex_attrs;

static void init_default_mutex_attrs() {
    pthread_mutexattr_init(&default_mutex_attrs);
#ifdef DEBUG
    pthread_mutexattr_settype(&default_mutex_attrs, PTHREAD_MUTEX_ERRORCHECK);
#endif
}

const pthread_mutexattr_t *get_default_mutex_attrs() {
    static pthread_once_t init_once_ctl = PTHREAD_ONCE_INIT;

    pthread_once(&init_once_ctl, init_default_mutex_attrs);

    return &default_mutex_attrs;
}
