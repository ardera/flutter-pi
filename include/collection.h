#ifndef _COLLECTION_H
#define _COLLECTION_H

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stdbool.h>

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

struct concurrent_pointer_set {
	pthread_mutex_t mutex;
	size_t count_pointers;
	size_t size;
	void **pointers;

	size_t max_size;
};

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

#define CPSET_DEFAULT_MAX_SIZE 64

#define CPSET_INITIALIZER(_max_size) \
	((struct concurrent_pointer_set) { \
		.mutex = PTHREAD_MUTEX_INITIALIZER, \
		.count_pointers = 0, \
		.size = 0, \
		.pointers = NULL, \
		.max_size = _max_size \
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

static inline int cpset_init(
	struct concurrent_pointer_set *const set,
	const size_t max_size
) {
	memset(set, 0, sizeof(*set));

	pthread_mutex_init(&set->mutex, NULL);

	set->count_pointers = 0;
	set->size = 2;
	set->pointers = (void**) calloc(2, sizeof(void*));

	set->max_size = max_size;

	if (set->pointers == NULL) {
		set->size = 0;
		return ENOMEM;
	}

	return 0;
}

static inline int cpset_deinit(struct concurrent_pointer_set *const set) {
	pthread_mutex_destroy(&set->mutex);

	if (set->pointers != NULL) {
		free(set->pointers);
	}

	set->count_pointers = 0;
	set->size = 0;
	set->pointers = NULL;

	set->max_size = 0;

	return 0;
}

static inline int cpset_lock(struct concurrent_pointer_set *const set) {
	return pthread_mutex_lock(&set->mutex);
}

static inline int cpset_unlock(struct concurrent_pointer_set *const set) {
	return pthread_mutex_unlock(&set->mutex);
}

static inline int cpset_put_locked(
	struct concurrent_pointer_set *const set,
	void *pointer
) {
	size_t new_size;
	int index;

	index = -1;
	for (int i = 0; i < set->size; i++) {
		if (set->pointers[i] && (pointer == set->pointers[i])) {
			index = i;
			break;
		}
	}
	
	if ((index == -1) && (set->size == set->count_pointers)) {
		new_size = set->size ? set->size << 1 : 1;

		if (new_size < set->max_size) {
			void **new_pointers = (void**) realloc(set->pointers, new_size * sizeof(void*));
			
			if (new_pointers == NULL) {
				return ENOMEM;
			}

			memset(new_pointers + set->size, 0, (new_size - set->size) * sizeof(void*));

			index = set->size;

			set->pointers = new_pointers;
			set->size = new_size;
		} else {
			return ENOSPC;
		}
	}

	if (index == -1) {
		while (set->pointers[++index]);
	}

	set->pointers[index] = pointer;

	set->count_pointers++;

	return 0;
}

static inline int cpset_put(
	struct concurrent_pointer_set *const set,
	void *pointer
) {
	int ok;

	cpset_lock(set);
	ok = cpset_put_locked(set, pointer);
	cpset_unlock(set);

	return ok;
}

static inline bool cpset_contains_locked(
	struct concurrent_pointer_set *const set,
	const void const *pointer
) {
	for (int i = 0; i < set->size; i++) {
		if (set->pointers[i] && (set->pointers[i] == pointer)) {
			return true;
		}
	}

	return false;
}

static inline bool cpset_contains(
	struct concurrent_pointer_set *const set,
	const void const *pointer
) {
	bool result;
	
	cpset_lock(set);
	result = cpset_contains_locked(set, pointer);
	cpset_unlock(set);

	return result;
}



static inline void *cpset_remove_locked(
	struct concurrent_pointer_set *const set,
	const void const *pointer
) {
	void *result;
	size_t new_size;
	int index;

	result = NULL;

	for (index = 0; index < set->size; index++) {
		if (set->pointers[index] && (set->pointers[index] == pointer)) {
			result = set->pointers[index];
			
			set->pointers[index] = NULL;
			set->count_pointers--;
			
			return result;
		}
	}

	return NULL;
}

static inline void *cpset_remove(
	struct concurrent_pointer_set *const set,
	const void const *pointer
) {
	void *result;

	cpset_lock(set);
	result = cpset_remove_locked(set, pointer);
	cpset_unlock(set);

	return result;
}

static inline void *__cpset_next_pointer(
	struct concurrent_pointer_set *const set,
	const void const *pointer
) {
	int i = -1;

	if (pointer != NULL) {
		for (i = 0; i < set->size; i++) {
			if (set->pointers[i] == pointer) {
				break;
			}
		}

		if (i == set->size) return NULL;
	}

	for (i = i+1; i < set->size; i++) {
		if (set->pointers[i]) {
			return set->pointers[i];
		}
	}

	return NULL;
}

#define for_each_pointer_in_cpset(set, pointer) for ((pointer) = __cpset_next_pointer(set, NULL); (pointer) != NULL; (pointer) = __cpset_next_pointer(set, (pointer)))

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

#endif