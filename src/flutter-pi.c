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

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <flutter_embedder.h>

#include "flutter-pi.h"
#include "methodchannel.h"


char* usage ="\
Flutter for Raspberry Pi\n\n\
Usage:\n\
  flutter-pi [options] <asset_bundle_path> <flutter_flags...>\n\n\
Options:\n\
  -m <device_path>   Path to the mouse device file. Typically /dev/input/mouseX or /dev/input/eventX\n\
  -t <device_path>   Path to the touchscreen device file. Typically /dev/input/touchscreenX or /dev/input/eventX\n\
";

// width & height of the display in pixels
uint32_t width, height;

// physical width & height of the display in millimeters
uint32_t width_mm, height_mm;
uint32_t refresh_rate;

// this is the pixel ratio (used by flutter) for the Official Raspberry Pi 7inch display.
// if a HDMI screen is connected and being used by this application, the pixel ratio will be 
// computed inside init_display.
// for DSI the pixel ratio can not be calculated, because there's no (general) way to query the physical
// size for DSI displays.
double pixel_ratio = 1.3671;

struct {
	char device[128];
	int fd;
	uint32_t connector_id;
	drmModeModeInfo *mode;
	uint32_t crtc_id;
	size_t crtc_index;
	int waiting_for_flip;
	struct gbm_bo *previous_bo;
	drmEventContext evctx;
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
	intptr_t next_vblank_baton;
} flutter = {0};

struct {
	char device_path[128];
	int  fd;
	double x, y;
	uint8_t button;
	struct TouchscreenSlot  ts_slots[10];
	struct TouchscreenSlot* ts_slot;
	bool is_mouse;
	bool is_touchscreen;
} input = {0};

pthread_t io_thread_id;
pthread_t platform_thread_id;
struct LinkedTaskListElement task_list_head_sentinel = {
	.next = NULL,
	.is_vblank_event = false,
	.target_time = 0,
	.task = {.runner = NULL, .task = 0}
};
pthread_mutex_t task_list_lock;
bool should_notify_platform_thread = false;
sigset_t sigusr1_set;



/*********************
 * FLUTTER CALLBACKS *
 *********************/
bool     		make_current(void* userdata) {
	if (eglMakeCurrent(egl.display, egl.surface, egl.surface, egl.context) != EGL_TRUE) {
		fprintf(stderr, "Could not make the context current.\n");
		return false;
	}
	
	return true;
}
bool     		clear_current(void* userdata) {
	if (eglMakeCurrent(egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) != EGL_TRUE) {
		fprintf(stderr, "Could not clear the current context.\n");
		return false;
	}
	
	return true;
}
void	 		page_flip_handler(int fd, unsigned int frame, unsigned int sec, unsigned int usec, void *data) {
	int *waiting_for_flip = data;
	*waiting_for_flip = 0;
}
void     		drm_fb_destroy_callback(struct gbm_bo *bo, void *data) {
	struct drm_fb *fb = data;

	if (fb->fb_id)
		drmModeRmFB(drm.fd, fb->fb_id);
	
	free(fb);
}
struct drm_fb*	drm_fb_get_from_bo(struct gbm_bo *bo) {
	uint32_t width, height, format, strides[4] = {0}, handles[4] = {0}, offsets[4] = {0}, flags = 0;
	int ok = -1;

	struct drm_fb *fb = gbm_bo_get_user_data(bo);

	if (fb) return fb;

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
			fprintf(stderr, "Modifiers failed!\n");
		
		memcpy(handles, (uint32_t [4]){gbm_bo_get_handle(bo).u32,0,0,0}, 16);
		memcpy(strides, (uint32_t [4]){gbm_bo_get_stride(bo),0,0,0}, 16);
		memset(offsets, 0, 16);

