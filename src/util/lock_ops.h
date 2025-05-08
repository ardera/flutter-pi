// SPDX-License-Identifier: MIT
/*
 * Lock Ops - Macros for defining locking operations for a struct with
 * sane defaults.
 * 
 * Copyright (c) 2023, Hannes Winkler <hanneswinkler2000@web.de>
 */

#ifndef _FLUTTERPI_SRC_UTIL_LOCK_OPS_H
#define _FLUTTERPI_SRC_UTIL_LOCK_OPS_H

#include <pthread.h>
#include <errno.h>

#include "macros.h"
#include "asserts.h"

// Code based on the template from: https://clang.llvm.org/docs/ThreadSafetyAnalysis.html

// Enable thread safety attributes only with clang.
// The attributes can be safely erased when compiling with other compilers.
#if defined(__clang__) && (!defined(SWIG))
#define THREAD_ANNOTATION_ATTRIBUTE__(x)   __attribute__((x))
#else
#define THREAD_ANNOTATION_ATTRIBUTE__(x)   // no-op
#endif

#define CAPABILITY(x) \
	THREAD_ANNOTATION_ATTRIBUTE__(capability(x))

#define SCOPED_CAPABILITY \
	THREAD_ANNOTATION_ATTRIBUTE__(scoped_lockable)

#define GUARDED_BY(x) \
	THREAD_ANNOTATION_ATTRIBUTE__(guarded_by(x))

#define PT_GUARDED_BY(x) \
	THREAD_ANNOTATION_ATTRIBUTE__(pt_guarded_by(x))

#define ACQUIRED_BEFORE(...) \
	THREAD_ANNOTATION_ATTRIBUTE__(acquired_before(__VA_ARGS__))

#define ACQUIRED_AFTER(...) \
	THREAD_ANNOTATION_ATTRIBUTE__(acquired_after(__VA_ARGS__))

#define REQUIRES(...) \
	THREAD_ANNOTATION_ATTRIBUTE__(requires_capability(__VA_ARGS__))

#define REQUIRES_SHARED(...) \
	THREAD_ANNOTATION_ATTRIBUTE__(requires_shared_capability(__VA_ARGS__))

#define ACQUIRE(...) \
	THREAD_ANNOTATION_ATTRIBUTE__(acquire_capability(__VA_ARGS__))

#define ACQUIRE_SHARED(...) \
	THREAD_ANNOTATION_ATTRIBUTE__(acquire_shared_capability(__VA_ARGS__))

#define RELEASE(...) \
	THREAD_ANNOTATION_ATTRIBUTE__(release_capability(__VA_ARGS__))

#define RELEASE_SHARED(...) \
	THREAD_ANNOTATION_ATTRIBUTE__(release_shared_capability(__VA_ARGS__))

#define RELEASE_GENERIC(...) \
	THREAD_ANNOTATION_ATTRIBUTE__(release_generic_capability(__VA_ARGS__))

#define TRY_ACQUIRE(...) \
	THREAD_ANNOTATION_ATTRIBUTE__(try_acquire_capability(__VA_ARGS__))

#define TRY_ACQUIRE_SHARED(...) \
	THREAD_ANNOTATION_ATTRIBUTE__(try_acquire_shared_capability(__VA_ARGS__))

#define EXCLUDES(...) \
	THREAD_ANNOTATION_ATTRIBUTE__(locks_excluded(__VA_ARGS__))

#define ASSERT_CAPABILITY(x) \
	THREAD_ANNOTATION_ATTRIBUTE__(assert_capability(x))

#define ASSERT_SHARED_CAPABILITY(x) \
	THREAD_ANNOTATION_ATTRIBUTE__(assert_shared_capability(x))

#define RETURN_CAPABILITY(x) \
	THREAD_ANNOTATION_ATTRIBUTE__(lock_returned(x))

#define NO_THREAD_SAFETY_ANALYSIS \
	THREAD_ANNOTATION_ATTRIBUTE__(no_thread_safety_analysis)



const pthread_mutexattr_t *get_default_mutex_attrs();

const pthread_mutexattr_t *get_default_recursive_mutex_attrs();

typedef pthread_mutex_t mutex_t CAPABILITY("mutex");

static inline void mutex_init(mutex_t *restrict mutex) {
    ASSERTED int ok = pthread_mutex_init(mutex, get_default_mutex_attrs());
    ASSERT_ZERO_MSG(ok, "Error initializing mutex.");
}

static inline void mutex_init_recursive(mutex_t *restrict mutex) {
    ASSERTED int ok = pthread_mutex_init(mutex, get_default_recursive_mutex_attrs());
    ASSERT_ZERO_MSG(ok, "Error initializing mutex.");
}

static inline void mutex_lock(mutex_t *mutex) ACQUIRE() {
    ASSERTED int ok = pthread_mutex_lock(mutex);
    ASSERT_ZERO_MSG(ok, "Error locking mutex.");
}

static inline bool mutex_trylock(mutex_t *mutex) TRY_ACQUIRE(true) {
    int ok = pthread_mutex_trylock(mutex);
    ASSERT_MSG(ok == 0 || ok == EBUSY, "Error trying to lock mutex.");
    return ok == 0;
}

static inline void mutex_unlock(mutex_t *mutex) RELEASE() {
    ASSERTED int ok = pthread_mutex_unlock(mutex);
    ASSERT_ZERO_MSG(ok, "Error unlocking mutex.");
}


typedef pthread_rwlock_t rwlock_t CAPABILITY("mutex");

static inline void rwlock_init(rwlock_t *restrict lock) {
	ASSERTED int ok = pthread_rwlock_init(lock, NULL);
	ASSERT_ZERO_MSG(ok, "Error initializing rwlock.");
}

static inline void rwlock_lock_read(rwlock_t *rwlock) ACQUIRE_SHARED() {
    ASSERTED int ok = pthread_rwlock_rdlock(rwlock);
    ASSERT_ZERO_MSG(ok, "Error locking rwlock for reading.");
}

static inline bool rwlock_try_lock_read(rwlock_t *rwlock) TRY_ACQUIRE_SHARED(true) {
    int ok = pthread_rwlock_tryrdlock(rwlock);
    ASSERT_MSG(ok == 0 || ok == EBUSY, "Error trying to lock rwlock for reading.");
    return ok == 0;
}

static inline void rwlock_lock_write(rwlock_t *rwlock) ACQUIRE() {
    ASSERTED int ok = pthread_rwlock_wrlock(rwlock);
    ASSERT_ZERO_MSG(ok, "Error locking rwlock for writing.");
}

static inline bool rwlock_try_lock_write(rwlock_t *rwlock) TRY_ACQUIRE(true) {
    int ok = pthread_rwlock_trywrlock(rwlock);
    ASSERT_MSG(ok == 0 || ok == EBUSY, "Error trying to lock rwlock for writing.");
    return ok == 0;
}

static inline void rwlock_unlock(rwlock_t *rwlock) RELEASE() {
    ASSERTED int ok = pthread_rwlock_unlock(rwlock);
    ASSERT_ZERO_MSG(ok, "Error unlocking rwlock.");
}


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

#ifdef DEBUG
static inline void assert_mutex_locked(pthread_mutex_t *mutex) {
    int result = pthread_mutex_trylock(mutex);
    if (result == 0) {
        pthread_mutex_unlock(mutex);
        ASSERT_MSG(false, "Mutex is not locked.");
    }
}
#else
static inline void assert_mutex_locked(pthread_mutex_t *mutex) {
    (void) mutex;
}
#endif

#endif  // _FLUTTERPI_SRC_UTIL_LOCK_OPS_H
