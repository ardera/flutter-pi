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
#include <linux/input.h>
#include <math.h>
#include <limits.h>
#include <float.h>
#include <assert.h>
#include <time.h>
#include <sys/eventfd.h>
#include <getopt.h>

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
#include <console_keyboard.h>
#include <platformchannel.h>
#include <pluginregistry.h>
#include <texture_registry.h>
//#include <plugins/services.h>
#include <plugins/text_input.h>
#include <plugins/raw_keyboard.h>

const char const* usage ="\
flutter-pi - run flutter apps on your Raspberry Pi.\n\
\n\
USAGE:\n\
  flutter-pi [options] <asset bundle path> [flutter engine options]\n\
\n\
OPTIONS:\n\
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
  -r, --release              Run the app in release mode. This AOT snapshot\n\
                             of the app (\"app.so\") must be located inside the\n\
                             asset bundle directory.\n\
                             \n\
  -p, --profile              Run the app in profile mode. This runtime mode, too\n\
                             depends on the AOT snapshot.\n\
                             \n\
  -d, --debug                Run the app in debug mode. This is the default.\n\
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
  -h                         Show this help and exit.\n\
\n\
EXAMPLES:\n\
  flutter-pi -i \"/dev/input/event{0,1}\" -i \"/dev/input/event{2,3}\" /home/pi/helloworld_flutterassets\n\
  flutter-pi -i \"/dev/input/mouse*\" /home/pi/helloworld_flutterassets\n\
  flutter-pi -o portrait_up ./flutter_assets\n\
  flutter-pi -r 90 ./flutter_assets\n\
  flutter-pi /home/pi/helloworld_flutterassets\n\
\n\
SEE ALSO:\n\
  Author:  Hannes Winkler, a.k.a ardera\n\
  Source:  https://github.com/ardera/flutter-pi\n\
  License: MIT\n\
\n\
  For instructions on how to build an asset bundle, please see the linked\n\
    git repository.\n\
  For a list of options you can pass to the flutter engine, look here:\n\
    https://github.com/flutter/engine/blob/master/shell/common/switches.h\n\
";

struct flutterpi flutterpi;

static int post_platform_task(
	int (*callback)(void *userdata),
	void *userdata
);

static bool runs_platform_tasks_on_current_thread(void *userdata);

/*********************
 * FLUTTER CALLBACKS *
 *********************/
/// Called on some flutter internal thread when the flutter
/// rendering EGLContext should be made current.
static bool on_make_current(void* userdata) {
	EGLint egl_error;

	eglGetError();

	eglMakeCurrent(flutterpi.egl.display, flutterpi.egl.surface, flutterpi.egl.surface, flutterpi.egl.flutter_render_context);
	if (egl_error = eglGetError(), egl_error != EGL_SUCCESS) {
		fprintf(stderr, "[flutter-pi] Could not make the flutter rendering EGL context current. eglMakeCurrent: 0x%08X\n", egl_error);
		return false;
	}
	
	return true;
}

/// Called on some flutter internal thread to
/// clear the EGLContext.
static bool on_clear_current(void* userdata) {
	EGLint egl_error;

	eglGetError();

	eglMakeCurrent(flutterpi.egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	if (egl_error = eglGetError(), egl_error != EGL_SUCCESS) {
		fprintf(stderr, "[flutter-pi] Could not clear the flutter EGL context. eglMakeCurrent: 0x%08X\n", egl_error);
		return false;
	}
	
	return true;
}

/// Called on some flutter internal thread when the flutter
/// contents should be presented to the screen.
/// (Won't be called since we're supplying a compositor,
/// still needs to be present)
static bool on_present(void *userdata) {
	// no-op
	return true;
}

/// Called on some flutter internal thread to get the
/// GL FBO id flutter should render into
/// (Won't be called since we're supplying a compositor,
/// still needs to be present)
static uint32_t fbo_callback(void* userdata) {
	return 0;
}

/// Called on some flutter internal thread when the flutter
/// resource uploading EGLContext should be made current.
static bool on_make_resource_current(void *userdata) {
	EGLint egl_error;

	eglGetError();

	eglMakeCurrent(flutterpi.egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, flutterpi.egl.flutter_resource_uploading_context);
	if (egl_error = eglGetError(), egl_error != EGL_SUCCESS) {
		fprintf(stderr, "[flutter-pi] Could not make the flutter resource uploading EGL context current. eglMakeCurrent: 0x%08X\n", egl_error);
		return false;
	}
	
	return true;
}

/// Cut a word from a string, mutating "string"
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

/// An override for glGetString since the real glGetString
/// won't work.
static const GLubyte *hacked_glGetString(GLenum name) {
	static GLubyte *extensions = NULL;

	if (name != GL_EXTENSIONS)
		return glGetString(name);

	if (extensions == NULL) {
		GLubyte *orig_extensions = (GLubyte *) glGetString(GL_EXTENSIONS);
		
		extensions = malloc(strlen((const char*)orig_extensions) + 1);
		if (!extensions) {
			fprintf(stderr, "Could not allocate memory for modified GL_EXTENSIONS string\n");
			return NULL;
		}

		strcpy((char*)extensions, (const char*)orig_extensions);

		/*
			* working (apparently)
			*/
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

		/*
		* should be working, but isn't
		*/
		cut_word_from_string((char*)extensions, "GL_EXT_map_buffer_range");

		/*
		* definitely broken
		*/
		cut_word_from_string((char*)extensions, "GL_OES_element_index_uint");
		cut_word_from_string((char*)extensions, "GL_OES_fbo_render_mipmap");
		cut_word_from_string((char*)extensions, "GL_OES_mapbuffer");
		cut_word_from_string((char*)extensions, "GL_OES_rgb8_rgba8");
		cut_word_from_string((char*)extensions, "GL_OES_stencil8");
		cut_word_from_string((char*)extensions, "GL_OES_texture_3D");
		cut_word_from_string((char*)extensions, "GL_OES_packed_depth_stencil");
		cut_word_from_string((char*)extensions, "GL_OES_get_program_binary");
		cut_word_from_string((char*)extensions, "GL_APPLE_texture_max_level");
		cut_word_from_string((char*)extensions, "GL_EXT_discard_framebuffer");
		cut_word_from_string((char*)extensions, "GL_EXT_read_format_bgra");
		cut_word_from_string((char*)extensions, "GL_EXT_frag_depth");
		cut_word_from_string((char*)extensions, "GL_NV_fbo_color_attachments");
		cut_word_from_string((char*)extensions, "GL_OES_EGL_sync");
		cut_word_from_string((char*)extensions, "GL_OES_vertex_array_object");
		cut_word_from_string((char*)extensions, "GL_EXT_unpack_subimage");
		cut_word_from_string((char*)extensions, "GL_NV_draw_buffers");
		cut_word_from_string((char*)extensions, "GL_NV_read_buffer");
		cut_word_from_string((char*)extensions, "GL_NV_read_depth");
		cut_word_from_string((char*)extensions, "GL_NV_read_depth_stencil");
		cut_word_from_string((char*)extensions, "GL_NV_read_stencil");
		cut_word_from_string((char*)extensions, "GL_EXT_draw_buffers");
		cut_word_from_string((char*)extensions, "GL_KHR_debug");
		cut_word_from_string((char*)extensions, "GL_OES_required_internalformat");
		cut_word_from_string((char*)extensions, "GL_OES_surfaceless_context");
		cut_word_from_string((char*)extensions, "GL_EXT_separate_shader_objects");
		cut_word_from_string((char*)extensions, "GL_KHR_context_flush_control");
		cut_word_from_string((char*)extensions, "GL_KHR_no_error");
		cut_word_from_string((char*)extensions, "GL_KHR_parallel_shader_compile");
	}

	return extensions;
}

/// Called by flutter 
static void *proc_resolver(
	void* userdata,
	const char* name
) {
	static int is_VC4 = -1;
	void      *address;

	/*  
	 * The mesa V3D driver reports some OpenGL ES extensions as supported and working
	 * even though they aren't. hacked_glGetString is a workaround for this, which will
	 * cut out the non-working extensions from the list of supported extensions.
	 */

	if (name == NULL)
		return NULL;

	// first detect if we're running on a VideoCore 4 / using the VC4 driver.
	if ((is_VC4 == -1) && (is_VC4 = strcmp(flutterpi.egl.renderer, "VC4 V3D 2.1") == 0))
		printf( "detected VideoCore IV as underlying graphics chip, and VC4 as the driver.\n"
				"Reporting modified GL_EXTENSIONS string that doesn't contain non-working extensions.\n");

	// if we do, and the symbol to resolve is glGetString, we return our hacked_glGetString.
	if (is_VC4 && (strcmp(name, "glGetString") == 0))
		return hacked_glGetString;

	if ((address = dlsym(RTLD_DEFAULT, name)) || (address = eglGetProcAddress(name)))
		return address;
	
	fprintf(stderr, "[flutter-pi] proc_resolver: Could not resolve symbol \"%s\"\n", name);

	return NULL;
}

static void on_platform_message(
	const FlutterPlatformMessage* message,
	void* userdata
) {
	int ok;

	ok = plugin_registry_on_platform_message((FlutterPlatformMessage *) message);
	if (ok != 0) {
		fprintf(stderr, "[flutter-pi] Error handling platform message. plugin_registry_on_platform_message: %s\n", strerror(ok));
	}
}

/// Called on the main thread when a new frame request may have arrived.
/// Uses [drmCrtcGetSequence] or [FlutterEngineGetCurrentTime] to complete
/// the frame request.
static int on_execute_frame_request(
	void *userdata
) {
	FlutterEngineResult result;
	struct frame *peek;
	int ok;

	cqueue_lock(&flutterpi.frame_queue);

	ok = cqueue_peek_locked(&flutterpi.frame_queue, (void**) &peek);
	if (ok == 0) {
		if (peek->state == kFramePending) {
			uint64_t ns;
			if (flutterpi.drm.platform_supports_get_sequence_ioctl) {
				ns = 0;
				ok = drmCrtcGetSequence(flutterpi.drm.drmdev->fd, flutterpi.drm.drmdev->selected_crtc->crtc->crtc_id, NULL, &ns);
				if (ok < 0) {
					perror("[flutter-pi] Couldn't get last vblank timestamp. drmCrtcGetSequence");
					cqueue_unlock(&flutterpi.frame_queue);
					return errno;
				}
			} else {
				ns = FlutterEngineGetCurrentTime();
			}
			
			result = FlutterEngineOnVsync(
				flutterpi.flutter.engine,
				peek->baton,
				ns,
				ns + (1000000000 / flutterpi.display.refresh_rate)
			);
			if (result != kSuccess) {
				fprintf(stderr, "[flutter-pi] Could not reply to frame request. FlutterEngineOnVsync: %s\n", FLUTTER_RESULT_TO_STRING(result));
				cqueue_unlock(&flutterpi.frame_queue);
				return EIO;
			}

			peek->state = kFrameRendering;
		}
	} else if (ok == EAGAIN) {
		// do nothing	
	} else if (ok != 0) {
		fprintf(stderr, "[flutter-pi] Could not get peek of frame queue. cqueue_peek_locked: %s\n", strerror(ok));
		cqueue_unlock(&flutterpi.frame_queue);
		return ok;
	}

	cqueue_unlock(&flutterpi.frame_queue);

	return 0;
}

/// Called on some flutter internal thread to request a frame,
/// and also get the vblank timestamp of the pageflip preceding that frame.
static void on_frame_request(
	void* userdata,
	intptr_t baton
) {
	sd_event_source *event_source;
	struct frame *peek;
	int ok;

	cqueue_lock(&flutterpi.frame_queue);

	ok = cqueue_peek_locked(&flutterpi.frame_queue, (void**) &peek);
	if ((ok == 0) || (ok == EAGAIN)) {
		bool reply_instantly = ok == EAGAIN;

		ok = cqueue_try_enqueue_locked(&flutterpi.frame_queue, &(struct frame) {
			.state = kFramePending,
			.baton = baton
		});
		if (ok != 0) {
			fprintf(stderr, "[flutter-pi] Could not enqueue frame request. cqueue_try_enqueue_locked: %s\n", strerror(ok));
			cqueue_unlock(&flutterpi.frame_queue);
			return;
		}

		if (reply_instantly) {	
			post_platform_task(
				on_execute_frame_request,
				NULL
			);
		}
	} else if (ok != 0) {
		fprintf(stderr, "[flutter-pi] Could not get peek of frame queue. cqueue_peek_locked: %s\n", strerror(ok));
	}

	cqueue_unlock(&flutterpi.frame_queue);
}

static FlutterTransformation on_get_transformation(void *userdata) {
	printf("on_get_transformation\n");
	return FLUTTER_ROTZ_TRANSFORMATION((FlutterEngineGetCurrentTime() / 10000000) % 360);
}


/************************
 * PLATFORM TASK-RUNNER *
 ************************/
/*
		} else if (task->type == kUpdateOrientation) {
			rotation += ANGLE_FROM_ORIENTATION(task->orientation) - ANGLE_FROM_ORIENTATION(orientation);
			if (rotation < 0) rotation += 360;
			else if (rotation >= 360) rotation -= 360;

			orientation = task->orientation;

			// send updated window metrics to flutter
			FlutterEngineSendWindowMetricsEvent(engine, &(const FlutterWindowMetricsEvent) {
				.struct_size = sizeof(FlutterWindowMetricsEvent),

				// we send swapped width/height if the screen is rotated 90 or 270 degrees.
				.width = (rotation == 0) || (rotation == 180) ? width : height, 
				.height = (rotation == 0) || (rotation == 180) ? height : width,
				.pixel_ratio = pixel_ratio
			});

		}
*/

/// platform tasks
static int on_execute_platform_task(
	sd_event_source *s,
	void *userdata
) {
	struct platform_task *task;
	int ok;

	task = userdata;
	ok = task->callback(task->userdata);
	if (ok != 0) {
		fprintf(stderr, "[flutter-pi] Error executing platform task: %s\n", strerror(ok));
	}

	free(task);

	sd_event_source_set_enabled(s, SD_EVENT_OFF);
	sd_event_source_unrefp(&s);

	return 0;
}

static int post_platform_task(
	int (*callback)(void *userdata),
	void *userdata
) {
	struct platform_task *task;
	int ok;

	task = malloc(sizeof *task);
	if (task == NULL) {
		return ENOMEM;
	}

	task->callback = callback;
	task->userdata = userdata;

	if (pthread_self() != flutterpi.event_loop_thread) {
		pthread_mutex_lock(&flutterpi.event_loop_mutex);
	}

	ok = sd_event_add_defer(
		flutterpi.event_loop,
		NULL,
		on_execute_platform_task,
		task
	);
	if (ok < 0) {
		fprintf(stderr, "[flutter-pi] Error posting platform task to main loop. sd_event_add_defer: %s\n", strerror(-ok));
		ok = -ok;
		goto fail_unlock_event_loop;
	}

	if (pthread_self() != flutterpi.event_loop_thread) {
		ok = write(flutterpi.wakeup_event_loop_fd, (uint8_t[8]) {0, 0, 0, 0, 0, 0, 0, 1}, 8);
		if (ok < 0) {
			perror("[flutter-pi] Error arming main loop for platform task. write");
			ok = errno;
			goto fail_unlock_event_loop;
		}
	}

	if (pthread_self() != flutterpi.event_loop_thread) {
		pthread_mutex_unlock(&flutterpi.event_loop_mutex);
	}

	return 0;


	fail_unlock_event_loop:
	if (pthread_self() != flutterpi.event_loop_thread) {
		pthread_mutex_unlock(&flutterpi.event_loop_mutex);
	}

	return ok;
}

/// timed platform tasks
static int on_execute_platform_task_with_time(
	sd_event_source *s,
	uint64_t usec,
	void *userdata
) {
	struct platform_task *task;
	int ok;

	task = userdata;
	ok = task->callback(task->userdata);
	if (ok != 0) {
		fprintf(stderr, "[flutter-pi] Error executing timed platform task: %s\n", strerror(ok));
	}

	free(task);

	sd_event_source_set_enabled(s, SD_EVENT_OFF);
	sd_event_source_unrefp(&s);

	return 0;
}

static int post_platform_task_with_time(
	int (*callback)(void *userdata),
	void *userdata,
	uint64_t target_time_usec
) {
	struct platform_task *task;
	//sd_event_source *source;
	int ok;

	task = malloc(sizeof *task);
	if (task == NULL) {
		return ENOMEM;
	}

	task->callback = callback;
	task->userdata = userdata;
	
	if (pthread_self() != flutterpi.event_loop_thread) {
		pthread_mutex_lock(&flutterpi.event_loop_mutex);
	}

	ok = sd_event_add_time(
		flutterpi.event_loop,
		NULL,
		CLOCK_MONOTONIC,
		target_time_usec,
		1,
		on_execute_platform_task_with_time,
		task
	);
	if (ok < 0) {
		fprintf(stderr, "[flutter-pi] Error posting platform task to main loop. sd_event_add_time: %s\n", strerror(-ok));
		ok = -ok;
		goto fail_unlock_event_loop;
	}

	if (pthread_self() != flutterpi.event_loop_thread) {
		ok = write(flutterpi.wakeup_event_loop_fd, (uint8_t[8]) {0, 0, 0, 0, 0, 0, 0, 1}, 8);
		if (ok < 0) {
		perror("[flutter-pi] Error arming main loop for platform task. write");
		ok = errno;
		goto fail_unlock_event_loop;
	}
	}

	if (pthread_self() != flutterpi.event_loop_thread) {
		pthread_mutex_unlock(&flutterpi.event_loop_mutex);
	}

	return 0;


	fail_unlock_event_loop:
	if (pthread_self() != flutterpi.event_loop_thread) {
		pthread_mutex_unlock(&flutterpi.event_loop_mutex);
	}

	return ok;
}

/// flutter tasks
static int on_execute_flutter_task(
	void *userdata
) {
	FlutterEngineResult result;
	FlutterTask *task;

	task = userdata;

	result = FlutterEngineRunTask(flutterpi.flutter.engine, task);
	if (result != kSuccess) {
		fprintf(stderr, "[flutter-pi] Error running platform task. FlutterEngineRunTask: %d\n", result);
		free(task);
		return EINVAL;
	}

	free(task);

	return 0;
}

static void on_post_flutter_task(
	FlutterTask task,
	uint64_t target_time,
	void *userdata
) {
	sd_event_source *source;
	FlutterTask *dup_task;
	int ok;

	dup_task = malloc(sizeof *dup_task);
	if (dup_task == NULL) {
		return;
	}
	
	*dup_task = task;

	ok = post_platform_task_with_time(
		on_execute_flutter_task,
		dup_task,
		target_time / 1000
	);
	if (ok != 0) {
		free(dup_task);
	}
}

/// platform messages
static int on_send_platform_message(
	void *userdata
) {
	struct platform_message *msg;
	FlutterEngineResult result;

	msg = userdata;

	if (msg->is_response) {
		result = FlutterEngineSendPlatformMessageResponse(flutterpi.flutter.engine, msg->target_handle, msg->message, msg->message_size);
	} else {
		result = FlutterEngineSendPlatformMessage(
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
	const char *channel,
	const uint8_t *restrict message,
	size_t message_size,
	FlutterPlatformMessageResponseHandle *responsehandle
) {
	struct platform_message *msg;
	FlutterEngineResult result;
	int ok;
	
	if (runs_platform_tasks_on_current_thread(NULL)) {
		result = FlutterEngineSendPlatformMessage(
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

		ok = post_platform_task(
			on_send_platform_message,
			msg
		);
	}

	return 0;
}

int flutterpi_respond_to_platform_message(
	FlutterPlatformMessageResponseHandle *handle,
	const uint8_t *restrict message,
	size_t message_size
) {
	struct platform_message *msg;
	FlutterEngineResult result;
	int ok;
	
	if (runs_platform_tasks_on_current_thread(NULL)) {
		result = FlutterEngineSendPlatformMessageResponse(
			flutterpi.flutter.engine,
			handle,
			message,
			message_size
		);
		if (result != kSuccess) {
			fprintf(stderr, "[flutter-pi] Error sending platform message response. FlutterEngineSendPlatformMessageResponse: %s\n", FLUTTER_RESULT_TO_STRING(result));
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
			if (!msg->message) {
				free(msg);
				return ENOMEM;
			}
		} else {
			msg->message_size = 0;
			msg->message = 0;
		}

		ok = post_platform_task(
			on_send_platform_message,
			msg
		);
		if (ok != 0) {
			if (msg->message) {
				free(msg->message);
			}
			free(msg);
		}
	}

	return 0;
}


static bool runs_platform_tasks_on_current_thread(void* userdata) {
	return pthread_equal(pthread_self(), flutterpi.event_loop_thread) != 0;
}

static int run_main_loop(void) {
	int ok, evloop_fd;

	pthread_mutex_lock(&flutterpi.event_loop_mutex);
	ok = sd_event_get_fd(flutterpi.event_loop);
	if (ok < 0) {
		fprintf(stderr, "[flutter-pi] Could not get fd for main event loop. sd_event_get_fd: %s\n", strerror(-ok));
		pthread_mutex_unlock(&flutterpi.event_loop_mutex);
		return -ok;
	}
	pthread_mutex_unlock(&flutterpi.event_loop_mutex);

	evloop_fd = ok;

	{
		fd_set fds;
		int state;
		FD_ZERO(&fds);
		FD_SET(evloop_fd, &fds);

		const fd_set const_fds = fds;

		pthread_mutex_lock(&flutterpi.event_loop_mutex);
		 
		do {
			state = sd_event_get_state(flutterpi.event_loop);
			switch (state) {
				case SD_EVENT_INITIAL:
					ok = sd_event_prepare(flutterpi.event_loop);
					if (ok < 0) {
						fprintf(stderr, "[flutter-pi] Could not prepare event loop. sd_event_prepare: %s\n", strerror(-ok));
						return -ok;
					}

					break;
				case SD_EVENT_ARMED:
					pthread_mutex_unlock(&flutterpi.event_loop_mutex);

					do {
						fds = const_fds;
						ok = select(evloop_fd + 1, &fds, &fds, &fds, NULL);
						if ((ok < 0) && (errno != EINTR)) {
							perror("[flutter-pi] Could not wait for event loop events. select");
							return errno;
						}
					} while ((ok < 0) && (errno == EINTR));
					
					pthread_mutex_lock(&flutterpi.event_loop_mutex);
						
					ok = sd_event_wait(flutterpi.event_loop, 0);
					if (ok < 0) {
						fprintf(stderr, "[flutter-pi] Could not check for event loop events. sd_event_wait: %s\n", strerror(-ok));
						return -ok;
					}

					break;
				case SD_EVENT_PENDING:
					ok = sd_event_dispatch(flutterpi.event_loop);
					if (ok < 0) {
						fprintf(stderr, "[flutter-pi] Could not dispatch event loop events. sd_event_dispatch: %s\n", strerror(-ok));
						return -ok;
					}

					break;
				case SD_EVENT_FINISHED:
					printf("SD_EVENT_FINISHED\n");
					break;
				default:
					fprintf(stderr, "[flutter-pi] Unhandled event loop state: %d. Aborting\n", state);
					abort();
			}
		} while (state != SD_EVENT_FINISHED);

		pthread_mutex_unlock(&flutterpi.event_loop_mutex);
	}

	pthread_mutex_destroy(&flutterpi.event_loop_mutex);
	sd_event_unrefp(&flutterpi.event_loop);

	return 0;
}

static int on_wakeup_main_loop(sd_event_source *s, int fd, uint32_t revents, void *userdata) {
	uint8_t buffer[8];
	int ok;

	ok = read(fd, buffer, 8);
	if (ok < 0) {
		perror("[flutter-pi] Could not read mainloop wakeup userdata. read");
		return errno;
	}

	return 0;
}

static int init_main_loop(void) {
	int ok, wakeup_fd;

	flutterpi.event_loop_thread = pthread_self();

	wakeup_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (wakeup_fd < 0) {
		perror("[flutter-pi] Could not create fd for waking up the main loop. eventfd");
		return errno;
	}

	ok = sd_event_new(&flutterpi.event_loop);
	if (ok < 0) {
		fprintf(stderr, "[flutter-pi] Could not create main event loop. sd_event_new: %s\n", strerror(-ok));
		return -ok;
	}

	ok = sd_event_add_io(
		flutterpi.event_loop,
		NULL,
		wakeup_fd,
		EPOLLIN,
		on_wakeup_main_loop,
		NULL
	);
	if (ok < 0) {
		fprintf(stderr, "[flutter-pi] Error adding wakeup callback to main loop. sd_event_add_io: %s\n", strerror(-ok));
		sd_event_unrefp(&flutterpi.event_loop);
		close(wakeup_fd);
		return -ok;
	}

	flutterpi.wakeup_event_loop_fd = wakeup_fd;

	return 0;
}

/**************************
 * DISPLAY INITIALIZATION *
 **************************/
/// Called on the main thread when a pageflip ocurred.
static void on_pageflip_event(
	int fd,
	unsigned int frame,
	unsigned int sec,
	unsigned int usec,
	void *userdata
) {
	FlutterEngineResult result;
	struct frame presented_frame, *peek;
	int ok;

	FlutterEngineTraceEventInstant("pageflip");

	cqueue_lock(&flutterpi.frame_queue);
	
	ok = cqueue_try_dequeue_locked(&flutterpi.frame_queue, &presented_frame);
	if (ok != 0) {
		fprintf(stderr, "[flutter-pi] Could not dequeue completed frame from frame queue: %s\n", strerror(ok));
		goto fail_unlock_frame_queue;
	}

	ok = cqueue_peek_locked(&flutterpi.frame_queue, (void**) &peek);
	if (ok == EAGAIN) {
		// no frame queued after the one that was completed right now.
		// do nothing here.
	} else if (ok != 0) {
		fprintf(stderr, "[flutter-pi] Could not get frame queue peek. cqueue_peek_locked: %s\n", strerror(ok));
		goto fail_unlock_frame_queue;
	} else {
		if (peek->state == kFramePending) {
			uint64_t ns = (sec * 1000000000ll) + (usec * 1000ll);

			result = FlutterEngineOnVsync(
				flutterpi.flutter.engine,
				peek->baton,
				ns,
				ns + (1000000000ll / flutterpi.display.refresh_rate)
			);
			if (result != kSuccess) {
				fprintf(stderr, "[flutter-pi] Could not reply to frame request. FlutterEngineOnVsync: %s\n", FLUTTER_RESULT_TO_STRING(result));
				goto fail_unlock_frame_queue;
			}

			peek->state = kFrameRendering;
		} else {
			fprintf(stderr, "[flutter-pi] frame queue in inconsistent state. aborting\n");
			abort();
		}
	}

	cqueue_unlock(&flutterpi.frame_queue);

	ok = compositor_on_page_flip(sec, usec);
	if (ok != 0) {
		fprintf(stderr, "[flutter-pi] Error notifying compositor about page flip. compositor_on_page_flip: %s\n", strerror(ok));
	}

	return;


	fail_unlock_frame_queue:
	cqueue_unlock(&flutterpi.frame_queue);
}

static int on_drm_fd_ready(sd_event_source *s, int fd, uint32_t revents, void *userdata) {
	int ok;

	ok = drmHandleEvent(fd, &flutterpi.drm.evctx);
	if (ok < 0) {
		perror("[flutter-pi] Could not handle DRM event. drmHandleEvent");
		return -errno;
	}

	return 0;
}

int flutterpi_fill_view_properties(
	bool has_orientation,
	enum device_orientation orientation,
	bool has_rotation,
	int rotation
) {
	enum device_orientation default_orientation = flutterpi.display.width >= flutterpi.display.height ? kLandscapeLeft : kPortraitUp;

	if (flutterpi.view.has_orientation) {
		if (flutterpi.view.has_rotation == false) {
			flutterpi.view.rotation = ANGLE_BETWEEN_ORIENTATIONS(default_orientation, flutterpi.view.orientation);
			flutterpi.view.has_rotation = true;
		}
	} else if (flutterpi.view.has_rotation) {
		for (int i = kPortraitUp; i <= kLandscapeRight; i++) {
			if (ANGLE_BETWEEN_ORIENTATIONS(default_orientation, i) == flutterpi.view.rotation) {
				flutterpi.view.orientation = i;
				flutterpi.view.has_orientation = true;
				break;
			}
		}
	} else {
		flutterpi.view.orientation = default_orientation;
		flutterpi.view.has_orientation = true;
		flutterpi.view.rotation = 0;
		flutterpi.view.has_rotation = true;
	}

	if (has_orientation) {
		flutterpi.view.rotation += ANGLE_BETWEEN_ORIENTATIONS(flutterpi.view.orientation, orientation);
		if (flutterpi.view.rotation >= 360) {
			flutterpi.view.rotation -= 360;
		}
		
		flutterpi.view.orientation = orientation;
	} else if (has_rotation) {
		for (int i = kPortraitUp; i <= kLandscapeRight; i++) {
			if (ANGLE_BETWEEN_ORIENTATIONS(default_orientation, i) == rotation) {
				flutterpi.view.orientation = i;
				flutterpi.view.rotation = rotation;
				break;
			}
		}
	}

	if ((flutterpi.view.rotation <= 45) || ((flutterpi.view.rotation >= 135) && (flutterpi.view.rotation <= 225)) || (flutterpi.view.rotation >= 315)) {
		flutterpi.view.width = flutterpi.display.width;
		flutterpi.view.height = flutterpi.display.height;
		flutterpi.view.width_mm = flutterpi.display.width_mm;
		flutterpi.view.height_mm = flutterpi.display.height_mm;
	} else {
		flutterpi.view.width = flutterpi.display.height;
		flutterpi.view.height = flutterpi.display.width;
		flutterpi.view.width_mm = flutterpi.display.height_mm;
		flutterpi.view.height_mm = flutterpi.display.width_mm;
	}

	FlutterTransformation center_view_around_origin = FLUTTER_TRANSLATION_TRANSFORMATION(flutterpi.display.width - flutterpi.view.width, 0);
	FlutterTransformation rotate = FLUTTER_ROTZ_TRANSFORMATION(flutterpi.view.rotation);
	FlutterTransformation translate_to_fill_screen = FLUTTER_TRANSLATION_TRANSFORMATION(flutterpi.display.width/2, flutterpi.display.height/2);

	FlutterTransformation multiplied;
	multiplied = FLUTTER_MULTIPLIED_TRANSFORMATIONS(center_view_around_origin, rotate);
	multiplied = FLUTTER_MULTIPLIED_TRANSFORMATIONS(multiplied, translate_to_fill_screen);

	flutterpi.view.view_to_display_transform = center_view_around_origin;

	FlutterTransformation inverse_center_view_around_origin = FLUTTER_TRANSLATION_TRANSFORMATION(flutterpi.view.width/2, flutterpi.view.height/2);
	FlutterTransformation inverse_rotate = FLUTTER_ROTZ_TRANSFORMATION(-flutterpi.view.rotation);
	FlutterTransformation inverse_translate_to_fill_screen = FLUTTER_TRANSLATION_TRANSFORMATION(-flutterpi.display.width/2, -flutterpi.display.height/2);

	multiplied = FLUTTER_MULTIPLIED_TRANSFORMATIONS(inverse_translate_to_fill_screen, inverse_rotate);
	multiplied = FLUTTER_MULTIPLIED_TRANSFORMATIONS(multiplied, inverse_center_view_around_origin);

	flutterpi.view.display_to_view_transform = inverse_center_view_around_origin;

	return 0;
}

static int load_egl_gl_procs(void) {
	LOAD_EGL_PROC(flutterpi, getPlatformDisplay);
	LOAD_EGL_PROC(flutterpi, createPlatformWindowSurface);
	LOAD_EGL_PROC(flutterpi, createPlatformPixmapSurface);
	LOAD_EGL_PROC(flutterpi, createDRMImageMESA);
	LOAD_EGL_PROC(flutterpi, exportDRMImageMESA);

	LOAD_GL_PROC(flutterpi, EGLImageTargetTexture2DOES);
	LOAD_GL_PROC(flutterpi, EGLImageTargetRenderbufferStorageOES);

	return 0;
}

static int init_display(void) {
	/**********************
	 * DRM INITIALIZATION *
	 **********************/
	const struct drm_connector *connector;
	const struct drm_encoder *encoder;
	const struct drm_crtc *crtc;
	const drmModeModeInfo *mode, *mode_iter;
	drmDevicePtr devices[64];
	EGLint egl_error;
	GLenum gl_error;
	int ok, area, num_devices;

	/**********************
	 * DRM INITIALIZATION *
	 **********************/
	
	num_devices = drmGetDevices2(0, devices, sizeof(devices)/sizeof(*devices));
	if (num_devices < 0) {
		fprintf(stderr, "[flutter-pi] Could not query DRM device list: %s\n", strerror(-num_devices));
		return -num_devices;
	}
	
	// find a GPU that has a primary node
	flutterpi.drm.drmdev = NULL;
	for (int i = 0; i < num_devices; i++) {
		drmDevicePtr device;
		
		device = devices[i];

		if (!(device->available_nodes & (1 << DRM_NODE_PRIMARY))) {
			// We need a primary node.
			continue;
		}

		ok = drmdev_new_from_path(&flutterpi.drm.drmdev, device->nodes[DRM_NODE_PRIMARY]);
		if (ok != 0) {
			fprintf(stderr, "[flutter-pi] Could not create drmdev from device at \"%s\". Continuing.\n", device->nodes[DRM_NODE_PRIMARY]);
			continue;
		}

		break;
	}

	if (flutterpi.drm.drmdev == NULL) {
		fprintf(stderr, "flutter-pi couldn't find a usable DRM device.\n"
						"Please make sure you've enabled the Fake-KMS driver in raspi-config.\n"
						"If you're not using a Raspberry Pi, please make sure there's KMS support for your graphics chip.\n");
		return ENOENT;
	}

	// find a connected connector
	flutterpi.display.width_mm = 0;
	flutterpi.display.height_mm = 0;
	for_each_connector_in_drmdev(flutterpi.drm.drmdev, connector) {
		if (connector->connector->connection == DRM_MODE_CONNECTED) {
			// only update the physical size of the display if the values
			//   are not yet initialized / not set with a commandline option
			if ((connector->connector->connector_type == DRM_MODE_CONNECTOR_DSI) &&
				(connector->connector->mmWidth == 0) &&
				(connector->connector->mmHeight == 0))
			{
				// if it's connected via DSI, and the width & height are 0,
				//   it's probably the official 7 inch touchscreen.
				flutterpi.display.width_mm = 155;
				flutterpi.display.height_mm = 86;
			} else if ((connector->connector->mmHeight % 10 == 0) &&
						(connector->connector->mmWidth % 10 == 0)) {
				// don't change anything.
			} else {
				flutterpi.display.width_mm = connector->connector->mmWidth;
				flutterpi.display.height_mm = connector->connector->mmHeight;
			}

			break;
		}
	}

	if (connector == NULL) {
		fprintf(stderr, "[flutter-pi] Could not find a connected connector!\n");
		return EINVAL;
	}
	
	// Find the preferred mode (GPU drivers _should_ always supply a preferred mode, but of course, they don't)
	// Alternatively, find the mode with the highest width*height. If there are multiple modes with the same w*h,
	// prefer higher refresh rates. After that, prefer progressive scanout modes.
	for_each_mode_in_connector(connector, mode_iter) {
		if (mode_iter->type & DRM_MODE_TYPE_PREFERRED) {
			mode = mode_iter;
			break;
		} else if (mode == NULL) {
			mode = mode_iter;
		} else {
			int area = mode_iter->hdisplay * mode_iter->vdisplay;
			int old_area = mode->hdisplay * mode->vdisplay;

			if ((area > old_area) ||
				((area == old_area) && (mode_iter->vrefresh > mode->vrefresh)) ||
				((area == old_area) && (mode_iter->vrefresh == mode->vrefresh) && ((mode->flags & DRM_MODE_FLAG_INTERLACE) == 0))) {
				mode = mode_iter;
			}
		}
	}

	if (mode == NULL) {
		fprintf(stderr, "[flutter-pi] Could not find a preferred output mode!\n");
		return EINVAL;
	}

	flutterpi.display.width = mode->hdisplay;
	flutterpi.display.height = mode->vdisplay;
	flutterpi.display.refresh_rate = mode->vrefresh;

	if ((flutterpi.display.width_mm == 0) || (flutterpi.display.height_mm == 0)) {
		fprintf(
			stderr,
			"[flutter-pi] WARNING: display didn't provide valid physical dimensions.\n"
			"             The device-pixel ratio will default to 1.0, which may not be the fitting device-pixel ratio for your display.\n"
		);
		flutterpi.display.pixel_ratio = 1.0;
	} else {
		flutterpi.display.pixel_ratio = (10.0 * flutterpi.display.width) / (flutterpi.display.width_mm * 38.0);
		
		int horizontal_dpi = (int) (flutterpi.display.width / (flutterpi.display.width_mm / 25.4));
		int vertical_dpi = (int) (flutterpi.display.height / (flutterpi.display.height_mm / 25.4));

		if (horizontal_dpi != vertical_dpi) {
			fprintf(stderr, "[flutter-pi] WARNING: display has non-square pixels. Non-square-pixels are not supported by flutter.\n");
		}
	}

	for_each_encoder_in_drmdev(flutterpi.drm.drmdev, encoder) {
		if (encoder->encoder->encoder_id == connector->connector->encoder_id) {
			break;
		}
	}

	if (encoder == NULL) {
		fprintf(stderr, "[flutter-pi] Could not find a suitable DRM encoder.\n");
		return EINVAL;
	}

	for_each_crtc_in_drmdev(flutterpi.drm.drmdev, crtc) {
		if (crtc->crtc->crtc_id == encoder->encoder->crtc_id) {
			break;
		}
	}

	if (crtc == NULL) {
		fprintf(stderr, "[flutter-pi] Could not find a suitable DRM CRTC.\n");
		return EINVAL;
	}

	ok = drmdev_configure(flutterpi.drm.drmdev, connector->connector->connector_id, encoder->encoder->encoder_id, crtc->crtc->crtc_id, mode);
	if (ok != 0) return ok;

	// only enable vsync if the kernel supplies valid vblank timestamps
	{
		uint64_t ns = 0;
		ok = drmCrtcGetSequence(flutterpi.drm.drmdev->fd, flutterpi.drm.drmdev->selected_crtc->crtc->crtc_id, NULL, &ns);
		int _errno = errno;

		if ((ok == 0) && (ns != 0)) {
			flutterpi.drm.platform_supports_get_sequence_ioctl = true;
		} else {
			flutterpi.drm.platform_supports_get_sequence_ioctl = false;
			if (ok != 0) {
				fprintf(
					stderr,
					"WARNING: Error getting last vblank timestamp. drmCrtcGetSequence: %s\n",
					strerror(_errno)
				);
			} else {
				fprintf(
					stderr,
					"WARNING: Kernel didn't return a valid vblank timestamp. (timestamp == 0)\n"
				);
			}
			fprintf(
				stderr,
				"         VSync will be disabled.\n"
				"         See https://github.com/ardera/flutter-pi/issues/38 for more info.\n"
			);
		}
	}

	memset(&flutterpi.drm.evctx, 0, sizeof(drmEventContext));
	flutterpi.drm.evctx.version = 4;
	flutterpi.drm.evctx.page_flip_handler = on_pageflip_event;

	ok = sd_event_add_io(
		flutterpi.event_loop,
		&flutterpi.drm.drm_pageflip_event_source,
		flutterpi.drm.drmdev->fd,
		EPOLLIN | EPOLLHUP | EPOLLPRI,
		on_drm_fd_ready,
		NULL
	);
	if (ok < 0) {
		fprintf(stderr, "[flutter-pi] Could not add DRM pageflip event listener. sd_event_add_io: %s\n", strerror(-ok));
		return -ok;
	}

	printf(
		"===================================\n"
		"display mode:\n"
		"  resolution: %u x %u\n"
		"  refresh rate: %uHz\n"
		"  physical size: %umm x %umm\n"
		"  flutter device pixel ratio: %f\n"
		"===================================\n",
		flutterpi.display.width, flutterpi.display.height,
		flutterpi.display.refresh_rate,
		flutterpi.display.width_mm, flutterpi.display.height_mm,
		flutterpi.display.pixel_ratio
	);

	/**********************
	 * GBM INITIALIZATION *
	 **********************/
	flutterpi.gbm.device = gbm_create_device(flutterpi.drm.drmdev->fd);
	flutterpi.gbm.format = DRM_FORMAT_ARGB8888;
	flutterpi.gbm.surface = NULL;
	flutterpi.gbm.modifier = DRM_FORMAT_MOD_LINEAR;

	flutterpi.gbm.surface = gbm_surface_create_with_modifiers(flutterpi.gbm.device, flutterpi.display.width, flutterpi.display.height, flutterpi.gbm.format, &flutterpi.gbm.modifier, 1);
	if (flutterpi.gbm.surface == NULL) {
		perror("[flutter-pi] Could not create GBM Surface. gbm_surface_create_with_modifiers");
		return errno;
	}

	/**********************
	 * EGL INITIALIZATION *
	 **********************/
	EGLint major, minor;

	static const EGLint context_attribs[] = {
		EGL_CONTEXT_CLIENT_VERSION, 2,
		EGL_NONE
	};

	const EGLint config_attribs[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_SAMPLES, 0,
		EGL_NONE
	};

	const char *egl_exts_client, *egl_exts_dpy, *gl_exts;

	egl_exts_client = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);

	ok = load_egl_gl_procs();
	if (ok != 0) {
		fprintf(stderr, "[flutter-pi] Could not load EGL / GL ES procedure addresses! error: %s\n", strerror(ok));
		return ok;
	}

	eglGetError();

#ifdef EGL_KHR_platform_gbm
	flutterpi.egl.display = flutterpi.egl.getPlatformDisplay(EGL_PLATFORM_GBM_KHR, flutterpi.gbm.device, NULL);
	if ((egl_error = eglGetError()) != EGL_SUCCESS) {
		fprintf(stderr, "[flutter-pi] Could not get EGL display! eglGetPlatformDisplay: 0x%08X\n", egl_error);
		return EIO;
	}
#else
	flutterpi.egl.display = eglGetDisplay((void*) flutterpi.gbm.device);
	if ((egl_error = eglGetError()) != EGL_SUCCESS) {
		fprintf(stderr, "[flutter-pi] Could not get EGL display! eglGetDisplay: 0x%08X\n", egl_error);
		return EIO;
	}
#endif
	
	eglInitialize(flutterpi.egl.display, &major, &minor);
	if ((egl_error = eglGetError()) != EGL_SUCCESS) {
		fprintf(stderr, "[flutter-pi] Failed to initialize EGL! eglInitialize: 0x%08X\n", egl_error);
		return EIO;
	}

	egl_exts_dpy = eglQueryString(flutterpi.egl.display, EGL_EXTENSIONS);

	printf("EGL information:\n");
	printf("  version: %s\n", eglQueryString(flutterpi.egl.display, EGL_VERSION));
	printf("  vendor: \"%s\"\n", eglQueryString(flutterpi.egl.display, EGL_VENDOR));
	printf("  client extensions: \"%s\"\n", egl_exts_client);
	printf("  display extensions: \"%s\"\n", egl_exts_dpy);
	printf("===================================\n");

	eglBindAPI(EGL_OPENGL_ES_API);
	if ((egl_error = eglGetError()) != EGL_SUCCESS) {
		fprintf(stderr, "[flutter-pi] Failed to bind OpenGL ES API! eglBindAPI: 0x%08X\n", egl_error);
		return EIO;
	}

	EGLint count = 0, matched = 0;
	EGLConfig *configs;
	bool _found_matching_config = false;
	
	eglGetConfigs(flutterpi.egl.display, NULL, 0, &count);
	if ((egl_error = eglGetError()) != EGL_SUCCESS) {
		fprintf(stderr, "[flutter-pi] Could not get the number of EGL framebuffer configurations. eglGetConfigs: 0x%08X\n", egl_error);
		return EIO;
	}

	configs = malloc(count * sizeof(EGLConfig));
	if (!configs) return ENOMEM;

	eglChooseConfig(flutterpi.egl.display, config_attribs, configs, count, &matched);
	if ((egl_error = eglGetError()) != EGL_SUCCESS) {
		fprintf(stderr, "[flutter-pi] Could not query EGL framebuffer configurations with fitting attributes. eglChooseConfig: 0x%08X\n", egl_error);
		return EIO;
	}

	if (matched == 0) {
		fprintf(stderr, "[flutter-pi] No fitting EGL framebuffer configuration found.\n");
		return EIO;
	}

	for (int i = 0; i < count; i++) {
		EGLint native_visual_id;

		eglGetConfigAttrib(flutterpi.egl.display, configs[i], EGL_NATIVE_VISUAL_ID, &native_visual_id);
		if ((egl_error = eglGetError()) != EGL_SUCCESS) {
			fprintf(stderr, "[flutter-pi] Could not query native visual ID of EGL config. eglGetConfigAttrib: 0x%08X\n", egl_error);
			continue;
		}

		if (native_visual_id == flutterpi.gbm.format) {
			flutterpi.egl.config = configs[i];
			_found_matching_config = true;
			break;
		}
	}
	free(configs);

	if (_found_matching_config == false) {
		fprintf(stderr, "[flutter-pi] Could not find EGL framebuffer configuration with appropriate attributes & native visual ID.\n");
		return EIO;
	}

	/****************************
	 * OPENGL ES INITIALIZATION *
	 ****************************/
	flutterpi.egl.root_context = eglCreateContext(flutterpi.egl.display, flutterpi.egl.config, EGL_NO_CONTEXT, context_attribs);
	if ((egl_error = eglGetError()) != EGL_SUCCESS) {
		fprintf(stderr, "[flutter-pi] Could not create OpenGL ES root context. eglCreateContext: 0x%08X\n", egl_error);
		return EIO;
	}

	flutterpi.egl.flutter_render_context = eglCreateContext(flutterpi.egl.display, flutterpi.egl.config, flutterpi.egl.root_context, context_attribs);
	if ((egl_error = eglGetError()) != EGL_SUCCESS) {
		fprintf(stderr, "[flutter-pi] Could not create OpenGL ES context for flutter rendering. eglCreateContext: 0x%08X\n", egl_error);
		return EIO;
	}

	flutterpi.egl.flutter_resource_uploading_context = eglCreateContext(flutterpi.egl.display, flutterpi.egl.config, flutterpi.egl.root_context, context_attribs);
	if ((egl_error = eglGetError()) != EGL_SUCCESS) {
		fprintf(stderr, "[flutter-pi] Could not create OpenGL ES context for flutter resource uploads. eglCreateContext: 0x%08X\n", egl_error);
		return EIO;
	}

	flutterpi.egl.compositor_context = eglCreateContext(flutterpi.egl.display, flutterpi.egl.config, flutterpi.egl.root_context, context_attribs);
	if ((egl_error = eglGetError()) != EGL_SUCCESS) {
		fprintf(stderr, "[flutter-pi] Could not create OpenGL ES context for compositor. eglCreateContext: 0x%08X\n", egl_error);
		return EIO;
	}

	flutterpi.egl.surface = eglCreateWindowSurface(flutterpi.egl.display, flutterpi.egl.config, (EGLNativeWindowType) flutterpi.gbm.surface, NULL);
	if ((egl_error = eglGetError()) != EGL_SUCCESS) {
		fprintf(stderr, "[flutter-pi] Could not create EGL window surface. eglCreateWindowSurface: 0x%08X\n", egl_error);
		return EIO;
	}

	eglMakeCurrent(flutterpi.egl.display, flutterpi.egl.surface, flutterpi.egl.surface, flutterpi.egl.root_context);
	if ((egl_error = eglGetError()) != EGL_SUCCESS) {
		fprintf(stderr, "[flutter-pi] Could not make OpenGL ES root context current to get OpenGL information. eglMakeCurrent: 0x%08X\n", egl_error);
		return EIO;
	}

	flutterpi.egl.renderer = (char*) glGetString(GL_RENDERER);

	gl_exts = (char*) glGetString(GL_EXTENSIONS);
	printf("OpenGL ES information:\n");
	printf("  version: \"%s\"\n", glGetString(GL_VERSION));
	printf("  shading language version: \"%s\"\n", glGetString(GL_SHADING_LANGUAGE_VERSION));
	printf("  vendor: \"%s\"\n", glGetString(GL_VENDOR));
	printf("  renderer: \"%s\"\n", flutterpi.egl.renderer);
	printf("  extensions: \"%s\"\n", gl_exts);
	printf("===================================\n");

	// it seems that after some Raspbian update, regular users are sometimes no longer allowed
	//   to use the direct-rendering infrastructure; i.e. the open the devices inside /dev/dri/
	//   as read-write. flutter-pi must be run as root then.
	// sometimes it works fine without root, sometimes it doesn't.
	if (strncmp(flutterpi.egl.renderer, "llvmpipe", sizeof("llvmpipe")-1) == 0) {
		printf("WARNING: Detected llvmpipe (ie. software rendering) as the OpenGL ES renderer.\n"
			   "         Check that flutter-pi has permission to use the 3D graphics hardware,\n"
			   "         or try running it as root.\n"
			   "         This warning will probably result in a \"failed to set mode\" error\n"
			   "         later on in the initialization.\n");
	}

	eglMakeCurrent(flutterpi.egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	if ((egl_error = eglGetError()) != EGL_SUCCESS) {
		fprintf(stderr, "[flutter-pi] Could not clear OpenGL ES context. eglMakeCurrent: 0x%08X\n", egl_error);
		return EIO;
	}

	/// miscellaneous initialization
	/// initialize the compositor
	ok = compositor_initialize(flutterpi.drm.drmdev);
	if (ok != 0) {
		return ok;
	}

	/// initialize the frame queue
	ok = cqueue_init(&flutterpi.frame_queue, sizeof(struct frame), QUEUE_DEFAULT_MAX_SIZE);
	if (ok != 0) {
		return ok;
	}

	/// We're starting without any rotation by default.
	flutterpi_fill_view_properties(false, 0, false, 0);

	return 0;
}

/**************************
 * FLUTTER INITIALIZATION *
 **************************/
static int init_application(void) {
	FlutterRendererConfig renderer_config = {0};
	FlutterEngineResult engine_result;
	FlutterProjectArgs project_args = {0};
	void *app_elf_handle;
	int ok;

	if (flutterpi.flutter.runtime_mode == kRelease || flutterpi.flutter.runtime_mode == kProfile) {
		fprintf(
			stderr,
			"flutter-pi doesn't support running apps in release or profile mode yet.\n"
			"See https://github.com/ardera/flutter-pi/issues/65 for more info.\n"
		);
		return EINVAL;
	}

	ok = plugin_registry_init();
	if (ok != 0) {
		fprintf(stderr, "[flutter-pi] Could not initialize plugin registry: %s\n", strerror(ok));
		return ok;
	}

	// configure flutter rendering
	renderer_config = (FlutterRendererConfig) {
		.type = kOpenGL,
		.open_gl = {
			.struct_size = sizeof(FlutterOpenGLRendererConfig),
			.make_current = on_make_current,
			.clear_current = on_clear_current,
			.present = on_present,
			.fbo_callback = fbo_callback,
			.make_resource_current = on_make_resource_current,
			.gl_proc_resolver = proc_resolver,
			.surface_transformation = on_get_transformation,
			.gl_external_texture_frame_callback = texreg_gl_external_texture_frame_callback,
		}
	};

	// configure the project
	project_args = (FlutterProjectArgs) {
		.struct_size = sizeof(FlutterProjectArgs),
		.assets_path = flutterpi.flutter.asset_bundle_path,
		.icu_data_path = flutterpi.flutter.icu_data_path,
		.command_line_argc = flutterpi.flutter.engine_argc,
		.command_line_argv = (const char * const*) flutterpi.flutter.engine_argv,
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
		.vsync_callback = on_frame_request,
		.custom_dart_entrypoint = NULL,
		.custom_task_runners = &(FlutterCustomTaskRunners) {
			.struct_size = sizeof(FlutterCustomTaskRunners),
			.platform_task_runner = &(FlutterTaskRunnerDescription) {
				.struct_size = sizeof(FlutterTaskRunnerDescription),
				.user_data = NULL,
				.runs_task_on_current_thread_callback = runs_platform_tasks_on_current_thread,
				.post_task_callback = on_post_flutter_task
			}
		},
		.shutdown_dart_vm_when_done = true,
		.compositor = &flutter_compositor
	};

	// spin up the engine
	engine_result = FlutterEngineRun(FLUTTER_ENGINE_VERSION, &renderer_config, &project_args, NULL, &flutterpi.flutter.engine);
	if (engine_result != kSuccess) {
		fprintf(stderr, "[flutter-pi] Could not run the flutter engine. FlutterEngineRun: %s\n", FLUTTER_RESULT_TO_STRING(engine_result));
		return EINVAL;
	}

	// update window size
	engine_result = FlutterEngineSendWindowMetricsEvent(
		flutterpi.flutter.engine,
		&(FlutterWindowMetricsEvent) {
			.struct_size = sizeof(FlutterWindowMetricsEvent),
			.width = flutterpi.view.width,
			.height = flutterpi.view.height,
			.pixel_ratio = flutterpi.display.pixel_ratio
		}
	);
	if (engine_result != kSuccess) {
		fprintf(stderr, "[flutter-pi] Could not send window metrics to flutter engine.\n");
		return EINVAL;
	}
	
	return 0;
}

/**************
 * USER INPUT *
 **************/
static int libinput_interface_on_open(const char *path, int flags, void *userdata) {
	return open(path, flags | O_CLOEXEC);
}

static void libinput_interface_on_close(int fd, void *userdata) {
	close(fd);
}

static int on_libinput_ready(sd_event_source *s, int fd, uint32_t revents, void *userdata) {
	struct libinput_event *event;
	FlutterPointerEvent pointer_events[64];
	FlutterEngineResult result;
	int n_pointer_events = 0;
	int ok;
	
	ok = libinput_dispatch(flutterpi.input.libinput);
	if (ok < 0) {
		fprintf(stderr, "[flutter-pi] Could not dispatch libinput events. libinput_dispatch: %s\n", strerror(-ok));
		return -ok;
	}

	while (event = libinput_get_event(flutterpi.input.libinput), event != NULL) {
		enum libinput_event_type type = libinput_event_get_type(event);

		if (type == LIBINPUT_EVENT_DEVICE_ADDED) {
			struct libinput_device *device = libinput_event_get_device(event);
			
			struct input_device_data *data = calloc(1, sizeof(*data));
			data->flutter_device_id_offset = flutterpi.input.next_unused_flutter_device_id;

			libinput_device_set_user_data(device, data);

			if (libinput_device_has_capability(device, LIBINPUT_DEVICE_CAP_POINTER)) {
				pointer_events[n_pointer_events++] = (FlutterPointerEvent) {
					.struct_size = sizeof(FlutterPointerEvent),
					.phase = kAdd,
					.timestamp = FlutterEngineGetCurrentTime(),
					.x = 0.0,
					.y = 0.0,
					.device = flutterpi.input.next_unused_flutter_device_id++,
					.signal_kind = kFlutterPointerSignalKindNone,
					.scroll_delta_x = 0.0,
					.scroll_delta_y = 0.0,
					.device_kind = kFlutterPointerDeviceKindMouse,
					.buttons = 0
				};
			} else if (libinput_device_has_capability(device, LIBINPUT_DEVICE_CAP_TOUCH)) {
				int touch_count = libinput_device_touch_get_touch_count(device);

				for (int i = 0; i < touch_count; i++) {
					pointer_events[n_pointer_events++] = (FlutterPointerEvent) {
						.struct_size = sizeof(FlutterPointerEvent),
						.phase = kAdd,
						.timestamp = FlutterEngineGetCurrentTime(),
						.x = 0.0,
						.y = 0.0,
						.device = flutterpi.input.next_unused_flutter_device_id++,
						.signal_kind = kFlutterPointerSignalKindNone,
						.scroll_delta_x = 0.0,
						.scroll_delta_y = 0.0,
						.device_kind = kFlutterPointerDeviceKindTouch,
						.buttons = 0
					};
				}
			}
		} else if (LIBINPUT_EVENT_IS_TOUCH(type)) {
			struct libinput_event_touch *touch_event = libinput_event_get_touch_event(event);

			struct input_device_data *data = libinput_device_get_user_data(libinput_event_get_device(event));

			if ((type == LIBINPUT_EVENT_TOUCH_DOWN) || (type == LIBINPUT_EVENT_TOUCH_MOTION) || (type == LIBINPUT_EVENT_TOUCH_UP)) {
				int slot = libinput_event_touch_get_slot(touch_event);
				if (slot < 0) {
					slot = 0;
				}

				if ((type == LIBINPUT_EVENT_TOUCH_DOWN) || (type == LIBINPUT_EVENT_TOUCH_MOTION)) {
					double x = libinput_event_touch_get_x_transformed(touch_event, flutterpi.display.width);
					double y = libinput_event_touch_get_y_transformed(touch_event, flutterpi.display.height);

					apply_flutter_transformation(flutterpi.view.display_to_view_transform, &x, &y);

					FlutterPointerPhase phase;
					if (type == LIBINPUT_EVENT_TOUCH_DOWN) {
						phase = kDown;
					} else if (type == LIBINPUT_EVENT_TOUCH_MOTION) {
						phase = kMove;
					}

					pointer_events[n_pointer_events++] = (FlutterPointerEvent) {
						.struct_size = sizeof(FlutterPointerEvent),
						.phase = phase,
						.timestamp = libinput_event_touch_get_time_usec(touch_event),
						.x = x,
						.y = y,
						.device = data->flutter_device_id_offset + slot,
						.signal_kind = kFlutterPointerSignalKindNone,
						.scroll_delta_x = 0.0,
						.scroll_delta_y = 0.0,
						.device_kind = kFlutterPointerDeviceKindTouch,
						.buttons = 0
					};

					data->x = x;
					data->y = y;
					data->timestamp = libinput_event_touch_get_time_usec(touch_event);
				} else {
					pointer_events[n_pointer_events++] = (FlutterPointerEvent) {
						.struct_size = sizeof(FlutterPointerEvent),
						.phase = kUp,
						.timestamp = libinput_event_touch_get_time_usec(touch_event),
						.x = 0.0,
						.y = 0.0,
						.device = data->flutter_device_id_offset + slot,
						.signal_kind = kFlutterPointerSignalKindNone,
						.scroll_delta_x = 0.0,
						.scroll_delta_y = 0.0,
						.device_kind = kFlutterPointerDeviceKindTouch,
						.buttons = 0
					};
				}
			}
		} else if (LIBINPUT_EVENT_IS_POINTER(type)) {
			struct libinput_event_pointer *pointer_event = libinput_event_get_pointer_event(event);
			struct input_device_data *data = libinput_device_get_user_data(libinput_event_get_device(event));

			if (type == LIBINPUT_EVENT_POINTER_MOTION) {

			} else if (type == LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE) {
				double x = libinput_event_pointer_get_absolute_x_transformed(pointer_event, flutterpi.display.width);
				double y = libinput_event_pointer_get_absolute_y_transformed(pointer_event, flutterpi.display.height);

				apply_flutter_transformation(flutterpi.view.display_to_view_transform, &x, &y);

				pointer_events[n_pointer_events++] = (FlutterPointerEvent) {
					.struct_size = sizeof(FlutterPointerEvent),
					.phase = data->buttons & kFlutterPointerButtonMousePrimary ? kMove : kHover,
					.timestamp = libinput_event_pointer_get_time_usec(pointer_event),
					.x = x,
					.y = y,
					.device = data->flutter_device_id_offset,
					.signal_kind = kFlutterPointerSignalKindNone,
					.scroll_delta_x = 0.0,
					.scroll_delta_y = 0.0,
					.device_kind = kFlutterPointerDeviceKindMouse,
					.buttons = data->buttons
				};

				data->x = x;
				data->y = y;
				data->timestamp = libinput_event_pointer_get_time_usec(pointer_event);
			} else if (type == LIBINPUT_EVENT_POINTER_BUTTON) {
				uint32_t button = libinput_event_pointer_get_button(pointer_event);
				enum libinput_button_state button_state = libinput_event_pointer_get_button_state(pointer_event);

				int64_t flutter_button = 0;
				if (button == BTN_LEFT) {
					flutter_button = kFlutterPointerButtonMousePrimary;
				} else if (button == BTN_RIGHT) {
					flutter_button = kFlutterPointerButtonMouseSecondary;
				} else if (button == BTN_MIDDLE) {
					flutter_button = kFlutterPointerButtonMouseMiddle;
				} else if (button == BTN_BACK) {
					flutter_button = kFlutterPointerButtonMouseBack;
				} else if (button == BTN_FORWARD) {
					flutter_button = kFlutterPointerButtonMouseForward;
				}

				int64_t new_flutter_button_state = data->buttons;
				if (button_state == LIBINPUT_BUTTON_STATE_RELEASED) {
					new_flutter_button_state &= ~flutter_button;
				} else {
					new_flutter_button_state |= flutter_button;
				}

				if (new_flutter_button_state != data->buttons) {
					FlutterPointerPhase phase;
					if (new_flutter_button_state == 0) {
						phase = kUp;
					} else if (data->buttons == 0) {
						phase = kDown;
					} else {
						phase = kMove;
					}

					pointer_events[n_pointer_events++] = (FlutterPointerEvent) {
						.struct_size = sizeof(FlutterPointerEvent),
						.phase = phase,
						.timestamp = libinput_event_pointer_get_time_usec(pointer_event),
						.x = data->x,
						.y = data->y,
						.device = data->flutter_device_id_offset,
						.signal_kind = kFlutterPointerSignalKindNone,
						.scroll_delta_x = 0.0,
						.scroll_delta_y = 0.0,
						.device_kind = kFlutterPointerDeviceKindMouse,
						.buttons = new_flutter_button_state
					};

					data->buttons = new_flutter_button_state;
				}
			} else if (type == LIBINPUT_EVENT_POINTER_AXIS) {

			}
		}
	}

	if (n_pointer_events > 0) {
		result = FlutterEngineSendPointerEvent(
			flutterpi.flutter.engine,
			pointer_events,
			n_pointer_events
		);
		if (result != kSuccess) {
			fprintf(stderr, "[flutter-pi] Could not add mouse as flutter input device. FlutterEngineSendPointerEvent: %s\n", FLUTTER_RESULT_TO_STRING(result));
		}
	}

	return 0;
}

static int on_stdin_ready(sd_event_source *s, int fd, uint32_t revents, void *userdata) {
	static char buffer[4096];
	glfw_key key;
	char *cursor;
	char *c;
	int ok;

	ok = read(STDIN_FILENO, buffer, sizeof(buffer) - 1);
	if (ok == -1) {
		perror("[flutter-pi] Could not read from stdin");
		return errno;
	} else if (ok == 0) {
		fprintf(stderr, "[flutter-pi] WARNING: reached EOF for stdin\n");
		return EBADF;
	}

	buffer[ok] = '\0';

	cursor = buffer;
	while (*cursor) {
		if (key = console_try_get_key(cursor, &cursor), key != GLFW_KEY_UNKNOWN) {
			textin_on_key(key);
		} else if (c = console_try_get_utf8char(cursor, &cursor), c != NULL) {
			textin_on_utf8_char(c);
		} else {
			// neither a char nor a (function) key. we don't know when
			// we can start parsing the buffer again, so just stop here
			break;
		}
	}
}

static int init_user_input(void) {
	sd_event_source *libinput_event_source, *stdin_event_source;
	struct libinput *libinput;
	struct udev *udev;
	int ok;

	udev = udev_new();
	if (udev == NULL) {
		perror("[flutter-pi] Could not create udev instance. udev_new");
		return errno;
	}

	libinput = libinput_udev_create_context(
		&(const struct libinput_interface) {
			.open_restricted = libinput_interface_on_open,
			.close_restricted = libinput_interface_on_close 
		},
		NULL,
		udev
	);
	if (libinput == NULL) {
		perror("[flutter-pi] Could not create libinput instance. libinput_udev_create_context");
		udev_unref(udev);
		return errno;
	}

	ok = libinput_udev_assign_seat(libinput, "seat0");
	if (ok < 0) {
		fprintf(stderr, "[flutter-pi] Could not assign udev seat to libinput instance. libinput_udev_assign_seat: %s\n", strerror(-ok));
		libinput_unref(libinput);
		udev_unref(udev);
		return -ok;
	}

	ok = sd_event_add_io(
		flutterpi.event_loop,
		&libinput_event_source,
		libinput_get_fd(libinput),
		EPOLLIN | EPOLLRDHUP | EPOLLPRI,
		on_libinput_ready,
		NULL
	);
	if (ok < 0) {
		fprintf(stderr, "[flutter-pi] Could not add libinput callback to main loop. sd_event_add_io: %s\n", strerror(-ok));
		libinput_unref(libinput);
		udev_unref(udev);
		return -ok;
	}

	ok = sd_event_add_io(
		flutterpi.event_loop,
		&stdin_event_source,
		STDIN_FILENO,
		EPOLLIN,
		on_stdin_ready,
		NULL
	);
	if (ok < 0) {
		fprintf(stderr, "[flutter-pi] Could not add libinput callback to main loop. sd_event_add_io: %s\n", strerror(-ok));
		sd_event_source_unrefp(&libinput_event_source);
		libinput_unref(libinput);
		udev_unref(udev);
		return -ok;
	}

	ok = 1; //console_make_raw();
	if (ok == 0) {
		console_flush_stdin();
	} else {
		fprintf(stderr, "[flutter-pi] WARNING: could not make stdin raw\n");
		sd_event_source_unrefp(&stdin_event_source);
	}

	flutterpi.input.udev = udev;
	flutterpi.input.libinput = libinput;
	flutterpi.input.libinput_event_source = libinput_event_source;
	flutterpi.input.stdin_event_source = stdin_event_source;

	return 0;
}


static bool setup_paths(void) {
	char *kernel_blob_path, *icu_data_path, *app_elf_path;
	#define PATH_EXISTS(path) (access((path),R_OK)==0)

	if (!PATH_EXISTS(flutterpi.flutter.asset_bundle_path)) {
		fprintf(stderr, "Asset Bundle Directory \"%s\" does not exist\n", flutterpi.flutter.asset_bundle_path);
		return false;
	}
	
	asprintf(&kernel_blob_path, "%s/kernel_blob.bin", flutterpi.flutter.asset_bundle_path);
	asprintf(&app_elf_path, "%s/app.so", flutterpi.flutter.asset_bundle_path);

	if (flutterpi.flutter.runtime_mode == kDebug) {
		if (!PATH_EXISTS(kernel_blob_path)) {
			fprintf(stderr, "[flutter-pi] Could not find \"kernel.blob\" file inside \"%s\", which is required for debug mode.\n", flutterpi.flutter.asset_bundle_path);
			return false;
		}
	} else {
		if (!PATH_EXISTS(app_elf_path)) {
			fprintf(stderr, "[flutter-pi] Could not find \"app.so\" file inside \"%s\", which is required for release and profile mode.\n", flutterpi.flutter.asset_bundle_path);
			return false;
		}
	}

	asprintf(&icu_data_path, "/usr/lib/icudtl.dat");
	if (!PATH_EXISTS(icu_data_path)) {
		fprintf(stderr, "[flutter-pi] Could not find \"icudtl.dat\" file inside \"/usr/lib/\".\n");
		return false;
	}

	flutterpi.flutter.kernel_blob_path = kernel_blob_path;
	flutterpi.flutter.icu_data_path = icu_data_path;
	flutterpi.flutter.app_elf_path = app_elf_path;

	return true;

	#undef PATH_EXISTS
}

static bool parse_cmd_args(int argc, char **argv) {
	glob_t input_devices_glob = {0};
	bool input_specified = false;
	int ok, opt, longopt_index = 0, runtime_mode_int = kDebug;

	struct option long_options[] = {
		{"release", no_argument, &runtime_mode_int, kRelease},
		{"profile", no_argument, &runtime_mode_int, kProfile},
		{"debug", no_argument, &runtime_mode_int, kDebug},
		{"input", required_argument, 0, 'i'},
		{"orientation", required_argument, 0, 'o'},
		{"rotation", required_argument, 0, 'r'},
		{"help", no_argument, 0, 'h'},
		{0, 0, 0, 0}
	};

	while (1) {
		longopt_index = 0;
		opt = getopt_long(argc, argv, "+rpdi:o:r:h", long_options, &longopt_index);

		if (opt == -1) {
			break;
		} else if (((opt == 0) && (longopt_index == 3)) || (opt == 'i')) {
			glob(optarg, GLOB_BRACE | GLOB_TILDE | (input_specified ? GLOB_APPEND : 0), NULL, &input_devices_glob);
			input_specified = true;
		} else if (((opt == 0) && (longopt_index == 4)) || (opt == 'o')) {
			if (STREQ(optarg, "portrait_up")) {
				flutterpi.view.orientation = kPortraitUp;
				flutterpi.view.has_orientation = true;
			} else if (STREQ(optarg, "landscape_left")) {
				flutterpi.view.orientation = kLandscapeLeft;
				flutterpi.view.has_orientation = true;
			} else if (STREQ(optarg, "portrait_down")) {
				flutterpi.view.orientation = kPortraitDown;
				flutterpi.view.has_orientation = true;
			} else if (STREQ(optarg, "landscape_right")) {
				flutterpi.view.orientation = kLandscapeRight;
				flutterpi.view.has_orientation = true;
			} else {
				fprintf(
					stderr, 
					"ERROR: Invalid argument for --orientation passed.\n"
					"Valid values are \"portrait_up\", \"landscape_left\", \"portrait_down\", \"landscape_right\".\n"
					"%s", 
					usage
				);
				return false;
			}
		} else if (((opt == 0) && (longopt_index == 5)) || (opt == 'r')) {
			errno = 0;
			long rotation = strtol(optarg, NULL, 0);
			if ((errno != 0) || ((rotation != 0) && (rotation != 90) && (rotation != 180) && (rotation != 270))) {
				fprintf(
					stderr,
					"ERROR: Invalid argument for --rotation passed.\n"
					"Valid values are 0, 90, 180, 270.\n"
					"%s",
					usage
				);
				return false;
			}

			flutterpi.view.rotation = rotation;
		} else {
			printf("%s", usage);
			return false;
		}
	}
	
	if (!input_specified) {
		// user specified no input devices. use "/dev/input/event*"".
		glob("/dev/input/event*", GLOB_BRACE | GLOB_TILDE, NULL, &input_devices_glob);
	}
	
	if (runtime_mode_int != kDebug) {
		fprintf(
			stderr,
			"flutter-pi doesn't support running apps in release or profile mode yet.\n"
			"See https://github.com/ardera/flutter-pi/issues/65 for more info.\n"
		);
		return false;
	}

	if (optind >= argc) {
		fprintf(stderr, "error: expected asset bundle path after options.\n");
		printf("%s", usage);
		return false;
	}

	flutterpi.input.use_paths = input_specified;
	flutterpi.flutter.asset_bundle_path = strdup(argv[optind]);
	flutterpi.flutter.runtime_mode = (enum flutter_runtime_mode) runtime_mode_int;

	argv[optind] = argv[0];
	flutterpi.flutter.engine_argc = argc - optind;
	flutterpi.flutter.engine_argv = argv + optind;

	return true;
}

int init(int argc, char **argv) {
	int ok;

	ok = parse_cmd_args(argc, argv);
	if (ok == false) {
		return EINVAL;
	}

	ok = setup_paths();
	if (ok == false) {
		return EINVAL;
	}

	ok = init_main_loop();
	if (ok != 0) {
		return ok;
	}

	ok = init_display();
	if (ok != 0) {
		return ok;
	}

	ok = init_user_input();
	if (ok != 0) {
		return ok;
	}

	ok = init_application();
	if (ok != 0) {
		return ok;
	}

	return 0;
}

int run() {
	return run_main_loop();
}

void deinit() {
	return;
}


int main(int argc, char **argv) {
	int ok;

	ok = init(argc, argv);
	if (ok != 0) {
		return EXIT_FAILURE;
	}

	ok = run();
	if (ok != 0) {
		return EXIT_FAILURE;
	}

	deinit();

	return EXIT_SUCCESS;
}