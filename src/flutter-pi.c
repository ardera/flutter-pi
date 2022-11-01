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
#include <egl.h>
#include <gles.h>
#include <libinput.h>
#include <libudev.h>
#include <systemd/sd-event.h>
#include <flutter_embedder.h>

#include <flutter-pi.h>
#include <pixel_format.h>
#include <gl_renderer.h>
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
#include <vk_renderer.h>
#include <frame_scheduler.h>
#include <window.h>

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
  --vulkan                   Use vulkan for rendering.\n\
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
    int ok;
    
    DEBUG_ASSERT_NOT_NULL(userdata);
    flutterpi = userdata;
    DEBUG_ASSERT_NOT_NULL(flutterpi->gl_renderer);

    surface = compositor_get_egl_surface(flutterpi->compositor);
    if (surface == EGL_NO_SURFACE) {
        /// TODO: Get a fake EGL Surface just for initialization.
        LOG_ERROR("Couldn't get an EGL surface from the compositor.\n");
        return false;
    }

    ok = gl_renderer_make_flutter_rendering_context_current(flutterpi->gl_renderer, surface);
    if (ok != 0) {
        return false;
    }

    return true;
}

/// Called on some flutter internal thread to
/// clear the EGLContext.
static bool on_clear_current(void* userdata) {
    struct flutterpi *flutterpi;
    int ok;

    DEBUG_ASSERT_NOT_NULL(userdata);
    flutterpi = userdata;
    DEBUG_ASSERT_NOT_NULL(flutterpi->gl_renderer);

    ok = gl_renderer_clear_current(flutterpi->gl_renderer);
    if (ok != 0) {
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
    UNREACHABLE();
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
    int ok;

    DEBUG_ASSERT_NOT_NULL(userdata);
    flutterpi = userdata;
    DEBUG_ASSERT_NOT_NULL(flutterpi->gl_renderer);
    
    ok = gl_renderer_make_flutter_resource_uploading_context_current(flutterpi->gl_renderer);
    if (ok != 0) {
        return false;
    }
    
    return true;
}

/// Called by flutter 
static void *proc_resolver(
    void* userdata,
    const char* name
) {
    struct flutterpi *flutterpi;

    DEBUG_ASSERT_NOT_NULL(userdata);
    flutterpi = userdata;
    DEBUG_ASSERT_NOT_NULL(flutterpi->gl_renderer);

    return gl_renderer_get_proc_address(flutterpi->gl_renderer, name);
}


MAYBE_UNUSED static void *on_get_vulkan_proc_address(
    void* userdata,
    FlutterVulkanInstanceHandle instance,
    const char* name
) {
    DEBUG_ASSERT_NOT_NULL(userdata);
    DEBUG_ASSERT_NOT_NULL(name);

#ifdef HAS_VULKAN
    return (void*) vkGetInstanceProcAddr((VkInstance) instance, name);
#else
    (void) userdata;
    (void) instance;
    (void) name;
    UNREACHABLE();
#endif
}

MAYBE_UNUSED static FlutterVulkanImage on_get_next_vulkan_image(
    void *userdata,
    const FlutterFrameInfo *frameinfo
) {
    struct flutterpi *flutterpi;

    DEBUG_ASSERT_NOT_NULL(userdata);
    DEBUG_ASSERT_NOT_NULL(frameinfo);
    flutterpi = userdata;
    
    (void) flutterpi;
    (void) frameinfo;

    UNIMPLEMENTED();
}

MAYBE_UNUSED static bool on_present_vulkan_image(
    void *userdata,
    const FlutterVulkanImage *image
) {
    struct flutterpi *flutterpi;

    DEBUG_ASSERT_NOT_NULL(userdata);
    DEBUG_ASSERT_NOT_NULL(image);
    flutterpi = userdata;
    
    (void) flutterpi;
    (void) image;

    UNREACHABLE();
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

    return MAT3F_AS_FLUTTER_TRANSFORM(geometry.view_to_display_transform);
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

    result = flutterpi.flutter.procs.RunTask(flutterpi.flutter.engine, task);
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
        result = flutterpi.flutter.procs.SendPlatformMessageResponse(flutterpi.flutter.engine, msg->target_handle, msg->message, msg->message_size);
    } else {
        result = flutterpi.flutter.procs.SendPlatformMessage(
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
    struct flutterpi *flutterpi,
    const char *channel,
    const uint8_t *restrict message,
    size_t message_size,
    FlutterPlatformMessageResponseHandle *responsehandle
) {
    struct platform_message *msg;
    FlutterEngineResult result;
    int ok;
    
    if (runs_platform_tasks_on_current_thread(flutterpi)) {
        result = flutterpi->flutter.procs.SendPlatformMessage(
            flutterpi->flutter.engine,
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
        result = flutterpi.flutter.procs.SendPlatformMessageResponse(
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

bool flutterpi_has_gl_renderer(struct flutterpi *flutterpi) {
    DEBUG_ASSERT_NOT_NULL(flutterpi);
    return flutterpi->gl_renderer != NULL;
}

struct gl_renderer *flutterpi_get_gl_renderer(struct flutterpi *flutterpi) {
    DEBUG_ASSERT_NOT_NULL(flutterpi);
    return flutterpi->gl_renderer;
}

void flutterpi_trace_event_instant(struct flutterpi *flutterpi, const char *name) {
    flutterpi->flutter.procs.TraceEventInstant(name);
}

void flutterpi_trace_event_begin(struct flutterpi *flutterpi, const char *name) {
    flutterpi->flutter.procs.TraceEventDurationBegin(name);
}

void flutterpi_trace_event_end(struct flutterpi *flutterpi, const char *name) {
    flutterpi->flutter.procs.TraceEventDurationEnd(name);
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
static int on_drmdev_ready(sd_event_source *s, int fd, uint32_t revents, void *userdata) {
    struct drmdev *drmdev;

    (void) s;
    (void) fd;
    (void) revents;
    (void) userdata;

    DEBUG_ASSERT_NOT_NULL(userdata);
    drmdev = userdata;

    return drmdev_on_event_fd_ready(drmdev);
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

// clang-format off
static int init_display(
    struct flutterpi *flutterpi,
    sd_event *event_loop,
    enum renderer_type renderer_type,
    bool has_pixel_format, enum pixfmt pixel_format,
    bool has_rotation, drm_plane_transform_t rotation,
    bool has_orientation, enum device_orientation orientation,
    bool has_explicit_dimensions, int width_mm, int height_mm
) {
    // clang-format on
    struct frame_scheduler *scheduler;
    struct gl_renderer *gl_renderer;
    struct vk_renderer *vk_renderer;
    struct compositor *compositor;
    struct gbm_device *gbm_device;
    sd_event_source *compositor_event_source;
    struct window *window;
    struct drmdev *drmdev;
    struct tracer *tracer;
    drmDevicePtr devices[64];
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
    if (gbm_device == NULL) {
        LOG_ERROR("Couldn't create GBM device.\n");
        goto fail_unref_drmdev;
    }

    tracer = tracer_new_with_stubs();
    if (tracer == NULL) {
        LOG_ERROR("Couldn't create event tracer.\n");
        ok = EIO;
        goto fail_unref_drmdev;
    }

    scheduler = frame_scheduler_new(false, kDoubleBufferedVsync_PresentMode, NULL, NULL);
    if (scheduler == NULL) {
        LOG_ERROR("Couldn't create vsync scheduler.\n");
        ok = EIO;
        goto fail_unref_tracer;
    }


    if (renderer_type == kVulkan_RendererType) {
#ifdef HAS_VULKAN
        gl_renderer = NULL;
        vk_renderer = vk_renderer_new();
        if (vk_renderer == NULL) {
            LOG_ERROR("Couldn't create vulkan renderer.\n");
            ok = EIO;
            goto fail_unref_waiter;
        }
#else
        UNREACHABLE();
#endif
    } if (renderer_type == kOpenGL_RendererType) {
        vk_renderer = NULL;
        gl_renderer = gl_renderer_new_from_gbm_device(tracer, gbm_device, has_pixel_format, pixel_format);
        if (gl_renderer == NULL) {
            LOG_ERROR("Couldn't create EGL/OpenGL renderer.\n");
            ok = EIO;
            goto fail_unref_waiter;
        }

        // it seems that after some Raspbian update, regular users are sometimes no longer allowed
        //   to use the direct-rendering infrastructure; i.e. the open the devices inside /dev/dri/
        //   as read-write. flutter-pi must be run as root then.
        // sometimes it works fine without root, sometimes it doesn't.
        if (gl_renderer_is_llvmpipe(gl_renderer)) {
            LOG_ERROR_UNPREFIXED(
                "WARNING: Detected llvmpipe (ie. software rendering) as the OpenGL ES renderer.\n"
                "         Check that flutter-pi has permission to use the 3D graphics hardware,\n"
                "         or try running it as root.\n"
                "         This warning will probably result in a \"failed to set mode\" error\n"
                "         later on in the initialization.\n"
            );
            ok = EINVAL;
            goto fail_unref_renderer;
        }
    } else {
        UNREACHABLE();
    }

    window = kms_window_new(
        // clang-format off
        tracer,
        scheduler,
        renderer_type,
        gl_renderer,
        vk_renderer,
        has_rotation, rotation,
        has_orientation, orientation,
        has_explicit_dimensions, width_mm, height_mm,
        has_pixel_format, pixel_format,
        drmdev
        // clang-format on
    );
    if (window == NULL) {
        LOG_ERROR("Couldn't create KMS window.\n");
        goto fail_unref_renderer;
    }

    compositor = compositor_new(tracer, window);
    if (compositor == NULL) {
        LOG_ERROR("Couldn't create compositor.\n");
        goto fail_unref_window;
    }

    ok = sd_event_add_io(
        event_loop,
        &compositor_event_source,
        drmdev_get_event_fd(drmdev),
        EPOLLIN | EPOLLHUP | EPOLLPRI,
        on_drmdev_ready,
        drmdev_ref(drmdev)
    );
    if (ok < 0) {
        ok = -ok;
        LOG_ERROR("Could not add DRM pageflip event listener. sd_event_add_io: %s\n", strerror(ok));
        goto fail_unref_compositor;
    }
    
    flutterpi->tracer = tracer;
    flutterpi->compositor = compositor;
    flutterpi->compositor_event_source = compositor_event_source;
    flutterpi->gl_renderer = gl_renderer;
    flutterpi->vk_renderer = vk_renderer;
    return 0;

    fail_unref_compositor:
    compositor_unref(compositor);

    fail_unref_window:
    window_unref(window);

    fail_unref_renderer:
    if (gl_renderer) {
        gl_renderer_unref(gl_renderer);
    }
#ifdef HAS_VULKAN
    if (vk_renderer) {
        vk_renderer_unref(vk_renderer);
    }
#endif

    fail_unref_waiter:
    frame_scheduler_unref(scheduler);

    fail_unref_tracer:
    tracer_unref(tracer);

    fail_unref_drmdev:
    drmdev_unref(drmdev);
    return ok;
}

/**************************
 * FLUTTER INITIALIZATION *
 **************************/
static int init_application(void) {
    FlutterEngineResult (*get_proc_addresses)(FlutterEngineProcTable* table);
    FlutterEngineAOTDataSource aot_source;
    enum flutter_runtime_mode runtime_mode;
    struct texture_registry *texture_registry;
    struct plugin_registry *plugin_registry;
    FlutterEngineProcTable *procs;
    FlutterRendererConfig renderer_config = {0};
    struct view_geometry geometry;
    FlutterEngineAOTData aot_data;
    FlutterEngineResult engine_result;
    FlutterProjectArgs project_args = {0};
    void *engine_handle;
    int ok;

    asprintf(&libflutter_engine_path, "%s/libflutter_engine.so", flutterpi.flutter.asset_bundle_path);

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

    procs = &flutterpi.flutter.procs;

    get_proc_addresses = dlsym(engine_handle, "FlutterEngineGetProcAddresses");
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
    if (flutterpi.vk_renderer) {
#ifdef HAS_VULKAN
        renderer_config = (FlutterRendererConfig) {
            .type = kVulkan,
            .vulkan = {
                .struct_size = sizeof(FlutterVulkanRendererConfig),
                .version = vk_renderer_get_vk_version(flutterpi.vk_renderer),
                .instance = vk_renderer_get_instance(flutterpi.vk_renderer),
                .physical_device = vk_renderer_get_physical_device(flutterpi.vk_renderer),
                .device = vk_renderer_get_device(flutterpi.vk_renderer),
                .queue_family_index = vk_renderer_get_queue_family_index(flutterpi.vk_renderer),
                .queue = vk_renderer_get_queue(flutterpi.vk_renderer),
                .enabled_instance_extension_count = vk_renderer_get_enabled_instance_extension_count(flutterpi.vk_renderer),
                .enabled_instance_extensions = vk_renderer_get_enabled_instance_extensions(flutterpi.vk_renderer),
                .enabled_device_extension_count = vk_renderer_get_enabled_device_extension_count(flutterpi.vk_renderer),
                .enabled_device_extensions = vk_renderer_get_enabled_device_extensions(flutterpi.vk_renderer),
                .get_instance_proc_address_callback = on_get_vulkan_proc_address,
                .get_next_image_callback = on_get_next_vulkan_image,
                .present_image_callback = on_present_vulkan_image,
            },
        };
#else
        UNREACHABLE();
#endif
    } else {
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
    }

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
        .persistent_cache_path = flutterpi.flutter.paths->asset_bundle_path,
        .is_persistent_cache_read_only = false,
        .vsync_callback = NULL, // on_frame_request, /* broken since 2.2, kinda */
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

    bool engine_is_aot = procs->RunsAOTCompiledDartCode();
    if ((engine_is_aot == true) && (flutterpi.flutter.runtime_mode == kDebug)) {
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

        engine_result = procs->CreateAOTData(&aot_source, &aot_data);
        if (engine_result != kSuccess) {
            LOG_ERROR("Could not load AOT data. FlutterEngineCreateAOTData: %s\n", FLUTTER_RESULT_TO_STRING(engine_result));
            return EIO;
        }

        project_args.aot_data = aot_data;
    }

    flutterpi.flutter.next_frame_request_is_secondary = false;

    LOG_DEBUG("initializing flutter engine...\n");
    // spin up the engine
    engine_result = procs->Initialize(FLUTTER_ENGINE_VERSION, &renderer_config, &project_args, &flutterpi, &flutterpi.flutter.engine);
    if (engine_result != kSuccess) {
        LOG_ERROR("Could not initialize the flutter engine. FlutterEngineInitialize: %s\n", FLUTTER_RESULT_TO_STRING(engine_result));
        return EINVAL;
    }

    LOG_DEBUG("running flutter engine...\n");
    engine_result = procs->RunInitialized(flutterpi.flutter.engine);
    if (engine_result != kSuccess) {
        LOG_ERROR("Could not run the flutter engine. FlutterEngineRunInitialized: %s\n", FLUTTER_RESULT_TO_STRING(engine_result));
        return EINVAL;
    }

    ok = locales_add_to_fl_engine(flutterpi.locales, flutterpi.flutter.engine, procs->UpdateLocales);
    if (ok != 0) {
        return ok;
    }

    texture_registry = texture_registry_new(
        &(const struct flutter_external_texture_interface) {
            .register_external_texture = procs->RegisterExternalTexture,
            .unregister_external_texture = procs->UnregisterExternalTexture,
            .mark_external_texture_frame_available = procs->MarkExternalTextureFrameAvailable,
            .engine = flutterpi.flutter.engine
        }
    );
    flutterpi.texture_registry = texture_registry;

    engine_result = procs->NotifyDisplayUpdate(
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
    engine_result = procs->SendWindowMetricsEvent(
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

    engine_result = flutterpi->flutter.procs.SendPointerEvent(
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

    bool use_vulkan;
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
    int vulkan_int = false;
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
        {"vulkan", no_argument, &vulkan_int, true},
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

    result_out->use_vulkan = vulkan_int;

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

    locales_print(flutterpi.locales);

    ok = init_display(
        &flutterpi,
        flutterpi.event_loop,
        cmd_args.use_vulkan ? kVulkan_RendererType : kOpenGL_RendererType,
        cmd_args.has_pixel_format, cmd_args.pixel_format,
        cmd_args.has_rotation,
            cmd_args.rotation == 0   ? PLANE_TRANSFORM_ROTATE_0   :
            cmd_args.rotation == 90  ? PLANE_TRANSFORM_ROTATE_90  :
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
