#define  _GNU_SOURCE

#include <ctype.h>
#include <features.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <math.h>
#include <limits.h>
#include <float.h>
#include <assert.h>
#include <time.h>
#include <getopt.h>
#include <elf.h>
#include <sys/eventfd.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <linux/input.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <gbm.h>
#include <EGL/egl.h>
#define  EGL_EGLEXT_PROTOTYPES
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#define  GL_GLEXT_PROTOTYPES
#include <GLES2/gl2ext.h>
#include <libinput.h>
#include <libudev.h>
#include <systemd/sd-event.h>
#include <flutter_embedder.h>

#include <flutter-pi.h>
#include <compositor.h>
#include <keyboard.h>
#include <messenger.h>
#include <pluginregistry.h>
#include <texture_registry.h>
#include <dylib_deps.h>
#include <user_input.h>
#include <renderer.h>
#include <event_loop.h>

#include <plugins/text_input.h>
#include <plugins/raw_keyboard.h>

const char *const usage ="\
flutter-pi - run flutter apps on your Raspberry Pi.\n\
\n\
USAGE:\n\
  flutter-pi [options] <asset bundle path> [flutter engine options]\n\
\n\
OPTIONS:\n\
  --release                  Run the app in release mode. The AOT snapshot\n\
                             of the app (\"app.so\") must be located inside the\n\
                             asset bundle directory.\n\
                             This also requires a libflutter_engine.so that was\n\
                             built with --runtime-mode=release.\n\
                             \n\
  -o, --orientation <orientation>  Start the app in this orientation. Valid\n\
                             for <orientation> are: portrait_up, landscape_left,\n\
                             portrait_down, landscape_right.\n\
                             For more information about this orientation, see\n\
                             the flutter docs for the \"DeviceOrientation\"\n\
                             enum.\n\
                             Only one of the --orientation and --rotation\n\
                             options can be specified.\n\
                             \n\
  -r, --rotation <degrees>   Start the app with this rotation. This is just an\n\
                             alternative, more intuitive way to specify the\n\
                             startup orientation. The angle is in degrees and\n\
                             clock-wise.\n\
                             Valid values are 0, 90, 180 and 270.\n\
                             \n\
  -d, --dimensions \"width_mm,height_mm\" The width & height of your display in\n\
                             millimeters. Useful if your GPU doesn't provide\n\
                             valid physical dimensions for your display.\n\
                             The physical dimensions of your display are used\n\
                             to calculate the flutter device-pixel-ratio, which\n\
                             in turn basically \"scales\" the UI.\n\
                             \n\
  -i, --input <glob pattern> Appends all files matching this glob pattern to the\n\
                             list of input (touchscreen, mouse, touchpad, \n\
                             keyboard) devices. Brace and tilde expansion is \n\
                             enabled.\n\
                             Every file that matches this pattern, but is not\n\
                             a valid touchscreen / -pad, mouse or keyboard is \n\
                             silently ignored.\n\
                             If no -i options are given, flutter-pi will try to\n\
                             use all input devices assigned to udev seat0.\n\
                             If that fails, or udev is not installed, flutter-pi\n\
                             will fallback to using all devices matching \n\
                             \"/dev/input/event*\" as inputs.\n\
                             In most cases, there's no need to specify this\n\
                             option.\n\
                             Note that you need to properly escape each glob \n\
                             pattern you use as a parameter so it isn't \n\
                             implicitly expanded by your shell.\n\
                             \n\
  -h, --help                 Show this help and exit.\n\
\n\
EXAMPLES:\n\
  flutter-pi ~/hello_world_app\n\
  flutter-pi --release ~/hello_world_app\n\
  flutter-pi -o portrait_up ./my_app\n\
  flutter-pi -r 90 ./my_app\n\
  flutter-pi -d \"155, 86\" ./my_app\n\
  flutter-pi -i \"/dev/input/event{0,1}\" -i \"/dev/input/event{2,3}\" /home/pi/helloworld_flutterassets\n\
  flutter-pi -i \"/dev/input/mouse*\" /home/pi/helloworld_flutterassets\n\
\n\
SEE ALSO:\n\
  Author:  Hannes Winkler, a.k.a ardera\n\
  Source:  https://github.com/ardera/flutter-pi\n\
  License: MIT\n\
\n\
  For instructions on how to build an asset bundle or an AOT snapshot\n\
    of your app, please see the linked github repository.\n\
  For a list of options you can pass to the flutter engine, look here:\n\
    https://github.com/flutter/engine/blob/master/shell/common/switches.h\n\
";

struct flutterpi_flutter_task {
	struct flutterpi *flutterpi;
	FlutterTask task;
};

struct flutterpi_private {
	struct flutterpi *flutterpi;
	struct text_input_plugin *textin;
	struct raw_keyboard_plugin *rawkb;
	struct event_loop *platform, *render;
};

#define FLUTTERPI_PRIVATE(flutterpi) ((struct flutterpi_private*) (flutterpi)->private)

struct flutterpi_frame_request {
	struct flutterpi *flutterpi;
	intptr_t baton;
};

struct flutterpi_frame_response {
	struct flutterpi *flutterpi;
	intptr_t baton;
	uint64_t frame_nanos;
	uint64_t next_frame_nanos;
};

// OpenGL contexts are thread-local. So this needs to be thread-local as well.
static bool on_flutter_gl_make_current(void* userdata) {
	struct flutterpi *flutterpi = userdata;

	printf("on_flutter_gl_make_current\n");

	return gl_renderer_flutter_make_rendering_context_current(flutterpi->renderer);
}

static bool on_flutter_gl_clear_current(void *userdata) {
	struct flutterpi *flutterpi = userdata;

	printf("on_flutter_gl_clear_current\n");

	return gl_renderer_flutter_make_rendering_context_current(flutterpi->renderer);
}

static bool on_flutter_gl_present(void *userdata) {
	printf("on_flutter_gl_present\n");
	return gl_renderer_flutter_present(((struct flutterpi*) userdata)->renderer);
}

static uint32_t on_flutter_gl_get_fbo(void* userdata) {
	printf("on_flutter_gl_get_fbo\n");
	return gl_renderer_flutter_get_fbo(((struct flutterpi *) userdata)->renderer);
}

static bool on_flutter_gl_make_resource_context_current(void *userdata) {
	struct flutterpi *flutterpi = userdata;
	printf("on_flutter_gl_make_resource_context_current\n");
	return gl_renderer_flutter_make_resource_context_current(flutterpi->renderer);
}

static FlutterTransformation on_flutter_gl_get_surface_transformation(void *userdata) {
	printf("on_flutter_gl_get_surface_transformation\n");
	return gl_renderer_flutter_get_surface_transformation(((struct flutterpi *) userdata)->renderer);
}

static void *on_flutter_gl_resolve_proc(void *userdata, const char *name) {
	printf("on_flutter_gl_resolve_proc\n");
	return gl_renderer_flutter_resolve_gl_proc(((struct flutterpi *) userdata)->renderer, name);
}

