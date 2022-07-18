#ifndef _COLLECTION_H
#define _COLLECTION_H

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
#include <stdatomic.h>

#include <pthread.h>

struct queue {
	size_t start_index;
	size_t length;
	size_t size;
	void  *elements;

	size_t max_queue_size;
	size_t element_size;
};

struct concurrent_queue {
	pthread_mutex_t mutex;
	pthread_cond_t  is_dequeueable;
	pthread_cond_t  is_enqueueable;
	struct queue queue;
};

struct pointer_set {
	/**
	 * @brief The number of non-NULL pointers currently stored in @ref pointers. 
	 */
	size_t count_pointers;

	/**
	 * @brief The current size of the @ref pointers memory block, in pointers.
	 */
	size_t size;

	/**
	 * @brief The actual memory where the pointers are stored.
	 */
	void **pointers;

	/**
	 * @brief The maximum size of the @ref pointers memory block, in pointers.
	 */
	size_t max_size;

	/**
	 * @brief Whether this pointer_set is using static memory.
	 */
	bool is_static;
};

struct concurrent_pointer_set {
	pthread_mutex_t mutex;
	struct pointer_set set;
};

typedef _Atomic(int) refcount_t;

#define QUEUE_DEFAULT_MAX_SIZE 64

#define QUEUE_INITIALIZER(element_type, _max_size) \
	((struct queue) { \
		.start_index = 0, \
		.length = 0, \
		.size = 0, \
		.elements = NULL, \
		.max_queue_size = _max_queue_size, \
		.element_size = sizeof(element_type) \
	})

#define CQUEUE_DEFAULT_MAX_SIZE 64

#define CQUEUE_INITIALIZER(element_type, _max_size) \
	((struct concurrent_queue) { \
		.mutex = PTHREAD_MUTEX_INITIALIZER, \
		.is_dequeueable = PTHREAD_COND_INITIALIZER, \
		.is_enqueueable = PTHREAD_COND_INITIALIZER, \
		.queue = QUEUE_INITIALIZER(element_type, _max_queue_size) \
	})

#define PSET_DEFAULT_MAX_SIZE 64

#define PSET_INITIALIZER(_max_size) \
	((struct pointer_set) { \
		.count_pointers = 0, \
		.size = 0, \
		.pointers = NULL, \
		.max_size = _max_size, \
		.is_static = false \
	})

#define PSET_INITIALIZER_STATIC(_storage, _size) \
	((struct pointer_set) { \
		.count_pointers = 0, \
		.size = _size, \
		.pointers = _storage, \
		.max_size = _size, \
		.is_static = true \
	})

#define CPSET_DEFAULT_MAX_SIZE 64

#define CPSET_INITIALIZER(_max_size) \
	((struct concurrent_pointer_set) { \
		.mutex = PTHREAD_MUTEX_INITIALIZER, \
		.set = { \
			.count_pointers = 0, \
			.size = 0, \
			.pointers = NULL, \
			.max_size = _max_size, \
			.is_static = false \
		} \
	})


int queue_init(
	struct queue *queue,
	size_t element_size,
	size_t max_queue_size
);

int queue_deinit(
	struct queue *queue
);

int queue_enqueue(
	struct queue *queue,
	const void *p_element
);

int queue_dequeue(
	struct queue *queue,
	void *element_out
);

int queue_peek(
	struct queue *queue,
	void **pelement_out
);


int cqueue_init(
	struct concurrent_queue *queue,
	size_t element_size,
	size_t max_queue_size
);

int cqueue_deinit(
	struct concurrent_queue *queue
);

static inline int cqueue_lock(struct concurrent_queue * const queue) {
	return pthread_mutex_lock(&queue->mutex);
}

static inline int cqueue_unlock(struct concurrent_queue * const queue) {
	return pthread_mutex_unlock(&queue->mutex);
}

int cqueue_try_enqueue_locked(
	struct concurrent_queue *queue,
	const void *p_element
);

int cqueue_enqueue_locked(
	struct concurrent_queue *queue,
	const void *p_element
);

int cqueue_try_enqueue(
	struct concurrent_queue *queue,
	const void *p_element
);

int cqueue_enqueue(
	struct concurrent_queue *queue,
	const void *p_element
);

int cqueue_try_dequeue_locked(
	struct concurrent_queue *queue,
	void *element_out
);

