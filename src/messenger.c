#include <semaphore.h>

#include <flutter-pi.h>
#include <platformchannel.h>
#include <messenger.h>
#include <collection.h>


struct platch_listener_data {
	char *channel;

	bool is_raw;
	union {
		platch_message_callback_t message_callback;
		struct {
			enum platch_codec codec;
			error_or_platch_obj_callback_t platch_obj_callback;
			error_or_platch_obj_callback_t error_callback;
		};
	};

	void *userdata;
};

struct flutter_messenger {
	runs_platform_tasks_on_current_thread_t runs_platform_tasks_on_current_thread;

	post_platform_task_t post_platform_task;
	flutter_engine_send_platform_message_t send_platform_message;
	flutter_platform_message_create_response_handle_t create_response_handle;
	flutter_platform_message_release_response_handle_t release_response_handle;
	flutter_engine_send_platform_message_response_t send_response;
	struct flutterpi *flutterpi;
	FlutterEngine engine;

	struct concurrent_pointer_set listeners;
	bool platform_task_thread_owns_listeners_mutex;
};

struct deferred_send_or_respond_data {
	struct flutter_messenger *fm;

	/**
	 * @brief Whether the message should be send to channel @ref target_channel or
	 * send as a response to @ref target_handle.
	 */
	bool is_response;

	union {
		struct {
			char *target_channel;
			data_callback_t response_callback;
			void *response_callback_userdata;
		};
		struct {
			const FlutterPlatformMessageResponseHandle *target_handle;
		};
	};

	/**
	 * Called after the message was handed over to the engine, or if something goes
	 * wrong in the process of handing the message over to the engine.
	 */
	shipped_callback_t shipped_callback;
	void *shipped_callback_userdata;

	uint8_t *message;
	size_t message_size;
};

struct response_callback_data {
	enum platch_codec decoding_codec;
	raw_response_callback_t response_callback;
	raw_response_callback_t error_callback;
	void *userdata;
};

struct flutter_message_response_handle {
	struct flutter_messenger *fm;
	const FlutterPlatformMessageResponseHandle *flutter_handle;
};

struct flutter_messenger *fm_new(
	runs_platform_tasks_on_current_thread_t runs_platform_tasks_on_current_thread,
	post_platform_task_t post_platform_task,
	flutter_engine_send_platform_message_t send_platform_message,
	flutter_platform_message_create_response_handle_t create_response_handle,
	flutter_platform_message_release_response_handle_t release_response_handle,
	flutter_engine_send_platform_message_response_t send_response,
	struct flutterpi *flutterpi,
	FlutterEngine engine
) {
	struct flutter_messenger *fm;
	int ok;

	fm = malloc(sizeof *fm);
	if (fm == NULL) {
		return NULL;
	}

	ok = cpset_init(&fm->listeners, CPSET_DEFAULT_MAX_SIZE);
	if (ok != 0) {
		free(fm);
		return NULL;
	}

	fm->runs_platform_tasks_on_current_thread = runs_platform_tasks_on_current_thread;
	fm->post_platform_task = post_platform_task;
	fm->send_platform_message = send_platform_message;
	fm->create_response_handle = create_response_handle;
	fm->release_response_handle = release_response_handle;
	fm->send_response = send_response;
	fm->flutterpi = flutterpi;
	fm->engine = engine;
	fm->platform_task_thread_owns_listeners_mutex = false;

	return fm;
}

int fm_destroy(struct flutter_messenger *fm) {
	struct platch_listener_data *data;

	for_each_pointer_in_cpset(&fm->listeners, data) {
		free(data->channel);
		free(data);
	}
	
	cpset_deinit(&fm->listeners);
	free(fm);
	return 0;
}

static inline bool runs_platform_tasks_on_current_thread(struct flutter_messenger *fm) {
	return fm->runs_platform_tasks_on_current_thread(fm->flutterpi);
}