static bool on_flutter_gl_get_external_texture_frame(
	void *userdata,
	int64_t texture_id,
	size_t width, size_t height,
	FlutterOpenGLTexture *texture_out
) {
	return texreg_on_external_texture_frame_callback(
		((struct flutterpi *) userdata)->texture_registry,
		texture_id,
		width, height,
		texture_out
	) == 0;
}

static uint32_t on_flutter_gl_get_fbo_with_info(void *userdata, const FlutterFrameInfo *info) {
	printf("on_flutter_gl_get_fbo_with_info\n");
	return gl_renderer_flutter_get_fbo_with_info(((struct flutterpi *) userdata)->renderer, info);
}

bool on_flutter_gl_present_with_info(void *userdata, const FlutterPresentInfo *info) {
	printf("on_flutter_gl_present_with_info\n");
	return gl_renderer_flutter_present_with_info(((struct flutterpi *) userdata)->renderer, info);
}

static const struct flutter_renderer_gl_interface gl_interface = {
	.make_current = on_flutter_gl_make_current,
	.clear_current = on_flutter_gl_clear_current,
	.present = on_flutter_gl_present,
	.fbo_callback = on_flutter_gl_get_fbo,
	.make_resource_current = on_flutter_gl_make_resource_context_current,
	.surface_transformation = on_flutter_gl_get_surface_transformation,
	.gl_proc_resolver = on_flutter_gl_resolve_proc,
	.gl_external_texture_frame_callback = on_flutter_gl_get_external_texture_frame,
	.fbo_with_frame_info_callback = on_flutter_gl_get_fbo_with_info,
	.present_with_info = on_flutter_gl_present_with_info
};

bool on_flutter_sw_present(void *userdata, const void *allocation, size_t bytes_per_row, size_t height) {
	printf("on_flutter_sw_present\n");
	return sw_renderer_flutter_present(((struct flutterpi *) userdata)->renderer, allocation, bytes_per_row, height);
}

static const struct flutter_renderer_sw_interface sw_interface = {
	.surface_present_callback = on_flutter_sw_present
};

static bool runs_platform_tasks_on_current_thread(void *userdata);

/// Cut a word from a string, mutating "string"
/*
static void cut_word_from_string(
	char* string,
	const char* word
) {
	size_t word_length = strlen(word);
	char*  word_in_str = strstr(string, word);

	// check if the given word is surrounded by spaces in the string
	if (word_in_str
		&& ((word_in_str == string) || (word_in_str[-1] == ' '))
		&& ((word_in_str[word_length] == 0) || (word_in_str[word_length] == ' '))
	) {
		if (word_in_str[word_length] == ' ') word_length++;

		int i = 0;
		do {
			word_in_str[i] = word_in_str[i+word_length];
		} while (word_in_str[i++ + word_length] != 0);
	}
}
*/

static void on_platform_message(
	const FlutterPlatformMessage* message,
	void* userdata
) {
	struct flutterpi *fpi;
	int ok;

	fpi = userdata;

	DEBUG_ASSERT(fpi != NULL);

	ok = fm_on_platform_message(fpi->flutter_messenger, message->response_handle, message->channel, message->message, message->message_size);
	if (ok != 0) {
		LOG_FLUTTERPI_ERROR("Error handling platform message. fm_on_platform_message: %s\n", strerror(ok));
	}
}

static void on_deferred_respond_to_frame_request(void *userdata) {
	struct flutterpi_frame_response *resp;

	resp = userdata;

	printf("on_deferred_respond_to_frame_request: %p\n", (void*) (resp->baton));

	resp->flutterpi->flutter.libflutter_engine->FlutterEngineOnVsync(
		resp->flutterpi->flutter.engine,
		resp->baton,
		resp->frame_nanos,
		resp->next_frame_nanos
	);

	free(resp);
}

static void on_begin_frame(uint64_t frame_nanos, uint64_t next_frame_nanos, void *userdata) {
	struct flutterpi_frame_response *resp;
	struct flutterpi_frame_request *req;
	struct flutterpi *flutterpi;
	intptr_t baton;

	req = userdata;
	flutterpi = req->flutterpi;
	baton = req->baton;

	printf("on_begin_frame\n");

	DEBUG_ASSERT(req != NULL);

	if (flutterpi_runs_platform_tasks_on_current_thread(flutterpi)) {
		flutterpi->flutter.libflutter_engine->FlutterEngineOnVsync(
			flutterpi->flutter.engine,
			frame_nanos,
			next_frame_nanos,
			baton
		);
	} else {
		resp = malloc(sizeof *resp);
		if (resp == NULL) {
			free(req);
		}

		resp->flutterpi = flutterpi;
		resp->frame_nanos = frame_nanos;
		resp->next_frame_nanos = next_frame_nanos;
		resp->baton = baton;

		event_loop_post_task(
			flutterpi->private->platform,
			on_deferred_respond_to_frame_request,
			resp
		);
	}

	free(req);	
}

/// Called on some flutter internal thread to request a frame,
/// and also get the vblank timestamp of the pageflip preceding that frame.
static void on_frame_request(void* userdata, intptr_t baton) {
	struct flutterpi_frame_request *req;
	struct flutterpi *flutterpi;
	int ok;

	printf("on_frame_request: %p\n", (void*) baton);

	flutterpi = userdata;

	DEBUG_ASSERT(flutterpi != NULL);

	req = malloc(sizeof *req);
	DEBUG_ASSERT(req != NULL);

	req->flutterpi = flutterpi;
	req->baton = baton;
	
	ok = compositor_request_frame(flutterpi->compositor, on_begin_frame, req);
	DEBUG_ASSERT(ok == 0);
	(void) ok;
}

struct event_loop *flutterpi_get_event_loop(struct flutterpi *flutterpi, enum event_loop_type type) {
	struct flutterpi_private *private;

	DEBUG_ASSERT(flutterpi != NULL);
	private = FLUTTERPI_PRIVATE(flutterpi);

	if (type == kPlatform) {
		return private->platform;
	} else if (type == kRender) {
		return private->render;
	} else {
		DEBUG_ASSERT(false);
		return NULL;
	}
}

/*
static FlutterTransformation flutterpi_on_get_transformation(void *userdata) {
	struct flutterpi *fpi;
	fpi = userdata;
	return fpi->view.view_to_display_transform;
}
*/

/// flutter tasks
static void on_execute_flutter_task(
	void *userdata
) {
	struct flutterpi_flutter_task *task;
	FlutterEngineResult engine_result;

	task = userdata;

	printf("on_execute_flutter_task: 0x%016" PRIX64 "\n", task->task.task);

	engine_result = task->flutterpi->flutter.libflutter_engine->FlutterEngineRunTask(task->flutterpi->flutter.engine, &task->task);
	if (engine_result != kSuccess) {
		LOG_FLUTTERPI_ERROR("Error running platform task. FlutterEngineRunTask: %s\n", FLUTTER_RESULT_TO_STRING(engine_result));
	}

	printf("on_execute_flutter_task done\n");

	free(task);
}

