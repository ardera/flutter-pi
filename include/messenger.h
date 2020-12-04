#ifndef _FLUTTERPI_MESSENGER_H
#define _FLUTTERPI_MESSENGER_H

#include <flutter_embedder.h>
#include <platformchannel.h>

struct flutterpi;

typedef bool (*runs_platform_tasks_on_current_thread_t)(struct flutterpi *flutterpi);

typedef int (*post_platform_task_t)(
	struct flutterpi *flutterpi,
	int (*callback)(void *userdata),
	void *userdata
);

typedef FlutterEngineResult (*flutter_engine_send_platform_message_t)(
    FlutterEngine engine,
    const FlutterPlatformMessage* message
);

typedef FlutterEngineResult (*flutter_platform_message_create_response_handle_t)(
    FlutterEngine engine,
    FlutterDataCallback data_callback,
    void* user_data,
    FlutterPlatformMessageResponseHandle** response_out
);

typedef FlutterEngineResult (*flutter_platform_message_release_response_handle_t)(
    FlutterEngine engine,
    FlutterPlatformMessageResponseHandle* response
);

typedef FlutterEngineResult (*flutter_engine_send_platform_message_response_t)(
    FlutterEngine engine,
    const FlutterPlatformMessageResponseHandle* handle,
    const uint8_t* data,
    size_t data_length
);

struct flutter_message_response_handle;

struct platch_message_listener_handle;

typedef FlutterDataCallback data_callback_t;

typedef void (*void_callback_t)(void *userdata);

typedef void (*shipped_callback_t)(bool success, void *userdata);

typedef void (*error_or_raw_response_callback_t)(bool success, const uint8_t *data, size_t size, void *userdata);

typedef void (*raw_response_callback_t)(const uint8_t *data, size_t size, void *userdata);

typedef void (*error_or_response_callback_t)(bool success, struct platch_obj *object, void *userdata);

typedef void (*response_callback_t)(struct platch_obj *object, void *userdata);

typedef void (*platch_message_callback_t)(const struct flutter_message_response_handle *responsehandle, const char *channel, const uint8_t *data, size_t size, void *userdata);

typedef void (*error_or_platch_obj_callback_t)(bool success, const struct flutter_message_response_handle *responsehandle, const char *channel, const struct platch_obj *object, void *userdata);

/**
 * @brief Create a new flutter messenger instance.
 */
struct flutter_messenger *fm_new(
    runs_platform_tasks_on_current_thread_t runs_platform_tasks_on_current_thread,
    post_platform_task_t post_platform_task,
    flutter_engine_send_platform_message_t send_platform_message,
    flutter_platform_message_create_response_handle_t create_response_handle,
    flutter_platform_message_release_response_handle_t release_response_handle,
    flutter_engine_send_platform_message_response_t send_response,
    struct flutterpi *flutterpi,
    FlutterEngine engine
);

/**
 * @brief Destroy flutter messenger @ref fm.
 */
int fm_destroy(struct flutter_messenger *fm);

/**
 * @brief Notifies the flutter messenger that a platform message has arrived.
 * 
 * @param fm The flutter messenger that should handle the message.
 * @param flutter_responsehandle The flutter engine response handle that can be used to reply to this message.
 * @param channel The channel on which the message has arrived.
 * @param message Pointer to the message data. Can be NULL for a "not implemented" message.
 * @param message_size The size of the message in bytes. Must be 0 when `message == null`.
 */
int fm_on_platform_message(
	struct flutter_messenger *fm,
	const FlutterPlatformMessageResponseHandle *flutter_responsehandle,
	const char *channel,
	const uint8_t *message,
	size_t message_size
);

/**
 * @brief Sets a raw callback to be invoked when a message arrives on channel @ref channel.
 * 
 * Note that you can only configure a raw listener OR a decoding listener, not both.
 * The userdata set in @ref fm_set_listener will affect the userdata passed to @ref message_callback.
 * 
 * @param fm The flutter messenger that should register the listener.
 * @param channel The channel on which to listen for messages.
 * @param message_callback When present, invoked on the platform task thread when a message arrives.
 *                         Must reply to the message, not doing so will result in a memory leak.
 * @param userdata The userdata to pass to @ref message_callback.
 */