int fm_on_platform_message(
	struct flutter_messenger *fm,
	const FlutterPlatformMessageResponseHandle *flutter_responsehandle,
	const char *channel,
	const uint8_t *message,
	size_t message_size
) {
	struct flutter_message_response_handle *handle;
	struct platch_listener_data *data;
	struct platch_obj obj;
	int ok;

	cpset_lock(&fm->listeners);
	fm->platform_task_thread_owns_listeners_mutex = true;

	for_each_pointer_in_cpset(&fm->listeners, data) {
		if (strcmp(data->channel, channel) == 0) {
			break;
		}
	}

	if (data == NULL) {
		ok = EINVAL;
		goto fail_unlock_listeners;
	}
	
	if ((data->is_raw && data->message_callback) || (!data->is_raw && (data->platch_obj_callback || data->error_callback))) {
		handle = malloc(sizeof *handle);
		if (handle == NULL) {
			ok = ENOMEM;
			goto fail_unlock_listeners;
		}

		handle->flutter_handle = flutter_responsehandle;
		handle->fm = fm;
		
		if (data->is_raw) {
			data->message_callback(
				handle,
				data->channel,
				message,
				message_size,
				data->userdata
			);
			// from this point on, all access to data is invalid.
		} else {
			/// FIXME: proper const propagation
			ok = platch_decode((uint8_t*) message, message_size, data->codec, &obj);
			if (ok != 0) {
				if (data->error_callback) {
					data->error_callback(false, handle, data->channel, &obj, data->userdata);
					// from this point on, all access to data is invalid.
				} else {
					fprintf(stderr, "[flutter messenger] Error decoding platform message on channel \"%s\": platch_decode: %s", data->channel, strerror(ok));
					fm_respond_blocking(handle, &(struct platch_obj) {.codec = kNotImplemented});
				}
			} else {
				if (data->platch_obj_callback) {
					data->platch_obj_callback(true, handle, data->channel, &obj, data->userdata);
					// from this point on, all access to data is invalid.
				} else {
					fm_respond_blocking(handle, &(struct platch_obj) {.codec = kNotImplemented});
				}

				platch_free_obj(&obj);
			}
		}
	}


	fm->platform_task_thread_owns_listeners_mutex = false;
	cpset_unlock(&fm->listeners);

	return 0;

	fail_unlock_listeners:
	fm->platform_task_thread_owns_listeners_mutex = false;
	cpset_unlock(&fm->listeners);
	return ok;
}

/**
 * Internal functions, *must* be called on the main / platform task thread.
 * These do the actual sending / responding
 */
static int send_raw(
	struct flutter_messenger *fm,
	const char *channel,
	const uint8_t *message,
	size_t message_size,
	data_callback_t response_callback,
	void *callback_userdata
) {
	FlutterPlatformMessageResponseHandle *response_handle;
	FlutterEngineResult engine_result;
	int ok;

	if (response_callback != NULL) {
		engine_result = fm->create_response_handle(
			fm->engine,
			response_callback,
			callback_userdata,
			&response_handle
		);
		if (engine_result != kSuccess) {
			fprintf(stderr, "[flutter messenger] Could not create response handle. fm->create_response_handle: %s\n", FLUTTER_RESULT_TO_STRING(engine_result));
			ok = EIO;
			goto fail_return_ok;
		}
	} else {
		response_handle = NULL;
	}

	engine_result = fm->send_platform_message(
		fm->engine,
		&(const FlutterPlatformMessage) {
			.struct_size = sizeof(FlutterPlatformMessage),
			.channel = channel,
			.message = message,
			.message_size = message_size,
			.response_handle = response_handle
		}
	);
	if (engine_result != kSuccess) {
		fprintf(stderr, "[flutter messenger] Could not send platform message. fm->send_platform_message: %s\n", FLUTTER_RESULT_TO_STRING(engine_result));
		ok = EIO;
		goto fail_maybe_release_response_handle;
	}

	if (response_handle != NULL) {
		fm->release_response_handle(fm->engine, response_handle);
		response_handle = NULL;
	}

	return 0;


	fail_maybe_release_response_handle:
	if (response_handle != NULL) {
		fm->release_response_handle(fm->engine, response_handle);
		response_handle = NULL;
	}

	fail_return_ok:
	return ok;
}

static int respond_raw(
	struct flutter_messenger *fm,
	const FlutterPlatformMessageResponseHandle *handle,
	const uint8_t *message,
	size_t message_size
) {
	FlutterEngineResult engine_result;

	engine_result = fm->send_response(
		fm->engine,
		handle,
		message,
		message_size
	);
	if (engine_result != kSuccess) {
		fprintf(stderr, "[flutter messenger] Could not create response handle. fm->send_response: %s\n", FLUTTER_RESULT_TO_STRING(engine_result));
		return EIO;
	}

	return 0;
}

/**
 * Used as a callback when @ref fm_send_raw or @ref fm_respond_raw need to defer the
 * sending / responding using fm->post_platform_task because they weren't called
 * on the platform task thread.
 */
static int on_execute_send_or_respond_raw(
	void *userdata
) {
	struct deferred_send_or_respond_data *data;
	int ok;

	data = userdata;

	if (data->is_response == false) {
		ok = send_raw(
			data->fm,
			data->target_channel,
			data->message,
			data->message_size,
			data->response_callback,
			data->response_callback_userdata
		);
	} else {
		ok = respond_raw(
			data->fm,
			data->target_handle,
			data->message,
			data->message_size
		);
	}

	if (data->shipped_callback != NULL) {
		data->shipped_callback(ok == 0, data->shipped_callback_userdata);
	}

	if (data->message != NULL) {
		free(data->message);
	}
	free(data->target_channel);
	free(data);

	return ok;
}

/**
 * SEND RAW NONBLOCKING ZEROCOPY
 */