static void on_post_flutter_task(
	FlutterTask task,
	uint64_t target_time,
	void *userdata
) {
	struct flutterpi_flutter_task *fpi_task;
	struct flutterpi *flutterpi;
	int ok;

	printf("on_post_flutter_task: 0x%016" PRIX64 "\n", task.task);

	flutterpi = userdata;

	fpi_task = malloc(sizeof *fpi_task);
	if (fpi_task == NULL) {
		return;
	}
	
	fpi_task->task = task;
	fpi_task->flutterpi = flutterpi;

	ok = flutterpi_post_platform_task_with_time(
		flutterpi,
		on_execute_flutter_task,
		fpi_task,
		target_time / 1000
	);
	if (ok != 0) {
		free(fpi_task);
	}
}

/// platform messages
/*
static int on_send_platform_message(
	void *userdata
) {
	struct platform_message_response_handler_data *handler_data;
	struct platform_message *msg;
	FlutterEngineResult result;

	msg = userdata;

	if (msg->is_response) {
		result = msg->flutterpi.flutter.libflutter_engine.FlutterEngineSendPlatformMessageResponse(flutterpi.flutter.engine, msg->target_handle, msg->message, msg->message_size);
	} else {
		// if we have a response callback, allocate a response handle here.
		handlerdata = malloc(sizeof(struct platch_msg_resp_handler_data));
		if (!handlerdata) {
			return ENOMEM;
		}
		
		handlerdata->codec = response_codec;
		handlerdata->on_response = on_response;
		handlerdata->userdata = userdata;

		result = flutterpi.flutter.libflutter_engine.FlutterPlatformMessageCreateResponseHandle(flutterpi.flutter.engine, platch_on_response_internal, handlerdata, &response_handle);
		if (result != kSuccess) {
			fprintf(stderr, "[flutter-pi] Error create platform message response handle. FlutterPlatformMessageCreateResponseHandle: %s\n", FLUTTER_RESULT_TO_STRING(result));
			goto fail_free_handlerdata;
		}

		result = msg->flutterpi.flutter.libflutter_engine.FlutterEngineSendPlatformMessage(
			flutterpi.flutter.engine,
			&(FlutterPlatformMessage) {
				.struct_size = sizeof(FlutterPlatformMessage),
				.channel = msg->target_channel,
				.message = msg->message,
				.message_size = msg->message_size,
				.response_handle = msg->response_handle
			}
		);
	}

	if (msg->message) {
		free(msg->message);
	}

	if (msg->is_response == false) {
		free(msg->target_channel);
	}

	free(msg);

	if (result != kSuccess) {
		fprintf(stderr, "[flutter-pi] Error sending platform message. FlutterEngineSendPlatformMessage: %s\n", FLUTTER_RESULT_TO_STRING(result));
	}

	return 0;
}

int flutterpi_send_platform_message(
	struct flutterpi *flutterpi,
	const char *channel,
	const uint8_t *restrict message,
	size_t message_size,
	FlutterPlatformMessageResponseHandle *responsehandle
) {
	struct platform_message *msg;
	FlutterEngineResult result;
	int ok;
	
	if (runs_platform_tasks_on_current_thread(NULL)) {
		result = flutterpi.flutter.libflutter_engine.FlutterEngineSendPlatformMessage(
			flutterpi.flutter.engine,
			&(const FlutterPlatformMessage) {
				.struct_size = sizeof(FlutterPlatformMessage),
				.channel = channel,
				.message = message,
				.message_size = message_size,
				.response_handle = responsehandle
			}
		);
		if (result != kSuccess) {
			fprintf(stderr, "[flutter-pi] Error sending platform message. FlutterEngineSendPlatformMessage: %s\n", FLUTTER_RESULT_TO_STRING(result));
			return EIO;
		}
	} else {
		msg = calloc(1, sizeof *msg);
		if (msg == NULL) {
			return ENOMEM;
		}

		msg->is_response = false;
		msg->target_channel = strdup(channel);
		if (msg->target_channel == NULL) {
			free(msg);
			return ENOMEM;
		}

		msg->response_handle = responsehandle;
		
		if (message && message_size) {
			msg->message_size = message_size;
			msg->message = memdup(message, message_size);
			if (msg->message == NULL) {
				free(msg->target_channel);
				free(msg);
				return ENOMEM;
			}
		} else {
			msg->message = NULL;
			msg->message_size = 0;
		}

		ok = flutterpi_post_platform_task(
			on_send_platform_message,
			msg
		);
		if (ok != 0) {
			if (message && message_size) {
				free(msg->message);
			}
			free(msg->target_channel);
			free(msg);
			return ok;
		}
	}

	return 0;
}

int flutterpi_respond_to_platform_message(
	struct platform_message_response_handle *handle,
	const uint8_t *message,
	size_t message_size
) {
	struct flutterpi *flutterpi;
	struct platform_message *msg;
	FlutterEngineResult engine_result;
	int ok;

	flutterpi = handle->flutterpi;
	
	if (runs_platform_tasks_on_current_thread(NULL)) {
		engine_result = flutterpi->flutter.libflutter_engine.FlutterEngineSendPlatformMessageResponse(
			flutterpi->flutter.engine,
			handle->flutter_handle,
			message,
			message_size
		);
		if (engine_result != kSuccess) {
			fprintf(stderr, "[flutter-pi] Error sending platform message response. FlutterEngineSendPlatformMessageResponse: %s\n", FLUTTER_RESULT_TO_STRING(engine_result));
			return EIO;
		}
	} else {
		msg = malloc(sizeof *msg);
		if (msg == NULL) {
			return ENOMEM;
		}

		msg->is_response = true;
		msg->target_handle = handle;
		if (message && message_size) {
			msg->message_size = message_size;
			msg->message = memdup(message, message_size);
			if (msg->message == NULL) {
				free(msg);
				return ENOMEM;
			}
		} else {
			msg->message_size = 0;
			msg->message = 0;
		}

		ok = flutterpi_post_platform_task(
			on_send_platform_message,
			msg
		);
		if (ok != 0) {
			if (msg->message != NULL) {
				free(msg->message);
			}
			free(msg);
		}
	}

	return 0;
}
*/

static bool runs_platform_tasks_on_current_thread(void* userdata) {
	return flutterpi_runs_platform_tasks_on_current_thread(userdata);
}

bool flutterpi_runs_platform_tasks_on_current_thread(struct flutterpi *flutterpi) {
	DEBUG_ASSERT(flutterpi != NULL);
	return event_loop_processing_on_current_thread(flutterpi_get_event_loop(flutterpi, kPlatform));
}

static int run_main_loop(struct flutterpi *flutterpi) {
	DEBUG_ASSERT(flutterpi != NULL);
	return event_loop_process(flutterpi_get_event_loop(flutterpi, kPlatform));
}

/**************************
 * DISPLAY INITIALIZATION *
 **************************/
