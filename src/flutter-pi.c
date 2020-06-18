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
#include <glob.h>

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


char* usage ="\
flutter-pi - run flutter apps on your Raspberry Pi.\n\
\n\
USAGE:\n\
  flutter-pi [options] <asset bundle path> [flutter engine options]\n\
\n\
OPTIONS:\n\
  -i <glob pattern>   Appends all files matching this glob pattern\n\
                      to the list of input (touchscreen, mouse, touchpad)\n\
                      devices. Brace and tilde expansion is enabled.\n\
                      Every file that matches this pattern, but is not\n\
                      a valid touchscreen / -pad or mouse is silently\n\
                      ignored.\n\
                        If no -i options are given, all files matching\n\
                      \"/dev/input/event*\" will be used as inputs.\n\
                      This should be what you want in most cases.\n\
                        Note that you need to properly escape each glob pattern\n\
                      you use as a parameter so it isn't implicitly expanded\n\
                      by your shell.\n\
                      \n\
  -h                  Show this help and exit.\n\
\n\
EXAMPLES:\n\
  flutter-pi -i \"/dev/input/event{0,1}\" -i \"/dev/input/event{2,3}\" /home/pi/helloworld_flutterassets\n\
  flutter-pi -i \"/dev/input/mouse*\" /home/pi/helloworld_flutterassets\n\
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

/// width & height of the display in pixels
uint32_t width, height;

/// physical width & height of the display in millimeters
/// the physical size can only be queried for HDMI displays (and even then, most displays will
///   probably return bogus values like 160mm x 90mm).
/// for DSI displays, the physical size of the official 7-inch display will be set in init_display.
/// init_display will only update width_mm and height_mm if they are set to zero, allowing you
///   to hardcode values for you individual display.
uint32_t width_mm = 0, height_mm = 0;
uint32_t refresh_rate;

/// The pixel ratio used by flutter.
/// This is computed inside init_display using width_mm and height_mm.
/// flutter only accepts pixel ratios >= 1.0
/// init_display will only update this value if it is equal to zero,
///   allowing you to hardcode values.
double pixel_ratio = 0.0;

/// The current device orientation.
/// The initial device orientation is based on the width & height data from drm.
enum device_orientation orientation;

/// The angle between the initial device orientation and the current device orientation in degrees.
/// (applied as a rotation to the flutter window in transformation_callback, and also
/// is used to determine if width/height should be swapped when sending a WindowMetrics event to flutter)
int rotation = 0;

struct drm drm = {0};
struct gbm gbm = {0};
struct egl egl = {0};
struct gl gl = {0};

struct {
	char asset_bundle_path[240];
	char kernel_blob_path[256];
	char executable_path[256];
	char icu_data_path[256];
	FlutterRendererConfig renderer_config;
	FlutterProjectArgs args;
	int engine_argc;
	const char* const *engine_argv;
} flutter = {0};

// Flutter VSync handles
// stored as a ring buffer. i_batons is the offset of the first baton (0 - 63)
// scheduled_frames - 1 is the number of total number of stored batons.
// (If 5 vsync events were asked for by the flutter engine, you only need to store 4 batons.
//  The baton for the first one that was asked for would've been returned immediately.)
intptr_t        batons[64];
uint8_t         i_batons = 0;
int			    scheduled_frames = 0;

glob_t					   input_devices_glob;
size_t                     n_input_devices;
struct input_device       *input_devices;
struct mousepointer_mtslot mousepointer;

pthread_t io_thread_id;
pthread_t platform_thread_id;
struct flutterpi_task tasklist = {
	.next = NULL,
	.type = kFlutterTask,
	.target_time = 0,
	.task = {.runner = NULL, .task = 0}
};
pthread_mutex_t tasklist_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  task_added = PTHREAD_COND_INITIALIZER;

FlutterEngine engine;
_Atomic bool  engine_running = false;

/*********************
 * FLUTTER CALLBACKS *
 *********************/
static bool on_make_current(void* userdata) {
	if (eglMakeCurrent(egl.display, egl.surface, egl.surface, egl.flutter_render_context) != EGL_TRUE) {
		fprintf(stderr, "make_current: could not make the context current. eglMakeCurrent: %d\n", eglGetError());
		return false;
	}
	
	return true;
}

static bool on_clear_current(void* userdata) {
	if (eglMakeCurrent(egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) != EGL_TRUE) {
		fprintf(stderr, "clear_current: could not clear the current context.\n");
		return false;
	}
	
	return true;
}

static bool on_present(void *userdata) {

}

static void on_pageflip_event(
	int fd,
	unsigned int frame,
	unsigned int sec,
	unsigned int usec,
	void *userdata
) {
	FlutterEngineTraceEventInstant("pageflip");
	
	compositor_on_page_flip(sec, usec);

	post_platform_task(&(struct flutterpi_task) {
		.type = kVBlankReply,
		.target_time = 0,
		.vblank_ns = sec*1000000000ull + usec*1000ull,
	});
}

static uint32_t fbo_callback(void* userdata) {
	return 0;
}

static bool on_make_resource_current(void *userdata) {
	if (eglMakeCurrent(egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, egl.flutter_resource_uploading_context) != EGL_TRUE) {
		fprintf(stderr, "make_resource_current: could not make the resource context current. eglMakeCurrent: %d\n", eglGetError());
		return false;
	}

	return true;
}