int fm_send_raw_zerocopy_nonblocking(
	struct flutter_messenger *fm,
	const char *channel,
	const uint8_t *message,
	size_t message_size,
	raw_response_callback_t response_callback,
	void *response_callback_userdata,
	shipped_callback_t shipped_callback,
	void *shipped_callback_userdata
) {
	struct deferred_send_or_respond_data *deferred_data;
	int ok;

	/// are we the on the right thread?
	if (fm->runs_platform_tasks_on_current_thread(fm->flutterpi)) {
		ok = send_raw(
			fm,
			channel,
			message,
			message_size,
			response_callback,
			response_callback_userdata
		);
		if (ok != 0) {
			goto fail_return_ok;
		}
		
		if (shipped_callback != NULL) {
			shipped_callback(true, shipped_callback_userdata);
		}
	} else {
		deferred_data = malloc(sizeof *deferred_data);
		if (deferred_data == NULL) {
			ok = ENOMEM;
			goto fail_return_ok;
		}

		deferred_data->fm = fm;
		deferred_data->is_response = false;
		deferred_data->target_channel = (char *) channel;
		deferred_data->response_callback = response_callback;
		deferred_data->response_callback_userdata = response_callback_userdata;
		deferred_data->shipped_callback = shipped_callback;
		deferred_data->shipped_callback_userdata = shipped_callback_userdata;
		deferred_data->message = (uint8_t*) message;
		deferred_data->message_size = message_size;

		ok = fm->post_platform_task(
			fm->flutterpi,
			on_execute_send_or_respond_raw,
			deferred_data
		);
		if (ok != 0) {
			goto fail_free_deferred_data;
		}
	}

	return 0;


	fail_free_deferred_data:
	free(deferred_data);

	fail_return_ok:
	return ok;
}

/**
 * SEND RAW NONBLOCKING
 */
struct raw_nonblocking_metadata {
	void *channel, *message;
	error_or_raw_response_callback_t response_callback;
	void *response_callback_userdata;
	error_or_raw_response_callback_t error_callback;
	void *error_callback_userdata;
};

static void on_send_raw_nonblocking__raw_response(const uint8_t *data, size_t size, void *userdata) {
	struct raw_nonblocking_metadata *metadata;

	metadata = userdata;

	metadata->response_callback(true, data, size, metadata->response_callback_userdata);

	free(metadata);
}

static void on_send_raw_nonblocking__shipped(bool success, void *userdata) {
	struct raw_nonblocking_metadata *data;

	data = userdata;

	if (data->channel != NULL) {
		free(data->channel);
	}

	if (data->message != NULL) {
		free(data->message);
	}

	if ((success == false) && (data->error_callback != NULL)) {
		data->error_callback(false, NULL, 0, data->error_callback_userdata);
	}

	if ((success == false) || (data->response_callback == NULL)) {
		// There will be no response_callback called, so noone
		// else needs the data.
		free(data);
	}
}

int fm_send_raw_nonblocking(
	struct flutter_messenger *fm,
	const char *channel,
	const uint8_t *message,
	size_t message_size,
	error_or_raw_response_callback_t response_callback,
	void *response_callback_userdata,
	error_or_raw_response_callback_t error_callback,
	void *error_callback_userdata
) {
	struct raw_nonblocking_metadata *metadata;
	uint8_t *duped_message;
	char *duped_channel;
	int ok;

	metadata = NULL;
	duped_channel = NULL;
	duped_message = NULL;

	if (!fm->runs_platform_tasks_on_current_thread(fm->flutterpi) || response_callback != NULL) {
		metadata = malloc(sizeof *metadata);
		if (metadata == NULL) {
			ok = ENOMEM;
			goto fail_return_ok;
		}

		duped_channel = strdup(channel);
		if (duped_channel == NULL) {
			ok = ENOMEM;
			goto fail_maybe_free_metadata;
		}

		if ((message != NULL) && (message_size > 0)) {
			duped_message = memdup(message, message_size);
			if (duped_message == NULL) {
				ok = ENOMEM;
				goto fail_maybe_free_duped_channel;
			}
		} else {
			duped_message = NULL;
		}

		metadata->channel = duped_channel;
		metadata->message = duped_message;
		metadata->response_callback = response_callback;
		metadata->response_callback_userdata = response_callback_userdata;
		metadata->error_callback = error_callback;
		metadata->error_callback_userdata = error_callback_userdata;
	}

	/// are we the on the right thread?
	if (fm->runs_platform_tasks_on_current_thread(fm->flutterpi)) {
		ok = send_raw(
			fm,
			channel,
			message,
			message_size,
			response_callback != NULL ? on_send_raw_nonblocking__raw_response : NULL,
			metadata
		);
		if (ok != 0) {
			goto fail_maybe_free_duped_message;
		}
	} else {
		ok = fm_send_raw_zerocopy_nonblocking(
			fm,
			duped_channel,
			duped_message,
			message_size,
			response_callback != NULL ? on_send_raw_nonblocking__raw_response : NULL,
			metadata,
			on_send_raw_nonblocking__shipped,
			metadata
		);
		if (ok != 0) {
			goto fail_maybe_free_duped_message;
		}
	}

	return 0;


	fail_maybe_free_duped_message:
	if (duped_message != NULL) {
		free(duped_message);
	}

	fail_maybe_free_duped_channel:
	if (duped_channel != NULL) {
		free(duped_channel);
	}

	fail_maybe_free_metadata:
	if (metadata != NULL) {
		free(metadata);
	}

	fail_return_ok:
	return ok;
}