int flutterpi_fill_view_properties(
	struct flutterpi *flutterpi,
	bool has_orientation,
	enum device_orientation orientation,
	bool has_rotation,
	int rotation
) {
	enum device_orientation default_orientation = flutterpi->display.width >= flutterpi->display.height ? kLandscapeLeft : kPortraitUp;

	if (flutterpi->view.has_orientation) {
		if (flutterpi->view.has_rotation == false) {
			flutterpi->view.rotation = ANGLE_BETWEEN_ORIENTATIONS(default_orientation, flutterpi->view.orientation);
			flutterpi->view.has_rotation = true;
		}
	} else if (flutterpi->view.has_rotation) {
		for (int i = kPortraitUp; i <= kLandscapeRight; i++) {
			if (ANGLE_BETWEEN_ORIENTATIONS(default_orientation, i) == flutterpi->view.rotation) {
				flutterpi->view.orientation = i;
				flutterpi->view.has_orientation = true;
				break;
			}
		}
	} else {
		flutterpi->view.orientation = default_orientation;
		flutterpi->view.has_orientation = true;
		flutterpi->view.rotation = 0;
		flutterpi->view.has_rotation = true;
	}

	if (has_orientation) {
		flutterpi->view.rotation += ANGLE_BETWEEN_ORIENTATIONS(flutterpi->view.orientation, orientation);
		if (flutterpi->view.rotation >= 360) {
			flutterpi->view.rotation -= 360;
		}
		
		flutterpi->view.orientation = orientation;
	} else if (has_rotation) {
		for (int i = kPortraitUp; i <= kLandscapeRight; i++) {
			if (ANGLE_BETWEEN_ORIENTATIONS(default_orientation, i) == rotation) {
				flutterpi->view.orientation = i;
				flutterpi->view.rotation = rotation;
				break;
			}
		}
	}

	if ((flutterpi->view.rotation <= 45) || ((flutterpi->view.rotation >= 135) && (flutterpi->view.rotation <= 225)) || (flutterpi->view.rotation >= 315)) {
		flutterpi->view.width = flutterpi->display.width;
		flutterpi->view.height = flutterpi->display.height;
		flutterpi->view.width_mm = flutterpi->display.width_mm;
		flutterpi->view.height_mm = flutterpi->display.height_mm;
	} else {
		flutterpi->view.width = flutterpi->display.height;
		flutterpi->view.height = flutterpi->display.width;
		flutterpi->view.width_mm = flutterpi->display.height_mm;
		flutterpi->view.height_mm = flutterpi->display.width_mm;
	}

	if (flutterpi->view.rotation == 0) {
		flutterpi->view.view_to_display_transform = FLUTTER_TRANSLATION_TRANSFORMATION(0, 0);

		flutterpi->view.display_to_view_transform = FLUTTER_TRANSLATION_TRANSFORMATION(0, 0);
	} else if (flutterpi->view.rotation == 90) {
		flutterpi->view.view_to_display_transform = FLUTTER_ROTZ_TRANSFORMATION(90);
		flutterpi->view.view_to_display_transform.transX = flutterpi->display.width;

		flutterpi->view.display_to_view_transform = FLUTTER_ROTZ_TRANSFORMATION(-90);
		flutterpi->view.display_to_view_transform.transY = flutterpi->display.width;
	} else if (flutterpi->view.rotation == 180) {
		flutterpi->view.view_to_display_transform = FLUTTER_ROTZ_TRANSFORMATION(180);
		flutterpi->view.view_to_display_transform.transX = flutterpi->display.width;
		flutterpi->view.view_to_display_transform.transY = flutterpi->display.height;

		flutterpi->view.display_to_view_transform = FLUTTER_ROTZ_TRANSFORMATION(-180);
		flutterpi->view.display_to_view_transform.transX = flutterpi->display.width;
		flutterpi->view.display_to_view_transform.transY = flutterpi->display.height;
	} else if (flutterpi->view.rotation == 270) {
		flutterpi->view.view_to_display_transform = FLUTTER_ROTZ_TRANSFORMATION(270);
		flutterpi->view.view_to_display_transform.transY = flutterpi->display.height;

		flutterpi->view.display_to_view_transform = FLUTTER_ROTZ_TRANSFORMATION(-270);
		flutterpi->view.display_to_view_transform.transX = flutterpi->display.height;
	}

	return 0;
}

/*
		*
		* working (apparently)
		*
		//cut_word_from_string(extensions, "GL_EXT_blend_minmax");
		//cut_word_from_string(extensions, "GL_EXT_multi_draw_arrays");
		//cut_word_from_string(extensions, "GL_EXT_texture_format_BGRA8888");
		//cut_word_from_string(extensions, "GL_OES_compressed_ETC1_RGB8_texture");
		//cut_word_from_string(extensions, "GL_OES_depth24");
		//cut_word_from_string(extensions, "GL_OES_texture_npot");
		//cut_word_from_string(extensions, "GL_OES_vertex_half_float");
		//cut_word_from_string(extensions, "GL_OES_EGL_image");
		//cut_word_from_string(extensions, "GL_OES_depth_texture");
		//cut_word_from_string(extensions, "GL_AMD_performance_monitor");
		//cut_word_from_string(extensions, "GL_OES_EGL_image_external");
		//cut_word_from_string(extensions, "GL_EXT_occlusion_query_boolean");
		//cut_word_from_string(extensions, "GL_KHR_texture_compression_astc_ldr");
		//cut_word_from_string(extensions, "GL_EXT_compressed_ETC1_RGB8_sub_texture");
		//cut_word_from_string(extensions, "GL_EXT_draw_elements_base_vertex");
		//cut_word_from_string(extensions, "GL_EXT_texture_border_clamp");
		//cut_word_from_string(extensions, "GL_OES_draw_elements_base_vertex");
		//cut_word_from_string(extensions, "GL_OES_texture_border_clamp");
		//cut_word_from_string(extensions, "GL_KHR_texture_compression_astc_sliced_3d");
		//cut_word_from_string(extensions, "GL_MESA_tile_raster_order");

		*
		* should be working, but isn't
		*
		cut_word_from_string(extensions, "GL_EXT_map_buffer_range");

		*
		* definitely broken
		*
		cut_word_from_string(extensions, "GL_OES_element_index_uint");
		cut_word_from_string(extensions, "GL_OES_fbo_render_mipmap");
		cut_word_from_string(extensions, "GL_OES_mapbuffer");
		cut_word_from_string(extensions, "GL_OES_rgb8_rgba8");
		cut_word_from_string(extensions, "GL_OES_stencil8");
		cut_word_from_string(extensions, "GL_OES_texture_3D");
		cut_word_from_string(extensions, "GL_OES_packed_depth_stencil");
		cut_word_from_string(extensions, "GL_OES_get_program_binary");
		cut_word_from_string(extensions, "GL_APPLE_texture_max_level");
		cut_word_from_string(extensions, "GL_EXT_discard_framebuffer");
		cut_word_from_string(extensions, "GL_EXT_read_format_bgra");
		cut_word_from_string(extensions, "GL_EXT_frag_depth");
		cut_word_from_string(extensions, "GL_NV_fbo_color_attachments");
		cut_word_from_string(extensions, "GL_OES_EGL_sync");
		cut_word_from_string(extensions, "GL_OES_vertex_array_object");
		cut_word_from_string(extensions, "GL_EXT_unpack_subimage");
		cut_word_from_string(extensions, "GL_NV_draw_buffers");
		cut_word_from_string(extensions, "GL_NV_read_buffer");
		cut_word_from_string(extensions, "GL_NV_read_depth");
		cut_word_from_string(extensions, "GL_NV_read_depth_stencil");
		cut_word_from_string(extensions, "GL_NV_read_stencil");
		cut_word_from_string(extensions, "GL_EXT_draw_buffers");
		cut_word_from_string(extensions, "GL_KHR_debug");
		cut_word_from_string(extensions, "GL_OES_required_internalformat");
		cut_word_from_string(extensions, "GL_OES_surfaceless_context");
		cut_word_from_string(extensions, "GL_EXT_separate_shader_objects");
		cut_word_from_string(extensions, "GL_KHR_context_flush_control");
		cut_word_from_string(extensions, "GL_KHR_no_error");
		cut_word_from_string(extensions, "GL_KHR_parallel_shader_compile");
*/

