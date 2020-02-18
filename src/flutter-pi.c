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
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <flutter_embedder.h>

#include <flutter-pi.h>
#include <console_keyboard.h>
#include <platformchannel.h>
#include <pluginregistry.h>
#include "plugins/services-plugin.h"
#include "plugins/text_input.h"
#include "plugins/raw_keyboard.h"


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

struct {
	char device[PATH_MAX];
	bool has_device;
	int fd;
	uint32_t connector_id;
	drmModeModeInfo *mode;
	uint32_t crtc_id;
	size_t crtc_index;
	struct gbm_bo *previous_bo;
	drmEventContext evctx;
	bool disable_vsync;
} drm = {0};

struct {
	struct gbm_device  *device;
	struct gbm_surface *surface;
	uint32_t 			format;
	uint64_t			modifier;
} gbm = {0};

struct {
	EGLDisplay display;
	EGLConfig  config;
	EGLContext context;
	EGLSurface surface;

	bool	   modifiers_supported;
	char      *renderer;

	EGLDisplay (*eglGetPlatformDisplayEXT)(EGLenum platform, void *native_display, const EGLint *attrib_list);
	EGLSurface (*eglCreatePlatformWindowSurfaceEXT)(EGLDisplay dpy, EGLConfig config, void *native_window, const EGLint *attrib_list);
	EGLSurface (*eglCreatePlatformPixmapSurfaceEXT)(EGLDisplay dpy, EGLConfig config, void *native_pixmap, const EGLint *attrib_list);
} egl = {0};

struct {
	char asset_bundle_path[256];
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
bool     	   make_current(void* userdata) {
	if (eglMakeCurrent(egl.display, egl.surface, egl.surface, egl.context) != EGL_TRUE) {
		fprintf(stderr, "make_current: could not make the context current.\n");
		return false;
	}
	
	return true;
}
bool     	   clear_current(void* userdata) {
	if (eglMakeCurrent(egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) != EGL_TRUE) {
		fprintf(stderr, "clear_current: could not clear the current context.\n");
		return false;
	}
	
	return true;
}
void		   pageflip_handler(int fd, unsigned int frame, unsigned int sec, unsigned int usec, void *userdata) {
	FlutterEngineTraceEventInstant("pageflip");
	post_platform_task(&(struct flutterpi_task) {
		.type = kVBlankReply,
		.target_time = 0,
		.vblank_ns = sec*1000000000ull + usec*1000ull,
	});
}
void     	   drm_fb_destroy_callback(struct gbm_bo *bo, void *data) {
	struct drm_fb *fb = data;

	if (fb->fb_id)
		drmModeRmFB(drm.fd, fb->fb_id);
	
	free(fb);
}
struct drm_fb *drm_fb_get_from_bo(struct gbm_bo *bo) {
	uint32_t width, height, format, strides[4] = {0}, handles[4] = {0}, offsets[4] = {0}, flags = 0;
	int ok = -1;

	// if the buffer object already has some userdata associated with it,
	//   it's the framebuffer we allocated.
	struct drm_fb *fb = gbm_bo_get_user_data(bo);
	if (fb) return fb;

	// if there's no framebuffer for the bo, we need to create one.
	fb = calloc(1, sizeof(struct drm_fb));
	fb->bo = bo;

	width = gbm_bo_get_width(bo);
	height = gbm_bo_get_height(bo);
	format = gbm_bo_get_format(bo);

	if (gbm_bo_get_modifier && gbm_bo_get_plane_count && gbm_bo_get_stride_for_plane && gbm_bo_get_offset) {
		uint64_t modifiers[4] = {0};
		modifiers[0] = gbm_bo_get_modifier(bo);
		const int num_planes = gbm_bo_get_plane_count(bo);

		for (int i = 0; i < num_planes; i++) {
			strides[i] = gbm_bo_get_stride_for_plane(bo, i);
			handles[i] = gbm_bo_get_handle(bo).u32;
			offsets[i] = gbm_bo_get_offset(bo, i);
			modifiers[i] = modifiers[0];
		}

		if (modifiers[0]) {
			flags = DRM_MODE_FB_MODIFIERS;
		}

		ok = drmModeAddFB2WithModifiers(drm.fd, width, height, format, handles, strides, offsets, modifiers, &fb->fb_id, flags);
	}

