#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <bcm_host.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <linux/input.h>

#include <flutter_embedder.h>

#include "methodchannel.h"

//#define ICUDTL_IN_EXECUTABLE_DIR

#define PATH_EXISTS(path) (access((path),R_OK)==0)

char* usage = "Flutter Raspberry Pi\n\nUsage:\n  flutter-pi <asset_bundle_path> <flutter_flags>\n";

int argc;
const char* const *argv;
char asset_bundle_path[1024];
char kernel_blob_path[1024];
char executable_path[1024];
char icu_data_path[1024];
uint32_t width;
uint32_t height;
EGLDisplay display;
EGLConfig config;
EGLContext context;
EGLSurface surface;
DISPMANX_DISPLAY_HANDLE_T dispman_display;
DISPMANX_ELEMENT_HANDLE_T dispman_element;
EGL_DISPMANX_WINDOW_T native_window;
FlutterRendererConfig renderer_config;
FlutterProjectArgs project_args;
FlutterEngine engine;
int mouse_filehandle;
double mouse_x = 0;
double mouse_y = 0;
int last_button = -1;

// flutter callbacks
bool make_current(void* userdata) {
	if (eglMakeCurrent(display, surface, surface, context) != EGL_TRUE) {
		fprintf(stderr, "Could not make the context current.\n");
		return false;
	}
	
	return true;
}
bool clear_current(void* userdata) {
	if (eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) != EGL_TRUE) {
		fprintf(stderr, "Could not clear the current context.\n");
		return false;
	}
	
	return true;
}
bool present(void* userdata) {
	if (eglSwapBuffers(display, surface) != EGL_TRUE) {
		fprintf(stderr, "Could not swap buffers to present the screen.\n");
		return false;
	}
	
	return true;
}
uint32_t fbo_callback(void* userdata) {
	return 0;
}
void* proc_resolver(void* userdata, const char* name) {
	if (name == NULL) return NULL;
	
	void* address;
	if ((address = dlsym(RTLD_DEFAULT, name))) {
		return address;
	}
	
	return NULL;
}
void on_platform_message(const FlutterPlatformMessage* message, void* userdata) {
	printf("Got Platform Message on Channel: %s\n", message->channel);
}

bool set_window_size(size_t width, size_t height) {
	FlutterWindowMetricsEvent event = {
		.struct_size = sizeof(FlutterWindowMetricsEvent),
		.width = width,
		.height = height,
		.pixel_ratio = 1.0
	};

	return FlutterEngineSendWindowMetricsEvent(engine, &event) == kSuccess;
}
bool send_pointer_event(int button, double x, double y) {
	FlutterPointerPhase phase = kCancel;

	if (last_button == -1) {
		phase = kAdd;
	} else if (last_button == 0 && button == 0) {
		phase = kHover;
	} else if (last_button == 0 && button != 0) {
		phase = kDown;
	} else if (last_button == button) {
		phase = kMove;
	} else {
		phase = kUp;
	}

	last_button = button;

	FlutterPointerEvent event = {
		.struct_size = sizeof(FlutterPointerEvent),
		.phase = phase,
		.timestamp = (size_t) (FlutterEngineGetCurrentTime()*1000),
		.x = x,
		.y = y,
		.signal_kind = kFlutterPointerSignalKindNone,
	};

	return FlutterEngineSendPointerEvent(engine, &event, 1) == kSuccess;
}


