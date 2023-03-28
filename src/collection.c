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

	DEBUG_ASSERT_NOT_NULL(queue);
	DEBUG_ASSERT_NOT_NULL(p_element);

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
	DEBUG_ASSERT_NOT_NULL(queue);

	if (queue->length == 0) {
        return EAGAIN;
    }

	if (element_out != NULL) {
		memcpy(
			element_out,
			((char*) queue->elements) + (queue->element_size*queue->start_index),
			queue->element_size
		);
	}

	queue->start_index = (queue->start_index + 1) & (queue->size - 1);
	queue->length--;

	return 0;
}

int queue_peek(
	struct queue *queue,
	void **pelement_out
) {
	DEBUG_ASSERT_NOT_NULL(queue);

    if (queue->length == 0) {
        if (pelement_out != NULL) {
            *pelement_out = NULL;
        }
        return EAGAIN;
    }

    if (pelement_out != NULL) {
        *pelement_out = ((char*) (queue->elements)) + (queue->element_size*queue->start_index);
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
	return 0;
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

	return 0;
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


int pset_init(
	struct pointer_set *set,
	size_t max_size
) {
	void **storage = (void**) calloc(2, sizeof(void*));
	if (storage == NULL) {
		return ENOMEM;
	}

	memset(set, 0, sizeof(*set));

	set->count_pointers = 0;
	set->size = 2;
	set->pointers = storage;
	set->max_size = max_size;
	set->is_static = false;

	return 0;
}

int pset_init_static(
	struct pointer_set *set,
	void **storage,
	size_t size
) {
	if (storage == NULL) {
		return EINVAL;
	}

	memset(set, 0, sizeof *set);

	set->count_pointers = 0;
	set->size = size;
	set->pointers = storage;
	set->max_size = size;
	set->is_static = true;

	return 0;
}

void pset_deinit(
	struct pointer_set *set
) {
	if ((set->is_static == false) && (set->pointers != NULL)) {
		free(set->pointers);
	}

	set->count_pointers = 0;
	set->size = 0;
	set->pointers = NULL;
	set->max_size = 0;
	set->is_static = false;
}

int pset_put(
	struct pointer_set *set,
	void *pointer
) {
	size_t new_size;
	int index;

	index = -1;
	for (int i = 0; i < set->size; i++) {
		if (set->pointers[i] == NULL) {
			index = i;
		} else if (pointer == set->pointers[i]) {
			return 0;
		}
	}
	
	if (index != -1) {
		set->pointers[index] = pointer;
		set->count_pointers++;
		return 0;
	}
	
	if (set->is_static) {
		return ENOSPC;
	} else {
		new_size = set->size ? set->size << 1 : 1;

		if (new_size < set->max_size) {
			void **new_pointers = (void**) realloc(set->pointers, new_size * sizeof(void*));
			if (new_pointers == NULL) {
				return ENOMEM;
			}

			memset(new_pointers + set->size, 0, (new_size - set->size) * sizeof(void*));

			new_pointers[set->size] = pointer;
			set->count_pointers++;

			set->pointers = new_pointers;
			set->size = new_size;
		} else {
			return ENOSPC;
		}
	} 

	return 0;
}

bool pset_contains(
	const struct pointer_set *set,
	const void *pointer
) {
	for (int i = 0; i < set->size; i++) {
		if ((set->pointers[i] != NULL) && (set->pointers[i] == pointer)) {
			return true;
		}
	}

	return false;
}

int pset_remove(
	struct pointer_set *set,
	const void *pointer
) {
	for (int i = 0; i < set->size; i++) {
		if ((set->pointers[i] != NULL) && (set->pointers[i] == pointer)) {
			set->pointers[i] = NULL;
			set->count_pointers--;
			
			return 0;
		}
	}

	return EINVAL;
}

int pset_copy(
	const struct pointer_set *src,
	struct pointer_set *dest
) {
	if (dest->size < src->count_pointers) {
		if (dest->max_size < src->count_pointers) {
			return ENOSPC;
		} else {
			void *new_pointers = realloc(dest->pointers, src->count_pointers);
			if (new_pointers == NULL) {
				return ENOMEM;
			}

			dest->pointers = new_pointers;
			dest->size = src->count_pointers;
		}
	}

	if (dest->size >= src->size) {
		memcpy(dest->pointers, src->pointers, src->size * sizeof(void*));

		if (dest->size > src->size) {
			memset(dest->pointers + src->size, 0, (dest->size - src->size) * sizeof(void*));
		}
	} else {
		for (int i = 0, j = 0; i < src->size; i++) {
			if (src->pointers[i] != NULL) {
				dest->pointers[j] = src->pointers[i];
				j++;
			}
		}
	}

	dest->count_pointers = src->count_pointers;

	return 0;
}

void pset_intersect(
	struct pointer_set *src_dest,
	const struct pointer_set *b
) {
	for (int i = 0, j = 0; (i < src_dest->size) && (j < src_dest->count_pointers); i++) {
		if (src_dest->pointers[i] != NULL) {
			if (pset_contains(b, src_dest->pointers[i]) == false) {
				src_dest->pointers[i] = NULL;
				src_dest->count_pointers--;
			} else {
				j++;
			}
		}
	}
}

int pset_union(
	struct pointer_set *src_dest,
	const struct pointer_set *b
) {
	int ok;

	for (int i = 0, j = 0; (i < b->size) && (j < b->size); i++) {
		if (b->pointers[i] != NULL) {
			ok = pset_put(src_dest, b->pointers[i]);
			if (ok != 0) {
				return ok;
			}

			j++;
		}
	}

	return 0;
}

void *__pset_next_pointer(
	struct pointer_set *set,
	const void *pointer
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


int cpset_init(
	struct concurrent_pointer_set *set,
	size_t max_size
) {
	int ok;

	ok = pset_init(&set->set, max_size);
	if (ok != 0) {
		return ok;
	}

	ok = pthread_mutex_init(&set->mutex, NULL);
	if (ok < 0) {
		return errno;
	}

	return 0;
}

void cpset_deinit(struct concurrent_pointer_set *set) {
	pthread_mutex_destroy(&set->mutex);
	pset_deinit(&set->set);
}

static pthread_mutexattr_t default_mutex_attrs;

static void init_default_mutex_attrs() {
	pthread_mutexattr_init(&default_mutex_attrs);
#ifdef DEBUG
	pthread_mutexattr_settype(&default_mutex_attrs, PTHREAD_MUTEX_ERRORCHECK);
#endif
}

const pthread_mutexattr_t *get_default_mutex_attrs() {
	static pthread_once_t init_once_ctl = PTHREAD_ONCE_INIT;

	pthread_once(&init_once_ctl, init_default_mutex_attrs);

	return &default_mutex_attrs;
}