/**************
 * USER INPUT *
 **************/
static void on_flutter_pointer_event(void *userdata, const FlutterPointerEvent *events, size_t n_events) {
	FlutterEngineResult engine_result;
	struct flutterpi *flutterpi;

	flutterpi = userdata;

	engine_result = flutterpi->flutter.libflutter_engine->FlutterEngineSendPointerEvent(
		flutterpi->flutter.engine,
		events,
		n_events
	);
	if (engine_result != kSuccess) {
		LOG_FLUTTERPI_ERROR("Error sending touchscreen / mouse events to flutter. FlutterEngineSendPointerEvent: %s\n", FLUTTER_RESULT_TO_STRING(engine_result));
		flutterpi_schedule_exit(flutterpi);
	}
}

static void on_utf8_character(void *userdata, uint8_t *character) {
#ifdef BUILD_TEXT_INPUT_PLUGIN
	struct flutterpi *flutterpi;
	int ok;

	flutterpi = userdata;

	if (flutterpi->private->textin != NULL) {
		ok = textin_on_utf8_char(flutterpi->private->textin, character);
		if (ok != 0) {
			LOG_FLUTTERPI_ERROR("Error handling keyboard event. textin_on_utf8_char: %s\n", strerror(ok));
			flutterpi_schedule_exit(flutterpi);
		}
	}
#endif
}

static void on_xkb_keysym(void *userdata, xkb_keysym_t keysym) {
#ifdef BUILD_TEXT_INPUT_PLUGIN
	struct flutterpi *flutterpi;
	int ok;

	flutterpi = userdata;

	if (flutterpi->private->textin != NULL) {
		ok = textin_on_xkb_keysym(flutterpi->private->textin, keysym);
		if (ok != 0) {
			LOG_FLUTTERPI_ERROR("Error handling keyboard event. textin_on_xkb_keysym: %s\n", strerror(ok));
			flutterpi_schedule_exit(flutterpi);
		}
	}
#endif
}

static void on_gtk_keyevent(
	void *userdata,
	uint32_t unicode_scalar_values,
    uint32_t key_code,
    uint32_t scan_code,
    uint32_t modifiers,
    bool is_down
) {
#ifdef BUILD_RAW_KEYBOARD_PLUGIN
	struct flutterpi *flutterpi;
	int ok;

	flutterpi = userdata;

	if (flutterpi->private->rawkb != NULL) {
		ok = rawkb_send_gtk_keyevent(
			flutterpi->private->rawkb,
			unicode_scalar_values,
			key_code,
			scan_code,
			modifiers,
			is_down
		);
		if (ok != 0) {
			LOG_FLUTTERPI_ERROR("Error handling keyboard event. rawkb_send_gtk_keyevent: %s\n", strerror(ok));
			flutterpi_schedule_exit(flutterpi);
		}
	}
#endif
}

static void on_set_cursor_enabled(void *userdata, bool enabled) {
	struct flutterpi *flutterpi;
	int ok;

	flutterpi = userdata;

	ok = compositor_set_cursor_state(
		flutterpi->compositor,
		true, enabled,
		false, 0,
		false, 0
	);
	if (ok != 0) {
		LOG_FLUTTERPI_ERROR("Error enabling / disabling mouse cursor. compositor_apply_cursor_state: %s\n", strerror(ok));
	}
}

static void on_move_cursor(void *userdata, unsigned int x, unsigned int y) {
	struct flutterpi *flutterpi;
	int ok;

	flutterpi = userdata;

	ok = compositor_set_cursor_pos(flutterpi->compositor, x, y);
	if (ok != 0) {
		LOG_FLUTTERPI_ERROR("Error moving mouse cursor. compositor_set_cursor_pos: %s\n", strerror(ok));
	}
}

static const struct user_input_interface user_input_interface = {
    .on_flutter_pointer_event = on_flutter_pointer_event,
    .on_utf8_character = on_utf8_character,
    .on_xkb_keysym = on_xkb_keysym,
    .on_gtk_keyevent = on_gtk_keyevent,
    .on_set_cursor_enabled = on_set_cursor_enabled,
    .on_move_cursor = on_move_cursor
};


/**************************
 * FLUTTER INITIALIZATION *
 **************************/

int flutterpi_schedule_exit(struct flutterpi *flutterpi) {
	(void) flutterpi;
	/// TODO: Implement
	return 0;
}

static int setup_paths(
	const char *asset_bundle_path,
	enum flutter_runtime_mode runtime_mode,
	char **icu_data_path_out,
	char **app_elf_path_out
) {
	char *kernel_blob_path;
	char *icu_data_path;
	char *app_elf_path;
	int ok;
	
#	define PATH_EXISTS(path) (access((path), R_OK) == 0)

	if (!PATH_EXISTS(asset_bundle_path)) {
		fprintf(stderr, "Asset Bundle Directory \"%s\" does not exist\n", asset_bundle_path);
		return ENOENT;
	}
	
	ok = asprintf(&kernel_blob_path, "%s/kernel_blob.bin", asset_bundle_path);
	if (ok == -1) {
		return ENOMEM;
	}
	
	ok = asprintf(&app_elf_path, "%s/app.so", asset_bundle_path);
	if (ok == -1) {
		return ENOMEM;
	}

	if (runtime_mode == kDebug) {
		if (!PATH_EXISTS(kernel_blob_path)) {
			LOG_FLUTTERPI_ERROR("Could not find \"kernel.blob\" file inside \"%s\", which is required for debug mode.\n", asset_bundle_path);
			free(kernel_blob_path);
			free(app_elf_path);
			return ENOENT;
		}
	} else if (runtime_mode == kRelease) {
		if (!PATH_EXISTS(app_elf_path)) {
			LOG_FLUTTERPI_ERROR("Could not find \"app.so\" file inside \"%s\", which is required for release and profile mode.\n", asset_bundle_path);
			free(kernel_blob_path);
			free(app_elf_path);
			return ENOENT;
		}
	}

	free(kernel_blob_path);
	kernel_blob_path = NULL;

	ok = asprintf(&icu_data_path, "/usr/lib/icudtl.dat");
	if (ok == -1) {
		free(app_elf_path);
		free(icu_data_path);
		return ENOMEM;
	}

	if (!PATH_EXISTS(icu_data_path)) {
		LOG_FLUTTERPI_ERROR("Could not find \"icudtl.dat\" file inside \"/usr/lib/\".\n");
		free(app_elf_path);
		free(icu_data_path);
		return ENOENT;
	}

	*icu_data_path_out = icu_data_path;
	*app_elf_path_out = app_elf_path;

	return 0;

#	undef PATH_EXISTS
}

