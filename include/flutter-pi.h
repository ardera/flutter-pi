#ifndef _FLUTTERPI_H
#define _FLUTTERPI_H

#define LOG_FLUTTERPI_ERROR(...) fprintf(stderr, "[flutter-pi] " __VA_ARGS__)

#include <limits.h>
#include <linux/input.h>
#include <stdbool.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h> 
#include <stdio.h> 
#include <glob.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <libinput.h>
#include <systemd/sd-event.h>
#include <egl.h>
#include <gles.h>
#include <flutter_embedder.h>

#include <collection.h>

enum device_orientation {
	kPortraitUp, kLandscapeLeft, kPortraitDown, kLandscapeRight
};

#define ORIENTATION_IS_LANDSCAPE(orientation) ((orientation) == kLandscapeLeft || (orientation) == kLandscapeRight)
#define ORIENTATION_IS_PORTRAIT(orientation) ((orientation) == kPortraitUp || (orientation) == kPortraitDown)
#define ORIENTATION_IS_VALID(orientation) ((orientation) == kPortraitUp || (orientation) == kLandscapeLeft || (orientation) == kPortraitDown || (orientation) == kLandscapeRight)

#define ORIENTATION_ROTATE_CW(orientation) ( \
		(orientation) == kPortraitUp ? kLandscapeLeft : \
		(orientation) == kLandscapeLeft ? kPortraitDown : \
		(orientation) == kPortraitDown ? kLandscapeRight : \
		(orientation) == kLandscapeRight ? kPortraitUp : (assert(0 && "invalid device orientation"), 0) \
	)

#define ORIENTATION_ROTATE_CCW(orientation) ( \
		(orientation) == kPortraitUp ? kLandscapeRight : \
		(orientation) == kLandscapeLeft ? kPortraitUp : \
		(orientation) == kPortraitDown ? kLandscapeLeft : \
		(orientation) == kLandscapeRight ? kPortraitDown : (assert(0 && "invalid device orientation"), 0) \
	)

#define ANGLE_FROM_ORIENTATION(o) \
	((o) == kPortraitUp ? 0 : \
	 (o) == kLandscapeLeft ? 90 : \
	 (o) == kPortraitDown ? 180 : \
	 (o) == kLandscapeRight ? 270 : 0)
	
#define ANGLE_BETWEEN_ORIENTATIONS(o_start, o_end) \
	(ANGLE_FROM_ORIENTATION(o_end) \
	- ANGLE_FROM_ORIENTATION(o_start) \
	+ (ANGLE_FROM_ORIENTATION(o_start) > ANGLE_FROM_ORIENTATION(o_end) ? 360 : 0))

#define FLUTTER_RESULT_TO_STRING(result) \
	((result) == kSuccess ? "Success." : \
	 (result) == kInvalidLibraryVersion ? "Invalid library version." : \
	 (result) == kInvalidArguments ? "Invalid arguments." : \
	 (result) == kInternalInconsistency ? "Internal inconsistency." : "(?)")

/// TODO: Move this
#define LIBINPUT_EVENT_IS_TOUCH(event_type) (\
	((event_type) == LIBINPUT_EVENT_TOUCH_DOWN) || \
	((event_type) == LIBINPUT_EVENT_TOUCH_UP) || \
	((event_type) == LIBINPUT_EVENT_TOUCH_MOTION) || \
	((event_type) == LIBINPUT_EVENT_TOUCH_CANCEL) || \
	((event_type) == LIBINPUT_EVENT_TOUCH_FRAME))

#define LIBINPUT_EVENT_IS_POINTER(event_type) (\
	((event_type) == LIBINPUT_EVENT_POINTER_MOTION) || \
	((event_type) == LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE) || \
	((event_type) == LIBINPUT_EVENT_POINTER_BUTTON) || \
	((event_type) == LIBINPUT_EVENT_POINTER_AXIS))

#define LIBINPUT_EVENT_IS_KEYBOARD(event_type) (\
	((event_type) == LIBINPUT_EVENT_KEYBOARD_KEY))

enum flutter_runtime_mode {
	kDebug, kProfile, kRelease
};

#define FLUTTER_RUNTIME_MODE_IS_JIT(runtime_mode) ((runtime_mode) == kDebug)
#define FLUTTER_RUNTIME_MODE_IS_AOT(runtime_mode) ((runtime_mode) == kProfile || (runtime_mode) == kRelease)

