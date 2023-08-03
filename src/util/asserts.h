// SPDX-License-Identifier: MIT
/*
 * Common assert macros
 *
 * Copyright (c) 2023, Hannes Winkler <hanneswinkler2000@web.de>
 */

#ifndef _FLUTTERPI_SRC_UTIL_ASSERTS_H
#define _FLUTTERPI_SRC_UTIL_ASSERTS_H

#include <assert.h>

#define ASSERT assert
#define ASSERT_MSG(__cond, __msg) assert((__cond) && (__msg))
#define ASSERT_NOT_NULL(__var) assert(__var != NULL)
#define ASSERT_NOT_NULL_MSG(__var, __msg) ASSERT_MSG(__var != NULL, __msg)
#define ASSERT_EQUALS(__a, __b) assert((__a) == (__b))
#define ASSERT_EQUALS_MSG(__a, __b, __msg) ASSERT_MSG((__a) == (__b), __msg)
#define ASSERT_EGL_TRUE(__var) assert((__var) == EGL_TRUE)
#define ASSERT_EGL_TRUE_MSG(__var, __msg) ASSERT_MSG((__var) == EGL_TRUE, __msg)
#define ASSERT_MUTEX_LOCKED(__mutex)               \
    assert(({                                      \
        bool result;                               \
        int r = pthread_mutex_trylock(&(__mutex)); \
        if (r == 0) {                              \
            pthread_mutex_unlock(&(__mutex));      \
            result = false;                        \
        } else {                                   \
            result = true;                         \
        }                                          \
        result;                                    \
    }))
#define ASSERT_ZERO(__var) assert((__var) == 0)
#define ASSERT_ZERO_MSG(__var, __msg) ASSERT_MSG((__var) == 0, __msg)

#define ASSERT_IMPLIES(__cond1, __cond2) assert(!(__cond1) || (__cond2))
#define ASSERT_IMPLIES_MSG(__cond1, __cond2, __msg) ASSERT_MSG(!(__cond1) || (__cond2), __msg)

#if !(201112L <= __STDC_VERSION__ || (!defined __STRICT_ANSI__ && (__GNUC__ > 4) || ((__GNUC__ == 4) && (__GNUC_MINOR >= 6))))
    #error "Needs C11 or later or GCC (not in pedantic mode) 4.6.0 or later for compile time asserts."
#endif

#define COMPILE_ASSERT_MSG(expression, msg) _Static_assert(expression, msg)
#define COMPILE_ASSERT(expression) COMPILE_ASSERT_MSG(expression, "Expression evaluates to false")

#endif  // _FLUTTERPI_SRC_UTIL_ASSERTS_H
