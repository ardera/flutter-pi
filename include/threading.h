#ifndef _THREADING_H
#define _THREADING_H

#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include <pthread.h>

#define CQUEUE_DEFAULT_MAX_QUEUE_SIZE 64

struct concurrent_queue {
	pthread_mutex_t mutex;
	pthread_cond_t  is_dequeueable;
	pthread_cond_t  is_enqueueable;
	size_t start_index;
	size_t length;
	size_t size;
	void  *elements;

	size_t max_queue_size;
	size_t element_size;
};

#define CQUEUE_INITIALIZER(element_type, _max_queue_size) \
	((struct concurrent_queue) { \
		.mutex = PTHREAD_MUTEX_INITIALIZER, \
		.is_dequeueable = PTHREAD_COND_INITIALIZER, \
		.is_enqueueable = PTHREAD_COND_INITIALIZER, \
		.start_index = 0, \
		.length = 0, \
		.size = 0, \
		.elements = NULL, \
		.max_queue_size = _max_queue_size, \
		.element_size = sizeof(element_type) \
	})

static inline int cqueue_init(struct concurrent_queue *queue, size_t element_size, size_t max_queue_size) {
	memset(queue, 0, sizeof(*queue));

	pthread_mutex_init(&queue->mutex, NULL);
	pthread_cond_init(&queue->is_dequeueable, NULL);
	pthread_cond_init(&queue->is_enqueueable, NULL);

	queue->start_index = 0;
	queue->length = 0;
	queue->size = 2;
	queue->elements = calloc(2, element_size);

	queue->max_queue_size = max_queue_size;
	queue->element_size = element_size;

	if (queue->elements == NULL) {
		queue->size = 0;
		return ENOMEM;
	}

	return 0;
}

static inline int cqueue_deinit(struct concurrent_queue *queue) {
	pthread_mutex_destroy(&queue->mutex);
	pthread_cond_destroy(&queue->is_dequeueable);
	pthread_cond_destroy(&queue->is_enqueueable);

	if (queue->elements != NULL) {
		free(queue->elements);
	}

	queue->start_index = 0;
	queue->length = 0;
	queue->size = 0;
	queue->elements = NULL;

	queue->max_queue_size = 0;
	queue->element_size = 0;

	return 0;
}

static inline int cqueue_lock(struct concurrent_queue * const queue) {
	return pthread_mutex_lock(&queue->mutex);
}

static inline int cqueue_unlock(struct concurrent_queue * const queue) {
	return pthread_mutex_unlock(&queue->mutex);
}

static inline int cqueue_try_enqueue(
	struct concurrent_queue * const queue,
	const void const *p_element
) {
	cqueue_lock(queue);

	if (queue->size == queue->length) {
		// expand the queue.

		size_t new_size = queue->size ? queue->size << 1 : 1;
		
		if (new_size > queue->max_queue_size) {
			cqueue_unlock(queue);
			return EAGAIN;
		}

		void *new_elements = realloc(queue->elements, new_size * queue->element_size);
		
		if (new_elements == NULL) {
			cqueue_unlock(queue);
			return ENOMEM;
		}

		if (queue->size) {
			memcpy(((char*)new_elements) + queue->element_size * queue->size, new_elements, queue->element_size * queue->size);
		}

		queue->elements = new_elements;
		queue->size = new_size;
	}

	memcpy(
		((char*) queue->elements) + queue->element_size*(queue->start_index + queue->length),
		p_element,
		queue->element_size
	);

	queue->length++;

	cqueue_unlock(queue);

	pthread_cond_signal(&queue->is_dequeueable);

	return 0;
}

static inline int cqueue_try_dequeue(
	struct concurrent_queue * const queue,
	void const *element_out
) {
	cqueue_lock(queue);

	if (queue->length == 0) {
		cqueue_unlock(queue);
		return EAGAIN;
	}

	memcpy(
		((char*) queue->elements) + queue->element_size*queue->start_index,
		element_out,
		queue->element_size
	);

	queue->start_index = (queue->start_index + 1) & (queue->size - 1);
	queue->length--;

	cqueue_unlock(queue);

	pthread_cond_signal(&queue->is_enqueueable);

	return 0;
}

static inline int cqueue_enqueue(
	struct concurrent_queue * const queue,
	const void const *p_element
) {
	cqueue_lock(queue);

	if (queue->size == queue->length) {
		// expand the queue or wait for an element to be dequeued.

		size_t new_size = queue->size ? queue->size << 1 : 1;
		if (new_size < queue->max_queue_size) {
			void *new_elements = realloc(queue->elements, new_size * queue->element_size);
			
			if (new_elements == NULL) {
				cqueue_unlock(queue);
				return ENOMEM;
			}

			if (queue->size) {
				memcpy(((char*)new_elements) + queue->element_size * queue->size, new_elements, queue->element_size * queue->size);
			}

			queue->elements = new_elements;
			queue->size = new_size;
		} else {
			do {
				pthread_cond_wait(&queue->is_enqueueable, &queue->mutex);
			} while (queue->size == queue->length);
		}
	}

	memcpy(
		((char*) queue->elements) + queue->element_size*(queue->start_index + queue->length),
		p_element,
		queue->element_size
	);

	queue->length++;

	cqueue_unlock(queue);

	pthread_cond_signal(&queue->is_dequeueable);

	return 0;
}

static inline int cqueue_dequeue(
	struct concurrent_queue *const queue,
	void *const element_out
) {
	cqueue_lock(queue);

	while (queue->length == 0)
		pthread_cond_wait(&queue->is_dequeueable, &queue->mutex);

	memcpy(
		element_out,
		((char*) queue->elements) + queue->element_size*queue->start_index,
		queue->element_size
	);

	queue->start_index = (queue->start_index + 1) & (queue->size - 1);
	queue->length--;

	cqueue_unlock(queue);

	pthread_cond_signal(&queue->is_enqueueable);

	return 0;
}

#endif