/**
 * SEND RAW BLOCKING
 */
struct raw_blocking_metadata {
	bool success;
	sem_t shipped;
};

static void on_send_raw_blocking__shipped(bool success, void *userdata) {
	struct raw_blocking_metadata *metadata;

	metadata = userdata;

	metadata->success = success;

	sem_post(&metadata->shipped);
}

int fm_send_raw_blocking(
	struct flutter_messenger *fm,
	const char *channel,
	const uint8_t *message,
	size_t message_size,
	raw_response_callback_t response_callback,
	void *response_callback_userdata
) {
	struct raw_blocking_metadata metadata;
	int ok;

	if (fm->runs_platform_tasks_on_current_thread(fm->flutterpi)) {
		ok = send_raw(
			fm,
			channel,
			message,
			message_size,
			response_callback,
			response_callback_userdata
		);
		if (ok != 0) {
			goto fail_return_ok;
		}
	} else {
		metadata.success = true;
		sem_init(&metadata.shipped, 0, 0);

		ok = fm_send_raw_zerocopy_nonblocking(
			fm,
			channel,
			message,
			message_size,
			response_callback,
			response_callback_userdata,
			on_send_raw_blocking__shipped,
			&metadata
		);
		if (ok != 0) {
			goto fail_deinit_semaphore;
		}

		do {
			ok = sem_wait(&metadata.shipped);
		} while ((ok == -1) && (ok = errno, ok == EINTR));

		if (ok != 0) {
			goto fail_deinit_semaphore;
		}

		sem_destroy(&metadata.shipped);

		return metadata.success ? 0 : EIO;
	}

	return 0;


	fail_deinit_semaphore:
	sem_destroy(&metadata.shipped);

	fail_return_ok:
	return ok;
}


/**
 * RESPOND RAW ZEROCOPY NONBLOCKING
 */
int fm_respond_raw_zerocopy_nonblocking(
	struct flutter_message_response_handle *handle,
	const uint8_t *message,
	size_t message_size,
	shipped_callback_t shipped_callback,
	void *userdata
) {
	struct deferred_send_or_respond_data *deferred_data;
	int ok;

	/// are we the on the right thread?
	if (handle->fm->runs_platform_tasks_on_current_thread(handle->fm->flutterpi)) {
		ok = respond_raw(
			handle->fm,
			handle->flutter_handle,
			message,
			message_size
		);
		if (ok != 0) {
			return ok;
		}

		free(handle);

		return 0;
	} else {
		deferred_data = malloc(sizeof *deferred_data);
		if (deferred_data == NULL) {
			ok = ENOMEM;
			goto fail_return_ok;
		}

		deferred_data->fm = handle->fm;
		deferred_data->is_response = true;
		deferred_data->target_handle = handle->flutter_handle;
		deferred_data->shipped_callback = shipped_callback;
		deferred_data->shipped_callback_userdata = userdata;
		/// TODO: remove the cast. It's okay though since we know we'll not modify it.
		deferred_data->message = (uint8_t *) message;
		deferred_data->message_size = message_size;

		ok = handle->fm->post_platform_task(
			handle->fm->flutterpi,
			on_execute_send_or_respond_raw,
			deferred_data
		);
		if (ok != 0) {
			goto fail_free_deferred_data;
		}

		free(handle);
	}


	return 0;


	fail_free_deferred_data:
	free(deferred_data);

	fail_return_ok:
	return ok;
}

/**
 * RESPOND RAW NONBLOCKING
 */
struct respond_raw_nonblocking_metadata {
	void *message;
	void_callback_t error_callback;
	void *userdata;
};

static void on_respond_raw_nonblocking__shipped(bool success, void *userdata) {
	struct respond_raw_nonblocking_metadata *metadata;

	metadata = userdata;

	if (metadata->message != NULL) {
		free(metadata->message);
	}

	if ((success == false) && (metadata->error_callback != NULL)) {
		metadata->error_callback(metadata->userdata);
	}

	free(metadata);
}

/**
 * @brief Send raw platform message data as a response to @ref handle, but copy the message internally.
 * 
 * @param fm The flutter messenger that should send the message.
 * @param message The message to send. Can be NULL for a "missing plugin" message.
 * @param message_size The size of the message in bytes. Must be `0` when `message == NULL`.
 * @param error_callback Called on the platform task thread when an error ocurred while handing
 *                       the message over to the flutter engine.
 * @param error_callback_userdata The userdata to pass to @ref error_callback.
 */