static bool on_user_input_fd_ready(int fd, uint32_t events, void *userdata) {
	struct user_input *input = userdata;
	
	(void) fd;
	(void) events;
	DEBUG_ASSERT_NOT_NULL(userdata);
	
	user_input_on_fd_ready(input);
	
	return true;
}

struct flutterpi *flutterpi_new_from_args(
	enum flutter_runtime_mode runtime_mode,
	bool has_rotation,
	int rotation,
	bool has_orientation,
	enum device_orientation orientation,
	bool has_explicit_dimensions,
	unsigned int width_mm, unsigned int height_mm,
	char *asset_bundle_path,
	int n_engine_args,
	const char **engine_args
) {
	struct flutterpi_private *private;
	struct flutter_messenger *messenger;
	struct libflutter_engine *engine_lib;
	struct texture_registry *texture_registry;
	struct plugin_registry *plugin_registry;
	struct egl_client_info *egl_client_info;
	FlutterRendererConfig renderer_config;
	FlutterEngineAOTData aot_data;
	FlutterEngineResult engine_result;
	struct event_loop *platform, *render;
	struct compositor *compositor;
	FlutterCompositor flutter_compositor;
	struct user_input *input;
	struct flutterpi *flutterpi;
	struct renderer *renderer;
	struct display *const *displays;
	FlutterEngine engine;
	struct libegl *libegl;
#ifdef HAS_KMS
	struct kmsdev *kmsdev;
#endif
	size_t n_displays;
	char **engine_argv_dup;
	char *app_elf_path, *icu_data_path;
	bool use_kms;
	int ok;
	
	// allocate memory for our flutterpi instance
	flutterpi = malloc(sizeof *flutterpi);
	if (flutterpi == NULL) {
		goto fail_return_null;
	}

	private = malloc(sizeof *private);
	if (private == NULL) {
		goto fail_free_flutterpi;
	}

	// copy the asset bundle path and engine options,
	// so the caller doesn't need to pass the ownership
	asset_bundle_path = strdup(asset_bundle_path);
	if (asset_bundle_path == NULL) {
		goto fail_free_private;
	}

	engine_argv_dup = calloc(n_engine_args + 1, sizeof engine_args);
	if (engine_argv_dup == NULL) {
		goto fail_free_asset_bundle_path;
	}

	engine_argv_dup[0] = strdup("flutter-pi");
	if (engine_argv_dup[0] == NULL) {
		goto fail_free_engine_args;
	}
	
	for (int i = 0; i < n_engine_args; i++) {
		engine_args[i + 1] = strdup(engine_args[i]);
		if (engine_args[i + 1] == NULL) {
			goto fail_free_engine_args;
		}
	}

	// setup the paths to our kernel_blob.bin, icudtl.dat and app.so files
	// depending on the specified asset bundle path and runtime mode 
	ok = setup_paths(
		asset_bundle_path,
		runtime_mode,
		&icu_data_path,
		&app_elf_path
	);
	if (ok != 0) {
		goto fail_free_engine_args;
	}

	platform = event_loop_create(true, pthread_self());
	if (platform == NULL) {
		goto fail_free_paths;
	}

	render = event_loop_create(false, (pthread_t) 0);
	if (render == NULL) {
		goto fail_free_platform_event_loop;
	}

	use_kms = true;

	// initialize the display
	kmsdev = NULL;
	if (use_kms) {
		kmsdev = kmsdev_new_auto(render);
		if (kmsdev == NULL) {
			goto fail_free_render_event_loop;
		}

		for (int i = 0; i < kmsdev_get_n_connectors(kmsdev); i++) {
			if (kmsdev_is_connector_connected(kmsdev, i)) {
				ok = kmsdev_configure_crtc_with_preferences(
					kmsdev,
					0, /// TODO: Implement
					i,
					(enum kmsdev_mode_preference[]) {
						kKmsdevModePreferencePreferred,
						kKmsdevModePreferenceHighestRefreshrate,
						kKmsdevModePreferenceHighestResolution,
						kKmsdevModePreferenceProgressive,
						kKmsdevModePreferenceNone
					}
				);
				if (ok != 0) {
					goto fail_destroy_kmsdev;
				}
			}
		}

		ok = kmsdev_configure(
			kmsdev,
			&(const struct kms_config) {
				.n_display_configs = 1,
				.display_configs = (struct kms_display_config[]) {
					{
						.connector_name = "DSI0",
						.has_explicit_mode = false,
						.preferences = (enum kmsdev_mode_preference[5]) {
							kKmsdevModePreferencePreferred,
							kKmsdevModePreferenceHighestRefreshrate,
							kKmsdevModePreferenceHighestResolution,
							kKmsdevModePreferenceProgressive,
							kKmsdevModePreferenceNone
						},
						.has_explicit_dimensions = false
					}
				}
			}
		);
		if (ok != 0) {
			goto fail_destroy_kmsdev;
		}

		kmsdev_get_displays(kmsdev, &displays, &n_displays);
	} else {
		struct display *display = fbdev_display_new_from_path(
			"/dev/fb0",
			&(struct fbdev_display_config) {
				.has_explicit_dimensions = false,
				.width_mm = 0,
				.height_mm = 0
			}
		);
		if (display == NULL) {
			goto fail_free_render_event_loop;
		}

		struct display **modifiable_displays;

		modifiable_displays = malloc(sizeof *modifiable_displays);
		if (displays == NULL) {
			display_destroy(display);
			goto fail_free_render_event_loop;
		}

		modifiable_displays[0] = display;

		displays = modifiable_displays;
		n_displays = 1;
	}

	libegl = libegl_load();
	if (libegl == NULL) {
		goto fail_destroy_displays;
	}

	egl_client_info = egl_client_info_new(libegl);
	if (egl_client_info == NULL) {
		goto fail_unload_libegl;
	}

	engine_lib = libflutter_engine_load_for_runtime_mode(runtime_mode);
	if (engine_lib == NULL) {
		goto fail_destroy_egl_client_info;
	}

	compositor = compositor_new(
		displays,
		n_displays,
		libegl,
		egl_client_info,
		NULL /*libgl*/,
		render,
		&gl_interface,
		&sw_interface,
		&(struct flutter_tracing_interface) {
			.get_current_time = engine_lib->FlutterEngineGetCurrentTime,
			.trace_event_begin = engine_lib->FlutterEngineTraceEventDurationBegin,
			.trace_event_end = engine_lib->FlutterEngineTraceEventDurationEnd,
			.trace_event_instant = engine_lib->FlutterEngineTraceEventInstant
		},
		&(struct flutter_view_interface) {
			.send_window_metrics_event = engine_lib->FlutterEngineSendWindowMetricsEvent,
			.notify_display_update = NULL
		}
	);
	if (compositor == NULL) {
		goto fail_unload_engine;
	}

	renderer = compositor_get_renderer(compositor);

	compositor_fill_flutter_compositor(compositor, &flutter_compositor);
	compositor_fill_flutter_renderer_config(compositor, &renderer_config);

	texture_registry = NULL;

	bool engine_is_aot = engine_lib->FlutterEngineRunsAOTCompiledDartCode();
	if ((engine_is_aot == true) && (flutterpi->flutter.runtime_mode != kRelease)) {
		fprintf(
			stderr,
			"[flutter-pi] The flutter engine was built for release (AOT) mode,\n"
			"             but flutter-pi was not started up in release mode. \n"
			"             Either you swap out the libflutter_engine.so \n"
			"             with one that was built for debug mode, or you start\n"
			"             flutter-pi with the --release flag and make sure\n"
			"             a valid \"app.so\" is located inside the asset bundle\n"
			"             directory.\n"
		);
		
		goto fail_destroy_compositor;
	} else if ((engine_is_aot == false) && (flutterpi->flutter.runtime_mode != kDebug)) {
		fprintf(
			stderr,
			"[flutter-pi] The flutter engine was built for debug mode,\n"
			"             but flutter-pi was started up in release mode.\n"
			"             Either you swap out the libflutter_engine.so\n"
			"             with one that was built for release mode, or you\n"
			"             start flutter-pi without the --release flag.\n"
		);

		goto fail_destroy_compositor;
	}

	if (flutterpi->flutter.runtime_mode == kRelease) {
		FlutterEngineAOTDataSource aot_source = (FlutterEngineAOTDataSource) {
			.type = kFlutterEngineAOTDataSourceTypeElfPath,
			.elf_path = app_elf_path
		};

		engine_result = engine_lib->FlutterEngineCreateAOTData(&aot_source, &aot_data);
		
		free(app_elf_path);
		app_elf_path = NULL;

		if (engine_result != kSuccess) {
			LOG_FLUTTERPI_ERROR("Could not load AOT data. FlutterEngineCreateAOTData: %s\n", FLUTTER_RESULT_TO_STRING(engine_result));
			goto fail_destroy_compositor;
		}
	}

	engine_result = engine_lib->FlutterEngineInitialize(
		FLUTTER_ENGINE_VERSION,
		&renderer_config,
		&(FlutterProjectArgs) {
			.struct_size = sizeof(FlutterProjectArgs),
			.assets_path = asset_bundle_path,
			.main_path__unused__ = NULL,
			.packages_path__unused__ = NULL,
			.icu_data_path = icu_data_path,
			.command_line_argc = n_engine_args,
			.command_line_argv = (const char * const*) engine_args,
			.platform_message_callback = on_platform_message,
			.vm_snapshot_data = NULL,
			.vm_snapshot_data_size = 0,
			.vm_snapshot_instructions = NULL,
			.vm_snapshot_instructions_size = 0,
			.isolate_snapshot_data = NULL,
			.isolate_snapshot_data_size = 0,
			.isolate_snapshot_instructions = NULL,
			.isolate_snapshot_instructions_size = 0,
			.root_isolate_create_callback = NULL,
			.update_semantics_node_callback = NULL,
			.update_semantics_custom_action_callback = NULL,
			.persistent_cache_path = NULL,
			.is_persistent_cache_read_only = false,
			.vsync_callback = NULL /*on_frame_request*/,
			.custom_dart_entrypoint = NULL,
			.custom_task_runners = &(FlutterCustomTaskRunners) {
				.struct_size = sizeof(FlutterCustomTaskRunners),
				.platform_task_runner = &(FlutterTaskRunnerDescription) {
					.struct_size = sizeof(FlutterTaskRunnerDescription),
					.user_data = flutterpi,
					.runs_task_on_current_thread_callback = runs_platform_tasks_on_current_thread,
					.post_task_callback = on_post_flutter_task
				},
				.render_task_runner = NULL
			},
			.shutdown_dart_vm_when_done = true,
			.compositor = &flutter_compositor,
			.dart_old_gen_heap_size = -1,
			.aot_data = aot_data,
			.compute_platform_resolved_locale_callback = NULL,
			.dart_entrypoint_argc = 0,
			.dart_entrypoint_argv = NULL
		},
		flutterpi,
		&engine
	);
	if (engine_result != kSuccess) {
		LOG_FLUTTERPI_ERROR("Could not run the flutter engine. FlutterEngineInitialize: %s\n", FLUTTER_RESULT_TO_STRING(engine_result));
		goto fail_collect_aot_data;
	}

	messenger = fm_new(
		(runs_platform_tasks_on_current_thread_t) runs_platform_tasks_on_current_thread,
		(post_platform_task_t) flutterpi_post_platform_task,
		engine_lib->FlutterEngineSendPlatformMessage,
		engine_lib->FlutterPlatformMessageCreateResponseHandle,
		engine_lib->FlutterPlatformMessageReleaseResponseHandle,
		engine_lib->FlutterEngineSendPlatformMessageResponse,
		flutterpi,
		engine
	);
	if (messenger == NULL) {
		goto fail_deinitialize_engine;
	}

	// create the user input "subsystem".
	// display needs to be initialized before this so we can set
	// the correct view here.
	input = user_input_new(
		&user_input_interface,
		flutterpi,
		&FLUTTER_TRANSLATION_TRANSFORMATION(0, 0),
		display_get_width(displays[0]),
		display_get_height(displays[0])
	);
	if (input == NULL) {
		LOG_FLUTTERPI_ERROR("ERROR: Couldn't initialize user input.\n");
		goto fail_destroy_messenger;
	}

	event_loop_add_io(
		platform,
		user_input_get_fd(input),
		EPOLLIN | EPOLLPRI,
		on_user_input_fd_ready,
		input
	);

	// initialize the plugin registry
	plugin_registry = plugin_registry_new(flutterpi);
	if (plugin_registry == NULL) {
		LOG_FLUTTERPI_ERROR("Couldn't initialize plugin registry.\n");
		goto fail_destroy_user_input;
	}

	private->flutterpi = flutterpi;
	private->textin = NULL;
	private->rawkb = NULL;
	private->render = render;
	private->platform = platform;

	memset(flutterpi, 0, sizeof *flutterpi);

	/// TODO: Add more initializers in case the struct changes.
	flutterpi->private = private;
	flutterpi->renderer = renderer;

	if (has_explicit_dimensions) {
		flutterpi->display.width_mm = width_mm;
		flutterpi->display.height_mm = height_mm;
	}

	flutterpi->view.has_rotation = has_rotation;
	flutterpi->view.rotation = rotation;
	flutterpi->view.has_orientation = has_orientation;
	flutterpi->view.orientation = orientation;

	flutterpi->flutter.asset_bundle_path = asset_bundle_path;
	flutterpi->flutter.icu_data_path = icu_data_path;
	flutterpi->flutter.engine_argc = n_engine_args + 1;
	flutterpi->flutter.engine_argv = engine_argv_dup;
	flutterpi->flutter.runtime_mode = runtime_mode;
	flutterpi->flutter.libflutter_engine = engine_lib;
	flutterpi->flutter.engine = engine;

	flutterpi->compositor = compositor;
	flutterpi->renderer = renderer;
	flutterpi->texture_registry = texture_registry;
	flutterpi->plugin_registry = plugin_registry;
	flutterpi->flutter_messenger = messenger;

	fm_set_engine(messenger, engine);
	plugin_registry_ensure_plugins_initialized(plugin_registry);

	return flutterpi;


	fail_destroy_user_input:
	user_input_destroy(input);

	fail_destroy_messenger:
	fm_destroy(messenger);

	fail_deinitialize_engine:
	engine_lib->FlutterEngineDeinitialize(engine);

	fail_collect_aot_data:
	if (engine_is_aot) {
		engine_lib->FlutterEngineCollectAOTData(aot_data);
	}

	fail_destroy_compositor:
	compositor_destroy(compositor);

	fail_unload_engine:
	libflutter_engine_unload(engine_lib);

	fail_destroy_egl_client_info:
	egl_client_info_destroy(egl_client_info);

	fail_unload_libegl:
	libegl_unload(libegl);

	fail_destroy_displays:
	//for (size_t i = 0; i < n_displays; i++) display_destroy(displays[i]);
	//free(displays);

	fail_destroy_kmsdev:
	if (kmsdev != NULL) kmsdev_destroy(kmsdev);

	fail_free_render_event_loop:
	event_loop_destroy(render);

	fail_free_platform_event_loop:
	event_loop_destroy(platform);

	fail_free_paths:
	if (app_elf_path != NULL) {
		free(app_elf_path);
	}
	if (icu_data_path != NULL) {
		free(icu_data_path);
	}

	fail_free_engine_args:
	for (int i = 0; i < (n_engine_args + 1); i++) {
		if (engine_argv_dup[i] != NULL) {
			free(engine_argv_dup[i]);
		} else {
			break;
		}
	}
	free(engine_argv_dup);

	fail_free_asset_bundle_path:
	free(asset_bundle_path);	

	fail_free_private:
	free(private);

	fail_free_flutterpi:
	free(flutterpi);

	fail_return_null:
	return NULL;
}

