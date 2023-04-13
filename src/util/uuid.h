#ifndef UUID_H
#define UUID_H

#include <stdint.h>
#include <string.h>

typedef struct {
    uint8_t bytes[16];
} uuid_t;

#define UUID(_0, _1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15)         \
    ((uuid_t){                                                                             \
        .bytes = { _0, _1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15 }, \
    })

#define CONST_UUID(_0, _1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15)   \
    ((const uuid_t){                                                                       \
        .bytes = { _0, _1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15 }, \
    })

static inline bool uuid_equals(const uuid_t a, const uuid_t b) {
    return memcmp(&a, &b, sizeof(uuid_t)) == 0;
}

static inline void uuid_copy(uuid_t *dst, const uuid_t src) {
    memcpy(dst, &src, sizeof(uuid_t));
}

#endif