int fm_respond_raw_nonblocking(
	struct flutter_message_response_handle *handle,
	const uint8_t *message,
	size_t message_size,
	void_callback_t error_callback,
	void *error_callback_userdata
) {
	struct respond_raw_nonblocking_metadata *metadata;
	uint8_t *duped_message;
	int ok;

	if (handle->fm->runs_platform_tasks_on_current_thread(handle->fm->flutterpi)) {
		ok = respond_raw(
			handle->fm,
			handle->flutter_handle,
			message,
			message_size
		);
		if (ok != 0) {
			goto fail_return_ok;
		}
	} else {
		metadata = malloc(sizeof *metadata);
		if (metadata == NULL) {
			ok = ENOMEM;
			goto fail_return_ok;
		}

		if ((message != NULL) && (message_size > 0)) {
			duped_message = memdup(message, message_size);
			if (duped_message == NULL) {
				ok = ENOMEM;
				goto fail_free_metadata;
			}
		} else {
			duped_message = NULL;
		}

		metadata->message = duped_message;
		metadata->error_callback = error_callback;
		metadata->userdata = error_callback_userdata;

		ok = fm_respond_raw_zerocopy_nonblocking(
			handle,
			duped_message,
			message_size,
			on_respond_raw_nonblocking__shipped,
			metadata
		);
		if (ok != 0) {
			goto fail_maybe_free_duped_message;
		}
	}

	return 0;


	fail_maybe_free_duped_message:
	if (duped_message != NULL) {
		free(duped_message);
	}

	fail_free_metadata:
	free(metadata);

	fail_return_ok:
	return ok;
}

/**
 * RESPOND RAW BLOCKING
 */
struct respond_raw_blocking_metadata {
	bool success;
	sem_t shipped;
};

static void on_respond_raw_blocking__shipped(bool success, void *userdata) {
	struct respond_raw_blocking_metadata *metadata;

	metadata = userdata;

	metadata->success = success;

	sem_post(&metadata->shipped);
}

int fm_respond_raw_blocking(
	struct flutter_message_response_handle *handle,
	const uint8_t *message,
	size_t message_size
) {
	struct respond_raw_blocking_metadata metadata;
	int ok;

	if (handle->fm->runs_platform_tasks_on_current_thread(handle->fm->flutterpi)) {
		ok = respond_raw(
			handle->fm,
			handle->flutter_handle,
			message,
			message_size
		);
		if (ok != 0) {
			goto fail_return_ok;
		}
	} else {
		metadata.success = true;
		sem_init(&metadata.shipped, 0, 0);

		ok = fm_respond_raw_zerocopy_nonblocking(
			handle,
			message,
			message_size,
			on_respond_raw_blocking__shipped,
			&metadata
		);
		if (ok != 0) {
			goto fail_deinit_semaphore;
		}

		do {
			ok = sem_wait(&metadata.shipped);
		} while ((ok == -1) && (ok = errno, ok == EINTR));

		if (ok != 0) {
			goto fail_deinit_semaphore;
		}

		sem_destroy(&metadata.shipped);

		return metadata.success ? 0 : EIO;
	}

	return 0;


	fail_deinit_semaphore:
	sem_destroy(&metadata.shipped);

	fail_return_ok:
	return ok;
}


struct send_metadata {
	bool is_allocated;
	enum platch_codec codec;
	error_or_response_callback_t response_callback;
	error_or_response_callback_t error_callback;
	void *userdata;
};

static void on_send_nonblocking__raw_response(const uint8_t *data, size_t data_size, void *userdata) {
	struct send_metadata *metadata;
	struct platch_obj obj;
	int ok;

	metadata = userdata;

	if (metadata->response_callback != NULL) {
		/// TODO: Make platform channel API const-able
		ok = platch_decode((uint8_t *) data, data_size, metadata->codec, &obj);
		if (ok != 0) {
			if (metadata->error_callback) {
				metadata->error_callback(false, NULL, metadata->userdata);
			}
			return;
		}

		metadata->response_callback(true, &obj, metadata->userdata);

		platch_free_obj(&obj);
	}

	free(metadata);
}

static void on_send_nonblocking__error_or_raw_response(bool success, const uint8_t *data, size_t data_size, void *userdata) {
	struct send_metadata *metadata;
	struct platch_obj obj;
	int ok;

	metadata = userdata;

	if (success) {
		if (metadata->response_callback != NULL) {
			/// TODO: Make platform channel API const-able
			ok = platch_decode((uint8_t*) data, data_size, metadata->codec, &obj);
			if (ok != 0) {
				if (metadata->error_callback) {
					metadata->error_callback(false, NULL, metadata->userdata);
				}
				return;
			}

			metadata->response_callback(true, &obj, metadata->userdata);

			platch_free_obj(&obj);
		}
	} else {
		if (metadata->error_callback != NULL) {
			metadata->error_callback(false, NULL, metadata->userdata);
		}
	}

	free(metadata);
}