struct flutterpi *flutterpi_new_from_cmdline(
	int argc,
	char **argv
) {
	enum device_orientation orientation;
	unsigned int width_mm, height_mm;
	glob_t *input_devices_glob = {0};
	bool has_orientation, has_rotation, has_explicit_dimensions, input_specified, finished_parsing_options;
	int runtime_mode_int, rotation, ok, opt;

	struct option long_options[] = {
		{"release", no_argument, &runtime_mode_int, kRelease},
		{"input", required_argument, NULL, 'i'},
		{"orientation", required_argument, NULL, 'o'},
		{"rotation", required_argument, NULL, 'r'},
		{"dimensions", required_argument, NULL, 'd'},
		{"help", no_argument, 0, 'h'},
		{0, 0, 0, 0}
	};

	runtime_mode_int = kDebug;
	has_orientation = false;
	has_rotation = false;
	input_specified = false;
	finished_parsing_options = false;

	while (!finished_parsing_options) {
		opt = getopt_long(argc, argv, "+i:o:r:d:h", long_options, NULL);

		switch (opt) {
			case 0:
				// flag was encountered. just continue
				break;
			case 'i':
				if (input_devices_glob == NULL) {
					input_devices_glob = calloc(1, sizeof *input_devices_glob);
					if (input_devices_glob == NULL) {
						fprintf(stderr, "Out of memory\n");
						return NULL;
					}
				}

				glob(optarg, GLOB_BRACE | GLOB_TILDE | (input_specified ? GLOB_APPEND : 0), NULL, input_devices_glob);
				input_specified = true;
				break;

			case 'o':
				if (STREQ(optarg, "portrait_up")) {
					orientation = kPortraitUp;
					has_orientation = true;
				} else if (STREQ(optarg, "landscape_left")) {
					orientation = kLandscapeLeft;
					has_orientation = true;
				} else if (STREQ(optarg, "portrait_down")) {
					orientation = kPortraitDown;
					has_orientation = true;
				} else if (STREQ(optarg, "landscape_right")) {
					orientation = kLandscapeRight;
					has_orientation = true;
				} else {
					fprintf(
						stderr, 
						"ERROR: Invalid argument for --orientation passed.\n"
						"Valid values are \"portrait_up\", \"landscape_left\", \"portrait_down\", \"landscape_right\".\n"
						"%s", 
						usage
					);
					return NULL;
				}

				break;
			
			case 'r':
				errno = 0;
				long rotation_long = strtol(optarg, NULL, 0);
				if ((errno != 0) || ((rotation_long != 0) && (rotation_long != 90) && (rotation_long != 180) && (rotation_long != 270))) {
					fprintf(
						stderr,
						"ERROR: Invalid argument for --rotation passed.\n"
						"Valid values are 0, 90, 180, 270.\n"
						"%s",
						usage
					);
					return NULL;
				}

				rotation = rotation_long;
				has_rotation = true;
				break;
			
			case 'd': ;
				ok = sscanf(optarg, "%u,%u", &width_mm, &height_mm);
				if ((ok == 0) || (ok == EOF)) {
					fprintf(stderr, "ERROR: Invalid argument for --dimensions passed.\n%s", usage);
					return NULL;
				}

				has_explicit_dimensions = true;

				break;
			
			case 'h':
				printf("%s", usage);
				return NULL;

			case '?':
			case ':':
				fprintf(stderr, "Invalid option specified.\n%s", usage);
				return NULL;
			
			case -1:
				finished_parsing_options = true;
				break;
			
			default:
				break;
		}
	}

	if (optind >= argc) {
		fprintf(stderr, "error: expected asset bundle path after options.\n");
		printf("%s", usage);
		return NULL;
	}

	return flutterpi_new_from_args(
		(enum flutter_runtime_mode) runtime_mode_int,
		has_rotation, rotation,
		has_orientation, orientation,
		has_explicit_dimensions, width_mm, height_mm,
		argv[optind],
		argc - (optind + 1),
		(const char **) &argv[optind + 1]
	);
}