int cqueue_dequeue_locked(
	struct concurrent_queue *queue,
	void *element_out
);

int cqueue_try_dequeue(
	struct concurrent_queue *queue,
	void *element_out
);

int cqueue_dequeue(
	struct concurrent_queue *queue,
	void *element_out
);

int cqueue_peek_locked(
	struct concurrent_queue *queue,
	void **pelement_out
);

/*
 * pointer set
 */
int pset_init(
	struct pointer_set *set,
	size_t max_size
);

int pset_init_static(
	struct pointer_set *set,
	void **storage,
	size_t size
);

void pset_deinit(
	struct pointer_set *set
);

int pset_put(
	struct pointer_set *set,
	void *pointer
);

bool pset_contains(
	const struct pointer_set *set,
	const void *pointer
);

int pset_remove(
	struct pointer_set *set,
	const void *pointer
);

static inline int pset_get_count_pointers(
	const struct pointer_set *set
) {
	return set->count_pointers;
}

/**
 * @brief Returns the size of the internal storage of set, in pointers.
 */
static inline int pset_get_storage_size(
	const struct pointer_set *set
) {
	return set->size;
}

int pset_copy(
	const struct pointer_set *src,
	struct pointer_set *dest
);

void pset_intersect(
	struct pointer_set *src_dest,
	const struct pointer_set *b
);

int pset_union(
	struct pointer_set *src_dest,
	const struct pointer_set *b
);

int pset_subtract(
	struct pointer_set *minuend_difference,
	const struct pointer_set *subtrahend
);

void *__pset_next_pointer(
	struct pointer_set *set,
	const void *pointer
);

#define for_each_pointer_in_pset(set, pointer) for ((pointer) = __pset_next_pointer(set, NULL); (pointer) != NULL; (pointer) = __pset_next_pointer(set, (pointer)))

/*
 * concurrent pointer set
 */
int cpset_init(
	struct concurrent_pointer_set *set,
	size_t max_size
);

void cpset_deinit(struct concurrent_pointer_set *set);

static inline int cpset_lock(struct concurrent_pointer_set *set) {
	return pthread_mutex_lock(&set->mutex);
}

static inline int cpset_unlock(struct concurrent_pointer_set *set) {
	return pthread_mutex_unlock(&set->mutex);
}

static inline int cpset_put_locked(
	struct concurrent_pointer_set *set,
	void *pointer
) {
	return pset_put(&set->set, pointer);
}

static inline int cpset_put(
	struct concurrent_pointer_set *set,
	void *pointer
) {
	int ok;

	cpset_lock(set);
	ok = pset_put(&set->set, pointer);
	cpset_unlock(set);

	return ok;
}

static inline bool cpset_contains_locked(
	struct concurrent_pointer_set *set,
	const void *pointer
) {
	return pset_contains(&set->set, pointer);
}

static inline bool cpset_contains(
	struct concurrent_pointer_set *set,
	const void *pointer
) {
	bool result;

	cpset_lock(set);
	result = pset_contains(&set->set, pointer);
	cpset_unlock(set);

	return result;
}

static inline int cpset_remove_locked(
	struct concurrent_pointer_set *set,
	const void *pointer
) {
	return pset_remove(&set->set, pointer); 
}

static inline int cpset_remove(
	struct concurrent_pointer_set *set,
	const void *pointer
) {
	int ok;

	cpset_lock(set);
	ok = cpset_remove_locked(set, pointer);
	cpset_unlock(set);

	return ok;
}

static inline int cpset_get_count_pointers_locked(
	const struct concurrent_pointer_set *set
) {
	return set->set.count_pointers;
}

/**
 * @brief Returns the size of the internal storage of set, in pointers.
 */
static inline int cpset_get_storage_size_locked(
	const struct concurrent_pointer_set *set
) {
	return set->set.size;
}

static inline int cpset_copy_into_pset_locked(
	struct concurrent_pointer_set *src,
	struct pointer_set *dest
) {
	return pset_copy(&src->set, dest);
}

static inline void *__cpset_next_pointer_locked(
	struct concurrent_pointer_set *set,
	const void *pointer
) {
	return __pset_next_pointer(&set->set, pointer);
}

#define for_each_pointer_in_cpset(set, pointer) for ((pointer) = __cpset_next_pointer_locked(set, NULL); (pointer) != NULL; (pointer) = __cpset_next_pointer_locked(set, (pointer)))