/**
 * Sending messages
 */
int fm_send_nonblocking(
	struct flutter_messenger *fm,
	const char *channel,
	const struct platch_obj *object,
	enum platch_codec response_codec,
	error_or_response_callback_t response_callback,
	error_or_response_callback_t error_callback,
	void *userdata
) {
	struct send_metadata *metadata;
	uint8_t *buffer;
	size_t size;
	int ok;

	if ((response_callback != NULL) || (error_callback != NULL)) {
		metadata = malloc(sizeof *metadata);
		if (metadata == NULL) {
			ok = ENOMEM;
			goto fail_return_ok;
		}

		metadata->is_allocated = true;
		metadata->codec = response_codec;
		metadata->response_callback = response_callback;
		metadata->error_callback = error_callback;
		metadata->userdata = userdata;
	} else {
		metadata = NULL;
	}

	/// TODO: Make platform channel API const-able
	ok = platch_encode((struct platch_obj *) object, &buffer, &size);
	if (ok != 0) {
		goto fail_maybe_free_metadata;
	}

	ok = fm_send_raw_nonblocking(
		fm,
		channel,
		buffer,
		size,
		metadata != NULL? on_send_nonblocking__error_or_raw_response : NULL,
		metadata,
		metadata != NULL? on_send_nonblocking__error_or_raw_response : NULL,
		metadata
	);
	if (ok != 0) {
		goto fail_free_obj;
	}

	if (object->codec != kBinaryCodec) {
		free(buffer);
	}

	return 0;


	fail_free_obj:
	if (object->codec != kBinaryCodec) {
		free(buffer);
	}

	fail_maybe_free_metadata:
	if (metadata != NULL) {
		free(metadata);
	}

	fail_return_ok:
	return ok;
}

int fm_send_blocking(
	struct flutter_messenger *fm,
	const char *channel,
	const struct platch_obj *object,
	enum platch_codec response_codec,
	error_or_response_callback_t response_callback,
	error_or_response_callback_t error_callback,
	void *userdata
) {
	struct send_metadata *metadata;
	uint8_t *buffer;
	size_t size;
	int ok;

	if ((response_callback != NULL) || (error_callback != NULL)) {
		metadata = malloc(sizeof *metadata);
		if (metadata == NULL) {
			ok = ENOMEM;
			goto fail_return_ok;
		}

		metadata->is_allocated = true;
		metadata->codec = response_codec;
		metadata->response_callback = response_callback;
		metadata->error_callback = error_callback;
		metadata->userdata = userdata;
	} else {
		metadata = NULL;
	}

	/// TODO: Make platform channel API const-able
	ok = platch_encode((struct platch_obj *) object, &buffer, &size);
	if (ok != 0) {
		goto fail_maybe_free_metadata;
	}

	ok = fm_send_raw_blocking(
		fm,
		channel,
		buffer,
		size,
		metadata != NULL? on_send_nonblocking__raw_response : NULL,
		&metadata
	);
	if (ok != 0) {
		goto fail_maybe_free_metadata;
	}

	if (object->codec != kBinaryCodec) {
		free(buffer);
	}

	return 0;


	fail_maybe_free_metadata:
	if (metadata != NULL) {
		free(metadata);
	}

	fail_return_ok:
	return ok;
}

int fm_respond_nonblocking(
	struct flutter_message_response_handle *handle,
	const struct platch_obj *object,
	void_callback_t error_callback,
	void *userdata
) {
	uint8_t *buffer;
	size_t size;
	int ok;

	/// TODO: Make platform channel API const-able
	ok = platch_encode((struct platch_obj *) object, &buffer, &size);
	if (ok != 0) return ok;

	ok = fm_respond_raw_nonblocking(
		handle,
		buffer,
		size,
		error_callback,
		userdata
	);

	if (object->codec != kBinaryCodec) {
		free(buffer);
	}

	return ok;
}

int fm_respond_blocking(
	struct flutter_message_response_handle *handle,
	const struct platch_obj *object
) {
	uint8_t *buffer;
	size_t size;
	int ok;

	/// TODO: Make platform channel API const-able
	ok = platch_encode((struct platch_obj *) object, &buffer, &size);
	if (ok != 0) return ok;

	ok = fm_respond_raw_blocking(
		handle,
		buffer,
		size
	);

	if (object->codec != kBinaryCodec) {
		free(buffer);
	}

	return ok;
}