	if (ok) {
		if (flags)
			fprintf(stderr, "drm_fb_get_from_bo: modifiers failed!\n");
		
		memcpy(handles, (uint32_t [4]){gbm_bo_get_handle(bo).u32,0,0,0}, 16);
		memcpy(strides, (uint32_t [4]){gbm_bo_get_stride(bo),0,0,0}, 16);
		memset(offsets, 0, 16);

		ok = drmModeAddFB2(drm.fd, width, height, format, handles, strides, offsets, &fb->fb_id, 0);
	}

	if (ok) {
		fprintf(stderr, "drm_fb_get_from_bo: failed to create fb: %s\n", strerror(errno));
		free(fb);
		return NULL;
	}

	gbm_bo_set_user_data(bo, fb, drm_fb_destroy_callback);

	return fb;
}
bool     	   present(void* userdata) {
	fd_set fds;
	struct gbm_bo *next_bo;
	struct drm_fb *fb;
	int ok;

	FlutterEngineTraceEventDurationBegin("present");

	eglSwapBuffers(egl.display, egl.surface);
	next_bo = gbm_surface_lock_front_buffer(gbm.surface);
	fb = drm_fb_get_from_bo(next_bo);

	// workaround for #38
	if (!drm.disable_vsync) {
		ok = drmModePageFlip(drm.fd, drm.crtc_id, fb->fb_id, DRM_MODE_PAGE_FLIP_EVENT, drm.previous_bo);
		if (ok) {
			perror("failed to queue page flip");
			return false;
		}
	} else {
		ok = drmModeSetCrtc(drm.fd, drm.crtc_id, fb->fb_id, 0, 0, &drm.connector_id, 1, drm.mode);
		if (ok == -1) {
			perror("failed swap buffers\n");
			return false;
		}
	}

	gbm_surface_release_buffer(gbm.surface, drm.previous_bo);
	drm.previous_bo = (struct gbm_bo *) next_bo;

	FlutterEngineTraceEventDurationEnd("present");
	
	return true;
}
uint32_t 	   fbo_callback(void* userdata) {
	return 0;
}
void 	 	   cut_word_from_string(char* string, char* word) {
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
const GLubyte *hacked_glGetString(GLenum name) {
	static GLubyte *extensions = NULL;

	if (name != GL_EXTENSIONS)
		return glGetString(name);

	if (extensions == NULL) {
		GLubyte *orig_extensions = (GLubyte *) glGetString(GL_EXTENSIONS);
		
		extensions = malloc(strlen(orig_extensions) + 1);
		if (!extensions) {
			fprintf(stderr, "Could not allocate memory for modified GL_EXTENSIONS string\n");
			return NULL;
		}

		strcpy(extensions, orig_extensions);

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
		cut_word_from_string(extensions, "GL_EXT_map_buffer_range");

		/*
		* definitely broken
		*/
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
	}

	return extensions;
}
void          *proc_resolver(void* userdata, const char* name) {
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
void     	   on_platform_message(const FlutterPlatformMessage* message, void* userdata) {
	int ok;
	if ((ok = PluginRegistry_onPlatformMessage((FlutterPlatformMessage *)message)) != 0)
		fprintf(stderr, "PluginRegistry_onPlatformMessage failed: %s\n", strerror(ok));
}
void	 	   vsync_callback(void* userdata, intptr_t baton) {
	post_platform_task(&(struct flutterpi_task) {
		.type = kVBlankRequest,
		.target_time = 0,
		.baton = baton
	});
}
FlutterTransformation transformation_callback(void *userdata) {
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
bool  init_message_loop() {
	platform_thread_id = pthread_self();
	return true;
}
bool  message_loop(void) {
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
					drmCrtcGetSequence(drm.fd, drm.crtc_id, NULL, &ns);
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

		} else if (FlutterEngineRunTask(engine, &task->task) != kSuccess) {
			fprintf(stderr, "Error running platform task\n");
			return false;
		};

		free(task);
	}

	return true;
}
void  post_platform_task(struct flutterpi_task *task) {
	struct flutterpi_task *to_insert;
	
	to_insert = malloc(sizeof(struct flutterpi_task));
	if (!to_insert) return;

	memcpy(to_insert, task, sizeof(struct flutterpi_task));
	pthread_mutex_lock(&tasklist_lock);
		struct flutterpi_task* this = &tasklist;
		while ((this->next) != NULL && (to_insert->target_time > this->next->target_time))
			this = this->next;

		to_insert->next = this->next;
		this->next = to_insert;
	pthread_mutex_unlock(&tasklist_lock);
	pthread_cond_signal(&task_added);
}
void  flutter_post_platform_task(FlutterTask task, uint64_t target_time, void* userdata) {
	post_platform_task(&(struct flutterpi_task) {
		.type = kFlutterTask,
		.task = task,
		.target_time = target_time
	});
}
bool  runs_platform_tasks_on_current_thread(void* userdata) {
	return pthread_equal(pthread_self(), platform_thread_id) != 0;
}



/******************
 * INITIALIZATION *
 ******************/
bool setup_paths(void) {
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

bool init_display(void) {
	/**********************
	 * DRM INITIALIZATION *
	 **********************/

	drmModeRes *resources = NULL;
	drmModeConnector *connector;
	drmModeEncoder *encoder = NULL;
	int i, ok, area;
	
	if (!drm.has_device) {
		printf("Finding a suitable DRM device, since none is given...\n");
		drmDevicePtr devices[64] = { NULL };
		int num_devices, fd = -1;

		num_devices = drmGetDevices2(0, devices, sizeof(devices)/sizeof(drmDevicePtr));
		if (num_devices < 0) {
			fprintf(stderr, "could not query drm device list: %s\n", strerror(-num_devices));
			return false;
		}
		
		printf("looking for a suitable DRM device from %d available DRM devices...\n", num_devices);
		for (i = 0; i < num_devices; i++) {
			drmDevicePtr device = devices[i];

			printf("  devices[%d]: \n", i);

			printf("    available nodes: ");
			if (device->available_nodes & (1 << DRM_NODE_PRIMARY)) printf("DRM_NODE_PRIMARY, ");
			if (device->available_nodes & (1 << DRM_NODE_CONTROL)) printf("DRM_NODE_CONTROL, ");
			if (device->available_nodes & (1 << DRM_NODE_RENDER))  printf("DRM_NODE_RENDER");
			printf("\n");

			for (int j=0; j < DRM_NODE_MAX; j++) {
				if (device->available_nodes & (1 << j)) {
					printf("    nodes[%s] = \"%s\"\n",
						j == DRM_NODE_PRIMARY ? "DRM_NODE_PRIMARY" :
						j == DRM_NODE_CONTROL ? "DRM_NODE_CONTROL" :
						j == DRM_NODE_RENDER  ? "DRM_NODE_RENDER" : "unknown",
						device->nodes[j]
					);
				}
			}

			printf("    bustype: %s\n",
						device->bustype == DRM_BUS_PCI ? "DRM_BUS_PCI" :
						device->bustype == DRM_BUS_USB ? "DRM_BUS_USB" :
						device->bustype == DRM_BUS_PLATFORM ? "DRM_BUS_PLATFORM" :
						device->bustype == DRM_BUS_HOST1X ? "DRM_BUS_HOST1X" :
						"unknown"
				);

			if (device->bustype == DRM_BUS_PLATFORM) {
				printf("    businfo.fullname: %s\n", device->businfo.platform->fullname);
				// seems like deviceinfo.platform->compatible is not really used.
				//printf("    deviceinfo.compatible: %s\n", device->deviceinfo.platform->compatible);
			}

			// we want a device that's DRM_NODE_PRIMARY and that we can call a drmModeGetResources on.
			if (drm.has_device) continue;
			if (!(device->available_nodes & (1 << DRM_NODE_PRIMARY))) continue;
			
			printf("    opening DRM device candidate at \"%s\"...\n", device->nodes[DRM_NODE_PRIMARY]);
			fd = open(device->nodes[DRM_NODE_PRIMARY], O_RDWR);
			if (fd < 0) {
				printf("      could not open DRM device candidate at \"%s\": %s\n", device->nodes[DRM_NODE_PRIMARY], strerror(errno));
				continue;
			}

			printf("    getting resources of DRM device candidate at \"%s\"...\n", device->nodes[DRM_NODE_PRIMARY]);
			resources = drmModeGetResources(fd);
			if (resources == NULL) {
				printf("      could not query DRM resources for DRM device candidate at \"%s\":", device->nodes[DRM_NODE_PRIMARY]);
				if ((errno = EOPNOTSUPP) || (errno = EINVAL)) printf("doesn't look like a modeset device.\n");
				else										  printf("%s\n", strerror(errno));
				close(fd);
				continue;
			}

			// we found our DRM device.
			printf("    flutter-pi chose \"%s\" as its DRM device.\n", device->nodes[DRM_NODE_PRIMARY]);
			drm.fd = fd;
			drm.has_device = true;
			snprintf(drm.device, sizeof(drm.device)-1, "%s", device->nodes[DRM_NODE_PRIMARY]);
		}

		if (!drm.has_device) {
			fprintf(stderr, "flutter-pi couldn't find a usable DRM device.\n"
							"Please make sure you've enabled the Fake-KMS driver in raspi-config.\n"
							"If you're not using a Raspberry Pi, please make sure there's KMS support for your graphics chip.\n");
			return false;
		}
	}

	if (drm.fd <= 0) {
		printf("Opening DRM device...\n");
		drm.fd = open(drm.device, O_RDWR);
		if (drm.fd < 0) {
			fprintf(stderr, "Could not open DRM device\n");
			return false;
		}
	}

	if (!resources) {
		printf("Getting DRM resources...\n");
		resources = drmModeGetResources(drm.fd);
		if (resources == NULL) {
			if ((errno == EOPNOTSUPP) || (errno = EINVAL))
				fprintf(stderr, "%s doesn't look like a modeset device\n", drm.device);
			else
				fprintf(stderr, "drmModeGetResources failed: %s\n", strerror(errno));
			
			return false;
		}
	}


	printf("Finding a connected connector from %d available connectors...\n", resources->count_connectors);
	connector = NULL;
	for (i = 0; i < resources->count_connectors; i++) {
		drmModeConnector *conn = drmModeGetConnector(drm.fd, resources->connectors[i]);
		
		printf("  connectors[%d]: connected? %s, type: 0x%02X%s, %umm x %umm\n",
			   i,
			   (conn->connection == DRM_MODE_CONNECTED) ? "yes" :
			   (conn->connection == DRM_MODE_DISCONNECTED) ? "no" : "unknown",
			   conn->connector_type,
			   (conn->connector_type == DRM_MODE_CONNECTOR_HDMIA) ? " (HDMI-A)" :
			   (conn->connector_type == DRM_MODE_CONNECTOR_HDMIB) ? " (HDMI-B)" :
			   (conn->connector_type == DRM_MODE_CONNECTOR_DSI) ? " (DSI)" :
			   (conn->connector_type == DRM_MODE_CONNECTOR_DisplayPort) ? " (DisplayPort)" : "",
			   conn->mmWidth, conn->mmHeight
		);

		if ((connector == NULL) && (conn->connection == DRM_MODE_CONNECTED)) {
			connector = conn;

			// only update the physical size of the display if the values
			//   are not yet initialized / not set with a commandline option
			if ((width_mm == 0) && (height_mm == 0)) {
				if ((conn->mmWidth == 160) && (conn->mmHeight == 90)) {
					// if width and height is exactly 160mm x 90mm, the values are probably bogus.
					width_mm = 0;
					height_mm = 0;
				} else if ((conn->connector_type == DRM_MODE_CONNECTOR_DSI) && (conn->mmWidth == 0) && (conn->mmHeight == 0)) {
					// if it's connected via DSI, and the width & height are 0,
					//   it's probably the official 7 inch touchscreen.
					width_mm = 155;
					height_mm = 86;
				} else {
					width_mm = conn->mmWidth;
					height_mm = conn->mmHeight;
				}
			}
		} else {
			drmModeFreeConnector(conn);
		}
	}
	if (!connector) {
		fprintf(stderr, "could not find a connected connector!\n");
		return false;
	}

	printf("Choosing DRM mode from %d available modes...\n", connector->count_modes);
	bool found_preferred = false;
	for (i = 0, area = 0; i < connector->count_modes; i++) {
		drmModeModeInfo *current_mode = &connector->modes[i];

		printf("  modes[%d]: name: \"%s\", %ux%u%s, %uHz, type: %u, flags: %u\n",
			   i, current_mode->name, current_mode->hdisplay, current_mode->vdisplay,
			   (current_mode->flags & DRM_MODE_FLAG_INTERLACE) ? "i" : "p",
			   current_mode->vrefresh, current_mode->type, current_mode->flags
		);

		if (found_preferred) continue;

		// we choose the highest resolution with the highest refresh rate, preferably non-interlaced (= progressive) here.
		int current_area = current_mode->hdisplay * current_mode->vdisplay;
		if (( current_area  > area) ||
			((current_area == area) && (current_mode->vrefresh >  refresh_rate)) || 
			((current_area == area) && (current_mode->vrefresh == refresh_rate) && ((current_mode->flags & DRM_MODE_FLAG_INTERLACE) == 0)) ||
			( current_mode->type & DRM_MODE_TYPE_PREFERRED)) {

			drm.mode = current_mode;
			width = current_mode->hdisplay;
			height = current_mode->vdisplay;
			refresh_rate = current_mode->vrefresh;
			area = current_area;
			orientation = width >= height ? kLandscapeLeft : kPortraitUp;

			// if the preferred DRM mode is bogus, we're screwed.
			if (current_mode->type & DRM_MODE_TYPE_PREFERRED) {
				printf("    this mode is preferred by DRM. (DRM_MODE_TYPE_PREFERRED)\n");
				found_preferred = true;
			}
		}
	}

	if (!drm.mode) {
		fprintf(stderr, "could not find a suitable DRM mode!\n");
		return false;
	}
	
	// calculate the pixel ratio
	if (pixel_ratio == 0.0) {
		if ((width_mm == 0) || (height_mm == 0)) {
			pixel_ratio = 1.0;
		} else {
			pixel_ratio = (10.0 * width) / (width_mm * 38.0);
			if (pixel_ratio < 1.0) pixel_ratio = 1.0;
		}
	}

	printf("Display properties:\n  %u x %u, %uHz\n  %umm x %umm\n  pixel_ratio = %f\n", width, height, refresh_rate, width_mm, height_mm, pixel_ratio);

	printf("Finding DRM encoder...\n");
	for (i = 0; i < resources->count_encoders; i++) {
		encoder = drmModeGetEncoder(drm.fd, resources->encoders[i]);
		if (encoder->encoder_id == connector->encoder_id)
			break;
		drmModeFreeEncoder(encoder);
		encoder = NULL;
	}
	
	if (encoder) {
		drm.crtc_id = encoder->crtc_id;
	} else {
		fprintf(stderr, "could not find a suitable crtc!\n");
		return false;
	}

	for (i = 0; i < resources->count_crtcs; i++) {
		if (resources->crtcs[i] == drm.crtc_id) {
			drm.crtc_index = i;
			break;
		}
	}

	drmModeFreeResources(resources);

	drm.connector_id = connector->connector_id;



	/**********************
	 * GBM INITIALIZATION *
	 **********************/
	printf("Creating GBM device\n");
	gbm.device = gbm_create_device(drm.fd);
	gbm.format = DRM_FORMAT_XRGB8888;
	gbm.surface = NULL;
	gbm.modifier = DRM_FORMAT_MOD_LINEAR;

	gbm.surface = gbm_surface_create_with_modifiers(gbm.device, width, height, gbm.format, &gbm.modifier, 1);

	if (!gbm.surface) {
		if (gbm.modifier != DRM_FORMAT_MOD_LINEAR) {
			fprintf(stderr, "GBM Surface creation modifiers requested but not supported by GBM\n");
			return false;
		}
		gbm.surface = gbm_surface_create(gbm.device, width, height, gbm.format, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
	}

	if (!gbm.surface) {
		fprintf(stderr, "failed to create GBM surface\n");
		return false;
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
		EGL_RED_SIZE, 1,
		EGL_GREEN_SIZE, 1,
		EGL_BLUE_SIZE, 1,
		EGL_ALPHA_SIZE, 0,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_SAMPLES, 0,
		EGL_NONE
	};

	const char *egl_exts_client, *egl_exts_dpy, *gl_exts;

	printf("Querying EGL client extensions...\n");
	egl_exts_client = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);

	egl.eglGetPlatformDisplayEXT = (void*) eglGetProcAddress("eglGetPlatformDisplayEXT");
	printf("Getting EGL display for GBM device...\n");
	if (egl.eglGetPlatformDisplayEXT) egl.display = egl.eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR, gbm.device, NULL);
	else 							  egl.display = eglGetDisplay((void*) gbm.device);
	
	if (!egl.display) {
		fprintf(stderr, "Couldn't get EGL display\n");
		return false;
	}
	

	printf("Initializing EGL...\n");
	if (!eglInitialize(egl.display, &major, &minor)) {
		fprintf(stderr, "failed to initialize EGL\n");
		return false;
	}

	printf("Querying EGL display extensions...\n");
	egl_exts_dpy = eglQueryString(egl.display, EGL_EXTENSIONS);
	egl.modifiers_supported = strstr(egl_exts_dpy, "EGL_EXT_image_dma_buf_import_modifiers") != NULL;


	printf("Using display %p with EGL version %d.%d\n", egl.display, major, minor);
	printf("===================================\n");
	printf("EGL information:\n");
	printf("  version: %s\n", eglQueryString(egl.display, EGL_VERSION));
	printf("  vendor: \"%s\"\n", eglQueryString(egl.display, EGL_VENDOR));
	printf("  client extensions: \"%s\"\n", egl_exts_client);
	printf("  display extensions: \"%s\"\n", egl_exts_dpy);
	printf("===================================\n");


	printf("Binding OpenGL ES API...\n");
	if (!eglBindAPI(EGL_OPENGL_ES_API)) {
		fprintf(stderr, "failed to bind OpenGL ES API\n");
		return false;
	}


	printf("Choosing EGL config...\n");
	EGLint count = 0, matched = 0;
	EGLConfig *configs;
	bool _found_matching_config = false;
	
	if (!eglGetConfigs(egl.display, NULL, 0, &count) || count < 1) {
		fprintf(stderr, "No EGL configs to choose from.\n");
		return false;
	}

	configs = malloc(count * sizeof(EGLConfig));
	if (!configs) return false;

	printf("Finding EGL configs with appropriate attributes...\n");
	if (!eglChooseConfig(egl.display, config_attribs, configs, count, &matched) || !matched) {
		fprintf(stderr, "No EGL configs with appropriate attributes.\n");
		free(configs);
		return false;
	}

	if (!gbm.format) {
		_found_matching_config = true;
	} else {
		for (int i = 0; i < count; i++) {
			EGLint id;
			if (!eglGetConfigAttrib(egl.display, configs[i], EGL_NATIVE_VISUAL_ID, &id))	continue;

			if (id == gbm.format) {
				egl.config = configs[i];
				_found_matching_config = true;
				break;
			}
		}
	}
	free(configs);

	if (!_found_matching_config) {
		fprintf(stderr, "Could not find context with appropriate attributes and matching native visual ID.\n");
		return false;
	}


	printf("Creating EGL context...\n");
	egl.context = eglCreateContext(egl.display, egl.config, EGL_NO_CONTEXT, context_attribs);
	if (egl.context == NULL) {
		fprintf(stderr, "failed to create EGL context\n");
		return false;
	}


	printf("Creating EGL window surface...\n");
	egl.surface = eglCreateWindowSurface(egl.display, egl.config, (EGLNativeWindowType) gbm.surface, NULL);
	if (egl.surface == EGL_NO_SURFACE) {
		fprintf(stderr, "failed to create EGL window surface\n");
		return false;
	}

	if (!eglMakeCurrent(egl.display, egl.surface, egl.surface, egl.context)) {
		fprintf(stderr, "Could not make EGL context current to get OpenGL information\n");
		return false;
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
	if (strncmp(egl.renderer, "llvmpipe", sizeof("llvmpipe")-1) == 0)
		printf("WARNING: Detected llvmpipe (ie. software rendering) as the OpenGL ES renderer.\n"
			   "         Check that flutter-pi has permission to use the 3D graphics hardware,\n"
			   "         or try running it as root.\n"
			   "         This warning will probably result in a \"failed to set mode\" error\n"
			   "         later on in the initialization.\n");

	drm.evctx = (drmEventContext) {
		.version = 4,
		.vblank_handler = NULL,
		.page_flip_handler = pageflip_handler,
		.page_flip_handler2 = NULL,
		.sequence_handler = NULL
	};

	printf("Swapping buffers...\n");
	eglSwapBuffers(egl.display, egl.surface);

	printf("Locking front buffer...\n");
	drm.previous_bo = gbm_surface_lock_front_buffer(gbm.surface);

	printf("getting new framebuffer for BO...\n");
	struct drm_fb *fb = drm_fb_get_from_bo(drm.previous_bo);
	if (!fb) {
		fprintf(stderr, "failed to get a new framebuffer BO\n");
		return false;
	}

	printf("Setting CRTC...\n");
	ok = drmModeSetCrtc(drm.fd, drm.crtc_id, fb->fb_id, 0, 0, &drm.connector_id, 1, drm.mode);
	if (ok) {
		fprintf(stderr, "failed to set mode: %s\n", strerror(errno));
		return false;
	}

	printf("Clearing current context...\n");
	if (!eglMakeCurrent(egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT)) {
		fprintf(stderr, "Could not clear EGL context\n");
		return false;
	}

	printf("finished display setup!\n");

	return true;
}
void destroy_display(void) {
	fprintf(stderr, "Deinitializing display not yet implemented\n");
}

bool init_application(void) {
	int ok, _errno;

	printf("Initializing Plugin Registry...\n");
	ok = PluginRegistry_init();
	if (ok != 0) {
		fprintf(stderr, "Could not initialize plugin registry: %s\n", strerror(ok));
		return false;
	}

	// configure flutter rendering
	flutter.renderer_config.type = kOpenGL;
	flutter.renderer_config.open_gl.struct_size		= sizeof(flutter.renderer_config.open_gl);
	flutter.renderer_config.open_gl.make_current	= make_current;
	flutter.renderer_config.open_gl.clear_current	= clear_current;
	flutter.renderer_config.open_gl.present			= present;
	flutter.renderer_config.open_gl.fbo_callback	= fbo_callback;
	flutter.renderer_config.open_gl.gl_proc_resolver= proc_resolver;
	flutter.renderer_config.open_gl.surface_transformation = transformation_callback;

	// configure flutter
	flutter.args.struct_size				= sizeof(FlutterProjectArgs);
	flutter.args.assets_path				= flutter.asset_bundle_path;
	flutter.args.icu_data_path				= flutter.icu_data_path;
	flutter.args.isolate_snapshot_data_size = 0;
	flutter.args.isolate_snapshot_data		= NULL;
	flutter.args.isolate_snapshot_instructions_size = 0;
	flutter.args.isolate_snapshot_instructions	 = NULL;
	flutter.args.vm_snapshot_data_size		= 0;
	flutter.args.vm_snapshot_data			= NULL;
	flutter.args.vm_snapshot_instructions_size = 0;
	flutter.args.vm_snapshot_instructions	= NULL;
	flutter.args.command_line_argc			= flutter.engine_argc;
	flutter.args.command_line_argv			= flutter.engine_argv;
	flutter.args.platform_message_callback	= on_platform_message;
	flutter.args.custom_task_runners		= &(FlutterCustomTaskRunners) {
		.struct_size = sizeof(FlutterCustomTaskRunners),
		.platform_task_runner = &(FlutterTaskRunnerDescription) {
			.struct_size = sizeof(FlutterTaskRunnerDescription),
			.user_data = NULL,
			.runs_task_on_current_thread_callback = &runs_platform_tasks_on_current_thread,
			.post_task_callback = &flutter_post_platform_task
		}
	};
	
	// only enable vsync if the kernel supplies valid vblank timestamps
	uint64_t ns = 0;
	ok = drmCrtcGetSequence(drm.fd, drm.crtc_id, NULL, &ns);
	if (ok != 0) _errno = errno;

	if ((ok == 0) && (ns != 0)) {
		drm.disable_vsync = false;
		flutter.args.vsync_callback	= vsync_callback;
	} else {
		drm.disable_vsync = true;
		if (ok != 0) {
			fprintf(stderr,
					"WARNING: Could not get last vblank timestamp. %s", strerror(_errno));
		} else {
			fprintf(stderr,
					"WARNING: Kernel didn't return a valid vblank timestamp. (timestamp == 0)\n");
		}
		fprintf(stderr,
				"         VSync will be disabled.\n"
				"         See https://github.com/ardera/flutter-pi/issues/38 for more info.\n");
	}

	// spin up the engine
	FlutterEngineResult _result = FlutterEngineRun(FLUTTER_ENGINE_VERSION, &flutter.renderer_config, &flutter.args, NULL, &engine);
	if (_result != kSuccess) {
		fprintf(stderr, "Could not run the flutter engine\n");
		return false;
	} else {
		printf("flutter engine successfully started up.\n");
	}

	engine_running = true;

	// update window size
	ok = FlutterEngineSendWindowMetricsEvent(
		engine,
		&(FlutterWindowMetricsEvent) {.struct_size = sizeof(FlutterWindowMetricsEvent), .width=width, .height=height, .pixel_ratio = pixel_ratio}
	) == kSuccess;

	if (!ok) {
		fprintf(stderr, "Could not update Flutter application size.\n");
		return false;
	}
	
	return true;
}
void destroy_application(void) {
	int ok;

	if (engine != NULL) {
		if (FlutterEngineShutdown(engine) != kSuccess)
			fprintf(stderr, "Could not shutdown the flutter engine.\n");

		engine = NULL;
	}

	if ((ok = PluginRegistry_deinit()) != 0) {
		fprintf(stderr, "Could not deinitialize plugin registry: %s\n", strerror(ok));
	}
}

/****************
 * Input-Output *
 ****************/
void  init_io(void) {
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
void  on_evdev_input(fd_set fds, size_t n_ready_fds) {
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

					RawKeyboard_onKeyEvent(EVDEV_KEY_TO_GLFW_KEY(e->code), 0, action);
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
	ok = kSuccess == FlutterEngineSendPointerEvent(engine, flutterevents, i_flutterevent);
	if (!ok) {
		fprintf(stderr, "could not send pointer events to flutter engine\n");
	}
}
void  on_console_input(void) {
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
			TextInput_onKey(key);
		} else if (c = console_try_get_utf8char(cursor, &cursor), c != NULL) {
			TextInput_onUtf8Char(c);
		} else {
			// neither a char nor a (function) key. we don't know when
			// we can start parsing the buffer again, so just stop here
			break;
		} 
	}
}
void *io_loop(void *userdata) {
	int n_ready_fds;
	fd_set fds;
	int nfds;


	// put all file-descriptors in the `fds` fd set
	nfds = 0;
	FD_ZERO(&fds);

	for (int i = 0; i < n_input_devices; i++) {
		FD_SET(input_devices[i].fd, &fds);
		if (input_devices[i].fd + 1 > nfds) nfds = input_devices[i].fd + 1;
	}

	FD_SET(drm.fd, &fds);
	if (drm.fd + 1 > nfds) nfds = drm.fd + 1;
	
	FD_SET(STDIN_FILENO, &fds);

	const fd_set const_fds = fds;

	while (engine_running) {
		// wait for any device to offer new data,
		// but only if no file descriptors have new data.
		n_ready_fds = select(nfds, &fds, NULL, NULL, NULL);

		if (n_ready_fds == -1) {
			perror("error while waiting for I/O");
			return NULL;
		} else if (n_ready_fds == 0) {
			perror("reached EOF while waiting for I/O");
			return NULL;
		}
		
		if (FD_ISSET(drm.fd, &fds)) {
			drmHandleEvent(drm.fd, &drm.evctx);
			FD_CLR(drm.fd, &fds);
			n_ready_fds--;
		}
		
		if (FD_ISSET(STDIN_FILENO, &fds)) {
			on_console_input();
			FD_CLR(STDIN_FILENO, &fds);
			n_ready_fds--;
		}

		if (n_ready_fds > 0) {
			on_evdev_input(fds, n_ready_fds);
		}

		fds = const_fds;
	}

	return NULL;
}
bool  run_io_thread(void) {
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


bool  parse_cmd_args(int argc, char **argv) {
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
	printf("initializing display...\n");
	if (!init_display()) {
		return EXIT_FAILURE;
	}
	
	// initialize application
	printf("Initializing Application...\n");
	if (!init_application()) {
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