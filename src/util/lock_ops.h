#ifndef _UTIL_LOCKOPS_H
#define _UTIL_LOCKOPS_H

#include <pthread.h>

#include "asserts.h"

#define DECLARE_LOCK_OPS(obj_name)                     \
    UNUSED void obj_name##_lock(struct obj_name *obj); \
    UNUSED void obj_name##_unlock(struct obj_name *obj);

#define DEFINE_LOCK_OPS(obj_name, mutex_member_name)        \
    UNUSED void obj_name##_lock(struct obj_name *obj) {     \
        int ok;                                             \
        ok = pthread_mutex_lock(&obj->mutex_member_name);   \
        ASSERT_EQUALS_MSG(ok, 0, "Error locking mutex.");   \
        (void) ok;                                          \
    }                                                       \
    UNUSED void obj_name##_unlock(struct obj_name *obj) {   \
        int ok;                                             \
        ok = pthread_mutex_unlock(&obj->mutex_member_name); \
        ASSERT_EQUALS_MSG(ok, 0, "Error unlocking mutex."); \
        (void) ok;                                          \
    }

#define DEFINE_STATIC_LOCK_OPS(obj_name, mutex_member_name)      \
    UNUSED static void obj_name##_lock(struct obj_name *obj) {   \
        int ok;                                                  \
        ok = pthread_mutex_lock(&obj->mutex_member_name);        \
        ASSERT_EQUALS_MSG(ok, 0, "Error locking mutex.");        \
        (void) ok;                                               \
    }                                                            \
    UNUSED static void obj_name##_unlock(struct obj_name *obj) { \
        int ok;                                                  \
        ok = pthread_mutex_unlock(&obj->mutex_member_name);      \
        ASSERT_EQUALS_MSG(ok, 0, "Error unlocking mutex.");      \
        (void) ok;                                               \
    }

#define DEFINE_INLINE_LOCK_OPS(obj_name, mutex_member_name)             \
    UNUSED static inline void obj_name##_lock(struct obj_name *obj) {   \
        int ok;                                                         \
        ok = pthread_mutex_lock(&obj->mutex_member_name);               \
        ASSERT_EQUALS_MSG(ok, 0, "Error locking mutex.");               \
        (void) ok;                                                      \
    }                                                                   \
    UNUSED static inline void obj_name##_unlock(struct obj_name *obj) { \
        int ok;                                                         \
        ok = pthread_mutex_unlock(&obj->mutex_member_name);             \
        ASSERT_EQUALS_MSG(ok, 0, "Error unlocking mutex.");             \
        (void) ok;                                                      \
    }

#endif // _UTIL_LOCKOPS_H