int fm_set_listener_raw(
	struct flutter_messenger *fm,
	const char *channel,
	platch_message_callback_t message_callback,
	void *userdata
);

/**
 * @brief Registers a listener on channel @ref channel. If one of @ref platch_obj_callback or @ref error_callback
 * is not NULL, will decode the message(s) using codec @ref codec and invoke @ref platch_obj_callback with the decoded
 * object.
 * 
 * Note that you can only configure one raw listener OR one decoding listener, not both.
 * The userdata set in @ref fm_set_listener_raw will affect the userdata passed to @ref platch_obj_callback and
 * @ref error_callback.
 * 
 * @param fm The flutter messenger that should register the listener.
 * @param channel The channel on which to listen for messages.
 * @param codec The codec used to decode the raw message data, if one of @ref platch_obj_callback or @ref error_callback is present.
 * @param platch_obj_callback When present, invoked on the platform task thread with the decode platform channel object
 *                            when the message data was successfully decoded.
 *                            Must reply to the message, not doing so will result in a memory leak.
 * @param error_callback When present, invoked on the platform task thread when an error ocurred while decoding the message.
 *                       Must reply to the message, not doing so will result in a memory leak.
 * @param userdata The userdata to pass to @ref platch_obj_callback and @ref error_callback.
 */
int fm_set_listener(
	struct flutter_messenger *fm,
	const char *channel,
	enum platch_codec codec,
	error_or_platch_obj_callback_t platch_obj_callback,
	error_or_platch_obj_callback_t error_callback,
	void *userdata
);

/**
 * @brief Send raw platform message data to channel @ref channel.
 * 
 * This is only really useful if you want to send the same data multiple times, or to
 * send some constant data. Otherwise the @ref fm_send_raw_nonblocking or @ref fm_send_raw_blocking
 * functions, while having a little more overhead bc of the memory copying, are nicer to use.
 * The overhead is because they memcpy the the channel & message. But the flutter engine does that too internally,
 * so if you want high-speed, low-overhead message passing you should probably not use platform channels.
 * 
 * @param fm                The flutter messenger that should send the message.
 * @param channel           The channel on which to send the message.
 *                          Must point to valid memory until the message was handed over to the flutter engine.
 * @param message           The message to send. Can be NULL for a "missing plugin" message.
 *                          Must point to valid memory until the message was handed over to the flutter engine.
 * @param message_size      The size of the message in bytes. Must be `0` when `message == NULL`.
 * @param response_callback When present, called on the platform task thread when a response arrives.
 *                          This `data` and `size` arguments are the the data and size of the response message.
 * @param response_callback_userdata The userdata passed to @ref response_callback.
 * @param shipped_callback  When present, called on the platform task thread when the message was
 *                          handed over to the flutter engine. If an error ocurred while handing the
 *                          over the message, this will be called with `success == false`.
 * @param shipped_callback_userdata The userdata passed to @ref shipped_callback.
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
);

/**
 * @brief Send raw platform message data to channel @ref channel and copies the channel & message internally.
 * 
 * @param fm The flutter messenger that should send the message.
 * @param channel The channel on which to send the message. Doesn't need to be allocated,
 *                since it will be copied internally anyway.
 * @param message The message to send. Can be NULL for a "missing plugin" message. Doesn't need
 *                to be allocated, since it will be copied internally anyway.
 * @param message_size The size of the message in bytes. Must be `0` when `message == NULL`.
 * @param response_callback When present, called with `success == true` on the platform task
 *                          thread when a response arrives.
 * @param response_callback_userdata The userdata passed to @ref response_callback
 * @param error_callback When present, called with `success == false` on the platform task
 *                       thread when an error ocurrs while handing over the message
 *                       to the flutter engine.
 * @param error_callback_userdata The userdata passed to @ref error_callback.
 */
int fm_send_raw_nonblocking(
    struct flutter_messenger *fm,
    const char *channel,
    const uint8_t *message,
    size_t message_size,
    error_or_raw_response_callback_t response_callback,
    void *response_callback_userdata,
    error_or_raw_response_callback_t error_callback,
    void *error_callback_userdata
);

