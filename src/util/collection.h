// SPDX-License-Identifier: MIT
/*
 * Collection - common useful functions & macros
 *
 * Copyright (c) 2023, Hannes Winkler <hanneswinkler2000@web.de>
 */

#ifndef _FLUTTERPI_SRC_UTIL_COLLECTION_H
#define _FLUTTERPI_SRC_UTIL_COLLECTION_H

#if !defined(_XOPEN_SOURCE) || _XOPEN_SOURCE < 500L
    #define _XOPEN_SOURCE 500L
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <pthread.h>

#include "macros.h"

static inline void *memdup(const void *restrict src, const size_t n) {
    void *__restrict__ dest;

    if ((src == NULL) || (n == 0))
        return NULL;

    dest = malloc(n);
    if (dest == NULL)
        return NULL;

    return memcpy(dest, src, n);
}

/**
 * @brief Get the current time of the system monotonic clock.
 * @returns time in nanoseconds.
 */
static inline uint64_t get_monotonic_time(void) {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return time.tv_nsec + time.tv_sec * 1000000000ull;
}

#define BITCAST(to_type, value) (*((const to_type *) (&(value))))

static inline int32_t uint32_to_int32(const uint32_t v) {
    return BITCAST(int32_t, v);
}

static inline uint32_t int32_to_uint32(const int32_t v) {
    return BITCAST(uint32_t, v);
}

static inline uint64_t int64_to_uint64(const int64_t v) {
    return BITCAST(uint64_t, v);
}

static inline int64_t uint64_to_int64(const uint64_t v) {
    return BITCAST(int64_t, v);
}

static inline int64_t ptr_to_int64(const void *const ptr) {
    union {
        const void *ptr;
        int64_t int64;
    } u;

    u.int64 = 0;
    u.ptr = ptr;
    return u.int64;
}

static inline void *int64_to_ptr(const int64_t v) {
    union {
        void *ptr;
        int64_t int64;
    } u;

    u.int64 = v;
    return u.ptr;
}

static inline int64_t ptr_to_uint32(const void *const ptr) {
    union {
        const void *ptr;
        uint32_t u32;
    } u;

    u.u32 = 0;
    u.ptr = ptr;
    return u.u32;
}

static inline void *uint32_to_ptr(const uint32_t v) {
    union {
        void *ptr;
        uint32_t u32;
    } u;

    u.ptr = NULL;
    u.u32 = v;
    return u.ptr;
}

#define MAX_ALIGNMENT (__alignof__(max_align_t))
#define IS_MAX_ALIGNED(num) ((num) % MAX_ALIGNMENT == 0)

#define DOUBLE_TO_FP1616(v) ((uint32_t) ((v) *65536))
#define DOUBLE_TO_FP1616_ROUNDED(v) (((uint32_t) (v)) << 16)

typedef void (*void_callback_t)(void *userdata);

ATTR_PURE static inline bool streq(const char *a, const char *b) {
    return strcmp(a, b) == 0;
}

const pthread_mutexattr_t *get_default_mutex_attrs();

#endif  // _FLUTTERPI_SRC_UTIL_COLLECTION_H