struct compositor;
struct plugin_registry;
struct texture_registry;
struct drmdev;
struct locales;
struct vk_renderer;

struct flutter_paths {
	char *app_bundle_path;
	char *asset_bundle_path;
	char *app_elf_path;
	char *icudtl_path;
	char *kernel_blob_path;
	char *flutter_engine_path;
	char *flutter_engine_dlopen_name;
	char *flutter_engine_dlopen_name_fallback;
};

/// TODO: Make this an opaque struct
struct flutterpi {
	/**
	 * @brief The KMS device.
	 * 
	 */
	struct {
		struct drmdev *drmdev;
	} drm;

	/**
	 * @brief The flutter event tracing interface.
	 * 
	 */
	struct tracer *tracer;

	/**
	 * @brief The compositor. Manages all the window stuff.
	 * 
	 */
	struct compositor *compositor;

	/**
	 * @brief Event source which represents the compositor event fd as registered to the
	 * event loop.
	 * 
	 */
	sd_event_source *compositor_event_source;

	/**
	 * @brief The user input instance.
	 * 
	 * Handles touch, mouse and keyboard input and calls the callbacks.
	 */
	struct user_input *user_input;

	/**
	 * @brief The user input instance event fd registered to the event loop.
	 * 
	 */
	sd_event_source *user_input_event_source;

	/**
	 * @brief The locales instance. Provides the system locales to flutter.
	 * 
	 */
	struct locales *locales;
	
	/**
	 * @brief flutter stuff.
	 * 
	 */
	struct {
		char *bundle_path;
		struct flutter_paths *paths;
		void *app_elf_handle;

		FlutterLocale **locales;
		size_t n_locales;

		int engine_argc;
		char **engine_argv;
		enum flutter_runtime_mode runtime_mode;
		FlutterEngineProcTable procs;
		FlutterEngine engine;

		bool next_frame_request_is_secondary;
	} flutter;
	
	/// main event loop
	pthread_t event_loop_thread;
	pthread_mutex_t event_loop_mutex;
	sd_event *event_loop;
	int wakeup_event_loop_fd;

	/// flutter-pi internal stuff
	struct plugin_registry *plugin_registry;
	struct texture_registry *texture_registry;
	struct gl_renderer *gl_renderer;
	struct vk_renderer *vk_renderer;
};

struct platform_task {
	int (*callback)(void *userdata);
	void *userdata;
};

struct platform_message {
	bool is_response;
	union {
		FlutterPlatformMessageResponseHandle *target_handle;
		struct {
			char *target_channel;
			FlutterPlatformMessageResponseHandle *response_handle;
		};
	};
	uint8_t *message;
	size_t message_size;
};

extern struct flutterpi flutterpi;

int flutterpi_fill_view_properties(
	bool has_orientation,
	enum device_orientation orientation,
	bool has_rotation,
	int rotation
);

int flutterpi_post_platform_task(
	int (*callback)(void *userdata),
	void *userdata
);

int flutterpi_post_platform_task_with_time(
	int (*callback)(void *userdata),
	void *userdata,
	uint64_t target_time_usec
);

int flutterpi_sd_event_add_io(
	sd_event_source **source_out,
	int fd,
	uint32_t events,
	sd_event_io_handler_t callback,
	void *userdata
);

int flutterpi_send_platform_message(
	struct flutterpi *flutterpi,
	const char *channel,
	const uint8_t *restrict message,
	size_t message_size,
	FlutterPlatformMessageResponseHandle *responsehandle
);

int flutterpi_respond_to_platform_message(
	FlutterPlatformMessageResponseHandle *handle,
	const uint8_t *restrict message,
	size_t message_size
);

struct texture_registry *flutterpi_get_texture_registry(
	struct flutterpi *flutterpi
);

struct texture *flutterpi_create_texture(struct flutterpi *flutterpi);

const char *flutterpi_get_asset_bundle_path(
	struct flutterpi *flutterpi
);

int flutterpi_schedule_exit(void);

struct gbm_device *flutterpi_get_gbm_device(struct flutterpi *flutterpi);

bool flutterpi_has_gl_renderer(struct flutterpi *flutterpi);

struct gl_renderer *flutterpi_get_gl_renderer(struct flutterpi *flutterpi);

void flutterpi_trace_event_instant(struct flutterpi *flutterpi, const char *name);

void flutterpi_trace_event_begin(struct flutterpi *flutterpi, const char *name);

void flutterpi_trace_event_end(struct flutterpi *flutterpi, const char *name);

#endif