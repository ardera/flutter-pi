// The MIT License
//
// Copyright (c) 2008, by Attractive Chaos <attractor@live.co.uk>
//
// Permission is hereby granted, free of charge, to any person obtaining
// a copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to
// permit persons to whom the Software is furnished to do so, subject to
// the following conditions:
//
// The above copyright notice and this permission notice shall be
// included in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
// BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
// ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
// CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

// An example:
//
//     #include "kvec.h"
//     int main() {
//       kvec_t(int) array = KV_INITIAL_VALUE;
//       kv_push(array, 10); // append
//       kv_a(array, 20) = 5; // dynamic
//       kv_A(array, 20) = 4; // static
//       kv_destroy(array);
//       return 0;
//     }

#ifndef NVIM_LIB_KVEC_H
#define NVIM_LIB_KVEC_H

#include <stdlib.h>
#include <string.h>

#define kv_roundup32(x) ((--(x)), ((x) |= (x) >> 1, (x) |= (x) >> 2, (x) |= (x) >> 4, (x) |= (x) >> 8, (x) |= (x) >> 16), (++(x)))

#define KV_INITIAL_VALUE { .size = 0, .capacity = 0, .items = NULL }

#define kvec_t(type)     \
    struct {             \
        size_t size;     \
        size_t capacity; \
        type *items;     \
    }

#define kv_init(v) ((v).size = (v).capacity = 0, (v).items = 0)
#define kv_destroy(v)    \
    do {                 \
        free((v).items); \
        kv_init(v);      \
    } while (0)
#define kv_A(v, i) ((v).items[(i)])
#define kv_pop(v) ((v).items[--(v).size])
#define kv_size(v) ((v).size)
#define kv_max(v) ((v).capacity)
#define kv_Z(v, i) kv_A(v, kv_size(v) - (i) - 1)
#define kv_last(v) kv_Z(v, 0)

/// Drop last n items from kvec without resizing
///
/// Previously spelled as `(void)kv_pop(v)`, repeated n times.
///
/// @param[out]  v  Kvec to drop items from.
/// @param[in]  n  Number of elements to drop.
#define kv_drop(v, n) ((v).size -= (n))

#define kv_resize(v, s) ((v).capacity = (s), (v).items = realloc((v).items, sizeof((v).items[0]) * (v).capacity))

#define kv_resize_full(v) kv_resize(v, (v).capacity ? (v).capacity << 1 : 8)

#define kv_copy(v1, v0)                                                    \
    do {                                                                   \
        if ((v1).capacity < (v0).size) {                                   \
            kv_resize(v1, (v0).size);                                      \
        }                                                                  \
        (v1).size = (v0).size;                                             \
        memcpy((v1).items, (v0).items, sizeof((v1).items[0]) * (v0).size); \
    } while (0)

/// fit at least "len" more items
#define kv_ensure_space(v, len)              \
    do {                                     \
        if ((v).capacity < (v).size + len) { \
            (v).capacity = (v).size + len;   \
            kv_roundup32((v).capacity);      \
            kv_resize((v), (v).capacity);    \
        }                                    \
    } while (0)

#define kv_concat_len(v, data, len)                                     \
    if (len > 0) {                                                      \
        kv_ensure_space(v, len);                                        \
        assert((v).items);                                              \
        memcpy((v).items + (v).size, data, sizeof((v).items[0]) * len); \
        (v).size = (v).size + len;                                      \
    }

#define kv_concat(v, str) kv_concat_len(v, str, strlen(str))
#define kv_splice(v1, v0) kv_concat_len(v1, (v0).items, (v0).size)

#define kv_pushp(v) ((((v).size == (v).capacity) ? (kv_resize_full(v), 0) : 0), ((v).items + ((v).size++)))

#define kv_push(v, x) (*kv_pushp(v) = (x))

#define kv_pushp_c(v) ((v).items + ((v).size++))
#define kv_push_c(v, x) (*kv_pushp_c(v) = (x))

#define kv_a(v, i)                                                                                               \
    (*(((v).capacity <= (size_t) (i) ?                                                                           \
            ((v).capacity = (v).size = (i) + 1, kv_roundup32((v).capacity), kv_resize((v), (v).capacity), 0UL) : \
            ((v).size <= (size_t) (i) ? (v).size = (i) + 1 : 0UL)),                                              \
       &(v).items[(i)]))

#endif  // NVIM_LIB_KVEC_H