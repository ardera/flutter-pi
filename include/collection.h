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

#define BMAP_DECLARATION(name, n_bits) uint8_t name[(((n_bits) - 1) / 8) + 1]
#define BMAP_IS_SET(p_bmap, i_bit) ((p_bmap)[(i_bit) / sizeof(*(p_bmap))] & (1 << ((i_bit) & (sizeof(*(p_bmap)) - 1))))
#define BMAP_SET(p_bmap, i_bit) ((p_bmap)[(i_bit) / sizeof(*(p_bmap))] |= (1 << ((i_bit) & (sizeof(*(p_bmap)) - 1))))
#define BMAP_CLEAR(p_bmap, i_bit) ((p_bmap)[(i_bit) / sizeof(*(p_bmap))] &= ~(1 << ((i_bit) & (sizeof(*(p_bmap)) - 1))))
#define BMAP_ZERO(p_bmap, n_bits) (memset((p_bmap), 0, (((n_bits) - 1) / 8) + 1))

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
static const char *__file_logging_name = _logging_name;

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
#define DEBUG_ASSERT_EQUALS(__a, __b) DEBUG_ASSERT((__a) == (__b))
#define DEBUG_ASSERT_EGL_TRUE(__var) DEBUG_ASSERT((__var) == EGL_TRUE)

#if !(201112L <= __STDC_VERSION__ || (!defined __STRICT_ANSI__ && (__GNUC__ > 4) || ((__GNUC__ == 4) && (__GNUC_MINOR >= 6))))
#	error "Needs C11 or later or GCC (not in pedantic mode) 4.6.0 or later for compile time asserts."
#endif

#define COMPILE_ASSERT_MSG(expression, msg) _Static_assert(expression, msg)
#define COMPILE_ASSERT(expression) COMPILE_ASSERT_MSG(expression, "Expression evaluates to false")

#define UNIMPLEMENTED() assert(0 && "Unimplemented")

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
struct obj_name *obj_name ## _ref(struct obj_name *obj); \
void obj_name ## _unref(struct obj_name *obj); \
void obj_name ## _unrefp(struct obj_name **obj); \

#define DEFINE_REF_OPS(obj_name, refcount_member_name) \
struct obj_name *obj_name ## _ref(struct obj_name *obj) { \
	refcount_inc(&obj->refcount_member_name); \
	return obj; \
} \
void obj_name ## _unref(struct obj_name *obj) { \
	if (refcount_dec(&obj->refcount_member_name) == false) { \
		obj_name ## _destroy(obj); \
	} \
} \
void obj_name ## _unrefp(struct obj_name **obj) { \
	obj_name ## _unref(*obj); \
	*obj = NULL; \
}

#define DECLARE_LOCK_OPS(obj_name) \
void obj_name ## _lock(struct obj_name *obj); \
void obj_name ## _unlock(struct obj_name *obj);

#define DEFINE_LOCK_OPS(obj_name, mutex_member_name) \
void obj_name ## _lock(struct obj_name *obj) { \
	pthread_mutex_lock(&obj->mutex_member_name); \
} \
void obj_name ## _unlock(struct obj_name *obj) { \
	pthread_mutex_unlock(&obj->mutex_member_name); \
}

#define DEFINE_STATIC_LOCK_OPS(obj_name, mutex_member_name) \
static void obj_name ## _lock(struct obj_name *obj) { \
	pthread_mutex_lock(&obj->mutex_member_name); \
} \
static void obj_name ## _unlock(struct obj_name *obj) { \
	pthread_mutex_unlock(&obj->mutex_member_name); \
}

#define DEFINE_INLINE_LOCK_OPS(obj_name, mutex_member_name) \
static inline void obj_name ## _lock(struct obj_name *obj) { \
	pthread_mutex_lock(&obj->mutex_member_name); \
} \
static inline void obj_name ## _unlock(struct obj_name *obj) { \
	pthread_mutex_unlock(&obj->mutex_member_name); \
}

#define BITCAST(to_type, value) (*((const to_type*) (&(value))))

static inline int32_t uint32_to_int32(const uint32_t v) {
	return BITCAST(int32_t, v);
}

static inline uint32_t int32_to_uint32(const int32_t v) {
	return BITCAST(uint32_t, v);
}

#endif