/**
 * @brief Send raw platform message data to channel @ref channel and wait for it to be handed
 * over to the flutter engine, to avoid the shipped_callback.
 * 
 * @param fm The flutter messenger that should send the message.
 * @param channel The channel on which to send the message. Doesn't need to be allocated.
 * @param message The message to send. Can be NULL for a "missing plugin" message. Doesn't need
 *                to be allocated.
 * @param message_size The size of the message in bytes. Must be `0` when `message == NULL`.
 * @param response_callback When present, called on the platform task thread when a response arrives.
 * @param response_callback_userdata The userdata passed to @ref response_callback.
 */
int fm_send_raw_blocking(
    struct flutter_messenger *fm,
    const char *channel,
    const uint8_t *message,
    size_t message_size,
    raw_response_callback_t response_callback,
    void *response_callback_userdata
);

/**
 * @brief Send raw platform message data as a response to @ref handle.
 * 
 * @param fm                The flutter messenger that should send the message.
 * @param handle            The target handle that the response should be sent to.
 * @param message           The message to send. Can be NULL for a "missing plugin" message.
 *                          Will be copied internally so you can free this immediately after this function returned.
 * @param message_size      The size of the message in bytes. Must be `0` when `message == NULL`.
 * @param error_callback    Called on the platform task thread when an error ocurred sending the response.
 *                          If @ref fm_respond_raw is called on the platform task thread, @ref error_callback won't
 *                          be called and an error code will be returned instead. 
 * @param error_callback_userdata The userdata to pass to @ref error_callback.
 */
int fm_respond_raw_zerocopy_nonblocking(
    struct flutter_messenger *fm,
    struct flutter_message_response_handle *handle,
    const uint8_t *message,
    size_t message_size,
    shipped_callback_t shipped_callback,
    void *userdata
);

/**
 * @brief Send raw platform message data as a response to @ref handle, but copy the message internally.
 * 
 * @param fm The flutter messenger that should send the message.
 * @param handle The target handle that the response should be sent to.
 * @param message The message to send. Can be NULL for a "missing plugin" message.
 *                Doesn't need to be allocated, since the data will be copied internally anyway.
 * @param message_size The size of the message in bytes. Must be `0` when `message == NULL`.
 * @param error_callback Called on the platform task thread when an error ocurred while handing
 *                       the message over to the flutter engine.
 * @param error_callback_userdata The userdata to pass to @ref error_callback.
 */
int fm_respond_raw_nonblocking(
    struct flutter_messenger *fm,
    struct flutter_message_response_handle *handle,
    const uint8_t *message,
    size_t message_size,
    void_callback_t error_callback,
    void *error_callback_userdata
);

/**
 * @brief Send raw platform message data as a response to @ref handle and wait for it to be handed
 * over to the flutter engine, to avoid the shipped_callback.
 * 
 * @param fm The flutter messenger that should send the message.
 * @param handle The target handle that the response should be sent to.
 * @param message The message to send. Can be NULL for a "missing plugin" message.
 *                Doesn't need to be allocated.
 * @param message_size The size of the message in bytes. Must be `0` when `message == NULL`.
 */
int fm_respond_raw_blocking(
    struct flutter_messenger *fm,
    struct flutter_message_response_handle *handle,
    const uint8_t *message,
    size_t message_size
);

int fm_remove_listener(
	struct flutter_messenger *fm,
	const char *channel
);

int fm_call_std(
	struct flutter_messenger *fm,
	const char *channel,
	const char *method,
	const struct std_value *arg,
	error_or_response_callback_t response_callback,
	error_or_response_callback_t error_callback,
	void *userdata
);

int fm_respond_not_implemented_ext(
	struct flutter_messenger *fm,
	const struct flutter_message_response_handle *handle,
	void_callback_t error_callback,
	void *userdata
);

#define fm_respond_not_implemented(fm, handle) fm_respond_not_implemented_ext(fm, handle, NULL, NULL)

int fm_respond_success_std_ext(
	struct flutter_messenger *fm,
	const struct flutter_message_response_handle *handle,
	const struct std_value *return_value,
	void_callback_t error_callback,
	void *userdata
);

#define fm_respond_success_std(fm, handle, return_value) fm_respond_success_std_ext(fm, handle, return_value, NULL, NULL)

int fm_respond_error_std_ext(
	struct flutter_messenger *fm,
	const struct flutter_message_response_handle *handle,
	const char *error_code,
	const char *error_message,
	const struct std_value *error_details,
	void_callback_t error_callback,
	void *userdata
);

