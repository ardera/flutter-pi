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
#include <locale.h>
#include <elf.h>
#include <langinfo.h>
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
#include <pixel_format.h>
#include <compositor_ng.h>
#include <keyboard.h>
#include <user_input.h>
#include <locales.h>
#include <platformchannel.h>
#include <pluginregistry.h>
#include <texture_registry.h>
#include <modesetting.h>
#include <tracer.h>
#include <plugins/text_input.h>
#include <plugins/raw_keyboard.h>
#include <filesystem_layout.h>

#ifdef ENABLE_MTRACE
#   include <mcheck.h>
#endif

FILE_DESCR("flutter-pi")

#define LOAD_EGL_PROC(flutterpi_struct, name, full_name) \
    do { \
        (flutterpi_struct).egl.name = (void*) eglGetProcAddress(#full_name); \
        if ((flutterpi_struct).egl.name == NULL) { \
            LOG_ERROR("FATAL: Could not resolve EGL procedure " #full_name "\n"); \
            return EINVAL; \
        } \
    } while (false)

#define LOAD_GL_PROC(flutterpi_struct, name, full_name) \
	do { \
		(flutterpi_struct).gl.name = (void*) eglGetProcAddress(#full_name); \
		if ((flutterpi_struct).gl.name == NULL) { \
			LOG_ERROR("FATAL: Could not resolve GL procedure " #full_name "\n"); \
			return EINVAL; \
		} \
	} while (false)

#define PIXFMT_ARG_NAME(_name, _arg_name, ...) _arg_name ", "

const char *const usage ="\
flutter-pi - run flutter apps on your Raspberry Pi.\n\
\n\
USAGE:\n\
  flutter-pi [options] <bundle path> [flutter engine options]\n\
\n\
OPTIONS:\n\
  --release                  Run the app in release mode. The AOT snapshot\n\
                             of the app must be located inside the bundle directory.\n\
                             This also requires a libflutter_engine.so that was\n\
                             built with --runtime-mode=release.\n\
                             \n\
  --profile                  Run the app in profile mode. The AOT snapshot\n\
                             of the app must be located inside the bundle directory.\n\
                             This also requires a libflutter_engine.so that was\n\
                             built with --runtime-mode=profile.\n\
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
  --pixelformat <format>     Selects the pixel format to use for the framebuffers.\n\
                             If this is not specified, a good pixel format will\n\
                             be selected automatically.\n\
                             Available pixel formats: " PIXFMT_LIST(PIXFMT_ARG_NAME) "\n\
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
                             If that fails flutter-pi will fallback to using\n\
                             all devices matching \"/dev/input/event*\" as \n\
                             inputs.\n\
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
    https://github.com/flutter/engine/blob/main/shell/common/switches.h\n\
";

struct flutterpi flutterpi;

/*static int flutterpi_post_platform_task(
    int (*callback)(void *userdata),
    void *userdata
);*/

static bool runs_platform_tasks_on_current_thread(void *userdata);

/*********************
 * FLUTTER CALLBACKS *
 *********************/
/// Called on some flutter internal thread when the flutter
/// rendering EGLContext should be made current.
static bool on_make_current(void* userdata) {
    struct flutterpi *flutterpi;
    EGLSurface surface;
    EGLBoolean egl_ok;
    
    DEBUG_ASSERT_NOT_NULL(userdata);
    flutterpi = userdata;

    TRACER_INSTANT(flutterpi->tracer, "on_make_current");

    surface = compositor_get_egl_surface(flutterpi->compositor);
    if (surface == EGL_NO_SURFACE) {
        /// TODO: Should we allow this?
        LOG_ERROR("Couldn't get an EGL surface from the compositor.\n");
        return false;
    }


    egl_ok = eglMakeCurrent(
        flutterpi->egl.display,
        surface, surface,
        flutterpi->egl.flutter_render_context
    );
    if (egl_ok != EGL_TRUE) {
        LOG_ERROR("Could not make the flutter rendering EGL context current. eglMakeCurrent: 0x%08X\n", eglGetError());
        return false;
    }
    
    return true;
}

/// Called on some flutter internal thread to
/// clear the EGLContext.
static bool on_clear_current(void* userdata) {
    struct flutterpi *flutterpi;
    EGLBoolean egl_ok;

    DEBUG_ASSERT_NOT_NULL(userdata);
    flutterpi = userdata;

    TRACER_INSTANT(flutterpi->tracer, "on_clear_current");

    egl_ok = eglMakeCurrent(flutterpi->egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (egl_ok != EGL_TRUE) {
        LOG_ERROR("Could not clear the flutter EGL context. eglMakeCurrent: 0x%08X\n", eglGetError());
        return false;
    }
    
    return true;
}

/// Called on some flutter internal thread when the flutter
/// contents should be presented to the screen.
/// (Won't be called since we're supplying a compositor,
/// still needs to be present)
static bool on_present(void *userdata) {
    (void) userdata;
    return true;
}

/// Called on some flutter internal thread to get the
/// GL FBO id flutter should render into
/// (Won't be called since we're supplying a compositor,
/// still needs to be present)
static uint32_t fbo_callback(void* userdata) {
    struct flutterpi *flutterpi;

    DEBUG_ASSERT_NOT_NULL(userdata);
    flutterpi = userdata;

    TRACER_INSTANT(flutterpi->tracer, "fbo_callback");
    return 0;
}

/// Called on some flutter internal thread when the flutter
/// resource uploading EGLContext should be made current.
static bool on_make_resource_current(void *userdata) {
    struct flutterpi *flutterpi;
    EGLBoolean egl_ok;

    DEBUG_ASSERT_NOT_NULL(userdata);
    flutterpi = userdata;
    
    TRACER_INSTANT(flutterpi->tracer, "on_make_resource_current");

    egl_ok = eglMakeCurrent(flutterpi->egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, flutterpi->egl.flutter_resource_uploading_context);
    if (egl_ok != EGL_TRUE) {
        LOG_ERROR("Could not make the flutter resource uploading EGL context current. eglMakeCurrent: 0x%08X\n", eglGetError());
        return false;
    }
    
    return true;
}

/// Called by flutter 
static void *proc_resolver(
    void* userdata,
    const char* name
) {
    void *address;

    (void) userdata;

    address = eglGetProcAddress(name);
    if (address) {
        return address;
    }

    address = dlsym(RTLD_DEFAULT, name);
    if (address) {
        return address;
    }
    
    LOG_ERROR("Could not resolve EGL/GL symbol \"%s\"\n", name);
    return NULL;
}

static void on_platform_message(
    const FlutterPlatformMessage* message,
    void* userdata
) {
    int ok;

    (void) userdata;

    ok = plugin_registry_on_platform_message((FlutterPlatformMessage *) message);
    if (ok != 0) {
        LOG_ERROR("Error handling platform message. plugin_registry_on_platform_message: %s\n", strerror(ok));
    }
}

static bool flutterpi_runs_platfrom_tasks_on_current_thread(struct flutterpi *flutterpi) {
    DEBUG_ASSERT_NOT_NULL(flutterpi);
    return pthread_equal(pthread_self(), flutterpi->event_loop_thread) != 0;
}

struct frame_req {
    struct flutterpi *flutterpi;
    intptr_t baton;
    uint64_t vblank_ns, next_vblank_ns;
};

static int on_deferred_begin_frame(void *userdata) {
    FlutterEngineResult engine_result;
    struct frame_req *req;

    DEBUG_ASSERT_NOT_NULL(userdata);
    req = userdata;

    DEBUG_ASSERT(flutterpi_runs_platfrom_tasks_on_current_thread(req->flutterpi));

    TRACER_INSTANT(req->flutterpi->tracer, "FlutterEngineOnVsync");
    engine_result = req->flutterpi->flutter.procs.OnVsync(
        req->flutterpi->flutter.engine,
        req->baton,
        req->vblank_ns, req->next_vblank_ns
    );

    free(req);

    if (engine_result != kSuccess) {
        LOG_ERROR("Couldn't signal frame begin to flutter engine. FlutterEngineOnVsync: %s\n", FLUTTER_RESULT_TO_STRING(engine_result));
        return EIO;
    }

    return 0;
}

MAYBE_UNUSED static void on_begin_frame(void *userdata, uint64_t vblank_ns, uint64_t next_vblank_ns) {
    FlutterEngineResult engine_result;
    struct frame_req *req;
    int ok;

    DEBUG_ASSERT_NOT_NULL(userdata);
    req = userdata;

    if (flutterpi_runs_platfrom_tasks_on_current_thread(req->flutterpi)) {
        TRACER_INSTANT(req->flutterpi->tracer, "FlutterEngineOnVsync");
        
        engine_result = req->flutterpi->flutter.procs.OnVsync(
            req->flutterpi->flutter.engine,
            req->baton,
            vblank_ns, next_vblank_ns
        );
        if (engine_result != kSuccess) {
            LOG_ERROR("Couldn't signal frame begin to flutter engine. FlutterEngineOnVsync: %s\n", FLUTTER_RESULT_TO_STRING(engine_result));
            goto fail_free_req;
        }

        free(req);
    } else {
        req->vblank_ns = vblank_ns;
        req->next_vblank_ns = next_vblank_ns;
        ok = flutterpi_post_platform_task(on_deferred_begin_frame, req);
        if (ok != 0) {
            LOG_ERROR("Couldn't defer signalling frame begin.\n");
            goto fail_free_req;
        }
    }

    return;

    fail_free_req:
    free(req);
    return;
}

/// Called on some flutter internal thread to request a frame,
/// and also get the vblank timestamp of the pageflip preceding that frame.
MAYBE_UNUSED static void on_frame_request(
    void* userdata,
    intptr_t baton
) {
    FlutterEngineResult engine_result;
    struct flutterpi *flutterpi;
    struct frame_req *req;
    int ok;

    DEBUG_ASSERT_NOT_NULL(userdata);
    flutterpi = userdata;

    TRACER_INSTANT(flutterpi->tracer, "on_frame_request");

    req = malloc(sizeof *req);
    if (req == NULL) {
        LOG_ERROR("Out of memory\n");
        return;
    }
    
    req->flutterpi = flutterpi;
    req->baton = baton;
    req->vblank_ns = get_monotonic_time();
    req->next_vblank_ns = req->vblank_ns + (1000000000.0 / compositor_get_refresh_rate(flutterpi->compositor));

    if (flutterpi_runs_platfrom_tasks_on_current_thread(req->flutterpi)) {
        TRACER_INSTANT(req->flutterpi->tracer, "FlutterEngineOnVsync");
        
        engine_result = req->flutterpi->flutter.procs.OnVsync(
            req->flutterpi->flutter.engine,
            req->baton,
            req->vblank_ns,
            req->next_vblank_ns
        );
        if (engine_result != kSuccess) {
            LOG_ERROR("Couldn't signal frame begin to flutter engine. FlutterEngineOnVsync: %s\n", FLUTTER_RESULT_TO_STRING(engine_result));
            goto fail_free_req;
        }

        free(req);
    } else {
        ok = flutterpi_post_platform_task(on_deferred_begin_frame, req);
        if (ok != 0) {
            LOG_ERROR("Couldn't defer signalling frame begin.\n");
            goto fail_free_req;
        }
    }

    return;

    fail_free_req:
    free(req);
}

static FlutterTransformation on_get_transformation(void *userdata) {
    struct view_geometry geometry;
    struct flutterpi *flutterpi;

    DEBUG_ASSERT_NOT_NULL(userdata);
    flutterpi = userdata;

    compositor_get_view_geometry(flutterpi->compositor, &geometry);

    return geometry.view_to_display_transform;
}

atomic_int_least64_t platform_task_counter = 0;

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
        LOG_ERROR("Error executing platform task: %s\n", strerror(ok));
    }

    free(task);

    sd_event_source_set_enabled(s, SD_EVENT_OFF);
    sd_event_source_unrefp(&s);

    return 0;
}

int flutterpi_post_platform_task(
    int (*callback)(void *userdata),
    void *userdata
) {
    struct platform_task *task;
    sd_event_source *src;
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
        &src,
        on_execute_platform_task,
        task
    );
    if (ok < 0) {
        LOG_ERROR("Error posting platform task to main loop. sd_event_add_defer: %s\n", strerror(-ok));
        ok = -ok;
        goto fail_unlock_event_loop;
    }

    // Higher values mean lower priority. So later platform tasks are handled later too.
    sd_event_source_set_priority(src, atomic_fetch_add(&platform_task_counter, 1));

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

    (void) usec;

    task = userdata;
    ok = task->callback(task->userdata);
    if (ok != 0) {
        LOG_ERROR("Error executing timed platform task: %s\n", strerror(ok));
    }

    free(task);

    sd_event_source_set_enabled(s, SD_EVENT_OFF);
    sd_event_source_unrefp(&s);

    return 0;
}

int flutterpi_post_platform_task_with_time(
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
        LOG_ERROR("Error posting platform task to main loop. sd_event_add_time: %s\n", strerror(-ok));
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
    free(task);
    return ok;
}

int flutterpi_sd_event_add_io(
    sd_event_source **source_out,
    int fd,
    uint32_t events,
    sd_event_io_handler_t callback,
    void *userdata
) {
    int ok;

    if (pthread_self() != flutterpi.event_loop_thread) {
        pthread_mutex_lock(&flutterpi.event_loop_mutex);
    }

    ok = sd_event_add_io(
        flutterpi.event_loop,
        source_out,
        fd,
        events,
        callback,
        userdata
    );
    if (ok < 0) {
        LOG_ERROR("Could not add IO callback to event loop. sd_event_add_io: %s\n", strerror(-ok));
        return -ok;
    }

    if (pthread_self() != flutterpi.event_loop_thread) {
        ok = write(flutterpi.wakeup_event_loop_fd, (uint8_t[8]) {0, 0, 0, 0, 0, 0, 0, 1}, 8);
        if (ok < 0) {
            perror("[flutter-pi] Error arming main loop for io callback. write");
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

    result = flutterpi.flutter.libflutter_engine.FlutterEngineRunTask(flutterpi.flutter.engine, task);
    if (result != kSuccess) {
        LOG_ERROR("Error running platform task. FlutterEngineRunTask: %d\n", result);
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
    FlutterTask *dup_task;
    int ok;

    (void) userdata;

    dup_task = malloc(sizeof *dup_task);
    if (dup_task == NULL) {
        return;
    }
    
    *dup_task = task;

    ok = flutterpi_post_platform_task_with_time(
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
        result = flutterpi.flutter.libflutter_engine.FlutterEngineSendPlatformMessageResponse(flutterpi.flutter.engine, msg->target_handle, msg->message, msg->message_size);
    } else {
        result = flutterpi.flutter.libflutter_engine.FlutterEngineSendPlatformMessage(
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
        LOG_ERROR("Error sending platform message. FlutterEngineSendPlatformMessage: %s\n", FLUTTER_RESULT_TO_STRING(result));
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
            LOG_ERROR("Error sending platform message. FlutterEngineSendPlatformMessage: %s\n", FLUTTER_RESULT_TO_STRING(result));
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
    FlutterPlatformMessageResponseHandle *handle,
    const uint8_t *restrict message,
    size_t message_size
) {
    struct platform_message *msg;
    FlutterEngineResult result;
    int ok;
    
    if (runs_platform_tasks_on_current_thread(&flutterpi)) {
        result = flutterpi.flutter.libflutter_engine.FlutterEngineSendPlatformMessageResponse(
            flutterpi.flutter.engine,
            handle,
            message,
            message_size
        );
        if (result != kSuccess) {
            LOG_ERROR("Error sending platform message response. FlutterEngineSendPlatformMessageResponse: %s\n", FLUTTER_RESULT_TO_STRING(result));
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

        ok = flutterpi_post_platform_task(
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

struct texture_registry *flutterpi_get_texture_registry(
    struct flutterpi *flutterpi
) {
    return flutterpi->texture_registry;
}

struct texture *flutterpi_create_texture(struct flutterpi *flutterpi) {
    return texture_new(flutterpi_get_texture_registry(flutterpi));
}

const char *flutterpi_get_asset_bundle_path(
    struct flutterpi *flutterpi
) {
    return flutterpi->flutter.paths->asset_bundle_path;
}

/// TODO: Make this refcounted if we're gonna use it from multiple threads.
struct gbm_device *flutterpi_get_gbm_device(struct flutterpi *flutterpi) {
    return drmdev_get_gbm_device(flutterpi->drm.drmdev);
}

EGLDisplay flutterpi_get_egl_display(struct flutterpi *flutterpi) {
    return flutterpi->egl.display;
}

EGLContext flutterpi_create_egl_context(struct flutterpi *flutterpi) {
    EGLContext context;
    
    pthread_mutex_lock(&flutterpi->egl.root_context_lock);
    context = eglCreateContext(
        flutterpi->egl.display, 
        flutterpi->egl.config, 
        flutterpi->egl.root_context, 
        (EGLint[]) {
            EGL_CONTEXT_CLIENT_VERSION, 2,
            EGL_NONE
        }
    );
    if (context == EGL_NO_CONTEXT) {
        LOG_ERROR("Could not create new EGL context from temp context. eglCreateContext: %" PRId32 "\n", eglGetError());
    }
    pthread_mutex_unlock(&flutterpi->egl.root_context_lock);

    return context;
}

void *flutterpi_egl_get_proc_address(struct flutterpi *flutterpi, const char *name) {
    (void) flutterpi;
    return eglGetProcAddress(name);
}

bool flutterpi_supports_egl_extension(struct flutterpi *flutterpi, const char *extension_name_str) {
    return check_egl_extension(flutterpi->egl.client_exts, flutterpi->egl.display_exts, extension_name_str);
}

bool flutterpi_supports_gl_extension(struct flutterpi *flutterpi, const char *extension_name_str) {
    return check_egl_extension(flutterpi->gl.extensions, NULL, extension_name_str);
}

void flutterpi_trace_event_instant(struct flutterpi *flutterpi, const char *name) {
    flutterpi->flutter.libflutter_engine.FlutterEngineTraceEventInstant(name);
}

void flutterpi_trace_event_begin(struct flutterpi *flutterpi, const char *name) {
    flutterpi->flutter.libflutter_engine.FlutterEngineTraceEventDurationBegin(name);
}

void flutterpi_trace_event_end(struct flutterpi *flutterpi, const char *name) {
    flutterpi->flutter.libflutter_engine.FlutterEngineTraceEventDurationEnd(name);
}

static bool runs_platform_tasks_on_current_thread(void* userdata) {
    return flutterpi_runs_platfrom_tasks_on_current_thread(userdata);
}

static int run_main_loop(void) {
    int ok, evloop_fd;

    pthread_mutex_lock(&flutterpi.event_loop_mutex);
    ok = sd_event_get_fd(flutterpi.event_loop);
    if (ok < 0) {
        LOG_ERROR("Could not get fd for main event loop. sd_event_get_fd: %s\n", strerror(-ok));
        pthread_mutex_unlock(&flutterpi.event_loop_mutex);
        return -ok;
    }
    pthread_mutex_unlock(&flutterpi.event_loop_mutex);

    evloop_fd = ok;

    {
        fd_set rfds, wfds, xfds;
        int state;
        FD_ZERO(&rfds);
        FD_ZERO(&wfds);
        FD_ZERO(&xfds);
        FD_SET(evloop_fd, &rfds);
        FD_SET(evloop_fd, &wfds);
        FD_SET(evloop_fd, &xfds);

        const fd_set const_fds = rfds;

        pthread_mutex_lock(&flutterpi.event_loop_mutex);
         
        do {
            state = sd_event_get_state(flutterpi.event_loop);
            switch (state) {
                case SD_EVENT_INITIAL:
                    ok = sd_event_prepare(flutterpi.event_loop);
                    if (ok < 0) {
                        LOG_ERROR("Could not prepare event loop. sd_event_prepare: %s\n", strerror(-ok));
                        return -ok;
                    }

                    break;
                case SD_EVENT_ARMED:
                    pthread_mutex_unlock(&flutterpi.event_loop_mutex);

                    do {
                        rfds = const_fds;
                        wfds = const_fds;
                        xfds = const_fds;
                        ok = select(evloop_fd + 1, &rfds, &wfds, &xfds, NULL);
                        if ((ok < 0) && (errno != EINTR)) {
                            perror("[flutter-pi] Could not wait for event loop events. select");
                            return errno;
                        }
                    } while ((ok < 0) && (errno == EINTR));

                    pthread_mutex_lock(&flutterpi.event_loop_mutex);
                        
                    ok = sd_event_wait(flutterpi.event_loop, 0);
                    if (ok < 0) {
                        LOG_ERROR("Could not check for event loop events. sd_event_wait: %s\n", strerror(-ok));
                        return -ok;
                    }

                    break;
                case SD_EVENT_PENDING:
                    ok = sd_event_dispatch(flutterpi.event_loop);
                    if (ok < 0) {
                        LOG_ERROR("Could not dispatch event loop events. sd_event_dispatch: %s\n", strerror(-ok));
                        return -ok;
                    }

                    break;
                case SD_EVENT_FINISHED:
                    break;
                default:
                    LOG_ERROR("Unhandled event loop state: %d. Aborting\n", state);
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

    (void) s;
    (void) revents;
    (void) userdata;

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
        LOG_ERROR("Could not create main event loop. sd_event_new: %s\n", strerror(-ok));
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
        LOG_ERROR("Error adding wakeup callback to main loop. sd_event_add_io: %s\n", strerror(-ok));
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
static int on_compositor_ready(sd_event_source *s, int fd, uint32_t revents, void *userdata) {
    struct compositor *compositor;

    (void) s;
    (void) fd;
    (void) revents;
    (void) userdata;

    DEBUG_ASSERT_NOT_NULL(userdata);
    compositor = userdata;

    return compositor_on_event_fd_ready(compositor);
}

static const FlutterLocale* on_compute_platform_resolved_locales(const FlutterLocale **locales, size_t n_locales) {
    return locales_on_compute_platform_resolved_locale(flutterpi.locales, locales, n_locales);
}

static bool on_gl_external_texture_frame_callback(
    void* userdata,
    int64_t texture_id,
    size_t width,
    size_t height,
    FlutterOpenGLTexture *texture_out
) {
    struct flutterpi *flutterpi;

    DEBUG_ASSERT_NOT_NULL(userdata);

    flutterpi = userdata;

    return texture_registry_gl_external_texture_frame_callback(
        flutterpi->texture_registry,
        texture_id,
        width,
        height,
        texture_out
    );
}

static int init_display(
    struct flutterpi *flutterpi,
    sd_event *event_loop,
    bool has_pixel_format, enum pixfmt pixel_format,
    bool has_rotation, drm_plane_transform_t rotation,
    bool has_orientation, enum device_orientation orientation,
    bool has_explicit_dimensions, int width_mm, int height_mm
) {
    /**********************
     * DRM INITIALIZATION *
     **********************/
    struct compositor *compositor;
    struct gbm_device *gbm_device;
    sd_event_source *compositor_event_source;
    struct drmdev *drmdev;
    struct tracer *tracer;
    drmDevicePtr devices[64];
    const char *egl_exts_client, *egl_exts_dpy, *gl_renderer, *gl_exts;
    EGLDisplay egl_display;
    EGLContext root_context, flutter_render_context, flutter_resource_uploading_context;
    EGLBoolean egl_ok;
    EGLConfig egl_config;
    EGLint major, minor;
    int ok, n_devices;

    /**********************
     * DRM INITIALIZATION *
     **********************/
    
    ok = drmGetDevices2(0, devices, sizeof(devices)/sizeof(*devices));
    if (ok < 0) {
        LOG_ERROR("Could not query DRM device list: %s\n", strerror(-ok));
        return -ok;
    }

    n_devices = ok;
    
    // find a GPU that has a primary node
    drmdev = NULL;
    for (int i = 0; i < n_devices; i++) {
        drmDevicePtr device;
        
        device = devices[i];

        if (!(device->available_nodes & (1 << DRM_NODE_PRIMARY))) {
            // We need a primary node.
            continue;
        }

        ok = drmdev_new_from_path(&drmdev, device->nodes[DRM_NODE_PRIMARY]);
        if (ok != 0) {
            LOG_ERROR("Could not create drmdev from device at \"%s\". Continuing.\n", device->nodes[DRM_NODE_PRIMARY]);
            continue;
        }

        for_each_connector_in_drmdev(flutterpi.drm.drmdev, connector) {
            if (connector->connector->connection == DRM_MODE_CONNECTED) {
                goto found_connected_connector;
            }
        }
        LOG_ERROR("Device \"%s\" doesn't have a display connected. Skipping.\n", device->nodes[DRM_NODE_PRIMARY]);
        continue;


        found_connected_connector:
        break;
    }

    if (drmdev == NULL) {
        LOG_ERROR("flutter-pi couldn't find a usable DRM device.\n"
                  "Please make sure you've enabled the Fake-KMS driver in raspi-config.\n"
                  "If you're not using a Raspberry Pi, please make sure there's KMS support for your graphics chip.\n");
        return ENOENT;
    }

    gbm_device = drmdev_get_gbm_device(drmdev);

    egl_display = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm_device, NULL);
    if (egl_display == EGL_NO_DISPLAY) {
        LOG_ERROR("Could not get EGL display from GBM device. eglGetPlatformDisplay: 0x%08X\n", eglGetError());
        ok = EIO;
        goto fail_unref_drmdev;
    } 
    
    egl_ok = eglInitialize(egl_display, &major, &minor);
    if (egl_ok != EGL_TRUE) {
        LOG_ERROR("Failed to initialize EGL! eglInitialize: 0x%08X\n", eglGetError());
        ok = EIO;
        goto fail_unref_drmdev;
    }

    egl_exts_client = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    egl_exts_dpy = eglQueryString(egl_display, EGL_EXTENSIONS);

    locales_print(flutterpi.locales);
    LOG_DEBUG_UNPREFIXED(
        "EGL information:\n"
        "  version: %s\n"
        "  vendor: \"%s\"\n"
        "  client extensions: \"%s\"\n"
        "  display extensions: \"%s\"\n"
        "===================================\n",
        eglQueryString(egl_display, EGL_VERSION),
        eglQueryString(egl_display, EGL_VENDOR),
        egl_exts_client,
        egl_exts_dpy
    );

    eglBindAPI(EGL_OPENGL_ES_API);

    if (check_egl_extension(egl_exts_client, egl_exts_dpy, "EGL_KHR_no_config_context")) {
        // EGL supports creating contexts without an EGLConfig, which is nice.
        // Just create a context without selecting a config and let the backing stores (when they're created) select
        // the framebuffer config instead.
        egl_config = EGL_NO_CONFIG_KHR;
    } else {
        // choose a config
        const EGLint config_attribs[] = {
            EGL_SURFACE_TYPE,       EGL_WINDOW_BIT,
            EGL_RENDERABLE_TYPE,    EGL_OPENGL_ES2_BIT,
            EGL_SAMPLES,            0,
            EGL_NONE
        };
        
        egl_config = egl_choose_config_with_pixel_format(egl_display, config_attribs, has_pixel_format ? pixel_format : kARGB8888);
        if (egl_config == EGL_NO_CONFIG_KHR) {
            LOG_ERROR("No fitting EGL framebuffer configuration found.\n");
            ok = EIO;
            goto fail_terminate_display;
        }
    }

    static const EGLint context_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };

    root_context = eglCreateContext(egl_display, egl_config, EGL_NO_CONTEXT, context_attribs);
    if (root_context == EGL_NO_CONTEXT) {
        LOG_ERROR("Could not create root OpenGL ES context. eglCreateContext: 0x%08X\n", eglGetError());
        ok = EIO;
        goto fail_terminate_display;
    }

    flutter_render_context = eglCreateContext(egl_display, egl_config, root_context, context_attribs);
    if (flutter_render_context == EGL_NO_CONTEXT) {
        LOG_ERROR("Could not create OpenGL ES context for flutter rendering. eglCreateContext: 0x%08X\n", eglGetError());
        ok = EIO;
        goto fail_destroy_root_context;
    }

    flutter_resource_uploading_context = eglCreateContext(egl_display, egl_config, root_context, context_attribs);
    if (flutter_resource_uploading_context == EGL_NO_CONTEXT) {
        LOG_ERROR("Could not create OpenGL ES context for flutter resource uploads. eglCreateContext: 0x%08X\n", eglGetError());
        ok = EIO;
        goto fail_destroy_flutter_render_context;
    }

    if (!check_egl_extension(egl_exts_client, egl_exts_dpy, "EGL_KHR_surfaceless_context")) {
        LOG_ERROR("EGL doesn't support the EGL_KHR_surfaceless_context extension, which is required by flutter-pi.\n");
        ok = EIO;
        goto fail_destroy_flutter_resource_uploading_context;
    }

    egl_ok = eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, root_context);
    if (egl_ok != EGL_TRUE) {
        LOG_ERROR("Could not make OpenGL ES root context current to get OpenGL information. eglMakeCurrent: 0x%08X\n", eglGetError());
        ok = EIO;
        goto fail_destroy_flutter_resource_uploading_context;
    }

    gl_renderer = (char*) glGetString(GL_RENDERER);
    gl_exts = (char*) glGetString(GL_EXTENSIONS);
    LOG_DEBUG_UNPREFIXED(
        "OpenGL ES information:\n"
        "  version: \"%s\"\n"
        "  shading language version: \"%s\"\n"
        "  vendor: \"%s\"\n"
        "  renderer: \"%s\"\n"
        "  extensions: \"%s\"\n"
        "===================================\n",
        glGetString(GL_VERSION),
        glGetString(GL_SHADING_LANGUAGE_VERSION),
        glGetString(GL_VENDOR),
        gl_renderer,
        gl_exts
    );

    egl_ok = eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (egl_ok != EGL_TRUE) {
        LOG_ERROR("Could not clear OpenGL ES context. eglMakeCurrent: 0x%08X\n", eglGetError());
        ok = EIO;
        goto fail_clear_current;
    }

    // it seems that after some Raspbian update, regular users are sometimes no longer allowed
    //   to use the direct-rendering infrastructure; i.e. the open the devices inside /dev/dri/
    //   as read-write. flutter-pi must be run as root then.
    // sometimes it works fine without root, sometimes it doesn't.
    if (strstr(gl_renderer, "llvmpipe") != NULL) {
        LOG_ERROR_UNPREFIXED(
            "WARNING: Detected llvmpipe (ie. software rendering) as the OpenGL ES renderer.\n"
            "         Check that flutter-pi has permission to use the 3D graphics hardware,\n"
            "         or try running it as root.\n"
            "         This warning will probably result in a \"failed to set mode\" error\n"
            "         later on in the initialization.\n"
        );
    }

    tracer = tracer_new_with_stubs();
    if (tracer == NULL) {
        LOG_ERROR("Couldn't create event tracer.\n");
        ok = EIO;
        goto fail_destroy_flutter_resource_uploading_context;
    }

    compositor = compositor_new(
        drmdev,
        tracer,
        has_rotation, rotation,
        has_orientation, orientation,
        has_explicit_dimensions, width_mm, height_mm,
        egl_config,
        has_pixel_format, pixel_format,
        false,
        kTripleBufferedVsync_PresentMode
    );
    if (compositor == NULL) {
        LOG_ERROR("Couldn't create compositor.\n");
        ok = EIO;
        goto fail_unref_tracer;
    }

    ok = sd_event_add_io(
        event_loop,
        &compositor_event_source,
        compositor_get_event_fd(compositor),
        EPOLLIN | EPOLLHUP | EPOLLPRI,
        on_compositor_ready,
        compositor_ref(compositor)
    );
    if (ok < 0) {
        ok = -ok;
        LOG_ERROR("Could not add DRM pageflip event listener. sd_event_add_io: %s\n", strerror(ok));
        goto fail_unref_compositor;
    }
    
    flutterpi->tracer = tracer;
    flutterpi->compositor = compositor;
    flutterpi->compositor_event_source = compositor_event_source;
    flutterpi->egl.display = egl_display;
    flutterpi->egl.config = egl_config;
    flutterpi->egl.root_context = root_context;
    flutterpi->egl.flutter_render_context = flutter_render_context;
    flutterpi->egl.flutter_resource_uploading_context = flutter_resource_uploading_context;
    pthread_mutex_init(&flutterpi->egl.root_context_lock, NULL);
    flutterpi->egl.client_exts = egl_exts_client;
    flutterpi->egl.display_exts = egl_exts_dpy;
    flutterpi->gl.extensions = gl_exts;
    return 0;


    fail_unref_compositor:
    compositor_unref(compositor);

    fail_unref_tracer:
    tracer_unref(tracer);
    goto fail_destroy_flutter_resource_uploading_context;

    fail_clear_current:
    eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    fail_destroy_flutter_resource_uploading_context:
    eglDestroyContext(egl_display, flutter_resource_uploading_context);

    fail_destroy_flutter_render_context:
    eglDestroyContext(egl_display, flutter_render_context);

    fail_destroy_root_context:
    eglDestroyContext(egl_display, root_context);

    fail_terminate_display:
    eglTerminate(egl_display);

    fail_unref_drmdev:
    drmdev_unref(drmdev);
    return ok;
}

/**************************
 * FLUTTER INITIALIZATION *
 **************************/
static int init_application(void) {
    FlutterEngineAOTDataSource aot_source;
    enum flutter_runtime_mode runtime_mode;
    struct libflutter_engine *libflutter_engine;
    struct texture_registry *texture_registry;
    struct plugin_registry *plugin_registry;
    FlutterRendererConfig renderer_config = {0};
    struct view_geometry geometry;
    FlutterEngineAOTData aot_data;
    FlutterEngineResult engine_result;
    FlutterProjectArgs project_args = {0};
    void *engine_handle;
    int ok;

    runtime_mode = flutterpi.flutter.runtime_mode;

    engine_handle = NULL;
    if (flutterpi.flutter.paths->flutter_engine_path != NULL) {
        engine_handle = dlopen(flutterpi.flutter.paths->flutter_engine_path, RTLD_LOCAL | RTLD_NOW);
        if (engine_handle == NULL) {
            LOG_DEBUG(
                "Info: Could not load flutter engine from app bundle. dlopen(\"%s\"): %s.\n",
                flutterpi.flutter.paths->flutter_engine_path,
                dlerror()
            );
        }
    }

    if (engine_handle == NULL && flutterpi.flutter.paths->flutter_engine_dlopen_name != NULL) {
        engine_handle = dlopen(flutterpi.flutter.paths->flutter_engine_dlopen_name, RTLD_LOCAL | RTLD_NOW);
        if (engine_handle == NULL) {
            LOG_DEBUG(
                "Info: Could not load flutter engine. dlopen(\"%s\"): %s.\n",
                flutterpi.flutter.paths->flutter_engine_dlopen_name,
                dlerror()
            );
        }
    }

    if (engine_handle == NULL && flutterpi.flutter.paths->flutter_engine_dlopen_name_fallback != NULL) {
        engine_handle = dlopen(flutterpi.flutter.paths->flutter_engine_dlopen_name_fallback, RTLD_LOCAL | RTLD_NOW);
        if (engine_handle == NULL) {
            LOG_DEBUG(
                "Info: Could not load flutter engine. dlopen(\"%s\"): %s.\n",
                flutterpi.flutter.paths->flutter_engine_dlopen_name_fallback,
                dlerror()
            );
        }
    }

    if (engine_handle == NULL) {
        LOG_ERROR("Error: Could not load flutter engine from any location. Make sure you have installed the engine binaries.\n");
        return EINVAL;
    }

    libflutter_engine = &flutterpi.flutter.libflutter_engine;

    /// TODO: Use FlutterEngineGetProcAddresses for this

    FlutterEngineProcTable *procs = &flutterpi.flutter.procs;

    FlutterEngineResult (*get_proc_addresses)(FlutterEngineProcTable* table) = dlsym(libflutter_engine_handle, "FlutterEngineGetProcAddresses");
    if (get_proc_addresses == NULL) {
        LOG_ERROR("Could not resolve flutter engine function FlutterEngineGetProcAddresses.\n");
        return EINVAL;
    }

    procs->struct_size = sizeof(FlutterEngineProcTable);

    engine_result = get_proc_addresses(procs);
    if (engine_result != kSuccess) {
        LOG_ERROR("Could not resolve flutter engine proc addresses. FlutterEngineGetProcAddresses: %s\n", FLUTTER_RESULT_TO_STRING(engine_result));
        return EINVAL;
    }

#	define LOAD_LIBFLUTTER_ENGINE_PROC(name) \
        do { \
            libflutter_engine->name = dlsym(engine_handle, #name); \
            if (!libflutter_engine->name) {\
                perror("[flutter-pi] Could not resolve libflutter_engine procedure " #name ". dlsym"); \
                return EINVAL; \
            } \
        } while (false)

    LOAD_LIBFLUTTER_ENGINE_PROC(FlutterEngineCreateAOTData);
    LOAD_LIBFLUTTER_ENGINE_PROC(FlutterEngineCollectAOTData);
    LOAD_LIBFLUTTER_ENGINE_PROC(FlutterEngineRun);
    LOAD_LIBFLUTTER_ENGINE_PROC(FlutterEngineShutdown);
    LOAD_LIBFLUTTER_ENGINE_PROC(FlutterEngineInitialize);
    LOAD_LIBFLUTTER_ENGINE_PROC(FlutterEngineDeinitialize);
    LOAD_LIBFLUTTER_ENGINE_PROC(FlutterEngineRunInitialized);
    LOAD_LIBFLUTTER_ENGINE_PROC(FlutterEngineSendWindowMetricsEvent);
    LOAD_LIBFLUTTER_ENGINE_PROC(FlutterEngineSendPointerEvent);
    LOAD_LIBFLUTTER_ENGINE_PROC(FlutterEngineSendPlatformMessage);
    LOAD_LIBFLUTTER_ENGINE_PROC(FlutterPlatformMessageCreateResponseHandle);
    LOAD_LIBFLUTTER_ENGINE_PROC(FlutterPlatformMessageReleaseResponseHandle);
    LOAD_LIBFLUTTER_ENGINE_PROC(FlutterEngineSendPlatformMessageResponse);
    LOAD_LIBFLUTTER_ENGINE_PROC(__FlutterEngineFlushPendingTasksNow);
    LOAD_LIBFLUTTER_ENGINE_PROC(FlutterEngineRegisterExternalTexture);
    LOAD_LIBFLUTTER_ENGINE_PROC(FlutterEngineUnregisterExternalTexture);
    LOAD_LIBFLUTTER_ENGINE_PROC(FlutterEngineMarkExternalTextureFrameAvailable);
    LOAD_LIBFLUTTER_ENGINE_PROC(FlutterEngineUpdateSemanticsEnabled);
    LOAD_LIBFLUTTER_ENGINE_PROC(FlutterEngineUpdateAccessibilityFeatures);
    LOAD_LIBFLUTTER_ENGINE_PROC(FlutterEngineDispatchSemanticsAction);
    LOAD_LIBFLUTTER_ENGINE_PROC(FlutterEngineOnVsync);
    LOAD_LIBFLUTTER_ENGINE_PROC(FlutterEngineReloadSystemFonts);
    LOAD_LIBFLUTTER_ENGINE_PROC(FlutterEngineTraceEventDurationBegin);
    LOAD_LIBFLUTTER_ENGINE_PROC(FlutterEngineTraceEventDurationEnd);
    LOAD_LIBFLUTTER_ENGINE_PROC(FlutterEngineTraceEventInstant);
    LOAD_LIBFLUTTER_ENGINE_PROC(FlutterEnginePostRenderThreadTask);
    LOAD_LIBFLUTTER_ENGINE_PROC(FlutterEngineGetCurrentTime);
    LOAD_LIBFLUTTER_ENGINE_PROC(FlutterEngineRunTask);
    LOAD_LIBFLUTTER_ENGINE_PROC(FlutterEngineUpdateLocales);
    LOAD_LIBFLUTTER_ENGINE_PROC(FlutterEngineRunsAOTCompiledDartCode);
    LOAD_LIBFLUTTER_ENGINE_PROC(FlutterEnginePostDartObject);
    LOAD_LIBFLUTTER_ENGINE_PROC(FlutterEngineNotifyLowMemoryWarning);
    LOAD_LIBFLUTTER_ENGINE_PROC(FlutterEnginePostCallbackOnAllNativeThreads);
    LOAD_LIBFLUTTER_ENGINE_PROC(FlutterEngineNotifyDisplayUpdate);

#	undef LOAD_LIBFLUTTER_ENGINE_PROC

    tracer_set_cbs(
        flutterpi.tracer,
        procs->TraceEventDurationBegin,
        procs->TraceEventDurationEnd,
        procs->TraceEventInstant
    );

    plugin_registry = plugin_registry_new(&flutterpi);
    if (plugin_registry == NULL) {
        LOG_ERROR("Could not create plugin registry.\n");
        return EIO;
    }

    flutterpi.plugin_registry = plugin_registry;

    ok = plugin_registry_add_plugins_from_static_registry(plugin_registry);
    if (ok != 0) {
        LOG_ERROR("Could not register plugins to plugin registry.\n");
        return EIO;
    }

    ok = plugin_registry_ensure_plugins_initialized(plugin_registry);
    if (ok != 0) {
        LOG_ERROR("Could not initialize plugins.\n");
        return EIO;
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
            .gl_external_texture_frame_callback = on_gl_external_texture_frame_callback,
        }
    };

    COMPILE_ASSERT(sizeof(FlutterProjectArgs) == 144);

    // configure the project
    project_args = (FlutterProjectArgs) {
        .struct_size = sizeof(FlutterProjectArgs),
        .assets_path = flutterpi.flutter.paths->asset_bundle_path,
        .icu_data_path = flutterpi.flutter.paths->icudtl_path,
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
        .vsync_callback = on_frame_request, /* broken since 2.2 */
        .custom_dart_entrypoint = NULL,
        .custom_task_runners = &(FlutterCustomTaskRunners) {
            .struct_size = sizeof(FlutterCustomTaskRunners),
            .platform_task_runner = &(FlutterTaskRunnerDescription) {
                .struct_size = sizeof(FlutterTaskRunnerDescription),
                .user_data = &flutterpi,
                .runs_task_on_current_thread_callback = runs_platform_tasks_on_current_thread,
                .post_task_callback = on_post_flutter_task
            },
            .render_task_runner = NULL,
            .thread_priority_setter = NULL
        },
        .shutdown_dart_vm_when_done = true,
        .compositor = compositor_get_flutter_compositor(flutterpi.compositor),
        .dart_old_gen_heap_size = -1,
        .compute_platform_resolved_locale_callback = on_compute_platform_resolved_locales,
        .dart_entrypoint_argc = 0,
        .dart_entrypoint_argv = NULL,
        .log_message_callback = NULL,
        .log_tag = NULL,
        .on_pre_engine_restart_callback = NULL
    };

    bool engine_is_aot = libflutter_engine->FlutterEngineRunsAOTCompiledDartCode();
    if (engine_is_aot == true && runtime_mode == kDebug) {
        LOG_ERROR(
            "The flutter engine was built for release or profile (AOT) mode, but flutter-pi was not started up in release or profile mode.\n"
            "Either you swap out the libflutter_engine.so with one that was built for debug mode, or you start"
            "flutter-pi with the --release or --profile flag and make sure a valid \"app.so\" is located inside the asset bundle directory.\n"
        );
        return EINVAL;
    } else if (engine_is_aot == false && runtime_mode != kDebug) {
        LOG_ERROR(
            "The flutter engine was built for debug mode, but flutter-pi was started up in release mode.\n"
            "Either you swap out the libflutter_engine.so with one that was built for release mode,"
            "or you start flutter-pi without the --release flag.\n"
        );
        return EINVAL;
    }

    if (flutterpi.flutter.runtime_mode != kDebug) {
        aot_source = (FlutterEngineAOTDataSource) {
            .elf_path = flutterpi.flutter.paths->app_elf_path,
            .type = kFlutterEngineAOTDataSourceTypeElfPath
        };

        engine_result = libflutter_engine->FlutterEngineCreateAOTData(&aot_source, &aot_data);
        if (engine_result != kSuccess) {
            LOG_ERROR("Could not load AOT data. FlutterEngineCreateAOTData: %s\n", FLUTTER_RESULT_TO_STRING(engine_result));
            return EIO;
        }

        project_args.aot_data = aot_data;
    }

    flutterpi.flutter.next_frame_request_is_secondary = false;

    // spin up the engine
    engine_result = libflutter_engine->FlutterEngineInitialize(FLUTTER_ENGINE_VERSION, &renderer_config, &project_args, &flutterpi, &flutterpi.flutter.engine);
    if (engine_result != kSuccess) {
        LOG_ERROR("Could not initialize the flutter engine. FlutterEngineInitialize: %s\n", FLUTTER_RESULT_TO_STRING(engine_result));
        return EINVAL;
    }
    
    engine_result = libflutter_engine->FlutterEngineRunInitialized(flutterpi.flutter.engine);
    if (engine_result != kSuccess) {
        LOG_ERROR("Could not run the flutter engine. FlutterEngineRunInitialized: %s\n", FLUTTER_RESULT_TO_STRING(engine_result));
        return EINVAL;
    }

    ok = locales_add_to_fl_engine(flutterpi.locales, flutterpi.flutter.engine, libflutter_engine->FlutterEngineUpdateLocales);
    if (ok != 0) {
        return ok;
    }

    texture_registry = texture_registry_new(
        &(const struct flutter_external_texture_interface) {
            .register_external_texture = libflutter_engine->FlutterEngineRegisterExternalTexture,
            .unregister_external_texture = libflutter_engine->FlutterEngineUnregisterExternalTexture,
            .mark_external_texture_frame_available = libflutter_engine->FlutterEngineMarkExternalTextureFrameAvailable,
            .engine = flutterpi.flutter.engine
        }
    );
    flutterpi.texture_registry = texture_registry;

    engine_result = libflutter_engine->FlutterEngineNotifyDisplayUpdate(
        flutterpi.flutter.engine,
        kFlutterEngineDisplaysUpdateTypeStartup,
        &(FlutterEngineDisplay) {
            .struct_size = sizeof(FlutterEngineDisplay),
            .display_id = 0,
            .single_display = true,
            .refresh_rate = compositor_get_refresh_rate(flutterpi.compositor)
        },
        1
    );
    if (engine_result != kSuccess) {
        LOG_ERROR("Could not send display update to flutter engine. FlutterEngineNotifyDisplayUpdate: %s\n", FLUTTER_RESULT_TO_STRING(engine_result));
        return EINVAL;
    }

    compositor_get_view_geometry(flutterpi.compositor, &geometry);

    // just so we get an error if the window metrics event was expanded without us noticing
    COMPILE_ASSERT(sizeof(FlutterWindowMetricsEvent) == 64);

    // update window size
    engine_result = libflutter_engine->FlutterEngineSendWindowMetricsEvent(
        flutterpi.flutter.engine,
        &(FlutterWindowMetricsEvent) {
            .struct_size = sizeof(FlutterWindowMetricsEvent),
            .width = geometry.view_size.x,
            .height = geometry.view_size.y,
            .pixel_ratio = geometry.device_pixel_ratio,
            .left = 0,
            .top = 0,
            .physical_view_inset_top = 0,
            .physical_view_inset_right = 0,
            .physical_view_inset_bottom = 0,
            .physical_view_inset_left = 0
        }
    );
    if (engine_result != kSuccess) {
        LOG_ERROR("Could not send window metrics to flutter engine. FlutterEngineSendWindowMetricsEvent: %s\n", FLUTTER_RESULT_TO_STRING(engine_result));
        return EINVAL;
    }

    return 0;
}

int flutterpi_schedule_exit(void) {
    int ok;

    if (pthread_self() != flutterpi.event_loop_thread) {
        pthread_mutex_lock(&flutterpi.event_loop_mutex);
    }
    
    ok = sd_event_exit(flutterpi.event_loop, 0);
    if (ok < 0) {
        LOG_ERROR("Could not schedule application exit. sd_event_exit: %s\n", strerror(-ok));
        if (pthread_self() != flutterpi.event_loop_thread) {
            pthread_mutex_unlock(&flutterpi.event_loop_mutex);
        }
        return -ok;
    }

    if (pthread_self() != flutterpi.event_loop_thread) {
        pthread_mutex_unlock(&flutterpi.event_loop_mutex);
    }

    return 0;
}

/**************
 * USER INPUT *
 **************/
static void on_flutter_pointer_event(void *userdata, const FlutterPointerEvent *events, size_t n_events) {
    FlutterEngineResult engine_result;
    struct flutterpi *flutterpi;

    DEBUG_ASSERT_NOT_NULL(userdata);
    flutterpi = userdata;

    /// TODO: make this atomic
    flutterpi->flutter.next_frame_request_is_secondary = true;
    engine_result = flutterpi->flutter.libflutter_engine.FlutterEngineSendPointerEvent(
        flutterpi->flutter.engine,
        events,
        n_events
    );

    if (engine_result != kSuccess) {
        LOG_ERROR("Error sending touchscreen / mouse events to flutter. FlutterEngineSendPointerEvent: %s\n", FLUTTER_RESULT_TO_STRING(engine_result));
        //flutterpi_schedule_exit(flutterpi);
    }
}

static void on_utf8_character(void *userdata, uint8_t *character) {
    struct flutterpi *flutterpi;
    int ok;

    flutterpi = userdata;

    (void) flutterpi;

#ifdef BUILD_TEXT_INPUT_PLUGIN
    ok = textin_on_utf8_char(character);
    if (ok != 0) {
        LOG_ERROR("Error handling keyboard event. textin_on_utf8_char: %s\n", strerror(ok));
        //flutterpi_schedule_exit(flutterpi);
    }
#endif
}

static void on_xkb_keysym(void *userdata, xkb_keysym_t keysym) {
    struct flutterpi *flutterpi;
    int ok;

    flutterpi = userdata;
    (void) flutterpi;

#ifdef BUILD_TEXT_INPUT_PLUGIN
    ok = textin_on_xkb_keysym(keysym);
    if (ok != 0) {
        LOG_ERROR("Error handling keyboard event. textin_on_xkb_keysym: %s\n", strerror(ok));
        //flutterpi_schedule_exit(flutterpi);
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
    struct flutterpi *flutterpi;
    int ok;

    flutterpi = userdata;
    (void) flutterpi;

#ifdef BUILD_RAW_KEYBOARD_PLUGIN
    ok = rawkb_send_gtk_keyevent(
        unicode_scalar_values,
        key_code,
        scan_code,
        modifiers,
        is_down
    );
    if (ok != 0) {
        LOG_ERROR("Error handling keyboard event. rawkb_send_gtk_keyevent: %s\n", strerror(ok));
        //flutterpi_schedule_exit(flutterpi);
    }
#endif
}

static void on_set_cursor_enabled(void *userdata, bool enabled) {
    struct flutterpi *flutterpi;
    int ok;

    flutterpi = userdata;
    (void) flutterpi;
    (void) ok;
    (void) enabled;

    /// TODO: Implement
    /*
    ok = compositor_apply_cursor_state(
        enabled,
        flutterpi->view.rotation,
        flutterpi->display.pixel_ratio
    );
    if (ok != 0) {
        LOG_ERROR("Error enabling / disabling mouse cursor. compositor_apply_cursor_state: %s\n", strerror(ok));
    }
    */
}

static void on_move_cursor(void *userdata, unsigned int x, unsigned int y) {
    struct flutterpi *flutterpi;
    int ok;

    flutterpi = userdata;
    (void) ok;
    (void) flutterpi;
    (void) x;
    (void) y;

    /// TODO: Implement

    /*
    ok = compositor_set_cursor_pos(x, y);
    if (ok != 0) {
        LOG_ERROR("Error moving mouse cursor. compositor_set_cursor_pos: %s\n", strerror(ok));
    }
    */
}

static const struct user_input_interface user_input_interface = {
    .on_flutter_pointer_event = on_flutter_pointer_event,
    .on_utf8_character = on_utf8_character,
    .on_xkb_keysym = on_xkb_keysym,
    .on_gtk_keyevent = on_gtk_keyevent,
    .on_set_cursor_enabled = on_set_cursor_enabled,
    .on_move_cursor = on_move_cursor
};

static int on_user_input_fd_ready(sd_event_source *s, int fd, uint32_t revents, void *userdata) {
    struct user_input *input;
    
    (void) s;
    (void) fd;
    (void) revents;

    input = userdata;

    return user_input_on_fd_ready(input);
}

static int init_user_input(struct flutterpi *flutterpi, struct compositor *compositor, struct sd_event *event_loop) {
    struct view_geometry geometry;
    struct user_input *input;
    sd_event_source *event_source;
    int ok;

    compositor_get_view_geometry(compositor, &geometry);
    
    event_source = NULL;
    
    input = user_input_new(
        &user_input_interface,
        flutterpi,
        &geometry.display_to_view_transform,
        &geometry.view_to_display_transform,
        geometry.display_size.x,
        geometry.display_size.y
    );
    if (input == NULL) {
        LOG_ERROR("Couldn't initialize user input. flutter-pi will run without user input.\n");
    } else {
        ok = sd_event_add_io(
            event_loop,
            &event_source,
            user_input_get_fd(input),
            EPOLLIN | EPOLLRDHUP | EPOLLPRI,
            on_user_input_fd_ready,
            input
        );
        if (ok < 0) {
            LOG_ERROR("Couldn't listen for user input. flutter-pi will run without user input. sd_event_add_io: %s\n", strerror(-ok));
            user_input_destroy(input);
            input = NULL;
        }
    }
    
    flutterpi->user_input = input;
    flutterpi->user_input_event_source = event_source;
    return 0;
}

static bool path_exists(const char *path) {
    return access(path, R_OK) == 0;
}

struct cmd_args {
    bool has_orientation;
    enum device_orientation orientation;

    bool has_rotation;
    int rotation;

    bool has_physical_dimensions;
    int width_mm, height_mm;

    bool has_pixel_format;
    enum pixfmt pixel_format;

    bool has_runtime_mode;
    enum flutter_runtime_mode runtime_mode;

    const char *bundle_path;

    int engine_argc;
    char **engine_argv;
};

static struct flutter_paths *setup_paths(enum flutter_runtime_mode runtime_mode, const char *app_bundle_path) {
#if defined(FILESYSTEM_LAYOUT_DEFAULT)
    return fs_layout_flutterpi_resolve(app_bundle_path, runtime_mode);
#elif defined(FILESYSTEM_LAYOUT_METAFLUTTER)
    return fs_layout_metaflutter_resolve(app_bundle_path, runtime_mode);
#else
    #error "Exactly one of FILESYSTEM_LAYOUT_DEFAULT or FILESYSTEM_LAYOUT_METAFLUTTER must be defined."
    return NULL;
#endif
}

static bool parse_cmd_args(int argc, char **argv, struct cmd_args *result_out) {
    bool finished_parsing_options;
    int runtime_mode_int = kDebug;
    int longopt_index = 0;
    int opt, ok;

    struct option long_options[] = {
        {"release", no_argument, &runtime_mode_int, kRelease},
        {"profile", no_argument, &runtime_mode_int, kProfile},
        {"input", required_argument, NULL, 'i'},
        {"orientation", required_argument, NULL, 'o'},
        {"rotation", required_argument, NULL, 'r'},
        {"dimensions", required_argument, NULL, 'd'},
        {"help", no_argument, 0, 'h'},
        {"pixelformat", required_argument, NULL, 'p'},
        {0, 0, 0, 0}
    };

    memset(result_out, 0, sizeof *result_out);

    result_out->has_orientation = false;
    result_out->has_rotation = false;
    result_out->has_physical_dimensions = false;
    result_out->has_pixel_format = false;
    result_out->has_runtime_mode = false;
    result_out->bundle_path = NULL;
    result_out->engine_argc = 0;
    result_out->engine_argv = NULL;

    finished_parsing_options = false;
    while (!finished_parsing_options) {
        longopt_index = 0;
        opt = getopt_long(argc, argv, "+i:o:r:d:h", long_options, &longopt_index);

        switch (opt) {
            case 0:
                // flag was encountered. just continue
                break;

            case 'o':
                if (STREQ(optarg, "portrait_up")) {
                    result_out->orientation = kPortraitUp;
                    result_out->has_orientation = true;
                } else if (STREQ(optarg, "landscape_left")) {
                    result_out->orientation = kLandscapeLeft;
                    result_out->has_orientation = true;
                } else if (STREQ(optarg, "portrait_down")) {
                    result_out->orientation = kPortraitDown;
                    result_out->has_orientation = true;
                } else if (STREQ(optarg, "landscape_right")) {
                    result_out->orientation = kLandscapeRight;
                    result_out->has_orientation = true;
                } else {
                    LOG_ERROR(
                        "ERROR: Invalid argument for --orientation passed.\n"
                        "Valid values are \"portrait_up\", \"landscape_left\", \"portrait_down\", \"landscape_right\".\n"
                        "%s", 
                        usage
                    );
                    return false;
                }
                break;
            
            case 'r':
                errno = 0;
                long rotation = strtol(optarg, NULL, 0);
                if ((errno != 0) || ((rotation != 0) && (rotation != 90) && (rotation != 180) && (rotation != 270))) {
                    LOG_ERROR(
                        "ERROR: Invalid argument for --rotation passed.\n"
                        "Valid values are 0, 90, 180, 270.\n"
                        "%s",
                        usage
                    );
                    return false;
                }

                result_out->rotation = rotation;
                result_out->has_rotation = true;
                break;
            
            case 'd': ;
                unsigned int width_mm, height_mm;

                ok = sscanf(optarg, "%u,%u", &width_mm, &height_mm);
                if (ok != 2) {
                    LOG_ERROR("ERROR: Invalid argument for --dimensions passed.\n%s", usage);
                    return false;
                }

                result_out->width_mm = width_mm;
                result_out->height_mm = height_mm;
                
                break;
            
            case 'p':
                for (unsigned i = 0; i < n_pixfmt_infos; i++) {
                    if (strcmp(optarg, pixfmt_infos[i].arg_name) == 0) {
                        result_out->has_pixel_format = true;
                        result_out->pixel_format = pixfmt_infos[i].format;
                        goto valid_format;
                    }
                }

                LOG_ERROR(
                    "ERROR: Invalid argument for --pixelformat passed.\n"
                    "Valid values are: " PIXFMT_LIST(PIXFMT_ARG_NAME) "\n"
                    "%s", 
                    usage
                );
                return false;

                valid_format:
                break;
            
            case 'h':
                printf("%s", usage);
                return false;

            case '?':
            case ':':
                LOG_ERROR("Invalid option specified.\n%s", usage);
                return false;
            
            case -1:
                finished_parsing_options = true;
                break;
            
            default:
                break;
        }
    }
    

    if (optind >= argc) {
        LOG_ERROR("ERROR: Expected asset bundle path after options.\n");
        printf("%s", usage);
        return false;
    }

    result_out->bundle_path = realpath(argv[optind], NULL);
    result_out->runtime_mode = runtime_mode_int;

    argv[optind] = argv[0];
    result_out->engine_argc = argc - optind;
    result_out->engine_argv = argv + optind;

    return true;
}

int init(int argc, char **argv) {
    struct flutter_paths *paths;
    struct cmd_args cmd_args;
    int ok;

    ok = parse_cmd_args(argc, argv, &cmd_args);
    if (ok == false) {
        return EINVAL;
    }

    flutterpi.flutter.runtime_mode = cmd_args.has_runtime_mode ? cmd_args.runtime_mode : kDebug;
    flutterpi.flutter.bundle_path = cmd_args.bundle_path;
    flutterpi.flutter.engine_argc = cmd_args.engine_argc;
    flutterpi.flutter.engine_argv = cmd_args.engine_argv;

    paths = setup_paths(flutterpi.flutter.runtime_mode, flutterpi.flutter.bundle_path);
    if (paths == NULL) {
        return EINVAL;
    }

    flutterpi.flutter.paths = paths;

    ok = init_main_loop();
    if (ok != 0) {
        return ok;
    }

    flutterpi.locales = locales_new();
    if (flutterpi.locales == NULL) {
        LOG_ERROR("Couldn't setup locales.\n");
        return EINVAL;
    }

    ok = init_display(
        &flutterpi,
        flutterpi.event_loop,
        cmd_args.has_pixel_format, cmd_args.pixel_format,
        cmd_args.has_rotation,
            cmd_args.rotation == 0 ? PLANE_TRANSFORM_ROTATE_0 :
            cmd_args.rotation == 90 ? PLANE_TRANSFORM_ROTATE_90 :
            cmd_args.rotation == 180 ? PLANE_TRANSFORM_ROTATE_180 :
            cmd_args.rotation == 270 ? PLANE_TRANSFORM_ROTATE_270 :
            (assert(0 && "invalid rotation"), PLANE_TRANSFORM_ROTATE_0),
        cmd_args.has_orientation, cmd_args.orientation,
        cmd_args.has_physical_dimensions, cmd_args.width_mm, cmd_args.height_mm
    );
    if (ok != 0) {
        return ok;
    }

    ok = init_user_input(
        &flutterpi,
        flutterpi.compositor,
        flutterpi.event_loop
    );
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

#ifdef ENABLE_MTRACE
    mtrace();
#endif

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