static inline void *memdup(const void *restrict src, const size_t n) {
	void *__restrict__ dest;

	if ((src == NULL) || (n == 0)) return NULL;

	dest = malloc(n);
	if (dest == NULL) return NULL;

	return memcpy(dest, src, n);
}

#define BMAP_ELEMENT_TYPE uint8_t
#define BMAP_ELEMENT_SIZE (sizeof(BMAP_ELEMENT_TYPE))
#define BMAP_ELEMENT_BITS (BMAP_ELEMENT_SIZE * 8)
#define BMAP_DECLARATION(name, n_bits) BMAP_ELEMENT_TYPE name[(((n_bits) - 1) / BMAP_ELEMENT_BITS) + 1]
#define BMAP_IS_SET(p_bmap, i_bit) ((p_bmap)[(i_bit) / BMAP_ELEMENT_BITS] & (1 << ((i_bit) & (BMAP_ELEMENT_BITS - 1))))
#define BMAP_SET(p_bmap, i_bit) ((p_bmap)[(i_bit) / BMAP_ELEMENT_BITS] |= (1 << ((i_bit) & (BMAP_ELEMENT_BITS - 1))))
#define BMAP_CLEAR(p_bmap, i_bit) ((p_bmap)[(i_bit) / BMAP_ELEMENT_BITS] &= ~(1 << ((i_bit) & (BMAP_ELEMENT_BITS - 1))))
#define BMAP_ZERO(p_bmap) memset((p_bmap), 0, sizeof(p_bmap) / sizeof(*(p_bmap)))
#define BMAP_SIZE(p_bmap) (ARRAY_SIZE(p_bmap) * BMAP_ELEMENT_BITS)

#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))

/**
 * @brief Get the current time of the system monotonic clock.
 * @returns time in nanoseconds.
 */
static inline uint64_t get_monotonic_time(void) {
	struct timespec time;
	clock_gettime(CLOCK_MONOTONIC, &time);
	return time.tv_nsec + time.tv_sec*1000000000ull;
}

#define FILE_DESCR(_logging_name) \
static const char *__attribute__((unused)) __file_logging_name = _logging_name;

#ifdef DEBUG
#define DEBUG_ASSERT(__cond) assert(__cond)
#define DEBUG_ASSERT_MSG(__cond, __msg) assert((__cond) && (__msg))
#define LOG_ERROR(fmtstring, ...) fprintf(stderr, "[%s] " fmtstring, __file_logging_name, ##__VA_ARGS__)
#define LOG_ERROR_UNPREFIXED(fmtstring, ...) fprintf(stderr, fmtstring, ##__VA_ARGS__)
#define LOG_DEBUG(fmtstring, ...) fprintf(stderr, "[%s] " fmtstring, __file_logging_name, ##__VA_ARGS__)
#define LOG_DEBUG_UNPREFIXED(fmtstring, ...) fprintf(stderr, fmtstring, ##__VA_ARGS__)
#else
#define DEBUG_ASSERT(__cond) do {} while (0)
#define DEBUG_ASSERT_MSG(__cond, __msg) do {} while (0)
#define LOG_ERROR(fmtstring, ...) fprintf(stderr, "[%s] " fmtstring, __file_logging_name, ##__VA_ARGS__)
#define LOG_ERROR_UNPREFIXED(fmtstring, ...) fprintf(stderr, fmtstring, ##__VA_ARGS__)
#define LOG_DEBUG(fmtstring, ...) do {} while (0)
#define LOG_DEBUG_UNPREFIXED(fmtstring, ...) do {} while (0)
#endif

#define DEBUG_ASSERT_NOT_NULL(__var) DEBUG_ASSERT(__var != NULL)
#define DEBUG_ASSERT_NOT_NULL_MSG(__var, __msg) DEBUG_ASSERT_MSG(__var != NULL, __msg)
#define DEBUG_ASSERT_EQUALS(__a, __b) DEBUG_ASSERT((__a) == (__b))
#define DEBUG_ASSERT_EQUALS_MSG(__a, __b, __msg) DEBUG_ASSERT_MSG((__a) == (__b), __msg)
#define DEBUG_ASSERT_EGL_TRUE(__var) DEBUG_ASSERT((__var) == EGL_TRUE)
#define DEBUG_ASSERT_EGL_TRUE_MSG(__var, __msg) DEBUG_ASSERT_MSG((__var) == EGL_TRUE, __msg)