static int set_listener(
	struct flutter_messenger *fm,
	const char *channel,
	bool is_raw,
	platch_message_callback_t message_callback,
	enum platch_codec codec,
	error_or_platch_obj_callback_t platch_obj_callback,
	error_or_platch_obj_callback_t error_callback,
	void *userdata
) {
	struct platch_listener_data *data;
	bool should_unlock;
	int ok;

	if (runs_platform_tasks_on_current_thread(fm) && fm->platform_task_thread_owns_listeners_mutex) {
		should_unlock = false;
	} else {
		cpset_lock(&fm->listeners);
		should_unlock = true;
	}

	for_each_pointer_in_cpset(&fm->listeners, data) {
		if (strcmp(data->channel, channel) == 0) {
			break;
		}
	}

	if (data == NULL) {
		data = malloc(sizeof *data);
		if (data == NULL) {
			ok = ENOMEM;
			goto fail_maybe_unlock;
		}

		data->channel = strdup(channel);
		if (data->channel == NULL) {
			free(data);
			ok = ENOMEM;
			goto fail_maybe_unlock;
		}

		ok = cpset_put_locked(&fm->listeners, data);
		if (ok != 0) {
			free(data->channel);
			free(data);
			goto fail_maybe_unlock;
		}
	}

	data->is_raw = is_raw;
	if (is_raw) {
		data->message_callback = message_callback;
	} else {
		data->codec = codec;
		data->platch_obj_callback = platch_obj_callback;
		data->error_callback = error_callback;
	}
	data->userdata = userdata;

	if (should_unlock) {
		cpset_unlock(&fm->listeners);
	}

	return 0;


	fail_maybe_unlock:
	if (should_unlock) {
		cpset_unlock(&fm->listeners);
	}

	return ok;
}

int fm_set_listener_raw(
	struct flutter_messenger *fm,
	const char *channel,
	platch_message_callback_t message_callback,
	void *userdata
) {
	return set_listener(
		fm,
		channel,
		true,
		message_callback,
		0,
		NULL,
		NULL,
		userdata
	);
}

int fm_set_listener(
	struct flutter_messenger *fm,
	const char *channel,
	enum platch_codec codec,
	error_or_platch_obj_callback_t platch_obj_callback,
	error_or_platch_obj_callback_t error_callback,
	void *userdata
) {
	return set_listener(
		fm,
		channel,
		false,
		NULL,
		codec,
		platch_obj_callback,
		error_callback,
		userdata
	);
}

int fm_remove_listener(
	struct flutter_messenger *fm,
	const char *channel
) {
	struct platch_listener_data *data;
	bool should_unlock;
	int ok;

	if (runs_platform_tasks_on_current_thread(fm) && fm->platform_task_thread_owns_listeners_mutex) {
		should_unlock = false;
	} else {
		cpset_lock(&fm->listeners);
		should_unlock = true;
	}

	for_each_pointer_in_cpset(&fm->listeners, data) {
		if (strcmp(data->channel, channel) == 0) {
			break;
		}
	}

	if (data != NULL) {
		cpset_remove_locked(&fm->listeners, data);
		free(data);
	} else {
		ok = EINVAL;
		goto fail_maybe_unlock;
	}

	if (should_unlock) {
		cpset_unlock(&fm->listeners);
	}

	return 0;


	fail_maybe_unlock:
	if (should_unlock) {
		cpset_unlock(&fm->listeners);
	}

	return ok;
}


int fm_respond_not_implemented_ext(
	struct flutter_message_response_handle *handle,
	void_callback_t error_callback,
	void *userdata
) {
	return fm_respond_nonblocking(handle, &PLATCH_OBJ_NOT_IMPLEMENTED, error_callback, userdata);
}

int fm_call_std(
	struct flutter_messenger *fm,
	const char *channel,
	const char *method,
	const struct std_value *arg,
	error_or_response_callback_t response_callback,
	error_or_response_callback_t error_callback,
	void *userdata
) {
	return fm_send_blocking(
		fm,
		channel,
		&PLATCH_OBJ_STD_CALL(
			method,
			*arg
		),
		kStandardMethodCallResponse,
		response_callback,
		error_callback,
		userdata
	);
}

int fm_respond_success_std_ext(
	struct flutter_message_response_handle *handle,
	const struct std_value *return_value,
	void_callback_t error_callback,
	void *userdata
) {
	return fm_respond_nonblocking(
		handle,
		&PLATCH_OBJ_STD_CALL_SUCCESS_RESPONSE(*return_value),
		error_callback,
		userdata
	);
}

int fm_respond_error_std_ext(
	struct flutter_message_response_handle *handle,
	const char *error_code,
	const char *error_message,
	const struct std_value *error_details,
	void_callback_t error_callback,
	void *userdata
) {
	return fm_respond_nonblocking(
		handle,
		&PLATCH_OBJ_STD_CALL_ERROR_RESPONSE(
			error_code,
			error_message,
			*error_details
		),
		error_callback,
		userdata
	);
}

