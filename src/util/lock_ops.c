#include <pthread.h>

static pthread_mutexattr_t default_mutex_attrs;

static void init_default_mutex_attrs(void) {
    pthread_mutexattr_init(&default_mutex_attrs);
#ifdef DEBUG
    pthread_mutexattr_settype(&default_mutex_attrs, PTHREAD_MUTEX_ERRORCHECK_NP);
#endif
}

const pthread_mutexattr_t *get_default_mutex_attrs(void) {
    static pthread_once_t init_once_ctl = PTHREAD_ONCE_INIT;

    pthread_once(&init_once_ctl, init_default_mutex_attrs);

    return &default_mutex_attrs;
}

static pthread_mutexattr_t default_recursive_mutex_attrs;

static void init_default_recursive_mutex_attrs(void) {
    pthread_mutexattr_init(&default_recursive_mutex_attrs);

    // PTHREAD_MUTEX_ERRORCHECK_NP does not work with PTHREAD_MUTEX_RECURSIVE_NP.
    pthread_mutexattr_settype(&default_recursive_mutex_attrs, PTHREAD_MUTEX_RECURSIVE_NP);
}

const pthread_mutexattr_t *get_default_recursive_mutex_attrs(void) {
    static pthread_once_t init_once_ctl = PTHREAD_ONCE_INIT;

    pthread_once(&init_once_ctl, init_default_recursive_mutex_attrs);

    return &default_recursive_mutex_attrs;
}