#if !(201112L <= __STDC_VERSION__ || (!defined __STRICT_ANSI__ && (__GNUC__ > 4) || ((__GNUC__ == 4) && (__GNUC_MINOR >= 6))))
#	error "Needs C11 or later or GCC (not in pedantic mode) 4.6.0 or later for compile time asserts."
#endif

#define COMPILE_ASSERT_MSG(expression, msg) _Static_assert(expression, msg)
#define COMPILE_ASSERT(expression) COMPILE_ASSERT_MSG(expression, "Expression evaluates to false")

#define UNIMPLEMENTED() assert(0 && "Unimplemented")

#ifndef __has_builtin
	#define __has_builtin(x) 0
#endif

#if defined(__GNUC__) || __has_builtin(__builtin_unreachable)
#define UNREACHABLE() __builtin_unreachable
#else
#define UNREACHABLE() assert(0 && "Unreachable")
#endif

#if defined(__GNUC__) || __has_builtin(__builtin_popcount)
#define HWEIGHT(x) __builtin_popcount(x)
#else
#define HWEIGHT(x) UNIMPLEMENTED()
#endif

#if defined(__GNUC__) || defined(__clang__)
#define MAYBE_UNUSED __attribute__((unused))
#define ATTR_MALLOC __attribute__((malloc))
#define NONNULL(...) __attribute__((nonnull(__VA_ARGS__)))
#define ATTR_PURE __attribute__((pure))
#define ATTR_CONST __attribute__((const))
#else
#define MAYBE_UNUSED
#define ATTR_MALLOC
#define NONNULL(...)
#define ATTR_PURE
#define ATTR_CONST
#endif


static inline int refcount_inc_n(refcount_t *refcount, int n) {
	return atomic_fetch_add_explicit(refcount, n, memory_order_relaxed);
}

/// Increments the reference count and returns the previous value.
static inline int refcount_inc(refcount_t *refcount) {
	return refcount_inc_n(refcount, 1);
}

/// Decrement the reference count, return true if the refcount afterwards
/// is still non-zero.
static inline bool refcount_dec(refcount_t *refcount) {
	return atomic_fetch_sub_explicit(refcount, 1, memory_order_acq_rel) != 1;
}

/// Returns true if the reference count is one.
/// If this is the case you that means this thread has exclusive access
/// to the object.
static inline bool refcount_is_one(refcount_t *refcount) {
	return atomic_load_explicit(refcount, memory_order_acquire) == 1;
}

/// Returns true if the reference count is zero. Should never be true
/// in practice because that'd only be the case for a destroyed object.
/// So this is only really useful for debugging.
static inline bool refcount_is_zero(refcount_t *refcount) {
	return atomic_load_explicit(refcount, memory_order_acquire) == 0;
}

/// Get the current reference count, without any memory ordering restrictions.
/// Not strictly correct, should only be used for debugging. 
static inline int refcount_get_for_debug(refcount_t *refcount) {
	return atomic_load_explicit(refcount, memory_order_relaxed);
}

#define REFCOUNT_INIT_0 (0)
#define REFCOUNT_INIT_1 (1)
#define REFCOUNT_INIT_N(n) (n)

#define DECLARE_REF_OPS(obj_name) \
MAYBE_UNUSED struct obj_name *obj_name ## _ref(struct obj_name *obj); \
MAYBE_UNUSED void obj_name ## _unref(struct obj_name *obj); \
MAYBE_UNUSED void obj_name ## _unrefp(struct obj_name **obj); \
MAYBE_UNUSED void obj_name ## _swap(struct obj_name **objp, struct obj_name *obj);

#define DEFINE_REF_OPS(obj_name, refcount_member_name) \
MAYBE_UNUSED struct obj_name *obj_name ## _ref(struct obj_name *obj) { \
	refcount_inc(&obj->refcount_member_name); \
	return obj; \
} \
MAYBE_UNUSED void obj_name ## _unref(struct obj_name *obj) { \
	if (refcount_dec(&obj->refcount_member_name) == false) { \
		obj_name ## _destroy(obj); \
	} \
} \
MAYBE_UNUSED void obj_name ## _unrefp(struct obj_name **obj) { \
	obj_name ## _unref(*obj); \
	*obj = NULL; \
} \
MAYBE_UNUSED void obj_name ## _swap_ptrs(struct obj_name **objp, struct obj_name *obj) { \
	if (obj != NULL) { \
		obj_name ## _ref(obj); \
	} \
	if (*objp != NULL) { \
		obj_name ## _unrefp(objp); \
	} \
	*objp = obj; \
}