int fm_respond_illegal_arg_std_ext(
	struct flutter_message_response_handle *handle,
	const char *error_message,
	void_callback_t error_callback,
	void *userdata
) {
	return fm_respond_error_std_ext(
		handle,
		"illegalargument",
		error_message,
		&STDNULL,
		error_callback,
		userdata
	);
}

int fm_respond_native_error_std_ext(
	struct flutter_message_response_handle *handle,
	int _errno,
	void_callback_t error_callback,
	void *userdata
) {
	return fm_respond_error_std_ext(
		handle,
		"nativeerror",
		strerror(_errno),
		&STDINT32(_errno),
		error_callback,
		userdata
	);
}

int fm_send_success_event_std_ext(
	struct flutter_messenger *fm,
	const char *channel,
	const struct std_value *event_value,
	// TODO: change to void_callback_t
	error_or_response_callback_t error_callback,
	void *userdata
) {
	return fm_send_nonblocking(
		fm,
		channel,
		&PLATCH_OBJ_STD_SUCCESS_EVENT(event_value ? *event_value : STDNULL),
		kStandardMethodCallResponse,
		NULL,
		error_callback,
		userdata
	);
}

int fm_send_error_event_std_ext(
	struct flutter_messenger *fm,
	const char *channel,
	const char *error_code,
	const char *error_message,
	const struct std_value *error_details,
	// TODO: change to void_callback_t
	error_or_response_callback_t error_callback,
	void *userdata
) {
	return fm_send_nonblocking(
		fm,
		channel,
		&PLATCH_OBJ_STD_ERROR_EVENT(error_code, error_message, error_details != NULL ? *error_details : STDNULL),
		kStandardMethodCallResponse,
		NULL,
		error_callback,
		userdata
	);
}


int fm_call_json(
	struct flutter_messenger *fm,
	const char *channel,
	const char *method,
	const struct json_value *arg,
	error_or_response_callback_t response_callback,
	error_or_response_callback_t error_callback,
	void *userdata
) {
	return fm_send_nonblocking(
		fm,
		channel,
		&PLATCH_OBJ_JSON_CALL(
			method,
			*arg
		),
		kJSONMethodCallResponse,
		response_callback,
		error_callback,
		userdata
	);
}

int fm_respond_success_json_ext(
	struct flutter_message_response_handle *handle,
	const struct json_value *return_value,
	void_callback_t error_callback,
	void *userdata
) {
	return fm_respond_nonblocking(
		handle,
		&PLATCH_OBJ_JSON_CALL_SUCCESS_RESPONSE(*return_value),
		error_callback,
		userdata
	);
}

int fm_respond_error_json_ext(
	struct flutter_message_response_handle *handle,
	const char *error_code,
	const char *error_message,
	const struct json_value *error_details,
	void_callback_t error_callback,
	void *userdata
) {
	return fm_respond_nonblocking(
		handle,
		&PLATCH_OBJ_JSON_CALL_ERROR_RESPONSE(
			error_code,
			error_message,
			*error_details
		),
		error_callback,
		userdata
	);
}

int fm_respond_illegal_arg_json_ext(
	struct flutter_message_response_handle *handle,
	const char *error_message,
	void_callback_t error_callback,
	void *userdata
) {
	return fm_respond_error_json_ext(
		handle,
		"illegalargument",
		error_message,
		&JSONNULL,
		error_callback,
		userdata
	);
}

int fm_respond_native_error_json_ext(
	struct flutter_message_response_handle *handle,
	int _errno,
	void_callback_t error_callback,
	void *userdata
) {
	return fm_respond_error_json_ext(
		handle,
		"nativeerror",
		strerror(_errno),
		&JSONNUM(_errno),
		error_callback,
		userdata
	);
}

int fm_send_success_event_json_ext(
	struct flutter_messenger *fm,
	const char *channel,
	const struct json_value *event_value,
	// TODO: change to void_callback_t
	error_or_response_callback_t error_callback,
	void *userdata
) {
	return fm_send_nonblocking(
		fm,
		channel,
		&PLATCH_OBJ_JSON_SUCCESS_EVENT(event_value ? *event_value : JSONNULL),
		kJSONMethodCallResponse,
		NULL,
		error_callback,
		userdata
	);
}

int fm_send_error_event_json_ext(
	struct flutter_messenger *fm,
	const char *channel,
	const char *error_code,
	const char *error_message,
	const struct json_value *error_details,
	// TODO: change to void_callback_t
	error_or_response_callback_t error_callback,
	void *userdata
) {
	// we sadly need to cast the const error_code and error_message to non-const
	// since we can't tell the C compiler we'd like the memory pointed to by the pointers
	// inside const struct platch_obj to be readonly as well.
	return fm_send_nonblocking(
		fm,
		channel,
		&PLATCH_OBJ_JSON_ERROR_EVENT(error_code, error_message, error_details != NULL ? *error_details : JSONNULL),
		kJSONMethodCallResponse,
		NULL,
		error_callback,
		userdata
	);
}