int flutterpi_run(struct flutterpi *flutterpi) {
	FlutterEngineResult engine_result;
	int ok;

	engine_result = flutterpi->flutter.libflutter_engine->FlutterEngineRunInitialized(flutterpi->flutter.engine);
	if (engine_result != kSuccess) {
		LOG_FLUTTERPI_ERROR("Could not run flutter engine. FlutterEngineRunInitialized: %s\n", FLUTTER_RESULT_TO_STRING(engine_result));
		return EINVAL;
	}

	ok = compositor_setup_flutter_views(
		flutterpi->compositor,
		flutterpi->flutter.engine
	);
	if (ok != 0) {
		return ok;
	}

	return run_main_loop(flutterpi);
}

void flutterpi_destroy(struct flutterpi *flutterpi) {
	(void) flutterpi;
	return;
}


int main(int argc, char **argv) {
	struct flutterpi *flutterpi;
	int ok;

	printf("INIT\n");
	flutterpi = flutterpi_new_from_cmdline(argc, argv);
	if (flutterpi == NULL) {
		return EXIT_FAILURE;
	}

	printf("RUN\n");
	ok = flutterpi_run(flutterpi);

	printf("DESTROY\n");
	if (ok != 0) {
		flutterpi_destroy(flutterpi);
		return EXIT_FAILURE;
	}

	flutterpi_destroy(flutterpi);

	return EXIT_SUCCESS;
}