bool setup_paths(void) {
	if (!PATH_EXISTS(asset_bundle_path)) {
		fprintf(stderr, "Asset Bundle Directory \"%s\" does not exist\n", asset_bundle_path);
		return false;
	}
	
	snprintf(kernel_blob_path, 1024, "%s/kernel_blob.bin", asset_bundle_path);
	if (!PATH_EXISTS(kernel_blob_path)) {
		fprintf(stderr, "Kernel blob does not exist inside Asset Bundle Directory.\n");
		return false;
	}

	#ifdef ICUDTL_IN_EXECUTABLE_DIR
		char _link_path[256];
		snprintf(_link_path, 256, "/proc/%d/exe", getpid());
		size_t size = readlink(_link_path, executable_path, 1023);
		if (size <= 0)			sprintf(executable_path, "");
		
		char* lastSlash = strrchr(executable_path, ("/")[0]);
		if (lastSlash == NULL)	sprintf(icu_data_path, "/icudtl.dat");
		else					snprintf(icu_data_path, 1024, "%.*s/icudtl.dat", (int) (lastSlash - executable_path), executable_path);
	#else
		snprintf(icu_data_path, 1024, "/usr/lib/icudtl.dat");
	#endif

	if (!PATH_EXISTS(icu_data_path)) {
		fprintf(stderr, "ICU Data file not find at %s.\n", icu_data_path);
		return false;
	}

	return true;
}

bool init_display(void) {
	printf("Initializing bcm_host...\n");
	bcm_host_init();
	
	// setup the EGL Display
	printf("Getting the EGL display...\n");
	display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (display == EGL_NO_DISPLAY) {
		fprintf(stderr, "Could not get the EGL display.\n");
		return false;
	}
	
	printf("Initializing EGL...\n");
	if (eglInitialize(display, NULL, NULL) != EGL_TRUE) {
		fprintf(stderr, "Could not initialize the EGL display.\n");
		return false;
	}
	
	// choose an EGL config
	EGLConfig config = {0};
	EGLint num_config = 0;
	EGLint attribute_list[] = {
		EGL_RED_SIZE,		8,
		EGL_GREEN_SIZE,		8,
		EGL_BLUE_SIZE,		8,
		EGL_ALPHA_SIZE,		8,
		EGL_SURFACE_TYPE,	EGL_WINDOW_BIT,
		EGL_NONE
	};
	
	printf("Choosing an EGL config...\n");
	if (eglChooseConfig(display, attribute_list, &config, 1, &num_config) != EGL_TRUE) {
		fprintf(stderr, "Could not choose an EGL config.\n");
		return false;
	}
	
	// create the EGL context
	EGLint context_attributes[] = {
		EGL_CONTEXT_CLIENT_VERSION,
		2,
		EGL_NONE
	};
	
	printf("Creating the EGL context...\n");
	context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
	if (context == EGL_NO_CONTEXT) {
		fprintf(stderr, "Could not create the EGL context.\n");
		return false;
	}

	// query current display size
	printf("Querying the display size...\n");
	if (graphics_get_display_size(0, &width, &height) < 0) {
		fprintf(stderr, "Could not query the display size.\n");
		return false;
	}
	
	// setup dispman display
	printf("Opening the dispmanx display...\n");
	dispman_display = vc_dispmanx_display_open(0);
	
	printf("Setting up the dispmanx display...\n");
	DISPMANX_UPDATE_HANDLE_T update = vc_dispmanx_update_start(0);
	const VC_RECT_T dest_rect = {
		.x = 0, .y = 0,
		.width = width, .height = height,
	};
	const VC_RECT_T src_rect = {
		.x = 0, .y = 0,
		.width = width << 16, .height = height << 16,
	};
	
	dispman_element = vc_dispmanx_element_add(
		update,
		dispman_display,
		0,
		&dest_rect,
		0,
		&src_rect,
		DISPMANX_PROTECTION_NONE,
		0,
		0,
		DISPMANX_NO_ROTATE
	);
	
	vc_dispmanx_update_submit_sync(update);
	
	native_window.element = dispman_element;
	native_window.width = width;
	native_window.height = height;
	
	
	// Create EGL window surface
	printf("Creating the EGL window surface...\n");
	surface = eglCreateWindowSurface(display, config, &native_window, NULL);
	if (surface == EGL_NO_SURFACE) {
		fprintf(stderr, "Could not create the EGL Surface.\n");
		return false;
	}
	
	
	return true;
}
void destroy_display(void) {
	if (surface != EGL_NO_SURFACE) {
		eglDestroySurface(display, surface);
		surface = EGL_NO_SURFACE;
	}
	
	vc_dispmanx_display_close(dispman_display);
	
	if (context != EGL_NO_CONTEXT) {
		eglDestroyContext(display, context);
		context = EGL_NO_CONTEXT;
	}
	
	if (display != EGL_NO_DISPLAY) {
		eglTerminate(display);
		display = EGL_NO_DISPLAY;
	}
	
	bcm_host_deinit();
}

