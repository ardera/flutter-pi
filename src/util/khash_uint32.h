#ifndef _FLUTTERPI_SRC_UTIL_KHASH_UINT32_H
#define _FLUTTERPI_SRC_UTIL_KHASH_UINT32_H

#include <stdint.h>

#include "khash.h"

#define kh_uint32_hash_func(key) (uint32_t)(key)
#define kh_uint32_hash_equal(a, b) ((a) == (b))

#define KHASH_SET_INIT_UINT32(name) KHASH_INIT(name, uint32_t, char, 0, kh_uint32_hash_func, kh_uint32_hash_equal)

#define KHASH_MAP_INIT_UINT32(name, khval_t) KHASH_INIT(name, uint32_t, khval_t, 1, kh_uint32_hash_func, kh_uint32_hash_equal)

#endif  // _FLUTTERPI_SRC_UTIL_KHASH_UINT32_H