#define fm_respond_error_std(fm, handle, error_code, error_message, error_details) fm_respond_error_std_ext(fm, handle, error_code, error_message, error_details, NULL, NULL)

int fm_respond_illegal_arg_std_ext(
	struct flutter_messenger *fm,
	const struct flutter_message_response_handle *handle,
	const char *error_message,
	void_callback_t error_callback,
	void *userdata
);

#define fm_respond_illegal_arg_std(fm, handle, error_message) fm_respond_illegal_arg_std_ext(fm, handle, error_message, NULL, NULL)

int fm_respond_native_error_std_ext(
	struct flutter_messenger *fm,
	const struct flutter_message_response_handle *handle,
	int _errno,
	void_callback_t error_callback,
	void *userdata
);

#define fm_respond_native_error_std(fm, handle, _errno) fm_respond_native_error_std_ext(fm, handle, _errno, NULL, NULL)

int fm_send_success_event_std_ext(
	struct flutter_messenger *fm,
	const char *channel,
	struct std_value *event_value,
	// TODO: change to void_callback_t
	error_or_response_callback_t error_callback,
	void *userdata
);

#define fm_send_success_event_std(fm, channel, event_value) fm_send_success_event_std_ext(fm, channel, event_value, NULL, NULL)

int fm_send_error_event_std_ext(
	struct flutter_messenger *fm,
	const char *channel,
	const char *error_code,
	const char *error_message,
	struct std_value *error_details,
	// TODO: change to void_callback_t
	error_or_response_callback_t error_callback,
	void *userdata
);

#define fm_send_error_event_std(fm, channel, error_code, error_message, error_details) fm_send_error_event_std_ext(fm, channel, error_code, error_message, error_details, NULL, NULL)

int fm_call_json(
	struct flutter_messenger *fm,
	const char *channel,
	const char *method,
	const struct json_value *arg,
	error_or_response_callback_t response_callback,
	error_or_response_callback_t error_callback,
	void *userdata
);

int fm_respond_success_json_ext(
	struct flutter_messenger *fm,
	const struct flutter_message_response_handle *handle,
	const struct json_value *return_value,
	void_callback_t error_callback,
	void *userdata
);

#define fm_respond_success_json(fm, handle, return_value) fm_respond_success_json_ext(fm, handle, return_value, NULL, NULL)

int fm_respond_error_json_ext(
	struct flutter_messenger *fm,
	const struct flutter_message_response_handle *handle,
	const char *error_code,
	const char *error_message,
	const struct json_value *error_details,
	void_callback_t error_callback,
	void *userdata
);

#define fm_respond_error_json(fm, handle, error_code, error_message, error_details) fm_respond_error_json_ext(fm, handle, error_code, error_message, error_details, NULL, NULL)

int fm_respond_illegal_arg_json_ext(
	struct flutter_messenger *fm,
	const struct flutter_message_response_handle *handle,
	const char *error_message,
	void_callback_t error_callback,
	void *userdata
);

#define fm_respond_illegal_arg_json(fm, handle, error_message) fm_respond_error_json_ext(fm, handle, error_message, NULL, NULL)

int fm_respond_native_error_json_ext(
	struct flutter_messenger *fm,
	const struct flutter_message_response_handle *handle,
	int _errno,
	void_callback_t error_callback,
	void *userdata
);

#define fm_respond_native_error_json(fm, handle, _errno) fm_respond_native_error_json_ext(fm, handle, _errno, NULL, NULL)

int fm_send_success_event_json_ext(
	struct flutter_messenger *fm,
	const char *channel,
	struct json_value *event_value,
	// TODO: change to void_callback_t
	error_or_response_callback_t error_callback,
	void *userdata
);

#define fm_send_success_event_json(fm, channel, event_value) fm_send_success_event_json(fm, handle, event_value, NULL, NULL)

int fm_send_error_event_json_ext(
	struct flutter_messenger *fm,
	const char *channel,
	const char *error_code,
	const char *error_message,
	struct json_value *error_details,
	// TODO: change to void_callback_t
	error_or_response_callback_t error_callback,
	void *userdata
);

#define fm_send_error_event_json(fm, channel, error_code, error_message, error_details) fm_send_error_event_json_ext(fm, channel, error_code, error_message, error_details, NULL, NULL)

#endif