bool init_application(void) {
	// configure flutter rendering
	renderer_config.type = kOpenGL;
	renderer_config.open_gl.struct_size = sizeof(renderer_config.open_gl);
	renderer_config.open_gl.make_current = &make_current;
	renderer_config.open_gl.clear_current = &clear_current;
	renderer_config.open_gl.present = &present;
	renderer_config.open_gl.fbo_callback = &fbo_callback;
	renderer_config.open_gl.gl_proc_resolver = &proc_resolver;
	
	// configure flutter
	project_args.struct_size = sizeof(FlutterProjectArgs);
	project_args.assets_path = asset_bundle_path;
	project_args.icu_data_path = icu_data_path;
	project_args.command_line_argc = argc;
	project_args.command_line_argv = argv;
	project_args.platform_message_callback = &on_platform_message;
	
	FlutterEngineResult _result = FlutterEngineRun(FLUTTER_ENGINE_VERSION, &renderer_config, &project_args, NULL, &engine);
	
	if (_result != kSuccess) {
		fprintf(stderr, "Could not run the flutter engine\n");
		return false;
	}

	if (!set_window_size(width, height)) {
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

bool init_inputs(void) {
	mouse_filehandle = open("/dev/input/event0", O_RDONLY);
	if (mouse_filehandle < 0) {
		perror("error opening the mouse file");
		return false;
	}

	return true;
}

bool read_input_events(void) {
	struct input_event event[64];

	send_pointer_event(0, mouse_x, mouse_y);

	while (1) {
		int rd = read(mouse_filehandle, &event, sizeof(struct input_event)*64);

		if (rd < (int) sizeof(struct input_event)) {
			perror("error reading from mouse");
			return false;
		}

		for (int i = 0; i < rd / sizeof(struct input_event); i++) {
			bool changed = false;
			int button = last_button;

			if (event[i].type == EV_REL) {
				if (event[i].code == REL_X) {
					// mouse moved in x direction

					mouse_x += event[i].value;
					changed = true;
				} else if (event[i].code == REL_Y) {
					// mouse moved in y direction

					mouse_y += event[i].value;
					changed = true;
				}
			} else if (event[i].type == EV_KEY) {
				if ((event[i].code == BTN_LEFT) || (event[i].code == BTN_RIGHT)) {
					// mouse left or right button pressed or released
					
					button = event[i].value;
					changed = true;
				}
			}
			
			if (changed) {
				send_pointer_event(button, mouse_x, mouse_y);
			}
		}
		printf("mouse position: %f, %f\n", mouse_x, mouse_y);
	}

	return true;
}

int main(int argc, const char *const * argv) {
	if (argc <= 1) {
		fprintf(stderr, "Invalid Arguments\n");
		fprintf(stdout, "%s", usage);
		return EXIT_FAILURE;
	}
	
	// check if asset bundle path is valid
	printf("asset_bundle_path: %s\n", argv[1]);
	snprintf(asset_bundle_path, 1024, "%s", argv[1]);
	if (!setup_paths()) {
		return EXIT_FAILURE;
	}
	
	// initialize display
	printf("initializing display...\n");
	if (!init_display()) {
		return EXIT_FAILURE;
	}
	
	printf("Initializing Input devices...\n");
	if (!init_inputs()) {
		return EXIT_FAILURE;
	}

	// initialize application
	printf("Initializing Application...\n");
	if (!init_application()) {
		return EXIT_FAILURE;
	}
	
	// read input events
	read_input_events();

	// exit
	destroy_application();
	destroy_display();
	
	return EXIT_SUCCESS;
}