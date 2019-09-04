#include <features.h>
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
#include <GLES/gl.h>
#include <linux/input.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>

#include <math.h>
#include <limits.h>
#include <float.h>

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

int engine_argc;
const char* const *engine_argv;
char asset_bundle_path[1024] 	= {0};
char kernel_blob_path[1024] 	= {0};
char executable_path[1024] 		= {0};
char icu_data_path[1024] 		= {0};
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

bool input_is_mouse = false;
char input_device_path[1024];
int  input_filehandle;
double mouse_x = 0;
double mouse_y = 0;
uint8_t button = 0;
struct TouchscreenSlot ts_slots[10];
struct TouchscreenSlot* ts_slot = &(ts_slots[0]);

pthread_t io_thread_id;
pthread_t platform_thread_id;
struct LinkedTaskListElement task_list_head_sentinel
								= {.next = NULL, .target_time = 0, .task = {.runner = NULL, .task = 0}};
pthread_mutex_t task_list_lock;
bool should_notify_platform_thread = false;
sigset_t sigusr1_set;

/*********************
 * FLUTTER CALLBACKS *
 *********************/
bool     make_current(void* userdata) {
	if (eglMakeCurrent(display, surface, surface, context) != EGL_TRUE) {
		fprintf(stderr, "Could not make the context current.\n");
		return false;
	}
	
	return true;
}
bool     clear_current(void* userdata) {
	if (eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) != EGL_TRUE) {
		fprintf(stderr, "Could not clear the current context.\n");
		return false;
	}
	
	return true;
}
bool     present(void* userdata) {
	printf("*** PRESENT ***\n");
	eglWaitClient();

	if (eglSwapBuffers(display, surface) != EGL_TRUE) {
		fprintf(stderr, "Could not swap buffers to present the screen.\n");
		return false;
	}

	eglWaitNative(EGL_CORE_NATIVE_ENGINE);

	GLenum error = glGetError();
	if (error != GL_NO_ERROR) {
		printf("got gl error: %d\n", error);
	}
	
	return true;
}
uint32_t fbo_callback(void* userdata) {
	return 0;
}
void*    proc_resolver(void* userdata, const char* name) {
	if (name == NULL) return NULL;

	void* address;
	if ((address = dlsym(RTLD_DEFAULT, name))) {
		return address;
	}
	
	return NULL;
}
void     on_platform_message(const FlutterPlatformMessage* message, void* userdata) {
	struct MethodCall methodcall;
	if (!MethodChannel_decode(message->message_size, (uint8_t*) (message->message), &methodcall)) {
		fprintf(stderr, "Decoding method call failed\n");
		return;
	}
	printf("MethodCall: method name: %s argument type: %d\n", methodcall.method, methodcall.argument.type);

	if (strcmp(methodcall.method, "counter") == 0) {
		printf("method \"counter\" was called with argument %d\n", methodcall.argument.int_value);
	}

	MethodChannel_freeMethodCall(&methodcall);
}

/************************
 * PLATFORM TASK-RUNNER *
 ************************/
