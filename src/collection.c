#include <collection.h>

int queue_init(struct queue *queue, size_t element_size, size_t max_queue_size) {
	memset(queue, 0, sizeof(*queue));

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

int queue_deinit(struct queue *queue) {
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

int queue_enqueue(
	struct queue *queue,
	const void *p_element
) {
    size_t new_size;

	if (queue->size == queue->length) {
		// expand the queue or wait for an element to be dequeued.

		new_size = queue->size ? (queue->size << 1) : 1;

		if (new_size < queue->max_queue_size) {
			void *new_elements = realloc(queue->elements, new_size * queue->element_size);
			
			if (new_elements == NULL) {
				return ENOMEM;
			}

			if (queue->size) {
				memcpy(((char*)new_elements) + (queue->element_size*queue->size), new_elements, queue->element_size*queue->size);
			}

			queue->elements = new_elements;
			queue->size = new_size;
		} else {
			return ENOSPC;
		}
	}

	memcpy(
		((char*) queue->elements) + (queue->element_size*((queue->start_index + queue->length) & (queue->size - 1))),
		p_element,
		queue->element_size
	);

	queue->length++;

	return 0;
}

int queue_dequeue(
	struct queue *queue,
	void *element_out
) {
	if (queue->length == 0) {
        return EAGAIN;
    }

	memcpy(
		element_out,
		((char*) queue->elements) + (queue->element_size*queue->start_index),
		queue->element_size
	);

	queue->start_index = (queue->start_index + 1) & (queue->size - 1);
	queue->length--;

	return 0;
}

int queue_peek(
	struct queue *queue,
	void **pelement_out
) {
    if (queue->length == 0) {
        if (pelement_out != NULL) {
            *pelement_out = NULL;
        }
        return EAGAIN;
    }

    if (pelement_out != NULL) {
        *pelement_out = ((char*) queue->elements) + (queue->element_size*queue->start_index);
    }

	return 0;
}


int cqueue_init(
    struct concurrent_queue *queue,
    size_t element_size,
    size_t max_queue_size
) {
    int ok;
    
    memset(queue, 0, sizeof(*queue));

	ok = queue_init(&queue->queue, element_size, max_queue_size);
    if (ok != 0) {
        return ok;
    }

    pthread_mutex_init(&queue->mutex, NULL);
    pthread_cond_init(&queue->is_enqueueable, NULL);
    pthread_cond_init(&queue->is_dequeueable, NULL);

	return 0;
}

int cqueue_deinit(struct concurrent_queue *queue) {
    queue_deinit(&queue->queue);
    pthread_mutex_destroy(&queue->mutex);
    pthread_cond_destroy(&queue->is_enqueueable);
    pthread_cond_destroy(&queue->is_dequeueable);
}

int cqueue_try_enqueue_locked(
    struct concurrent_queue *queue,
    const void *p_element
) {
    return queue_enqueue(&queue->queue, p_element);
}

int cqueue_enqueue_locked(
	struct concurrent_queue *queue,
	const void *p_element
) {
    int ok;
    
    while (ok = queue_enqueue(&queue->queue, p_element), ok == ENOSPC) {
        ok = pthread_cond_wait(&queue->is_enqueueable, &queue->mutex);
        if (ok != 0) {
            return ok;
        }
    }

    return ok;
}

int cqueue_try_enqueue(
	struct concurrent_queue *queue,
	const void *p_element
) {
    int ok;

	cqueue_lock(queue);

	ok = cqueue_try_enqueue_locked(queue, p_element);
    if (ok != 0) {
        cqueue_unlock(queue);
        return ok;
    }

	cqueue_unlock(queue);

	pthread_cond_signal(&queue->is_dequeueable);

	return 0;
}

int cqueue_enqueue(
	struct concurrent_queue *queue,
	const void *p_element
) {
	int ok;

	cqueue_lock(queue);

    ok = cqueue_enqueue_locked(queue, p_element);
    if (ok != 0) {
        cqueue_unlock(queue);
        return ok;
    }

	cqueue_unlock(queue);

	pthread_cond_signal(&queue->is_dequeueable);

	return 0;
}


int cqueue_try_dequeue_locked(
	struct concurrent_queue *queue,
	void *element_out
) {
    int ok;
    
    ok = queue_dequeue(&queue->queue, element_out);
    if (ok == 0) {
        pthread_cond_signal(&queue->is_enqueueable);
    }
}

int cqueue_dequeue_locked(
	struct concurrent_queue *queue,
	void *element_out
) {
    int ok;
    
    while (ok = queue_dequeue(&queue->queue, element_out), ok == EAGAIN) {
        pthread_cond_wait(&queue->is_dequeueable, &queue->mutex);
    }

    if (ok == 0) {
        pthread_cond_signal(&queue->is_enqueueable);
    }

    return ok;
}

int cqueue_try_dequeue(
	struct concurrent_queue *queue,
	void *element_out
) {
    int ok;

    cqueue_lock(queue);

	ok = cqueue_try_dequeue_locked(queue, element_out);

	cqueue_unlock(queue);

	return ok;
}

int cqueue_dequeue(
    struct concurrent_queue *queue,
    void *element_out
) {
    int ok;

    cqueue_lock(queue);

	ok = cqueue_dequeue_locked(queue, element_out);

	cqueue_unlock(queue);

	return ok;
}


int cqueue_peek_locked(
	struct concurrent_queue *queue,
	void **pelement_out
) {
    return queue_peek(&queue->queue, pelement_out);
}