static void cut_word_from_string(
	char* string,
	char* word
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
	if ((is_VC4 == -1) && (is_VC4 = strcmp(egl.renderer, "VC4 V3D 2.1") == 0))
		printf( "detected VideoCore IV as underlying graphics chip, and VC4 as the driver.\n"
				"Reporting modified GL_EXTENSIONS string that doesn't contain non-working extensions.\n");

	// if we do, and the symbol to resolve is glGetString, we return our hacked_glGetString.
	if (is_VC4 && (strcmp(name, "glGetString") == 0))
		return hacked_glGetString;

	if ((address = dlsym(RTLD_DEFAULT, name)) || (address = eglGetProcAddress(name)))
		return address;
	
	fprintf(stderr, "proc_resolver: could not resolve symbol \"%s\"\n", name);

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

static void vsync_callback(
	void* userdata,
	intptr_t baton
) {
	post_platform_task(&(struct flutterpi_task) {
		.type = kVBlankRequest,
		.target_time = 0,
		.baton = baton
	});
}

static FlutterTransformation transformation_callback(void *userdata) {
	// report a transform based on the current device orientation.
	static bool _transformsInitialized = false;
	static FlutterTransformation rotate0, rotate90, rotate180, rotate270;
	
	static int counter = 0;

	if (!_transformsInitialized) {
		rotate0 = (FlutterTransformation) {
			.scaleX = 1, .skewX  = 0, .transX = 0,
			.skewY  = 0, .scaleY = 1, .transY = 0,
			.pers0  = 0, .pers1  = 0, .pers2  = 1
		};

		rotate90 = FLUTTER_ROTATION_TRANSFORMATION(90);
		rotate90.transX = width;
		rotate180 = FLUTTER_ROTATION_TRANSFORMATION(180);
		rotate180.transX = width;
		rotate180.transY = height;
		rotate270 = FLUTTER_ROTATION_TRANSFORMATION(270);
		rotate270.transY = height;

		_transformsInitialized = true;
	}

	if (rotation == 0) return rotate0;
	else if (rotation == 90) return rotate90;
	else if (rotation == 180) return rotate180;
	else if (rotation == 270) return rotate270;
	else return rotate0;
}


/************************
 * PLATFORM TASK-RUNNER *
 ************************/
static bool init_message_loop(void) {
	platform_thread_id = pthread_self();
	return true;
}

static bool message_loop(void) {
	struct timespec abstargetspec;
	uint64_t currenttime, abstarget;
	intptr_t baton;

	while (true) {
		pthread_mutex_lock(&tasklist_lock);

		// wait for a task to be inserted into the list
		while (tasklist.next == NULL)
			pthread_cond_wait(&task_added, &tasklist_lock);
		
		// wait for a task to be ready to be run
		while (tasklist.target_time > (currenttime = FlutterEngineGetCurrentTime())) {
			clock_gettime(CLOCK_REALTIME, &abstargetspec);
			abstarget = abstargetspec.tv_nsec + abstargetspec.tv_sec*1000000000ull - currenttime;
			abstargetspec.tv_nsec = abstarget % 1000000000;
			abstargetspec.tv_sec =  abstarget / 1000000000;
			
			pthread_cond_timedwait(&task_added, &tasklist_lock, &abstargetspec);
		}

		struct flutterpi_task *task = tasklist.next;
		tasklist.next = tasklist.next->next;

		pthread_mutex_unlock(&tasklist_lock);
		if (task->type == kVBlankRequest || task->type == kVBlankReply) {
			intptr_t baton;
			bool     has_baton = false;
			uint64_t ns;
			
			if (task->type == kVBlankRequest) {
				if (scheduled_frames == 0) {
					baton = task->baton;
					has_baton = true;
					drmCrtcGetSequence(drm.drmdev->fd, drm.drmdev->selected_crtc->crtc->crtc_id, NULL, &ns);
				} else {
					batons[(i_batons + (scheduled_frames-1)) & 63] = task->baton;
				}
				scheduled_frames++;
			} else if (task->type == kVBlankReply) {
				if (scheduled_frames > 1) {
					baton = batons[i_batons];
					has_baton = true;
					i_batons = (i_batons+1) & 63;
					ns = task->vblank_ns;
				}
				scheduled_frames--;
			}

			if (has_baton) {
				FlutterEngineOnVsync(engine, baton, ns, ns + (1000000000ull / refresh_rate));
			}
		
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

		} else if (task->type == kSendPlatformMessage || task->type == kRespondToPlatformMessage) {
			if (task->type == kSendPlatformMessage) {
				FlutterEngineSendPlatformMessage(
					engine,
					&(const FlutterPlatformMessage) {
						.struct_size = sizeof(FlutterPlatformMessage),
						.channel = task->channel,
						.message = task->message,
						.message_size = task->message_size,
						.response_handle = task->responsehandle
					}
				);

				free(task->channel);
			} else if (task->type == kRespondToPlatformMessage) {
				FlutterEngineSendPlatformMessageResponse(
					engine,
					task->responsehandle,
					task->message,
					task->message_size
				);
			}

			free(task->message);
		} else if (task->type == kRegisterExternalTexture) {
			FlutterEngineRegisterExternalTexture(engine, task->texture_id);
		} else if (task->type == kUnregisterExternalTexture) {
			FlutterEngineUnregisterExternalTexture(engine, task->texture_id);
		} else if (task->type == kMarkExternalTextureFrameAvailable) {
			FlutterEngineMarkExternalTextureFrameAvailable(engine, task->texture_id);
		} else if (task->type == kFlutterTask) {
			if (FlutterEngineRunTask(engine, &task->task) != kSuccess) {
				fprintf(stderr, "Error running platform task\n");
				return false;
			}
		} else if (task->type == kGeneric) {
			task->callback(task->userdata);
		}

		free(task);
	}

	return true;
}

void post_platform_task(struct flutterpi_task *task) {
	struct flutterpi_task *to_insert;
	
	to_insert = malloc(sizeof(*task));
	if (to_insert == NULL) {
		return;
	}

	memcpy(to_insert, task, sizeof(*task));

	pthread_mutex_lock(&tasklist_lock);
		struct flutterpi_task* this = &tasklist;
		while ((this->next) != NULL && (to_insert->target_time > this->next->target_time))
			this = this->next;

		to_insert->next = this->next;
		this->next = to_insert;
	pthread_mutex_unlock(&tasklist_lock);
	pthread_cond_signal(&task_added);
}

static void flutter_post_platform_task(
	FlutterTask task,
	uint64_t target_time,
	void* userdata
) {
	post_platform_task(&(struct flutterpi_task) {
		.type = kFlutterTask,
		.task = task,
		.target_time = target_time
	});
}

static bool runs_platform_tasks_on_current_thread(void* userdata) {
	return pthread_equal(pthread_self(), platform_thread_id) != 0;
}

int flutterpi_send_platform_message(
	const char *channel,
	const uint8_t *restrict message,
	size_t message_size,
	FlutterPlatformMessageResponseHandle *responsehandle
) {
	struct flutterpi_task *task;
	int ok;
	
	if (runs_platform_tasks_on_current_thread(NULL)) {
		ok = kSuccess == FlutterEngineSendPlatformMessage(
			engine,
			&(const FlutterPlatformMessage) {
				.struct_size = sizeof(FlutterPlatformMessage),
				.channel = channel,
				.message = message,
				.message_size = message_size,
				.response_handle = responsehandle
			}
		);

		return ok? 0 : 1;
	} else {
		task = malloc(sizeof(struct flutterpi_task));
		if (task == NULL) return ENOMEM;

		task->type = kSendPlatformMessage;
		task->channel = strdup(channel);
		task->responsehandle = responsehandle;

		if (message && message_size) {
			task->message_size = message_size;
			task->message = memdup(message, message_size);
			if (!task->message) {
				free(task->channel);
				return ENOMEM;
			}
		} else {
			task->message_size = 0;
			task->message = 0;
		}

		post_platform_task(task);
	}

	return 0;
}

int flutterpi_respond_to_platform_message(
	FlutterPlatformMessageResponseHandle *handle,
	const uint8_t *restrict message,
	size_t message_size
) {
	struct flutterpi_task *task;
	int ok;
	
	if (runs_platform_tasks_on_current_thread(NULL)) {
		ok = kSuccess == FlutterEngineSendPlatformMessageResponse(
			engine,
			handle,
			message,
			message_size
		);

		return ok? 0 : 1;
	} else {
		task = malloc(sizeof(struct flutterpi_task));
		if (task == NULL) return ENOMEM;

		task->type = kRespondToPlatformMessage;
		task->channel = NULL;
		task->responsehandle = handle;

		if (message && message_size) {
			task->message_size = message_size;
			task->message = memdup(message, message_size);
			if (!task->message) return ENOMEM;
		} else {
			task->message_size = 0;
			task->message = 0;
		}

		post_platform_task(task);
	}

	return 0;
}



/******************
 * INITIALIZATION *
 ******************/
static bool setup_paths(void) {
	#define PATH_EXISTS(path) (access((path),R_OK)==0)

	if (!PATH_EXISTS(flutter.asset_bundle_path)) {
		fprintf(stderr, "Asset Bundle Directory \"%s\" does not exist\n", flutter.asset_bundle_path);
		return false;
	}
	
	snprintf(flutter.kernel_blob_path, sizeof(flutter.kernel_blob_path), "%s/kernel_blob.bin", flutter.asset_bundle_path);
	if (!PATH_EXISTS(flutter.kernel_blob_path)) {
		fprintf(stderr, "Kernel blob does not exist inside Asset Bundle Directory.\n");
		return false;
	}

	snprintf(flutter.icu_data_path, sizeof(flutter.icu_data_path), "/usr/lib/icudtl.dat");

	if (!PATH_EXISTS(flutter.icu_data_path)) {
		fprintf(stderr, "ICU Data file not find at %s.\n", flutter.icu_data_path);
		return false;
	}

	//snprintf(drm.device, sizeof(drm.device), "/dev/dri/card0");

	return true;

	#undef PATH_EXISTS
}

static int load_gl_procs(void) {
	LOAD_EGL_PROC(getPlatformDisplay);
	LOAD_EGL_PROC(createPlatformWindowSurface);
	LOAD_EGL_PROC(createPlatformPixmapSurface);
	LOAD_EGL_PROC(createDRMImageMESA);
	LOAD_EGL_PROC(exportDRMImageMESA);

	LOAD_GL_PROC(EGLImageTargetTexture2DOES);
	LOAD_GL_PROC(EGLImageTargetRenderbufferStorageOES);

	return 0;
}

static int init_display() {
	/**********************
	 * DRM INITIALIZATION *
	 **********************/
	const struct drm_connector *connector;
	const struct drm_encoder *encoder;
	const struct drm_crtc *crtc;
	const drmModeModeInfo *mode;
	struct drmdev *drmdev;
	drmDevicePtr devices[64];
	EGLint egl_error;
	GLenum gl_error;
	int ok, area, num_devices;
	
	num_devices = drmGetDevices2(0, devices, sizeof(devices)/sizeof(*devices));
	if (num_devices < 0) {
		fprintf(stderr, "[flutter-pi] Could not query DRM device list: %s\n", strerror(-num_devices));
		return -num_devices;
	}
	
	for (int i = 0; i < num_devices; i++) {
		drmDevicePtr device;
		
		device = devices[i];

		if (!(device->available_nodes & (1 << DRM_NODE_PRIMARY))) {
			// We need a primary node.
			continue;
		}

		ok = drmdev_new_from_path(&drmdev, device->nodes[DRM_NODE_PRIMARY]);
		if (ok != 0) {
			fprintf(stderr, "[flutter-pi] Could not create drmdev from device at \"%s\". Continuing.\n", device->nodes[DRM_NODE_PRIMARY]);
			continue;
		}

		printf("[flutter-pi] Chose \"%s\" as the DRM device.\n", device->nodes[DRM_NODE_PRIMARY]);
		break;
	}

	if (drmdev == NULL) {
		fprintf(stderr, "flutter-pi couldn't find a usable DRM device.\n"
						"Please make sure you've enabled the Fake-KMS driver in raspi-config.\n"
						"If you're not using a Raspberry Pi, please make sure there's KMS support for your graphics chip.\n");
		return false;
	}

	for_each_connector_in_drmdev(drmdev, connector) {
		if (connector->connector->connection == DRM_MODE_CONNECTED) {
			// only update the physical size of the display if the values
			//   are not yet initialized / not set with a commandline option
			if ((width_mm == 0) && (height_mm == 0)) {
				if ((connector->connector->mmWidth == 160) &&
					(connector->connector->mmHeight == 90))
				{
					// if width and height is exactly 160mm x 90mm, the values are probably bogus.
					width_mm = 0;
					height_mm = 0;
				} else if ((connector->connector->connector_type == DRM_MODE_CONNECTOR_DSI) &&
						   (connector->connector->mmWidth == 0) &&
						   (connector->connector->mmHeight == 0))
				{
					// if it's connected via DSI, and the width & height are 0,
					//   it's probably the official 7 inch touchscreen.
					width_mm = 155;
					height_mm = 86;
				} else {
					width_mm = connector->connector->mmWidth;
					height_mm = connector->connector->mmHeight;
				}
			}

			break;
		}
	}

	if (connector == NULL) {
		fprintf(stderr, "[flutter-pi] Could not find a connected connector!\n");
		return EINVAL;
	}
	
	{
		drmModeModeInfo *mode_iter;
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
	}

	if (mode == NULL) {
		fprintf(stderr, "[flutter-pi] Could not find a preferred output mode!\n");
		return EINVAL;
	}

	width = mode->hdisplay;
	height = mode->vdisplay;
	refresh_rate = mode->vrefresh;
	orientation = width >= height ? kLandscapeLeft : kPortraitUp;

	if (pixel_ratio == 0.0) {
		if ((width_mm == 0) || (height_mm == 0)) {
			pixel_ratio = 1.0;
		} else {
			pixel_ratio = (10.0 * width) / (width_mm * 38.0);
			// lets try if this works.
			// if (pixel_ratio < 1.0) pixel_ratio = 1.0;
		}
	}

	for_each_encoder_in_drmdev(drmdev, encoder) {
		if (encoder->encoder->encoder_id == connector->connector->encoder_id) {
			break;
		}
	}

	if (encoder == NULL) {
		fprintf(stderr, "[flutter-pi] Could not find a suitable DRM encoder.\n");
		return EINVAL;
	}

	for_each_crtc_in_drmdev(drmdev, crtc) {
		if (crtc->crtc->crtc_id == encoder->encoder->crtc_id) {
			break;
		}
	}

	if (crtc == NULL) {
		fprintf(stderr, "[flutter-pi] Could not find a suitable DRM CRTC.\n");
		return EINVAL;
	}

	ok = drmdev_configure(drmdev, connector->connector->connector_id, encoder->encoder->encoder_id, crtc->crtc->crtc_id, mode);
	if (ok != 0) return ok;

	printf("[flutter-pi] display properties:\n  %u x %u, %uHz\n  %umm x %umm\n  pixel_ratio = %f\n", width, height, refresh_rate, width_mm, height_mm, pixel_ratio);

	/**********************
	 * GBM INITIALIZATION *
	 **********************/
	printf("Creating GBM device\n");
	gbm.device = gbm_create_device(drmdev->fd);
	gbm.format = DRM_FORMAT_ARGB8888;
	gbm.surface = NULL;
	gbm.modifier = DRM_FORMAT_MOD_LINEAR;

	gbm.surface = gbm_surface_create_with_modifiers(gbm.device, width, height, gbm.format, &gbm.modifier, 1);
	if (gbm.surface == NULL) {
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

	ok = load_gl_procs();
	if (ok != 0) {
		fprintf(stderr, "[flutter-pi] Could not load EGL / GL ES procedure addresses! error: %s\n", strerror(ok));
		return ok;
	}

	eglGetError();

#ifdef EGL_KHR_platform_gbm
	egl.display = egl.getPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm.device, NULL);
	if ((egl_error = eglGetError()) != EGL_SUCCESS) {
		fprintf(stderr, "[flutter-pi] Could not get EGL display! eglGetPlatformDisplay: 0x%08X\n", egl_error);
		return EIO;
	}
#else
	egl.display = eglGetDisplay((void*) gbm.device);
	if ((egl_error = eglGetError()) != EGL_SUCCESS) {
		fprintf(stderr, "[flutter-pi] Could not get EGL display! eglGetDisplay: 0x%08X\n", egl_error);
		return EIO;
	}
#endif
	
	eglInitialize(egl.display, &major, &minor);
	if ((egl_error = eglGetError()) != EGL_SUCCESS) {
		fprintf(stderr, "[flutter-pi] Failed to initialize EGL! eglInitialize: 0x%08X\n", egl_error);
		return EIO;
	}

	egl_exts_dpy = eglQueryString(egl.display, EGL_EXTENSIONS);

	printf("Using display %p with EGL version %d.%d\n", egl.display, major, minor);
	printf("===================================\n");
	printf("EGL information:\n");
	printf("  version: %s\n", eglQueryString(egl.display, EGL_VERSION));
	printf("  vendor: \"%s\"\n", eglQueryString(egl.display, EGL_VENDOR));
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
	
	eglGetConfigs(egl.display, NULL, 0, &count);
	if ((egl_error = eglGetError()) != EGL_SUCCESS) {
		fprintf(stderr, "[flutter-pi] Could not get the number of EGL framebuffer configurations. eglGetConfigs: 0x%08X\n", egl_error);
		return EIO;
	}

	configs = malloc(count * sizeof(EGLConfig));
	if (!configs) return ENOMEM;
	
	/*
	eglGetConfigs(egl.display, configs, count * sizeof(EGLConfig), &count);
	for (int i = 0; i < count; i++) {
		EGLint value;

#		define GET_ATTRIB(attrib) eglGetConfigAttrib(egl.display, configs[i], attrib, &value)
#		define PRINT_ATTRIB_STR(attrib, string) printf("  " #attrib ": %s,\n", string)
#		define PRINT_ATTRIB(attrib, format, ...) printf("  " #attrib ": " format ",\n", __VA_ARGS__)
#		define LOG_ATTRIB(attrib) \
			do { \
				eglGetConfigAttrib(egl.display, configs[i], attrib, &value); \
				printf("  " #attrib ": %d,\n", value); \
			} while (false)
		printf("fb_config[%i] = {\n", i);

		GET_ATTRIB(EGL_COLOR_BUFFER_TYPE);
		PRINT_ATTRIB_STR(
			EGL_COLOR_BUFFER_TYPE,
			value == EGL_RGB_BUFFER ? "EGL_RGB_BUFFER" : "EGL_LUMINANCE_BUFFER"
		);

		LOG_ATTRIB(EGL_RED_SIZE);
		LOG_ATTRIB(EGL_GREEN_SIZE);
		LOG_ATTRIB(EGL_BLUE_SIZE);
		LOG_ATTRIB(EGL_ALPHA_SIZE);
		LOG_ATTRIB(EGL_DEPTH_SIZE);
		LOG_ATTRIB(EGL_STENCIL_SIZE);
		LOG_ATTRIB(EGL_ALPHA_MASK_SIZE);
		LOG_ATTRIB(EGL_LUMINANCE_SIZE);

		LOG_ATTRIB(EGL_BUFFER_SIZE);

		GET_ATTRIB(EGL_NATIVE_RENDERABLE);
		PRINT_ATTRIB_STR(
			EGL_NATIVE_RENDERABLE,
			value ? "true" : "false"
		);

		LOG_ATTRIB(EGL_NATIVE_VISUAL_TYPE);

		GET_ATTRIB(EGL_NATIVE_VISUAL_ID);
		PRINT_ATTRIB(
			EGL_NATIVE_VISUAL_ID,
			"%4s",
			&value
		);

		LOG_ATTRIB(EGL_BIND_TO_TEXTURE_RGB);
		LOG_ATTRIB(EGL_BIND_TO_TEXTURE_RGBA);

		GET_ATTRIB(EGL_CONFIG_CAVEAT);
		PRINT_ATTRIB_STR(
			EGL_CONFIG_CAVEAT,
			value == EGL_NONE ? "EGL_NONE" :
			value == EGL_SLOW_CONFIG ? "EGL_SLOW_CONFIG" :
			value == EGL_NON_CONFORMANT_CONFIG ? "EGL_NON_CONFORMANT_CONFIG" :
			"(?)"
		);

		LOG_ATTRIB(EGL_CONFIG_ID);
		LOG_ATTRIB(EGL_CONFORMANT);
		
		LOG_ATTRIB(EGL_LEVEL);
		
		LOG_ATTRIB(EGL_MAX_PBUFFER_WIDTH);
		LOG_ATTRIB(EGL_MAX_PBUFFER_HEIGHT);
		LOG_ATTRIB(EGL_MAX_PBUFFER_PIXELS);
		LOG_ATTRIB(EGL_MAX_SWAP_INTERVAL);
		LOG_ATTRIB(EGL_MIN_SWAP_INTERVAL);
		
		LOG_ATTRIB(EGL_RENDERABLE_TYPE);
		LOG_ATTRIB(EGL_SAMPLE_BUFFERS);
		LOG_ATTRIB(EGL_SAMPLES);
		
		LOG_ATTRIB(EGL_SURFACE_TYPE);

		GET_ATTRIB(EGL_TRANSPARENT_TYPE);
		PRINT_ATTRIB_STR(
			EGL_TRANSPARENT_TYPE,
			value == EGL_NONE ? "EGL_NONE" :
			"EGL_TRANSPARENT_RGB"
		);

		LOG_ATTRIB(EGL_TRANSPARENT_RED_VALUE);
		LOG_ATTRIB(EGL_TRANSPARENT_GREEN_VALUE);
		LOG_ATTRIB(EGL_TRANSPARENT_BLUE_VALUE);

		printf("}\n");
			
#		undef LOG_ATTRIB
	}
	*/

	eglChooseConfig(egl.display, config_attribs, configs, count, &matched);
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

		eglGetConfigAttrib(egl.display, configs[i], EGL_NATIVE_VISUAL_ID, &native_visual_id);
		if ((egl_error = eglGetError()) != EGL_SUCCESS) {
			fprintf(stderr, "[flutter-pi] Could not query native visual ID of EGL config. eglGetConfigAttrib: 0x%08X\n", egl_error);
			continue;
		}

		if (native_visual_id == gbm.format) {
			egl.config = configs[i];
			_found_matching_config = true;
			break;
		}
	}
	free(configs);

	if (_found_matching_config == false) {
		fprintf(stderr, "[flutter-pi] Could not find EGL framebuffer configuration with appropriate attributes & native visual ID.\n");
		return EIO;
	}

	egl.root_context = eglCreateContext(egl.display, egl.config, EGL_NO_CONTEXT, context_attribs);
	if (egl.root_context == NULL) {
		fprintf(stderr, "failed to create OpenGL ES root context\n");
		return false;
	}

	egl.flutter_render_context = eglCreateContext(egl.display, egl.config, egl.root_context, context_attribs);
	if (egl.flutter_render_context == NULL) {
		fprintf(stderr, "failed to create OpenGL ES context for flutter rendering\n");
		return false;
	}

	egl.flutter_resource_uploading_context = eglCreateContext(egl.display, egl.config, egl.root_context, context_attribs);
	if (egl.flutter_resource_uploading_context == NULL) {
		fprintf(stderr, "failed to create OpenGL ES context for flutter resource uploads\n");
		return false;
	}

	egl.compositor_context = eglCreateContext(egl.display, egl.config, egl.root_context, context_attribs);
	if (egl.compositor_context == NULL) {
		fprintf(stderr, "failed to create OpenGL ES context for compositor\n");
		return false;
	}

	egl.surface = eglCreateWindowSurface(egl.display, egl.config, (EGLNativeWindowType) gbm.surface, NULL);
	if ((egl_error = eglGetError()) != EGL_SUCCESS) {
		fprintf(stderr, "[flutter-pi] Could not create EGL window surface. eglCreateWindowSurface: 0x%08X\n", egl_error);
		return EIO;
	}

	eglMakeCurrent(egl.display, egl.surface, egl.surface, egl.root_context);
	if ((egl_error = eglGetError()) != EGL_SUCCESS) {
		fprintf(stderr, "[flutter-pi] Could not make OpenGL ES root context current to get OpenGL information. eglMakeCurrent: 0x%08X\n", egl_error);
		return EIO;
	}

	egl.renderer = (char*) glGetString(GL_RENDERER);

	gl_exts = (char*) glGetString(GL_EXTENSIONS);
	printf("===================================\n");
	printf("OpenGL ES information:\n");
	printf("  version: \"%s\"\n", glGetString(GL_VERSION));
	printf("  shading language version: \"%s\"\n", glGetString(GL_SHADING_LANGUAGE_VERSION));
	printf("  vendor: \"%s\"\n", glGetString(GL_VENDOR));
	printf("  renderer: \"%s\"\n", egl.renderer);
	printf("  extensions: \"%s\"\n", gl_exts);
	printf("===================================\n");

	// it seems that after some Raspbian update, regular users are sometimes no longer allowed
	//   to use the direct-rendering infrastructure; i.e. the open the devices inside /dev/dri/
	//   as read-write. flutter-pi must be run as root then.
	// sometimes it works fine without root, sometimes it doesn't.
	if (strncmp(egl.renderer, "llvmpipe", sizeof("llvmpipe")-1) == 0) {
		printf("WARNING: Detected llvmpipe (ie. software rendering) as the OpenGL ES renderer.\n"
			   "         Check that flutter-pi has permission to use the 3D graphics hardware,\n"
			   "         or try running it as root.\n"
			   "         This warning will probably result in a \"failed to set mode\" error\n"
			   "         later on in the initialization.\n");
	}

	drm.evctx = (drmEventContext) {
		.version = 4,
		.vblank_handler = NULL,
		.page_flip_handler = on_pageflip_event,
		.page_flip_handler2 = NULL,
		.sequence_handler = NULL
	};

	/*
	printf("Swapping buffers...\n");
	eglSwapBuffers(egl.display, egl.surface);
	if ((egl_error = eglGetError()) != EGL_SUCCESS) {
		fprintf(stderr, "[flutter-pi] Could not make OpenGL ES root context current to get OpenGL information. eglMakeCurrent: 0x%08X\n", egl_error);
		return EIO;
	}

	
	printf("Locking front buffer...\n");
	drm.current_bo = gbm_surface_lock_front_buffer(gbm.surface);

	printf("getting new framebuffer for BO...\n");
	uint32_t current_fb_id = gbm_bo_get_drm_fb_id(drm.current_bo);
	if (!current_fb_id) {
		fprintf(stderr, "failed to get a new framebuffer BO\n");
		return false;
	}

	ok = drmModeSetCrtc(drm.fd, drm.crtc_id, current_fb_id, 0, 0, &drm.connector_id, 1, drm.mode);
	if (ok < 0) {
		fprintf(stderr, "failed to set mode: %s\n", strerror(errno));
		return false;
	}
	*/

	eglMakeCurrent(egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	if ((egl_error = eglGetError()) != EGL_SUCCESS) {
		fprintf(stderr, "[flutter-pi] Could not clear OpenGL ES context. eglMakeCurrent: 0x%08X\n", egl_error);
		return EIO;
	}

	ok = compositor_initialize(drmdev);
	if (ok != 0) {
		return ok;
	}

	drm.drmdev = drmdev;

	return 0;
}

static void destroy_display(void) {
	fprintf(stderr, "Deinitializing display not yet implemented\n");
}

static int init_application(void) {
	FlutterEngineResult engine_result;
	uint64_t ns;
	int ok, _errno;

	ok = plugin_registry_init();
	if (ok != 0) {
		fprintf(stderr, "[flutter-pi] Could not initialize plugin registry: %s\n", strerror(ok));
		return ok;
	}

	// configure flutter rendering
	memset(&flutter.renderer_config, 0, sizeof(FlutterRendererConfig));
	flutter.renderer_config.type = kOpenGL;
	flutter.renderer_config.open_gl = (FlutterOpenGLRendererConfig) {
		.struct_size = sizeof(FlutterOpenGLRendererConfig),
		.make_current = on_make_current,
		.clear_current = on_clear_current,
		.present = on_present,
		.fbo_callback = fbo_callback,
		.make_resource_current = on_make_resource_current,
		.gl_proc_resolver = proc_resolver,
		.surface_transformation = transformation_callback,
		.gl_external_texture_frame_callback = texreg_gl_external_texture_frame_callback
	};

	// configure the project
	memset(&flutter.args, 0, sizeof(FlutterProjectArgs));
	flutter.args = (FlutterProjectArgs) {
		.struct_size = sizeof(FlutterProjectArgs),
		.assets_path = flutter.asset_bundle_path,
		.icu_data_path = flutter.icu_data_path,
		.isolate_snapshot_data_size = 0,
		.isolate_snapshot_data = NULL,
		.isolate_snapshot_instructions_size = 0,
		.isolate_snapshot_instructions = NULL,
		.vm_snapshot_data_size = 0,
		.vm_snapshot_data = NULL,
		.vm_snapshot_instructions_size = 0,
		.vm_snapshot_instructions = NULL,
		.command_line_argc = flutter.engine_argc,
		.command_line_argv = flutter.engine_argv,
		.platform_message_callback = on_platform_message,
		.custom_task_runners = &(FlutterCustomTaskRunners) {
			.struct_size = sizeof(FlutterCustomTaskRunners),
			.platform_task_runner = &(FlutterTaskRunnerDescription) {
				.struct_size = sizeof(FlutterTaskRunnerDescription),
				.user_data = NULL,
				.runs_task_on_current_thread_callback = &runs_platform_tasks_on_current_thread,
				.post_task_callback = &flutter_post_platform_task
			}
		},
		.compositor = &flutter_compositor
	};

	// only enable vsync if the kernel supplies valid vblank timestamps
	ns = 0;
	ok = drmCrtcGetSequence(drm.drmdev->fd, drm.drmdev->selected_crtc->crtc->crtc_id, NULL, &ns);
	_errno = errno;

	if ((ok == 0) && (ns != 0)) {
		drm.disable_vsync = false;
		flutter.args.vsync_callback	= vsync_callback;
	} else {
		drm.disable_vsync = true;
		if (ok != 0) {
			fprintf(
				stderr,
				"WARNING: Could not get last vblank timestamp. %s\n",
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

	// spin up the engine
	engine_result = FlutterEngineRun(FLUTTER_ENGINE_VERSION, &flutter.renderer_config, &flutter.args, NULL, &engine);
	if (engine_result != kSuccess) {
		fprintf(stderr, "[flutter-pi] Could not run the flutter engine.\n");
		return EINVAL;
	}

	engine_running = true;

	// update window size
	engine_result = FlutterEngineSendWindowMetricsEvent(
		engine,
		&(FlutterWindowMetricsEvent) {
			.struct_size = sizeof(FlutterWindowMetricsEvent),
			.width=width,
			.height=height,
			.pixel_ratio = pixel_ratio
		}
	);
	if (engine_result != kSuccess) {
		fprintf(stderr, "[flutter-pi] Could not send window metrics to flutter engine.\n");
		return EINVAL;
	}
	
	return 0;
}

static void destroy_application(void) {
	FlutterEngineShutdown(engine);
	plugin_registry_deinit();
}

/****************
 * Input-Output *
 ****************/
static void  init_io(void) {
	int ok;
	int n_flutter_slots = 0;
	FlutterPointerEvent   flutterevents[64] = {0};
	size_t                i_flutterevent = 0;

	n_input_devices = 0;
	input_devices = NULL;

	// add the mouse slot
	mousepointer = (struct mousepointer_mtslot) {
		.id = 0,
		.flutter_slot_id = n_flutter_slots++,
		.x = 0, .y = 0,
		.phase = kCancel
	};

	flutterevents[i_flutterevent++] = (FlutterPointerEvent) {
		.struct_size = sizeof(FlutterPointerEvent),
		.phase = kAdd,
		.timestamp = (size_t) (FlutterEngineGetCurrentTime()*1000),
		.x = 0,
		.y = 0,
		.signal_kind = kFlutterPointerSignalKindNone,
		.device_kind = kFlutterPointerDeviceKindMouse,
		.device = 0,
		.buttons = 0
	};
	
	// go through all the given paths and add everything you can
	for (int i=0; i < input_devices_glob.gl_pathc; i++) {
		struct input_device dev = {0};
		uint32_t absbits[(ABS_CNT+31) /32] = {0};
		uint32_t relbits[(REL_CNT+31) /32] = {0};
		uint32_t keybits[(KEY_CNT+31) /32] = {0};
		uint32_t props[(INPUT_PROP_CNT+31) /32] = {0};
		
		snprintf(dev.path, sizeof(dev.path), "%s", input_devices_glob.gl_pathv[i]);
		printf("  input device %i: path=\"%s\"\n", i, dev.path);
		
		// first, try to open the event device.
		dev.fd = open(dev.path, O_RDONLY);
		if (dev.fd < 0) {
			perror("\n    error opening the input device");
			continue;
		}

		// query name
		ok = ioctl(dev.fd, EVIOCGNAME(sizeof(dev.name)), &dev.name);
		if (ok != -1) ok = ioctl(dev.fd, EVIOCGID, &dev.input_id);
		if (ok == -1) {
			perror("\n    could not query input device name / id: ioctl for EVIOCGNAME or EVIOCGID failed");
			goto close_continue;
		}

		printf("      %s, connected via %s. vendor: 0x%04X, product: 0x%04X, version: 0x%04X\n", dev.name,
			   INPUT_BUSTYPE_FRIENDLY_NAME(dev.input_id.bustype), dev.input_id.vendor, dev.input_id.product, dev.input_id.version);

		// query supported event codes (for EV_ABS, EV_REL and EV_KEY event types)
		ok = ioctl(dev.fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits);
		if (ok != -1) ok = ioctl(dev.fd, EVIOCGBIT(EV_REL, sizeof(relbits)), relbits);
		if (ok != -1) ok = ioctl(dev.fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits);
		if (ok != -1) ok = ioctl(dev.fd, EVIOCGPROP(sizeof(props)), props);
		if (ok == -1) {
			perror("    could not query input device: ioctl for EVIOCGBIT(EV_ABS), EVIOCGBIT(EV_REL), EVIOCGBIT(EV_KEY) or EVIOCGPROP failed");
			goto close_continue;
		}

		// check if this input device needs a mousepointer (true for mouse, touchpads)
		dev.is_pointer = ISSET(props, INPUT_PROP_POINTER);
		dev.is_direct  = ISSET(props, INPUT_PROP_DIRECT);
		if (!dev.is_pointer && !dev.is_direct) {
			bool touch = ISSET(absbits, ABS_MT_SLOT) || ISSET(keybits, BTN_TOUCH);
			bool touchpad = touch && ISSET(keybits, BTN_TOOL_FINGER);
			bool touchscreen = touch && !touchpad;
			bool mouse = !touch && (ISSET(relbits, REL_X) || ISSET(relbits, REL_Y));
			dev.is_pointer = touchpad || mouse;
			dev.is_direct  = touchscreen;
		}
		dev.kind = dev.is_pointer ? kFlutterPointerDeviceKindMouse : kFlutterPointerDeviceKindTouch;

		// check if the device emits ABS_X and ABS_Y events, and if yes
		// query some calibration data.
		if (ISSET(absbits, ABS_X) && ISSET(absbits, ABS_Y)) {
			ok = ioctl(dev.fd, EVIOCGABS(ABS_X), &(dev.xinfo));
			if (ok != -1) ok = ioctl(dev.fd, EVIOCGABS(ABS_Y), &(dev.yinfo));
			if (ok == -1) {
				perror("    could not query input_absinfo: ioctl for ECIOCGABS(ABS_X) or EVIOCGABS(ABS_Y) failed");
				goto close_continue;
			}
		}

		// check if the device is multitouch (so a multitouch touchscreen or touchpad)
		if (ISSET(absbits, ABS_MT_SLOT)) {
			struct input_absinfo slotinfo;

			// query the ABS_MT_SLOT absinfo
			ok = ioctl(dev.fd, EVIOCGABS(ABS_MT_SLOT), &slotinfo);
			if (ok == -1) {
				perror("    could not query input_absinfo: ioctl for EVIOCGABS(ABS_MT_SLOT) failed");
				goto close_continue;
			}

			// put the info in the input_device field
			dev.n_mtslots = slotinfo.maximum + 1;
			dev.i_active_mtslot = slotinfo.value;
		} else {
			// even if the device doesn't have multitouch support,
			// i may need some space to store coordinates, for example
			// to convert ABS_X/Y events into relative ones (no-multitouch touchpads for example)
			dev.n_mtslots = 1;
			dev.i_active_mtslot = 0;
		}

		dev.mtslots = calloc(dev.n_mtslots, sizeof(struct mousepointer_mtslot));
		for (int j=0; j < dev.n_mtslots; j++) {
			dev.mtslots[j].id = -1;
			dev.mtslots[j].flutter_slot_id = n_flutter_slots++;
			dev.mtslots[j].x = -1;
			dev.mtslots[j].y = -1;

			if (dev.is_pointer) continue;

			flutterevents[i_flutterevent++] = (FlutterPointerEvent) {
				.struct_size = sizeof(FlutterPointerEvent),
				.phase = kAdd,
				.timestamp = (size_t) (FlutterEngineGetCurrentTime()*1000),
				.x = dev.mtslots[j].x == -1 ? 0 : dev.mtslots[j].x,
				.y = dev.mtslots[j].y == -1 ? 0 : dev.mtslots[j].y,
				.signal_kind = kFlutterPointerSignalKindNone,
				.device_kind = dev.kind,
				.device = dev.mtslots[j].flutter_slot_id,
				.buttons = 0
			};
		}

		// if everything worked we expand the input_devices array and add our dev.
		input_devices = realloc(input_devices, ++n_input_devices * sizeof(struct input_device));
		input_devices[n_input_devices-1] = dev;
		continue;

		close_continue:
			close(dev.fd);
			dev.fd = -1;
	}

	if (n_input_devices == 0)
		printf("Warning: No evdev input devices configured.\n");
	
	// configure the console
	ok = console_make_raw();
	if (ok != 0) {
		printf("[flutter-pi] warning: could not make stdin raw\n");
	}

	console_flush_stdin();

	// now send all the kAdd events to flutter.
	ok = kSuccess == FlutterEngineSendPointerEvent(engine, flutterevents, i_flutterevent);
	if (!ok) fprintf(stderr, "error while sending initial mousepointer / multitouch slot information to flutter\n");
}

static void  on_evdev_input(fd_set fds, size_t n_ready_fds) {
	struct input_event    linuxevents[64];
	size_t                n_linuxevents;
	struct input_device  *device;
	struct mousepointer_mtslot *active_mtslot;
	FlutterPointerEvent   flutterevents[64] = {0};
	size_t                i_flutterevent = 0;
	int    ok, i, j;

	while (n_ready_fds > 0) {
		// find out which device got new data
		i = 0;
		while (!FD_ISSET(input_devices[i].fd, &fds))  i++;
		device = &input_devices[i];
		active_mtslot = &device->mtslots[device->i_active_mtslot];

		// read the input events, convert them to flutter pointer events
		ok = read(input_devices[i].fd, linuxevents, sizeof(linuxevents));
		if (ok == -1) {
			fprintf(stderr, "error reading input events from device \"%s\" with path \"%s\": %s\n",
					input_devices[i].name, input_devices[i].path, strerror(errno));
			return;
		} else if (ok == 0) {
			fprintf(stderr, "reached EOF for input device \"%s\" with path \"%s\": %s\n",
					input_devices[i].name, input_devices[i].path, strerror(errno));
			return;
		}
		n_ready_fds--;
		n_linuxevents = ok / sizeof(struct input_event);
		
		// now go through all linux events and update the state and flutterevents array accordingly.
		for (int i=0; i < n_linuxevents; i++) {
			struct input_event *e = &linuxevents[i];
		
			if (e->type == EV_REL) {
				// pointer moved relatively in the X or Y direction
				// for now, without a speed multiplier

				double relx = e->code == REL_X ? e->value : 0;
				double rely = e->code == REL_Y ? e->value : 0;

				mousepointer.x += relx;
				mousepointer.y += rely;

				// only change the current phase if we don't yet have one.
				if ((mousepointer.phase == kCancel) && (relx != 0 || rely != 0))
					mousepointer.phase = device->active_buttons ? kMove : kHover;

			} else if (e->type == EV_ABS) {

				if (e->code == ABS_MT_SLOT) {

					// select a new active mtslot.
					device->i_active_mtslot = e->value;
					active_mtslot = &device->mtslots[device->i_active_mtslot];

				} else if (e->code == ABS_MT_POSITION_X || e->code == ABS_X || e->code == ABS_MT_POSITION_Y || e->code == ABS_Y) {
					double relx = 0, rely = 0;
					
					if (e->code == ABS_MT_POSITION_X || (e->code == ABS_X && device->n_mtslots == 1)) {
						double newx = (e->value - device->xinfo.minimum) * width / (double) (device->xinfo.maximum - device->xinfo.minimum);
						relx = active_mtslot->phase == kDown ? 0 : newx - active_mtslot->x;
						active_mtslot->x = newx;
					} else if (e->code == ABS_MT_POSITION_Y || (e->code == ABS_Y && device->n_mtslots == 1)) {
						double newy = (e->value - device->yinfo.minimum) * height / (double) (device->yinfo.maximum - device->yinfo.minimum);
						rely = active_mtslot->phase == kDown ? 0 : newy - active_mtslot->y;
						active_mtslot->y = newy;
					}

					// if the device is associated with the mouse pointer (touchpad), update that pointer.
					if (relx != 0 || rely != 0) {
						struct mousepointer_mtslot *slot = active_mtslot;

						if (device->is_pointer) {
							mousepointer.x += relx;
							mousepointer.y += rely;
							slot = &mousepointer;
						}

						if (slot->phase == kCancel)
							slot->phase = device->active_buttons ? kMove : kHover;
					}
				} else if ((e->code == ABS_MT_TRACKING_ID) && (active_mtslot->id == -1 || e->value == -1)) {

					// id -1 means no id, or no touch. one tracking id is equivalent one continuous touch contact.
					bool before = device->active_buttons && true;

					if (active_mtslot->id == -1) {
						active_mtslot->id = e->value;
						// only set active_buttons if a touch equals a kMove (not kHover, as it is for multitouch touchpads)
						device->active_buttons |= (device->is_direct ? FLUTTER_BUTTON_FROM_EVENT_CODE(BTN_TOUCH) : 0);
					} else {
						active_mtslot->id = -1;
						device->active_buttons &= ~(device->is_direct ? FLUTTER_BUTTON_FROM_EVENT_CODE(BTN_TOUCH) : 0);
					}

					if (!before != !device->active_buttons)
						active_mtslot->phase = before ? kUp : kDown;
				}

			} else if (e->type == EV_KEY) {
				
				// remember if some buttons were pressed before this update
				bool before = device->active_buttons && true;

				// update the active_buttons bitmap
				// only apply BTN_TOUCH to the active buttons if a touch really equals a pressed button (device->is_direct is set)
				//   is_direct is true for touchscreens, but not for touchpads; so BTN_TOUCH doesn't result in a kMove for touchpads

				glfw_key glfw_key = EVDEV_KEY_TO_GLFW_KEY(e->code);
				if ((glfw_key != GLFW_KEY_UNKNOWN) && (glfw_key != 0)) {
					glfw_key_action action;
					switch (e->value) {
						case 0: action = GLFW_RELEASE; break;
						case 1: action = GLFW_PRESS; break;
						case 2: action = GLFW_REPEAT; break;
						default: action = -1; break;
					}

					rawkb_on_keyevent(EVDEV_KEY_TO_GLFW_KEY(e->code), 0, action);
				} else if (e->code != BTN_TOUCH || device->is_direct) {
					if (e->value == 1) device->active_buttons |=  FLUTTER_BUTTON_FROM_EVENT_CODE(e->code);
					else               device->active_buttons &= ~FLUTTER_BUTTON_FROM_EVENT_CODE(e->code);
				}

				// check if the button state changed
				// if yes, change the current pointer phase
				if (!before != !device->active_buttons)
					(device->is_pointer ? &mousepointer : active_mtslot) ->phase = before ? kUp : kDown;

			} else if ((e->type == EV_SYN) && (e->code == SYN_REPORT)) {
				
				// We can now summarise the updates we received from the evdev into a FlutterPointerEvent
				// and put it in the flutterevents buffer.
				
				size_t n_slots = 0;
				struct mousepointer_mtslot *slots;

				// if this is a pointer device, we don't care about the multitouch slots & only send the updated mousepointer.
				if (device->is_pointer) {
					slots = &mousepointer;
					n_slots = 1;
				} else if (device->is_direct) {
					slots = device->mtslots;
					n_slots = device->n_mtslots;
				}

				for (j = 0; j < n_slots; j++) {

					// we don't want to send an event to flutter if nothing changed.
					if (slots[j].phase == kCancel) continue;

					// convert raw pixel coordinates to flutter pixel coordinates
					// (raw pixel coordinates don't respect screen rotation)
					double flutterx, fluttery;
					if (rotation == 0) {
						flutterx = slots[j].x;
						fluttery = slots[j].y;
					} else if (rotation == 90) {
						flutterx = slots[j].y;
						fluttery = width - slots[j].x;
					} else if (rotation == 180) {
						flutterx = width - slots[j].x;
						fluttery = height - slots[j].y;
					} else if (rotation == 270) {
						flutterx = height - slots[j].y;
						fluttery = slots[j].x;
					}

					flutterevents[i_flutterevent++] = (FlutterPointerEvent) {
						.struct_size = sizeof(FlutterPointerEvent),
						.phase = slots[j].phase,
						.timestamp = e->time.tv_sec*1000000 + e->time.tv_usec,
						.x = flutterx, .y = fluttery,
						.device = slots[j].flutter_slot_id,
						.signal_kind = kFlutterPointerSignalKindNone,
						.scroll_delta_x = 0, .scroll_delta_y = 0,
						.device_kind = device->kind,
						.buttons = device->active_buttons & 0xFF
					};

					slots[j].phase = kCancel;
				}
			}
		}
	}

	if (i_flutterevent == 0) return;

	// now, send the data to the flutter engine
	// TODO: do this on the main thread
	ok = kSuccess == FlutterEngineSendPointerEvent(engine, flutterevents, i_flutterevent);
	if (!ok) {
		fprintf(stderr, "could not send pointer events to flutter engine\n");
	}
}

static void  on_console_input(void) {
	static char buffer[4096];
	glfw_key key;
	char *cursor;
	char *c;
	int ok;

	ok = read(STDIN_FILENO, buffer, sizeof(buffer));
	if (ok == -1) {
		perror("could not read from stdin");
		return;
	} else if (ok == 0) {
		fprintf(stderr, "warning: reached EOF for stdin\n");
		return;
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

static void *io_loop(void *userdata) {
	fd_set read_fds;
	fd_set write_fds;
	fd_set except_fds;
	int n_ready_fds;
	int nfds;

	// put all file-descriptors in the `fds` fd set
	nfds = 0;
	FD_ZERO(&read_fds);
	FD_ZERO(&write_fds);
	FD_ZERO(&except_fds);

	for (int i = 0; i < n_input_devices; i++) {
		FD_SET(input_devices[i].fd, &read_fds);
		if (input_devices[i].fd + 1 > nfds) nfds = input_devices[i].fd + 1;
	}

	FD_SET(drm.drmdev->fd, &read_fds);
	if (drm.drmdev->fd + 1 > nfds) nfds = drm.drmdev->fd + 1;
	
	FD_SET(STDIN_FILENO, &read_fds);

	fd_set const_read_fds = read_fds;
	fd_set const_write_fds = write_fds;
	fd_set const_except_fds = except_fds;

	while (engine_running) {
		// wait for any device to offer new data,
		// but only if no file descriptors have new data.
		n_ready_fds = select(nfds, &read_fds, &write_fds, &except_fds, NULL);
		
		if (n_ready_fds == -1) {
			perror("error while waiting for I/O");
			return NULL;
		} else if (n_ready_fds == 0) {
			continue;
		}
		
		if (FD_ISSET(drm.drmdev->fd, &read_fds)) {
			drmHandleEvent(drm.drmdev->fd, &drm.evctx);
			FD_CLR(drm.drmdev->fd, &read_fds);
			n_ready_fds--;
		}
		
		if (FD_ISSET(STDIN_FILENO, &read_fds)) {
			on_console_input();
			FD_CLR(STDIN_FILENO, &read_fds);
			n_ready_fds--;
		}

		if (n_ready_fds > 0) {
			on_evdev_input(read_fds, n_ready_fds);
		}

		read_fds = const_read_fds;
		write_fds = const_write_fds;
		except_fds = const_except_fds;
	}

	return NULL;
}

static bool  run_io_thread(void) {
	int ok = pthread_create(&io_thread_id, NULL, &io_loop, NULL);
	if (ok != 0) {
		fprintf(stderr, "couldn't create flutter-pi io thread: [%s]", strerror(ok));
		return false;
	}

	ok = pthread_setname_np(io_thread_id, "io.flutter-pi");
	if (ok != 0) {
		fprintf(stderr, "couldn't set name of flutter-pi io thread: [%s]", strerror(ok));
		return false;
	}

	return true;
}


static bool  parse_cmd_args(int argc, char **argv) {
	bool input_specified = false;
	int ok, opt, index = 0;
	input_devices_glob = (glob_t) {0};

	while ((opt = getopt(argc, (char *const *) argv, "+i:h")) != -1) {
		index++;
		switch(opt) {
			case 'i':
				input_specified = true;
				glob(optarg, GLOB_BRACE | GLOB_TILDE | (input_specified ? GLOB_APPEND : 0), NULL, &input_devices_glob);
				index++;
				break;
			case 'h':
			default:
				printf("%s", usage);
				return false;
		}
	}
	
	if (!input_specified)
		// user specified no input devices. use /dev/input/event*.
		glob("/dev/input/event*", GLOB_BRACE | GLOB_TILDE, NULL, &input_devices_glob);

	if (optind >= argc) {
		fprintf(stderr, "error: expected asset bundle path after options.\n");
		printf("%s", usage);
		return false;
	}

	snprintf(flutter.asset_bundle_path, sizeof(flutter.asset_bundle_path), "%s", argv[optind]);

	argv[optind] = argv[0];
	flutter.engine_argc = argc-optind;
	flutter.engine_argv = (const char* const*) &(argv[optind]);

	for (int i=0; i<flutter.engine_argc; i++)
		printf("engine_argv[%i] = %s\n", i, flutter.engine_argv[i]);
	
	return true;
}
int   main(int argc, char **argv) {
	int ok;
	if (!parse_cmd_args(argc, argv)) {
		return EXIT_FAILURE;
	}

	// check if asset bundle path is valid
	if (!setup_paths()) {
		return EXIT_FAILURE;
	}

	if (!init_message_loop()) {
		return EXIT_FAILURE;
	}
	
	// initialize display
	ok = init_display();
	if (ok != 0) {
		return EXIT_FAILURE;
	}
	
	// initialize application
	printf("Initializing Application...\n");
	ok = init_application();
	if (ok != 0) {
		return EXIT_FAILURE;
	}

	printf("Initializing Input devices...\n");
	init_io();
	
	// read input events
	printf("Running IO thread...\n");
	run_io_thread();

	// run message loop
	printf("Running message loop...\n");
	message_loop();

	// exit
	destroy_application();
	destroy_display();
	
	return EXIT_SUCCESS;
}