		ok = drmModeAddFB2(drm.fd, width, height, format, handles, strides, offsets, &fb->fb_id, 0);
	}

	if (ok) {
		fprintf(stderr, "failed to create fb: %s\n", strerror(errno));
		free(fb);
		return NULL;
	}

	gbm_bo_set_user_data(bo, fb, drm_fb_destroy_callback);

	return fb;
}
bool     		present(void* userdata) {
	fd_set fds;
	struct gbm_bo *next_bo;
	struct drm_fb *fb;
	int ok;

	eglSwapBuffers(egl.display, egl.surface);
	next_bo = gbm_surface_lock_front_buffer(gbm.surface);
	fb = drm_fb_get_from_bo(next_bo);

	/* wait for vsync, 
	ok = drmModePageFlip(drm.fd, drm.crtc_id, fb->fb_id, DRM_MODE_PAGE_FLIP_EVENT, &drm.waiting_for_flip);
	if (ok) {
		fprintf(stderr, "failed to queue page flip: %s\n", strerror(errno));
		return false;
	}

	uint64_t t1 = FlutterEngineGetCurrentTime();

	while (drm.waiting_for_flip) {
		FD_ZERO(&fds);
		FD_SET(0, &fds);
		FD_SET(drm.fd, &fds);

		ok = select(drm.fd+1, &fds, NULL, NULL, NULL);
		if (ok < 0) {
			fprintf(stderr, "select err: %s\n", strerror(errno));
			return false;
		} else if (ok == 0) {
			fprintf(stderr, "select timeout!\n");
			return false;
		} else if (FD_ISSET(0, &fds)) {
			fprintf(stderr, "user interrupted!\n");
			return false;
		}

		drmHandleEvent(drm.fd, &drm.evctx);
	}

	uint64_t t2 = FlutterEngineGetCurrentTime();
	printf("waited %" PRIu64 " nanoseconds for page flip\n", t2-t1);
	*/

	gbm_surface_release_buffer(gbm.surface, drm.previous_bo);
	drm.previous_bo = next_bo;

	drm.waiting_for_flip = 1;
	
	return true;
}
uint32_t 		fbo_callback(void* userdata) {
	return 0;
}
void 	 		cut_word_from_string(char* string, char* word) {
	size_t word_length = strlen(word);
	char* word_in_str = strstr(string, word);

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
const GLubyte*	hacked_glGetString(GLenum name) {
	if (name == GL_EXTENSIONS) {
		static GLubyte* extensions;

		if (extensions == NULL) {
			GLubyte* orig_extensions = glGetString(GL_EXTENSIONS);
			size_t len_orig_extensions = strlen(orig_extensions);
			
			extensions = malloc(len_orig_extensions+1);
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
	} else {
		return glGetString(name);
	}
}
void*    		proc_resolver(void* userdata, const char* name) {
	if (name == NULL) return NULL;

	/*  
	 * The mesa v3d driver reports some OpenGL ES extensions as supported and working
	 * even though they aren't. hacked_glGetString is a workaround for this, which will
	 * cut out the non-working extensions from the list of supported extensions.
	 */ 
	if (strcmp(name, "glGetString") == 0) {
		return hacked_glGetString;
	}

	void* address;
	if ((address = dlsym(RTLD_DEFAULT, name)) || (address = eglGetProcAddress(name))) {
		return address;
	}
	
	printf("could not resolve symbol %s\n", name);

	return NULL;
}
void     		on_platform_message(const FlutterPlatformMessage* message, void* userdata) {
	struct MethodCall* methodcall;

	if (!MethodChannel_decode(message->message_size, (uint8_t*) (message->message), &methodcall)) {
		fprintf(stderr, "Decoding method call failed\n");
		return;
	}

	printf("MethodCall: method name: %s argument type: %d\n", methodcall->method, methodcall->argument.type);

	if (strcmp(methodcall->method, "counter") == 0) {
		printf("method \"counter\" was called with argument %d\n", methodcall->argument.int_value);
	}

	MethodChannel_freeMethodCall(&methodcall);
}
void	 		vsync_callback(void* userdata, intptr_t baton) {
	// not yet implemented
	fprintf(stderr, "flutter vsync callback not yet implemented\n");
}



/************************
 * PLATFORM TASK-RUNNER *
 ************************/
void  handle_sigusr1(int _) {}
bool  init_message_loop() {
	platform_thread_id = pthread_self();

	// first, initialize the task heap mutex
	if (pthread_mutex_init(&task_list_lock, NULL) != 0) {
		fprintf(stderr, "Could not initialize task list mutex\n");
		return false;
	}

	sigemptyset(&sigusr1_set);
	sigaddset(&sigusr1_set, SIGUSR1);
	sigaction(SIGUSR1, &(struct sigaction) {.sa_handler = &handle_sigusr1}, NULL);
	pthread_sigmask(SIG_UNBLOCK, &sigusr1_set, NULL);

	return true;
}
bool  message_loop(void) {
	should_notify_platform_thread =  true;

	while (true) {
		pthread_mutex_lock(&task_list_lock);
		if (task_list_head_sentinel.next == NULL) {
			pthread_mutex_unlock(&task_list_lock);
			sigwaitinfo(&sigusr1_set, NULL);
			continue;
		} else {
			uint64_t target_time = task_list_head_sentinel.next->target_time;
			uint64_t current_time = FlutterEngineGetCurrentTime();

			if (target_time > current_time) {
				uint64_t diff = target_time - current_time;

				struct timespec target_timespec = {
					.tv_sec = (uint64_t) (diff / 1000000000ull),
					.tv_nsec = (uint64_t) (diff % 1000000000ull)
				};

				pthread_mutex_unlock(&task_list_lock);
				sigtimedwait(&sigusr1_set, NULL, &target_timespec);
				continue;
			}
		}

		FlutterTask task = task_list_head_sentinel.next->task;
		bool is_vblank_event = task_list_head_sentinel.next->is_vblank_event;
		drmVBlankReply vbl = task_list_head_sentinel.next->vbl;

		struct LinkedTaskListElement* new_first = task_list_head_sentinel.next->next;
		free(task_list_head_sentinel.next);
		task_list_head_sentinel.next = new_first;
		pthread_mutex_unlock(&task_list_lock);
		
		if (is_vblank_event) {
			
		} else if (FlutterEngineRunTask(engine, &task) != kSuccess) {
			fprintf(stderr, "Error running platform task\n");
			return false;
		};
	}

	return true;
}
void  post_platform_task(FlutterTask task, uint64_t target_time, void* userdata) {
	struct LinkedTaskListElement* to_insert = malloc(sizeof(struct LinkedTaskListElement));
	to_insert->next = NULL;
	to_insert->is_vblank_event = false;
	to_insert->task = task;
	to_insert->target_time = target_time;
	
	pthread_mutex_lock(&task_list_lock);
	struct LinkedTaskListElement* this = &task_list_head_sentinel;
	while ((this->next) != NULL && (target_time > this->next->target_time))
		this = this->next;

	to_insert->next = this->next;
	this->next = to_insert;
	pthread_mutex_unlock(&task_list_lock);

	if (should_notify_platform_thread) {
		pthread_kill(platform_thread_id, SIGUSR1);
	}
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

	snprintf(drm.device, sizeof(drm.device), "/dev/dri/card0");

	return true;

	#undef PATH_EXISTS
}

bool init_display(void) {
	/**********************
	 * DRM INITIALIZATION *
	 **********************/

	drmModeRes *resources;
	drmModeConnector *connector;
	drmModeEncoder *encoder;
	int i, ok, area;
	

	printf("Opening DRM device...\n");
	drm.fd = open(drm.device, O_RDWR);
	if (drm.fd < 0) {
		fprintf(stderr, "Could not open DRM device\n");
		return false;
	}


	printf("Getting DRM resources...\n");
	resources = drmModeGetResources(drm.fd);
	if (resources == NULL) {
		if (errno == EOPNOTSUPP)	fprintf(stderr, "%s doesn't look like a modeset device\n", drm.device);
		else						fprintf(stderr, "drmModeGetResources failed: %s\n", strerror(errno));
		
		return false;
	}


	printf("Finding a connected connector...\n");
	for (i = 0; i < resources->count_connectors; i++) {
		connector = drmModeGetConnector(drm.fd, resources->connectors[i]);
		if (connector->connection == DRM_MODE_CONNECTED) {
			width_mm = connector->mmWidth;
			height_mm = connector->mmHeight;
			break;
		}
		drmModeFreeConnector(connector);
		connector = NULL;
	}
	if (!connector) {
		fprintf(stderr, "could not find a connected connector!\n");
		return false;
	}

	printf("Choosing DRM mode...\n");
	for (i = 0, area = 0; i < connector->count_modes; i++) {
		drmModeModeInfo *current_mode = &connector->modes[i];

		if (current_mode->type & DRM_MODE_TYPE_PREFERRED) {
			drm.mode = current_mode;
			width = drm.mode->hdisplay;
			height = drm.mode->vdisplay;
			refresh_rate = drm.mode->vrefresh;

			if (width_mm) pixel_ratio = (10.0 * width) / (width_mm * 38.0);

			break;
		}

		int current_area = current_mode->hdisplay * current_mode->vdisplay;
		if (current_area > area) {
			drm.mode = current_mode;
			area = current_area;
		}
	}
	if (!drm.mode) {
		fprintf(stderr, "could not find a suitable DRM mode!\n");
		return false;
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

	if (gbm_surface_create_with_modifiers) {
		gbm.surface = gbm_surface_create_with_modifiers(gbm.device, width, height, gbm.format, &gbm.modifier, 1);
	}

	if (!gbm.surface) {
		if (gbm.modifier != 0) {
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
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
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


	printf("Using display %d with EGL version %d.%d\n", egl.display, major, minor);
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

	gl_exts = (char*) glGetString(GL_EXTENSIONS);
	printf("===================================\n");
	printf("OpenGL ES 2.x information:\n");
	printf("  version: \"%s\"\n", glGetString(GL_VERSION));
	printf("  shading language version: \"%s\"\n", glGetString(GL_SHADING_LANGUAGE_VERSION));
	printf("  vendor: \"%s\"\n", glGetString(GL_VENDOR));
	printf("  renderer: \"%s\"\n", glGetString(GL_RENDERER));
	printf("  extensions: \"%s\"\n", gl_exts);
	printf("===================================\n");


	drm.evctx.version = 2;
	drm.evctx.page_flip_handler = page_flip_handler;
	//drm.evctx.vblank_handler = vblank_handler;

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

	drm.waiting_for_flip = 1;

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
	// configure flutter rendering
	flutter.renderer_config.type = kOpenGL;
	flutter.renderer_config.open_gl.struct_size		= sizeof(flutter.renderer_config.open_gl);
	flutter.renderer_config.open_gl.make_current	= make_current;
	flutter.renderer_config.open_gl.clear_current	= clear_current;
	flutter.renderer_config.open_gl.present			= present;
	flutter.renderer_config.open_gl.fbo_callback	= fbo_callback;
	flutter.renderer_config.open_gl.gl_proc_resolver= proc_resolver;

	for (int i=0; i<flutter.engine_argc; i++)
		printf("engine_argv[%i] = %s\n", i, flutter.engine_argv[i]);
	
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
			.post_task_callback = &post_platform_task
		}
	};
	//flutter.args.vsync_callback				= vsync_callback;
	
	// spin up the engine
	FlutterEngineResult _result = FlutterEngineRun(FLUTTER_ENGINE_VERSION, &flutter.renderer_config, &flutter.args, NULL, &engine);

	for (int i=0; i<flutter.engine_argc; i++)
		printf("engine_argv[%i] = %s\n", i, flutter.engine_argv[i]);
	
	if (_result != kSuccess) {
		fprintf(stderr, "Could not run the flutter engine\n");
		return false;
	}

	// update window size
	bool ok = FlutterEngineSendWindowMetricsEvent(
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
	if (engine == NULL) return;
	
	FlutterEngineResult _result = FlutterEngineShutdown(engine);
	
	if (_result != kSuccess) {
		fprintf(stderr, "Could not shutdown the flutter engine.\n");
	}
}



/****************
 * Input-Output *
 ****************/
bool  init_io(void) {
	input.fd = open(input.device_path, O_RDONLY);
	if (input.fd < 0) {
		perror("error opening the input device file");
		return false;
	}

	return true;
}
void* io_loop(void* userdata) {
	FlutterPointerPhase	phase;
	struct input_event	event[64];
	struct input_event*	ev;
	FlutterPointerEvent pointer_event[64];
	int					n_pointerevents = 0;
	bool 				ok;

	// first, tell flutter that the mouse is inside the engine window
	if (input.is_mouse) {
		ok = FlutterEngineSendPointerEvent(
			engine,
			& (FlutterPointerEvent) {
				.struct_size = sizeof(FlutterPointerEvent),
				.phase = kAdd,
				.timestamp = (size_t) (FlutterEngineGetCurrentTime()*1000),
				.x = input.x,
				.y = input.y,
				.signal_kind = kFlutterPointerSignalKindNone
			}, 
			1
		) == kSuccess;
		if (!ok) return false;

		// mouse
		while (1) {
			// read up to 64 input events
			int rd = read(input.fd, &event, sizeof(struct input_event)*64);
			if (rd < (int) sizeof(struct input_event)) {
				fprintf(stderr, "Read %d bytes from input device, should have been %d; error msg: %s\n", rd, sizeof(struct input_event), strerror(errno));
				return false;
			}

			// process the input events
			// TODO: instead of processing an input event, and then send the single resulting Pointer Event (i.e., one at a time) to the Flutter Engine,
			//       process all input events, and send all resulting pointer events at once.
			for (int i = 0; i < rd / sizeof(struct input_event); i++) {
				phase = kCancel;
				ev = &(event[i]);

				if (ev->type == EV_REL) {
					if (ev->code == REL_X) {			// mouse moved in the x-direction
						input.x += ev->value;
						phase = input.button ? kMove : kHover;
					} else if (ev->code == REL_Y) {	// mouse moved in the y-direction
						input.y += ev->value;
						phase = input.button ? kMove : kHover;	
					}
				} else if (ev->type == EV_ABS) {
					if (ev->code == ABS_X) {
						input.x = ev->value;
						phase = input.button ? kMove : kHover;
					} else if (ev->code == ABS_Y) {
						input.y = ev->value;
						phase = input.button ? kMove : kHover;
					}
				} else if ((ev->type == EV_KEY) && ((ev->code == BTN_LEFT) || (ev->code == BTN_RIGHT))) {
					// either the left or the right mouse button was pressed
					// the 1st bit in "button" is set when BTN_LEFT is down. the 2nd bit when BTN_RIGHT is down.
					uint8_t mask = ev->code == BTN_LEFT ? 1 : 2;
					if (ev->value == 1)	input.button |=  mask;
					else				input.button &= ~mask;
					
					phase = ev->value == 1 ? kDown : kUp;
				}
				
				if (phase != kCancel) {
					// if something changed, send the pointer event to flutter
					ok = FlutterEngineSendPointerEvent(
						engine,
						& (FlutterPointerEvent) {
							.struct_size = sizeof(FlutterPointerEvent),
							.timestamp = (size_t) (ev->time.tv_sec * 1000000ul) + ev->time.tv_usec,
							.phase=phase,  .x=input.x,  .y=input.y,
							.signal_kind = kFlutterPointerSignalKindNone
						}, 
						1
					) == kSuccess;
					if (!ok) return false;
				}
			}

			printf("mouse position: %f, %f\n", input.x, input.y);
		}
	} else if (input.is_touchscreen) {
		for (int j = 0; j<10; j++) {
			printf("Sending kAdd %d to Flutter Engine\n", j);
			input.ts_slots[j].id = -1;
			ok = FlutterEngineSendPointerEvent(
				engine,
				& (FlutterPointerEvent) {
					.struct_size = sizeof(FlutterPointerEvent),
					.phase = kAdd,
					.timestamp = (size_t) (FlutterEngineGetCurrentTime()*1000),
					.device = j,
					.x = 0,
					.y = 0,
					.signal_kind = kFlutterPointerSignalKindNone
				},
				1
			) == kSuccess;
			if (!ok) {
				fprintf(stderr, "Error sending Pointer message to flutter engine\n");
				return false;
			}
		}

		input.ts_slot = &(input.ts_slots[0]);
		while (1) {
			int rd = read(input.fd, &event, sizeof(struct input_event)*64);
			if (rd < (int) sizeof(struct input_event)) {
				perror("error reading from input device");
				return false;
			}

			n_pointerevents = 0;
			for (int i = 0; i < rd / sizeof(struct input_event); i++) {
				ev = &(event[i]);

				if (ev->type == EV_ABS) {
					if (ev->code == ABS_MT_SLOT) {
						input.ts_slot = &(input.ts_slots[ev->value]);
					} else if (ev->code == ABS_MT_TRACKING_ID) {
						if (input.ts_slot->id == -1) {
							input.ts_slot->id = ev->value;
							input.ts_slot->phase = kDown;
						} else if (ev->value == -1) {
							input.ts_slot->id = ev->value;
							input.ts_slot->phase = kUp;
						}
					} else if (ev->code == ABS_MT_POSITION_X) {
						input.ts_slot->x = ev->value;
						if (input.ts_slot->phase == kCancel) input.ts_slot->phase = kMove;
					} else if (ev->code == ABS_MT_POSITION_Y) {
						input.ts_slot->y = ev->value;
						if (input.ts_slot->phase == kCancel) input.ts_slot->phase = kMove;
					}
				} else if ((ev->type == EV_SYN) && (ev->code == SYN_REPORT)) {
					for (int j = 0; j < 10; j++) {
						if (input.ts_slots[j].phase != kCancel) {
							pointer_event[n_pointerevents++] = (FlutterPointerEvent) {
								.struct_size = sizeof(FlutterPointerEvent),
								.phase = input.ts_slots[j].phase,
								.timestamp = (size_t) (ev->time.tv_sec * 1000000ul) + ev->time.tv_usec,
								.device = j,
								.x = input.ts_slots[j].x,
								.y = input.ts_slots[j].y,
								.signal_kind = kFlutterPointerSignalKindNone
							};
							input.ts_slots[j].phase = kCancel;
						}
					}
				}
			}

			ok = FlutterEngineSendPointerEvent(
				engine,
				pointer_event,
				n_pointerevents
			) == kSuccess;

			if (!ok) {
				fprintf(stderr, "Error sending pointer events to flutter\n");
				return false;
			}
		}
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
	int opt;
	int index = 0;

	while ((opt = getopt(argc, (char *const *) argv, "+m:t:")) != -1) {
		index++;
		switch(opt) {
			case 'm':
				printf("Using mouse input from mouse %s\n", optarg);
				snprintf(input.device_path, sizeof(input.device_path), "%s", optarg);
				input.is_mouse = true;
				input.is_touchscreen = false;

				index++;
				break;
			case 't':
				printf("Using touchscreen input from %s\n", optarg);
				snprintf(input.device_path, sizeof(input.device_path), "%s", optarg);
				input.is_mouse = false;
				input.is_touchscreen = true;

				index++;
				break;
			default:
				fprintf(stderr, "Unknown Option: %c\n%s", (char) optopt, usage);
				return false;
		}
	}

	if (strlen(input.device_path) == 0) {
		fprintf(stderr, "At least one of -t or -r has to be given\n%s", usage);
		return false;
	}

	if (optind >= argc) {
		fprintf(stderr, "Expected Asset bundle path argument after options\n%s", usage);
		return false;
	}

	snprintf(flutter.asset_bundle_path, sizeof(flutter.asset_bundle_path), "%s", argv[optind]);
	printf("Asset bundle path: %s\n", flutter.asset_bundle_path);

	argv[optind] = argv[0];
	flutter.engine_argc = argc-optind;
	flutter.engine_argv = &(argv[optind]);

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
	
	printf("Initializing Input devices...\n");
	if (!init_io()) {
		return EXIT_FAILURE;
	}

	// initialize application
	printf("Initializing Application...\n");
	if (!init_application()) {
		return EXIT_FAILURE;
	}
	
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