void  handle_signal(int _) {}
bool  init_message_loop() {
	platform_thread_id = pthread_self();

	// first, initialize the task heap mutex
	if (pthread_mutex_init(&task_list_lock, NULL) != 0) {
		fprintf(stderr, "Could not initialize task list mutex\n");
		return false;
	}

	sigemptyset(&sigusr1_set);
	sigaddset(&sigusr1_set, SIGUSR1);
	
	sigaction(SIGUSR1, &(struct sigaction) {.sa_handler = &handle_signal}, NULL);
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
		} else {
			uint64_t target_time = task_list_head_sentinel.next->target_time;
			uint64_t current_time = FlutterEngineGetCurrentTime();

			if (target_time > current_time) {
				uint64_t diff = target_time - current_time;

				struct timespec target_timespec = {
					.tv_sec = (uint64_t) (diff / 1000000000l),
					.tv_nsec = (uint64_t) (diff % 1000000000l)
				};

				pthread_mutex_unlock(&task_list_lock);

				int result = sigtimedwait(&sigusr1_set, NULL, &target_timespec);
				if (result == EINTR) continue;
			} else {
				pthread_mutex_unlock(&task_list_lock);
			}
		}

		pthread_mutex_lock(&task_list_lock);
		FlutterTask task = task_list_head_sentinel.next->task;

		struct LinkedTaskListElement* new_first = task_list_head_sentinel.next->next;
		free(task_list_head_sentinel.next);
		task_list_head_sentinel.next = new_first;
		pthread_mutex_unlock(&task_list_lock);

		if (FlutterEngineRunTask(engine, &task) != kSuccess) {
			fprintf(stderr, "Error running platform task\n");
			return false;
		};
	}

	return true;
}
void  post_platform_task(FlutterTask task, uint64_t target_time, void* userdata) {
	struct LinkedTaskListElement* to_insert = malloc(sizeof(struct LinkedTaskListElement));
	to_insert->next = NULL;
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

	#undef PATH_EXISTS
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
		EGL_DEPTH_SIZE,		8,
		EGL_STENCIL_SIZE,	1,
		EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,
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
		EGL_CONTEXT_CLIENT_VERSION,	2,
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
	renderer_config.open_gl.struct_size		= sizeof(renderer_config.open_gl);
	renderer_config.open_gl.make_current	= make_current;
	renderer_config.open_gl.clear_current	= clear_current;
	renderer_config.open_gl.present			= present;
	renderer_config.open_gl.fbo_callback	= fbo_callback;
	renderer_config.open_gl.gl_proc_resolver= proc_resolver;
	//renderer_config.open_gl.fbo_reset_after_present = true;
	
	// configure flutter
	project_args.struct_size				= sizeof(FlutterProjectArgs);
	project_args.assets_path				= asset_bundle_path;
	project_args.icu_data_path				= icu_data_path;
	project_args.command_line_argc			= engine_argc;
	project_args.command_line_argv			= engine_argv;
	project_args.platform_message_callback	= on_platform_message;
	project_args.custom_task_runners		= &(FlutterCustomTaskRunners) {
		.struct_size = sizeof(FlutterCustomTaskRunners),
		.platform_task_runner = &(FlutterTaskRunnerDescription) {
			.struct_size = sizeof(FlutterTaskRunnerDescription),
			.user_data = NULL,
			.runs_task_on_current_thread_callback = &runs_platform_tasks_on_current_thread,
			.post_task_callback = &post_platform_task
		}
	};
	
	// spin up the engine
	FlutterEngineResult _result = FlutterEngineRun(FLUTTER_ENGINE_VERSION, &renderer_config, &project_args, NULL, &engine);
	
	if (_result != kSuccess) {
		fprintf(stderr, "Could not run the flutter engine\n");
		return false;
	}

	// update window size
	bool ok = FlutterEngineSendWindowMetricsEvent(
		engine,
		&(FlutterWindowMetricsEvent) {.struct_size = sizeof(FlutterWindowMetricsEvent), .width=width, .height=height, .pixel_ratio=1.0}
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
	input_filehandle = open(input_device_path, O_RDONLY);
	if (input_filehandle < 0) {
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
	if (input_is_mouse) {
		ok = FlutterEngineSendPointerEvent(
			engine,
			& (FlutterPointerEvent) {
				.struct_size = sizeof(FlutterPointerEvent),
				.phase = kAdd,
				.timestamp = (size_t) (FlutterEngineGetCurrentTime()*1000),
				.x = mouse_x,
				.y = mouse_y,
				.signal_kind = kFlutterPointerSignalKindNone
			}, 
		}, 
			}, 
			1
		) == kSuccess;
		if (!ok) return false;
	}

	// mouse
	while (input_is_mouse) {
		// read up to 64 input events
		int rd = read(input_filehandle, &event, sizeof(struct input_event)*64);
		if (rd < (int) sizeof(struct input_event)) {
			perror("error reading from input device");
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
					mouse_x += ev->value;
					phase = button ? kMove : kHover;
				} else if (ev->code == REL_Y) {	// mouse moved in the y-direction
					mouse_y += ev->value;
					phase = button ? kMove : kHover;	
				}
			} else if (ev->type == EV_ABS) {
				if (ev->code == ABS_X) {
					mouse_x = ev->value;
					phase = button ? kMove : kHover;
				} else if (ev->code == ABS_Y) {
					mouse_y = ev->value;
					phase = button ? kMove : kHover;
				}
			} else if ((ev->type == EV_KEY) && ((ev->code == BTN_LEFT) || (ev->code == BTN_RIGHT))) {
				// either the left or the right mouse button was pressed
				// the 1st bit in "button" is set when BTN_LEFT is down. the 2nd bit when BTN_RIGHT is down.
				uint8_t mask = ev->code == BTN_LEFT ? 1 : 2;
				if (ev->value == 1)	button |=  mask;
				else						button &= ~mask;
				
				phase = ev->value == 1 ? kDown : kUp;
			}
			
			if (phase != kCancel) {
				// if something changed, send the pointer event to flutter
				ok = FlutterEngineSendPointerEvent(
					engine,
					& (FlutterPointerEvent) {
						.struct_size = sizeof(FlutterPointerEvent),
						.timestamp = (size_t) (FlutterEngineGetCurrentTime()*1000),
						.phase=phase,  .x=mouse_x,  .y=mouse_y,
						.signal_kind = kFlutterPointerSignalKindNone
					}, 
					1
				) == kSuccess;
				if (!ok) return false;
			}
		}

		printf("mouse position: %f, %f\n", mouse_x, mouse_y);
	}

	if (!input_is_mouse) {
		for (int j = 0; j<10; j++) {
			printf("Sending kAdd %d to Flutter Engine\n", j);
			ts_slots[j].id = -1;
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
	}

	// touchscreen
	while (!input_is_mouse) {
		int rd = read(input_filehandle, &event, sizeof(struct input_event)*64);
		if (rd < (int) sizeof(struct input_event)) {
			perror("error reading from input device");
			return false;
		}

		n_pointerevents = 0;
		for (int i = 0; i < rd / sizeof(struct input_event); i++) {
			ev = &(event[i]);

			if (ev->type == EV_ABS) {
				if (ev->code == ABS_MT_SLOT) {
					ts_slot = &(ts_slots[ev->value]);
				} else if (ev->code == ABS_MT_TRACKING_ID) {
					if (ts_slot->id == -1) {
						ts_slot->id = ev->value;
						ts_slot->phase = kDown;
					} else if (ev->value == -1) {
						ts_slot->id = ev->value;
						ts_slot->phase = kUp;
					}
				} else if (ev->code == ABS_MT_POSITION_X) {
					ts_slot->x = ev->value;
					if (ts_slot->phase == kCancel) ts_slot->phase = kMove;
				} else if (ev->code == ABS_MT_POSITION_Y) {
					ts_slot->y = ev->value;
					if (ts_slot->phase == kCancel) ts_slot->phase = kMove;
				}
			} else if ((ev->type == EV_SYN) && (ev->code == SYN_REPORT)) {
				for (int j = 0; j < 10; j++) {
					if (ts_slots[j].phase != kCancel) {
						pointer_event[n_pointerevents++] = (FlutterPointerEvent) {
							.struct_size = sizeof(FlutterPointerEvent),
							.phase = ts_slots[j].phase,
							.timestamp = (size_t) (FlutterEngineGetCurrentTime()*1000),
							.device = j,
							.x = ts_slots[j].x,
							.y = ts_slots[j].y,
							.signal_kind = kFlutterPointerSignalKindNone
						};
						ts_slots[j].phase = kCancel;
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

bool  parse_cmd_args(int argc, const char *const * argv) {
	int opt;
	int index = 0;

	while ((opt = getopt(argc, (char *const *) argv, "m:t:")) != -1) {
		index++;
		switch(opt) {
			case 'm':
				printf("Using mouse input from mouse %s\n", optarg);
				snprintf(input_device_path, 1023, "%s", optarg);
				input_is_mouse = true;

				index++;
				break;
			case 't':
				printf("Using touchscreen input from %s\n", optarg);
				snprintf(input_device_path, 1023, "%s", optarg);
				input_is_mouse = false;

				index++;
				break;
			default:
				fprintf(stderr, "Unknown Option: %c\n%s", (char) optopt, usage);
				return false;
		}
	}

	if (strlen(input_device_path) == 0) {
		fprintf(stderr, "At least one of -t or -r has to be given\n%s", usage);
		return false;
	}

	if (optind >= argc) {
		fprintf(stderr, "Expected Asset bundle path argument after options\n%s", usage);
		return false;
	}

	snprintf(asset_bundle_path, 1023, "%s", argv[optind]);
	printf("Asset bundle path: %s\n", asset_bundle_path);

	engine_argc = argc-optind-1;
	engine_argv = &(argv[optind+1]);

	return true;
}

int main(int argc, const char *const * argv) {
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