#define DEFINE_STATIC_REF_OPS(obj_name, refcount_member_name) \
MAYBE_UNUSED static struct obj_name *obj_name ## _ref(struct obj_name *obj) { \
	refcount_inc(&obj->refcount_member_name); \
	return obj; \
} \
MAYBE_UNUSED static void obj_name ## _unref(struct obj_name *obj) { \
	if (refcount_dec(&obj->refcount_member_name) == false) { \
		obj_name ## _destroy(obj); \
	} \
} \
MAYBE_UNUSED static void obj_name ## _unrefp(struct obj_name **obj) { \
	obj_name ## _unref(*obj); \
	*obj = NULL; \
} \
MAYBE_UNUSED static void obj_name ## _swap_ptrs(struct obj_name **objp, struct obj_name *obj) { \
	if (obj != NULL) { \
		obj_name ## _ref(obj); \
	} \
	if (*objp != NULL) { \
		obj_name ## _unrefp(objp); \
	} \
	*objp = obj; \
}

#define DECLARE_LOCK_OPS(obj_name) \
MAYBE_UNUSED void obj_name ## _lock(struct obj_name *obj); \
MAYBE_UNUSED void obj_name ## _unlock(struct obj_name *obj);

#define DEFINE_LOCK_OPS(obj_name, mutex_member_name) \
MAYBE_UNUSED void obj_name ## _lock(struct obj_name *obj) { \
	pthread_mutex_lock(&obj->mutex_member_name); \
} \
MAYBE_UNUSED void obj_name ## _unlock(struct obj_name *obj) { \
	pthread_mutex_unlock(&obj->mutex_member_name); \
}

#define DEFINE_STATIC_LOCK_OPS(obj_name, mutex_member_name) \
MAYBE_UNUSED static void obj_name ## _lock(struct obj_name *obj) { \
	pthread_mutex_lock(&obj->mutex_member_name); \
} \
MAYBE_UNUSED static void obj_name ## _unlock(struct obj_name *obj) { \
	pthread_mutex_unlock(&obj->mutex_member_name); \
}

#define DEFINE_INLINE_LOCK_OPS(obj_name, mutex_member_name) \
MAYBE_UNUSED static inline void obj_name ## _lock(struct obj_name *obj) { \
	pthread_mutex_lock(&obj->mutex_member_name); \
} \
MAYBE_UNUSED static inline void obj_name ## _unlock(struct obj_name *obj) { \
	pthread_mutex_unlock(&obj->mutex_member_name); \
}

#define BITCAST(to_type, value) (*((const to_type*) (&(value))))

static inline int32_t uint32_to_int32(const uint32_t v) {
	return BITCAST(int32_t, v);
}

static inline uint32_t int32_to_uint32(const int32_t v) {
	return BITCAST(uint32_t, v);
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

#define CONTAINER_OF(container_type, field_ptr, field_name) ({ \
	const typeof( ((container_type*) 0)->field_name ) *__field_ptr_2 = (field_ptr); \
	(container_type*) ((char*) __field_ptr_2 - offsetof(container_type, field_name)); \
})

#define MAX_ALIGNMENT (__alignof__(max_align_t))
#define IS_MAX_ALIGNED(num) ((num) % MAX_ALIGNMENT == 0)

typedef struct {
	uint8_t bytes[16];
} uuid_t;
#define UUID(_0, _1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15) ((uuid_t) {.bytes = {_0, _1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15}})
#define CONST_UUID(_0, _1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15) ((const uuid_t) {.bytes = {_0, _1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15}})


static inline bool uuid_equals(const uuid_t a, const uuid_t b) {
	return memcmp(&a, &b, sizeof(uuid_t)) == 0;
}

static inline void uuid_copy(uuid_t *dst, const uuid_t src) {
	memcpy(dst, &src, sizeof(uuid_t));
}

#define DOUBLE_TO_FP1616(v) ((uint32_t) ((v) * 65536))
#define DOUBLE_TO_FP1616_ROUNDED(v) (((uint32_t) (v)) << 16)

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#endif
