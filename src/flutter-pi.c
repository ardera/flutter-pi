#define _GNU_SOURCE

#include "flutter-pi.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <elf.h>
#include <features.h>
#include <flutter_embedder.h>
#include <gbm.h>
#include <getopt.h>
#include <langinfo.h>
#include <libinput.h>
#include <libudev.h>
#include <linux/input.h>
#include <sys/eventfd.h>
#include <systemd/sd-event.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "compositor_ng.h"
#include "filesystem_layout.h"
#include "frame_scheduler.h"
#include "keyboard.h"
#include "locales.h"
#include "modesetting.h"
#include "pixel_format.h"
#include "platformchannel.h"
#include "pluginregistry.h"
#include "plugins/raw_keyboard.h"
#include "plugins/text_input.h"
#include "texture_registry.h"
#include "tracer.h"
#include "user_input.h"
#include "util/list.h"
#include "util/logging.h"
#include "window.h"

#include "config.h"

#ifdef HAVE_LIBSEAT
    #include <libseat.h>
#endif

#ifdef HAVE_EGL_GLES2
    #include "egl.h"
    #include "gl_renderer.h"
    #include "gles.h"
#endif

#ifdef HAVE_VULKAN
    #include "vk_renderer.h"
#endif

#ifdef ENABLE_MTRACE
    #include <mcheck.h>
#endif

#define PIXFMT_ARG_NAME(_name, _arg_name, ...) _arg_name ", "

const char *const usage =
    "\
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
  --vulkan                   Use vulkan for rendering.\n"
#ifndef HAVE_VULKAN
    "\
                             NOTE: This flutter-pi executable was built without\n\
                             vulkan support.\n"
#endif
    "\n\
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
                             Available pixel formats: " PIXFMT_LIST(PIXFMT_ARG_NAME
    ) "\n\
  --videomode widthxheight\n\
  --videomode widthxheight@hz  Uses an output videomode that satisfies the argument.\n\
                             If no hz value is given, the highest possible refreshrate\n\
                             will be used.\n\
\n\
  --dummy-display            Simulate a display. Useful for running apps\n\
                             without a display attached.\n\
  --dummy-display-size \"width,height\" The width & height of the dummy display\n\
                             in pixels.\n\
  --drm-vout-display <drm-device>  The DRM display to use.\n\
                             HDMI-A-1, HDMI-A-2, DSI-1, DSI-2.\n\
\n\
  -h, --help                 Show this help and exit.\n\
\n\
EXAMPLES:\n\
  flutter-pi ~/hello_world_app\n\
  flutter-pi --release ~/hello_world_app\n\
  flutter-pi -o portrait_up ./my_app\n\
  flutter-pi -r 90 ./my_app\n\
  flutter-pi -d \"155, 86\" ./my_app\n\
  flutter-pi --videomode 1920x1080 ./my_app\n\
  flutter-pi --videomode 1280x720@60 ./my_app\n\
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

struct libseat;

struct flutterpi {
    /**
	 * @brief The KMS device.
	 *
	 */
    struct drmdev *drmdev;

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
    // sd_event_source *compositor_event_source;

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
    // sd_event_source *user_input_event_source;

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
        void *engine_handle;

        FlutterLocale **locales;
        size_t n_locales;

        int engine_argc;
        char **engine_argv;
        enum flutter_runtime_mode runtime_mode;
        FlutterEngineProcTable procs;
        FlutterEngine engine;
        FlutterEngineAOTData aot_data;

        bool next_frame_request_is_secondary;
    } flutter;

    /// main event loop
    pthread_t event_loop_thread;
    pthread_mutex_t event_loop_mutex;
    sd_event *event_loop;
    int wakeup_event_loop_fd;

    struct evloop *evloop;

    /**
     * @brief Manages all plugins.
     *
     */
    struct plugin_registry *plugin_registry;

    /**
     * @brief Manages all external textures registered to the flutter engine.
     *
     */
    struct texture_registry *texture_registry;

    struct gl_renderer *gl_renderer;
    struct vk_renderer *vk_renderer;

    struct libseat *libseat;
    struct list_head fd_for_device_id;
    bool session_active;

    char *desired_videomode;

    char *drm_vout_display;
};

struct device_id_and_fd {
    struct list_head entry;
    int device_id;
    int fd;
};

/// TODO: Remove this
struct flutterpi *flutterpi;

static bool runs_platform_tasks_on_current_thread(void *userdata);

/*********************
 * FLUTTER CALLBACKS *
 *********************/

#ifdef HAVE_EGL_GLES2
/// Called on some flutter internal thread when the flutter
/// rendering EGLContext should be made current.
static bool on_make_current(void *userdata) {
    struct flutterpi *flutterpi;
    EGLSurface surface;
    int ok;

    ASSERT_NOT_NULL(userdata);
    flutterpi = userdata;
    ASSERT_NOT_NULL(flutterpi->gl_renderer);

    // Ideally we don't make a surface current here at all.
    // But that doesn't work right now.
    // if (compositor_has_egl_surface(flutterpi->compositor)) {
    //     surface = compositor_get_egl_surface(flutterpi->compositor);
    //     if (surface == EGL_NO_SURFACE) {
    //         /// TODO: Get a fake EGL Surface just for initialization.
    //         LOG_ERROR("Couldn't get an EGL surface from the compositor.\n");
    //         return false;
    //     }
    //     ok = gl_renderer_make_flutter_rendering_context_current(flutterpi->gl_renderer, surface);
    //     if (ok != 0) {
    //         return false;
    //     }
    // } else {
    //     ok = gl_renderer_make_flutter_setup_context_current(flutterpi->gl_renderer);
    //     if (ok != 0) {
    //         return false;
    //     }
    // }

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
static bool on_clear_current(void *userdata) {
    struct flutterpi *flutterpi;
    int ok;

    ASSERT_NOT_NULL(userdata);
    flutterpi = userdata;
    ASSERT_NOT_NULL(flutterpi->gl_renderer);

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
static uint32_t fbo_callback(void *userdata) {
    struct flutterpi *flutterpi;

    ASSERT_NOT_NULL(userdata);
    flutterpi = userdata;
    (void) flutterpi;

    TRACER_INSTANT(flutterpi->tracer, "fbo_callback");
    return 0;
}

/// Called on some flutter internal thread when the flutter
/// resource uploading EGLContext should be made current.
static bool on_make_resource_current(void *userdata) {
    struct flutterpi *flutterpi;
    int ok;

    ASSERT_NOT_NULL(userdata);
    flutterpi = userdata;
    ASSERT_NOT_NULL(flutterpi->gl_renderer);

    ok = gl_renderer_make_flutter_resource_uploading_context_current(flutterpi->gl_renderer);
    if (ok != 0) {
        return false;
    }

    return true;
}

/// Called by flutter
static void *proc_resolver(void *userdata, const char *name) {
    struct flutterpi *flutterpi;

    ASSERT_NOT_NULL(userdata);
    flutterpi = userdata;
    ASSERT_NOT_NULL(flutterpi->gl_renderer);

    return gl_renderer_get_proc_address(flutterpi->gl_renderer, name);
}
#endif

UNUSED static void *on_get_vulkan_proc_address(void *userdata, FlutterVulkanInstanceHandle instance, const char *name) {
    ASSERT_NOT_NULL(userdata);
    ASSERT_NOT_NULL(name);
    (void) userdata;

#ifdef HAVE_VULKAN
    if (streq(name, "GetInstanceProcAddr")) {
        name = "vkGetInstanceProcAddr";
    }

    return (void *) vkGetInstanceProcAddr((VkInstance) instance, name);
#else
    (void) userdata;
    (void) instance;
    (void) name;
    UNREACHABLE();
#endif
}

UNUSED static FlutterVulkanImage on_get_next_vulkan_image(void *userdata, const FlutterFrameInfo *frameinfo) {
    struct flutterpi *flutterpi;

    ASSERT_NOT_NULL(userdata);
    ASSERT_NOT_NULL(frameinfo);
    flutterpi = userdata;

    (void) flutterpi;
    (void) frameinfo;

    UNIMPLEMENTED();
    UNREACHABLE();
}

UNUSED static bool on_present_vulkan_image(void *userdata, const FlutterVulkanImage *image) {
    struct flutterpi *flutterpi;

    ASSERT_NOT_NULL(userdata);
    ASSERT_NOT_NULL(image);
    flutterpi = userdata;

    (void) flutterpi;
    (void) image;

    UNIMPLEMENTED();
    UNREACHABLE();
}

static void on_platform_message(const FlutterPlatformMessage *message, void *userdata) {
    int ok;

    (void) userdata;

    ok = plugin_registry_on_platform_message(flutterpi->plugin_registry, message);
    if (ok != 0) {
        LOG_ERROR("Error handling platform message. plugin_registry_on_platform_message: %s\n", strerror(ok));
    }
}

static bool flutterpi_runs_platform_tasks_on_current_thread(struct flutterpi *flutterpi) {
    ASSERT_NOT_NULL(flutterpi);
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

    ASSERT_NOT_NULL(userdata);
    req = userdata;

    assert(flutterpi_runs_platform_tasks_on_current_thread(req->flutterpi));

    TRACER_INSTANT(req->flutterpi->tracer, "FlutterEngineOnVsync");
    engine_result = req->flutterpi->flutter.procs.OnVsync(req->flutterpi->flutter.engine, req->baton, req->vblank_ns, req->next_vblank_ns);

    free(req);

    if (engine_result != kSuccess) {
        LOG_ERROR("Couldn't signal frame begin to flutter engine. FlutterEngineOnVsync: %s\n", FLUTTER_RESULT_TO_STRING(engine_result));
        return EIO;
    }

    return 0;
}

UNUSED static void on_begin_frame(void *userdata, uint64_t vblank_ns, uint64_t next_vblank_ns) {
    FlutterEngineResult engine_result;
    struct frame_req *req;
    int ok;

    ASSERT_NOT_NULL(userdata);
    req = userdata;

    if (flutterpi_runs_platform_tasks_on_current_thread(req->flutterpi)) {
        TRACER_INSTANT(req->flutterpi->tracer, "FlutterEngineOnVsync");

        engine_result = req->flutterpi->flutter.procs.OnVsync(req->flutterpi->flutter.engine, req->baton, vblank_ns, next_vblank_ns);
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
UNUSED static void on_frame_request(void *userdata, intptr_t baton) {
    FlutterEngineResult engine_result;
    struct flutterpi *flutterpi;
    struct frame_req *req;
    int ok;

    ASSERT_NOT_NULL(userdata);
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

    if (flutterpi_runs_platform_tasks_on_current_thread(req->flutterpi)) {
        TRACER_INSTANT(req->flutterpi->tracer, "FlutterEngineOnVsync");

        engine_result =
            req->flutterpi->flutter.procs.OnVsync(req->flutterpi->flutter.engine, req->baton, req->vblank_ns, req->next_vblank_ns);
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

UNUSED static FlutterTransformation on_get_transformation(void *userdata) {
    struct view_geometry geometry;
    struct flutterpi *flutterpi;

    ASSERT_NOT_NULL(userdata);
    flutterpi = userdata;

    compositor_get_view_geometry(flutterpi->compositor, &geometry);

    return MAT3F_AS_FLUTTER_TRANSFORM(geometry.view_to_display_transform);
}

atomic_int_least64_t platform_task_counter = 0;

/// platform tasks
static int on_execute_platform_task(sd_event_source *s, void *userdata) {
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

int flutterpi_post_platform_task(int (*callback)(void *userdata), void *userdata) {
    struct platform_task *task;
    sd_event_source *src;
    int ok;

    task = malloc(sizeof *task);
    if (task == NULL) {
        return ENOMEM;
    }

    task->callback = callback;
    task->userdata = userdata;

    if (pthread_self() != flutterpi->event_loop_thread) {
        pthread_mutex_lock(&flutterpi->event_loop_mutex);
    }

    ok = sd_event_add_defer(flutterpi->event_loop, &src, on_execute_platform_task, task);
    if (ok < 0) {
        LOG_ERROR("Error posting platform task to main loop. sd_event_add_defer: %s\n", strerror(-ok));
        ok = -ok;
        goto fail_unlock_event_loop;
    }

    // Higher values mean lower priority. So later platform tasks are handled later too.
    sd_event_source_set_priority(src, atomic_fetch_add(&platform_task_counter, 1));

    if (pthread_self() != flutterpi->event_loop_thread) {
        ok = write(flutterpi->wakeup_event_loop_fd, (uint8_t[8]){ 0, 0, 0, 0, 0, 0, 0, 1 }, 8);
        if (ok < 0) {
            ok = errno;
            LOG_ERROR("Error arming main loop for platform task. write: %s\n", strerror(ok));
            goto fail_unlock_event_loop;
        }
    }

    if (pthread_self() != flutterpi->event_loop_thread) {
        pthread_mutex_unlock(&flutterpi->event_loop_mutex);
    }

    return 0;

fail_unlock_event_loop:
    if (pthread_self() != flutterpi->event_loop_thread) {
        pthread_mutex_unlock(&flutterpi->event_loop_mutex);
    }

    return ok;
}

/// timed platform tasks
static int on_execute_platform_task_with_time(sd_event_source *s, uint64_t usec, void *userdata) {
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

int flutterpi_post_platform_task_with_time(int (*callback)(void *userdata), void *userdata, uint64_t target_time_usec) {
    struct platform_task *task;
    //sd_event_source *source;
    int ok;

    task = malloc(sizeof *task);
    if (task == NULL) {
        return ENOMEM;
    }

    task->callback = callback;
    task->userdata = userdata;

    if (pthread_self() != flutterpi->event_loop_thread) {
        pthread_mutex_lock(&flutterpi->event_loop_mutex);
    }

    ok = sd_event_add_time(flutterpi->event_loop, NULL, CLOCK_MONOTONIC, target_time_usec, 1, on_execute_platform_task_with_time, task);
    if (ok < 0) {
        LOG_ERROR("Error posting platform task to main loop. sd_event_add_time: %s\n", strerror(-ok));
        ok = -ok;
        goto fail_unlock_event_loop;
    }

    if (pthread_self() != flutterpi->event_loop_thread) {
        ok = write(flutterpi->wakeup_event_loop_fd, (uint8_t[8]){ 0, 0, 0, 0, 0, 0, 0, 1 }, 8);
        if (ok < 0) {
            perror("[flutter-pi] Error arming main loop for platform task. write");
            ok = errno;
            goto fail_unlock_event_loop;
        }
    }

    if (pthread_self() != flutterpi->event_loop_thread) {
        pthread_mutex_unlock(&flutterpi->event_loop_mutex);
    }

    return 0;

fail_unlock_event_loop:
    if (pthread_self() != flutterpi->event_loop_thread) {
        pthread_mutex_unlock(&flutterpi->event_loop_mutex);
    }
    free(task);
    return ok;
}

int flutterpi_sd_event_add_io(sd_event_source **source_out, int fd, uint32_t events, sd_event_io_handler_t callback, void *userdata) {
    int ok;

    if (pthread_self() != flutterpi->event_loop_thread) {
        pthread_mutex_lock(&flutterpi->event_loop_mutex);
    }

    ok = sd_event_add_io(flutterpi->event_loop, source_out, fd, events, callback, userdata);
    if (ok < 0) {
        LOG_ERROR("Could not add IO callback to event loop. sd_event_add_io: %s\n", strerror(-ok));
        return -ok;
    }

    if (pthread_self() != flutterpi->event_loop_thread) {
        ok = write(flutterpi->wakeup_event_loop_fd, (uint8_t[8]){ 0, 0, 0, 0, 0, 0, 0, 1 }, 8);
        if (ok < 0) {
            perror("[flutter-pi] Error arming main loop for io callback. write");
            ok = errno;
            goto fail_unlock_event_loop;
        }
    }

    if (pthread_self() != flutterpi->event_loop_thread) {
        pthread_mutex_unlock(&flutterpi->event_loop_mutex);
    }

    return 0;

fail_unlock_event_loop:
    if (pthread_self() != flutterpi->event_loop_thread) {
        pthread_mutex_unlock(&flutterpi->event_loop_mutex);
    }
    return ok;
}

/// flutter tasks
static int on_execute_flutter_task(void *userdata) {
    FlutterEngineResult result;
    FlutterTask *task;

    task = userdata;

    result = flutterpi->flutter.procs.RunTask(flutterpi->flutter.engine, task);
    if (result != kSuccess) {
        LOG_ERROR("Error running platform task. FlutterEngineRunTask: %d\n", result);
        free(task);
        return EINVAL;
    }

    free(task);

    return 0;
}

static void on_post_flutter_task(FlutterTask task, uint64_t target_time, void *userdata) {
    FlutterTask *dup_task;
    int ok;

    (void) userdata;

    dup_task = malloc(sizeof *dup_task);
    if (dup_task == NULL) {
        return;
    }

    *dup_task = task;

    ok = flutterpi_post_platform_task_with_time(on_execute_flutter_task, dup_task, target_time / 1000);
    if (ok != 0) {
        free(dup_task);
    }
}

/// platform messages
static int on_send_platform_message(void *userdata) {
    struct platform_message *msg;
    FlutterEngineResult result;

    msg = userdata;

    if (msg->is_response) {
        result = flutterpi->flutter.procs
                     .SendPlatformMessageResponse(flutterpi->flutter.engine, msg->target_handle, msg->message, msg->message_size);
    } else {
        FlutterPlatformMessage message;
        memset(&message, 0, sizeof(message));

        message.struct_size = sizeof(FlutterPlatformMessage);
        message.channel = msg->target_channel;
        message.message_size = msg->message_size;
        message.response_handle = msg->response_handle;
        message.message = msg->message;

        result = flutterpi->flutter.procs.SendPlatformMessage(flutterpi->flutter.engine, &message);
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
            &(const FlutterPlatformMessage){
                .struct_size = sizeof(FlutterPlatformMessage),
                .channel = channel,
                .message = message,
                .message_size = message_size,
                .response_handle = responsehandle,
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

        ok = flutterpi_post_platform_task(on_send_platform_message, msg);
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
    const FlutterPlatformMessageResponseHandle *handle,
    const uint8_t *restrict message,
    size_t message_size
) {
    struct platform_message *msg;
    FlutterEngineResult result;
    int ok;

    if (flutterpi_runs_platform_tasks_on_current_thread(flutterpi)) {
        result = flutterpi->flutter.procs.SendPlatformMessageResponse(flutterpi->flutter.engine, handle, message, message_size);
        if (result != kSuccess) {
            LOG_ERROR(
                "Error sending platform message response. FlutterEngineSendPlatformMessageResponse: %s\n",
                FLUTTER_RESULT_TO_STRING(result)
            );
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

        ok = flutterpi_post_platform_task(on_send_platform_message, msg);
        if (ok != 0) {
            if (msg->message) {
                free(msg->message);
            }
            free(msg);
        }
    }

    return 0;
}

struct texture_registry *flutterpi_get_texture_registry(struct flutterpi *flutterpi) {
    ASSERT_NOT_NULL(flutterpi);
    ASSERT_NOT_NULL(flutterpi->texture_registry);
    return flutterpi->texture_registry;
}

struct plugin_registry *flutterpi_get_plugin_registry(struct flutterpi *flutterpi) {
    ASSERT_NOT_NULL(flutterpi);
    ASSERT_NOT_NULL(flutterpi->plugin_registry);
    return flutterpi->plugin_registry;
}

FlutterPlatformMessageResponseHandle *
flutterpi_create_platform_message_response_handle(struct flutterpi *flutterpi, FlutterDataCallback data_callback, void *userdata) {
    FlutterPlatformMessageResponseHandle *handle;
    FlutterEngineResult engine_result;

    ASSERT_NOT_NULL(flutterpi);
    ASSERT_NOT_NULL(data_callback);

    // FlutterEngineResult FlutterPlatformMessageCreateResponseHandle(
    //     FLUTTER_API_SYMBOL(FlutterEngine) engine,
    //     FlutterDataCallback data_callback,
    //     void* user_data,
    //     FlutterPlatformMessageResponseHandle** response_out
    // );

    engine_result =
        flutterpi->flutter.procs.PlatformMessageCreateResponseHandle(flutterpi->flutter.engine, data_callback, userdata, &handle);
    if (engine_result != kSuccess) {
        LOG_ERROR(
            "Couldn't create platform message response handle. FlutterPlatformMessageCreateResponseHandle: %s\n",
            FLUTTER_RESULT_TO_STRING(engine_result)
        );
        return NULL;
    }

    return handle;
}

void flutterpi_release_platform_message_response_handle(struct flutterpi *flutterpi, FlutterPlatformMessageResponseHandle *handle) {
    FlutterEngineResult engine_result;

    ASSERT_NOT_NULL(flutterpi);
    ASSERT_NOT_NULL(handle);

    // FlutterEngineResult FlutterPlatformMessageReleaseResponseHandle(
    //     FLUTTER_API_SYMBOL(FlutterEngine) engine,
    //     FlutterPlatformMessageResponseHandle* response
    // );

    engine_result = flutterpi->flutter.procs.PlatformMessageReleaseResponseHandle(flutterpi->flutter.engine, handle);
    if (engine_result != kSuccess) {
        // We can't do anything about it though.
        LOG_ERROR(
            "Couldn't release platform message response handle. FlutterPlatformMessageReleaseResponseHandle: %s\n",
            FLUTTER_RESULT_TO_STRING(engine_result)
        );
    }
}

struct texture *flutterpi_create_texture(struct flutterpi *flutterpi) {
    return texture_new(flutterpi_get_texture_registry(flutterpi));
}

const char *flutterpi_get_asset_bundle_path(struct flutterpi *flutterpi) {
    return flutterpi->flutter.paths->asset_bundle_path;
}

/// TODO: Make this refcounted if we're gonna use it from multiple threads.
struct gbm_device *flutterpi_get_gbm_device(struct flutterpi *flutterpi) {
    return drmdev_get_gbm_device(flutterpi->drmdev);
}

bool flutterpi_has_gl_renderer(struct flutterpi *flutterpi) {
    ASSERT_NOT_NULL(flutterpi);
    return flutterpi->gl_renderer != NULL;
}

struct gl_renderer *flutterpi_get_gl_renderer(struct flutterpi *flutterpi) {
    ASSERT_NOT_NULL(flutterpi);
    return flutterpi->gl_renderer;
}

void flutterpi_set_pointer_kind(struct flutterpi *flutterpi, enum pointer_kind kind) {
    return compositor_set_cursor(flutterpi->compositor, false, false, true, kind, false, VEC2F(0, 0));
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

static bool runs_platform_tasks_on_current_thread(void *userdata) {
    return flutterpi_runs_platform_tasks_on_current_thread(userdata);
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

/**************************
 * DISPLAY INITIALIZATION *
 **************************/
static int on_drmdev_ready(sd_event_source *s, int fd, uint32_t revents, void *userdata) {
    struct drmdev *drmdev;

    (void) s;
    (void) fd;
    (void) revents;
    (void) userdata;

    ASSERT_NOT_NULL(userdata);
    drmdev = userdata;

    return drmdev_on_event_fd_ready(drmdev);
}

static const FlutterLocale *on_compute_platform_resolved_locales(const FlutterLocale **locales, size_t n_locales) {
    return locales_on_compute_platform_resolved_locale(flutterpi->locales, locales, n_locales);
}

#ifdef HAVE_EGL_GLES2
static bool
on_gl_external_texture_frame_callback(void *userdata, int64_t texture_id, size_t width, size_t height, FlutterOpenGLTexture *texture_out) {
    struct flutterpi *flutterpi;

    ASSERT_NOT_NULL(userdata);

    flutterpi = userdata;

    return texture_registry_gl_external_texture_frame_callback(flutterpi->texture_registry, texture_id, width, height, texture_out);
}
#endif

/**************************
 * FLUTTER INITIALIZATION *
 **************************/

static void *load_flutter_engine_lib(struct flutter_paths *paths) {
    void *engine_handle = NULL;
    int dlopen_mode;

#ifdef ENABLE_ASAN
    // If address sanitizer is enabled, we specify RTLD_NODELETE so
    // the library isn't really unloaded on dlclose(). (dlclose will actually
    // do nothing)
    //
    // That enables asan to actually symbolize any backtraces for memory leaks
    // it finds.
    //
    // Without RTLD_NODELETE it can't symbolize the backtraces because the
    // library isn't present in memory anymore when asan tries to symbolize.
    dlopen_mode = RTLD_LOCAL | RTLD_NOW | RTLD_NODELETE;
#else
    dlopen_mode = RTLD_LOCAL | RTLD_NOW;
#endif

    if (paths->flutter_engine_path != NULL) {
        engine_handle = dlopen(paths->flutter_engine_path, dlopen_mode);
        if (engine_handle == NULL) {
            LOG_DEBUG("Info: Could not load flutter engine from app bundle. dlopen(\"%s\"): %s.\n", paths->flutter_engine_path, dlerror());
        }
    }

    if (engine_handle == NULL && paths->flutter_engine_dlopen_name != NULL) {
        engine_handle = dlopen(paths->flutter_engine_dlopen_name, dlopen_mode);
        if (engine_handle == NULL) {
            LOG_DEBUG("Info: Could not load flutter engine. dlopen(\"%s\"): %s.\n", paths->flutter_engine_dlopen_name, dlerror());
        }
    }

    if (engine_handle == NULL && paths->flutter_engine_dlopen_name_fallback != NULL) {
        engine_handle = dlopen(paths->flutter_engine_dlopen_name_fallback, dlopen_mode);
        if (engine_handle == NULL) {
            LOG_DEBUG("Info: Could not load flutter engine. dlopen(\"%s\"): %s.\n", paths->flutter_engine_dlopen_name_fallback, dlerror());
        }
    }

    if (engine_handle == NULL) {
        LOG_ERROR("Error: Could not load flutter engine from any location. Make sure you have installed the engine binaries.\n");
        return NULL;
    }

    return engine_handle;
}

static void unload_flutter_engine_lib(void *handle) {
    dlclose(handle);
}

static int get_flutter_engine_procs(void *engine_handle, FlutterEngineProcTable *procs_out) {
    // clang-format off
    FlutterEngineResult (*get_proc_addresses)(FlutterEngineProcTable *table);
    // clang-format on

    FlutterEngineResult engine_result;

    get_proc_addresses = dlsym(engine_handle, "FlutterEngineGetProcAddresses");
    if (get_proc_addresses == NULL) {
        LOG_ERROR("Could not resolve flutter engine function FlutterEngineGetProcAddresses.\n");
        return EINVAL;
    }

    procs_out->struct_size = sizeof(FlutterEngineProcTable);
    engine_result = get_proc_addresses(procs_out);
    if (engine_result != kSuccess) {
        LOG_ERROR(
            "Could not resolve flutter engine proc addresses. FlutterEngineGetProcAddresses: %s\n",
            FLUTTER_RESULT_TO_STRING(engine_result)
        );
        return EINVAL;
    }

    return 0;
}

static int on_register_texture(void *userdata, int64_t texture_identifier) {
    FlutterEngineResult engine_result;
    struct flutterpi *flutterpi;

    ASSERT_NOT_NULL(userdata);
    flutterpi = userdata;
    ASSERT_NOT_NULL(flutterpi->flutter.engine);

    engine_result = flutterpi->flutter.procs.RegisterExternalTexture(flutterpi->flutter.engine, texture_identifier);
    if (engine_result != kSuccess) {
        LOG_ERROR(
            "Error registering external texture to flutter engine. FlutterEngineRegisterExternalTexture: %s\n",
            FLUTTER_RESULT_TO_STRING(engine_result)
        );
        return EIO;
    }

    return 0;
}

static int on_unregister_texture(void *userdata, int64_t texture_identifier) {
    FlutterEngineResult engine_result;
    struct flutterpi *flutterpi;

    ASSERT_NOT_NULL(userdata);
    flutterpi = userdata;
    ASSERT_NOT_NULL(flutterpi->flutter.engine);

    engine_result = flutterpi->flutter.procs.UnregisterExternalTexture(flutterpi->flutter.engine, texture_identifier);
    if (engine_result != kSuccess) {
        LOG_ERROR(
            "Error unregistering external texture from flutter engine. FlutterEngineUnregisterExternalTexture: %s\n",
            FLUTTER_RESULT_TO_STRING(engine_result)
        );
        return EIO;
    }

    return 0;
}

static int on_mark_texture_frame_available(void *userdata, int64_t texture_identifier) {
    FlutterEngineResult engine_result;
    struct flutterpi *flutterpi;

    ASSERT_NOT_NULL(userdata);
    flutterpi = userdata;
    ASSERT_NOT_NULL(flutterpi->flutter.engine);

    engine_result = flutterpi->flutter.procs.MarkExternalTextureFrameAvailable(flutterpi->flutter.engine, texture_identifier);
    if (engine_result != kSuccess) {
        LOG_ERROR(
            "Error notifying flutter engine about new external texture frame. FlutterEngineMarkExternalTextureFrameAvailable: %s\n",
            FLUTTER_RESULT_TO_STRING(engine_result)
        );
        return EIO;
    }

    return 0;
}

static FlutterEngine create_flutter_engine(
    struct vk_renderer *vk_renderer,
    struct flutter_paths *paths,
    int engine_argc,
    char **engine_argv,
    struct compositor *compositor,
    FlutterEngineAOTData aot_data,
    const FlutterEngineProcTable *procs
) {
    FlutterEngineResult engine_result;
    FlutterEngine engine;

    FlutterRendererConfig renderer_config;
    memset(&renderer_config, 0, sizeof(renderer_config));

    // configure flutter rendering
    if (vk_renderer) {
#ifdef HAVE_VULKAN
        renderer_config.type = kVulkan;
        renderer_config.vulkan.struct_size = sizeof(FlutterVulkanRendererConfig);
        renderer_config.vulkan.version = vk_renderer_get_vk_version(vk_renderer);
        renderer_config.vulkan.instance = vk_renderer_get_instance(vk_renderer);
        renderer_config.vulkan.physical_device = vk_renderer_get_physical_device(vk_renderer);
        renderer_config.vulkan.device = vk_renderer_get_device(vk_renderer);
        renderer_config.vulkan.queue_family_index = vk_renderer_get_queue_family_index(vk_renderer);
        renderer_config.vulkan.queue = vk_renderer_get_queue(vk_renderer);
        renderer_config.vulkan.enabled_instance_extension_count = vk_renderer_get_enabled_instance_extension_count(vk_renderer);
        renderer_config.vulkan.enabled_instance_extensions = vk_renderer_get_enabled_instance_extensions(vk_renderer);
        renderer_config.vulkan.enabled_device_extension_count = vk_renderer_get_enabled_device_extension_count(vk_renderer);
        renderer_config.vulkan.enabled_device_extensions = vk_renderer_get_enabled_device_extensions(vk_renderer);
        renderer_config.vulkan.get_instance_proc_address_callback = on_get_vulkan_proc_address;
        renderer_config.vulkan.get_next_image_callback = on_get_next_vulkan_image;
        renderer_config.vulkan.present_image_callback = on_present_vulkan_image;
#else
        UNREACHABLE();
#endif
    } else {
#ifdef HAVE_EGL_GLES2
        renderer_config.type = kOpenGL;
        renderer_config.open_gl.struct_size = sizeof(FlutterOpenGLRendererConfig);
        renderer_config.open_gl.make_current = on_make_current;
        renderer_config.open_gl.clear_current = on_clear_current;
        renderer_config.open_gl.present = on_present;
        renderer_config.open_gl.fbo_callback = fbo_callback;
        renderer_config.open_gl.make_resource_current = on_make_resource_current;
        renderer_config.open_gl.gl_proc_resolver = proc_resolver;
        renderer_config.open_gl.surface_transformation = on_get_transformation;
        renderer_config.open_gl.gl_external_texture_frame_callback = on_gl_external_texture_frame_callback;
        renderer_config.open_gl.fbo_with_frame_info_callback = NULL;
        renderer_config.open_gl.present_with_info = NULL;
        renderer_config.open_gl.populate_existing_damage = NULL;
#else
        UNREACHABLE();
#endif
    }

    FlutterTaskRunnerDescription platform_task_runner;
    memset(&platform_task_runner, 0, sizeof(platform_task_runner));

    platform_task_runner.struct_size = sizeof(FlutterTaskRunnerDescription);
    platform_task_runner.user_data = flutterpi;
    platform_task_runner.runs_task_on_current_thread_callback = runs_platform_tasks_on_current_thread;
    platform_task_runner.post_task_callback = on_post_flutter_task;

    FlutterCustomTaskRunners custom_task_runners;
    memset(&custom_task_runners, 0, sizeof(custom_task_runners));

    custom_task_runners.struct_size = sizeof(FlutterCustomTaskRunners);
    custom_task_runners.platform_task_runner = &platform_task_runner;
    custom_task_runners.render_task_runner = NULL;
    custom_task_runners.thread_priority_setter = NULL;

    // configure the project
    FlutterProjectArgs project_args;
    memset(&project_args, 0, sizeof(project_args));

    project_args.struct_size = sizeof(FlutterProjectArgs);
    project_args.assets_path = paths->asset_bundle_path;
    project_args.icu_data_path = paths->icudtl_path;
    project_args.command_line_argc = engine_argc;
    project_args.command_line_argv = (const char *const *) engine_argv;
    project_args.platform_message_callback = on_platform_message;
    project_args.vm_snapshot_data = NULL;
    project_args.vm_snapshot_data_size = 0;
    project_args.vm_snapshot_instructions = NULL;
    project_args.vm_snapshot_instructions_size = 0;
    project_args.isolate_snapshot_data = NULL;
    project_args.isolate_snapshot_data_size = 0;
    project_args.isolate_snapshot_instructions = NULL;
    project_args.isolate_snapshot_instructions_size = 0;
    project_args.root_isolate_create_callback = NULL;
    project_args.update_semantics_node_callback = NULL;
    project_args.update_semantics_custom_action_callback = NULL;
    project_args.persistent_cache_path = paths->asset_bundle_path;
    project_args.is_persistent_cache_read_only = false;
    project_args.vsync_callback = NULL;  // on_frame_request, /* broken since 2.2, kinda *
    project_args.custom_dart_entrypoint = NULL;
    project_args.custom_task_runners = &custom_task_runners;
    project_args.shutdown_dart_vm_when_done = true;
    project_args.compositor = compositor_get_flutter_compositor(compositor);
    project_args.dart_old_gen_heap_size = -1;
    project_args.aot_data = aot_data;
    project_args.compute_platform_resolved_locale_callback = on_compute_platform_resolved_locales;
    project_args.dart_entrypoint_argc = 0;
    project_args.dart_entrypoint_argv = NULL;
    project_args.log_message_callback = NULL;
    project_args.log_tag = NULL;
    project_args.on_pre_engine_restart_callback = NULL;
    project_args.update_semantics_callback = NULL;
    project_args.update_semantics_callback2 = NULL;

    // spin up the engine
    engine_result = procs->Initialize(FLUTTER_ENGINE_VERSION, &renderer_config, &project_args, flutterpi, &engine);
    if (engine_result != kSuccess) {
        LOG_ERROR("Could not initialize the flutter engine. FlutterEngineInitialize: %s\n", FLUTTER_RESULT_TO_STRING(engine_result));
        return NULL;
    }

    return engine;
}

static int flutterpi_run(struct flutterpi *flutterpi) {
    FlutterEngineProcTable *procs;
    struct view_geometry geometry;
    FlutterEngineResult engine_result;
    FlutterEngine engine;
    int ok, evloop_fd;

    procs = &flutterpi->flutter.procs;

    if (flutterpi->libseat != NULL) {
#ifdef HAVE_LIBSEAT
        ok = libseat_dispatch(flutterpi->libseat, 0);
        if (ok < 0) {
            LOG_ERROR("initial libseat dispatch failed. libseat_dispatch: %s\n", strerror(errno));
        }
#else
        UNREACHABLE();
#endif
    }

    ok = plugin_registry_ensure_plugins_initialized(flutterpi->plugin_registry);
    if (ok != 0) {
        LOG_ERROR("Could not initialize plugins.\n");
        return EINVAL;
    }

    engine = create_flutter_engine(
        flutterpi->vk_renderer,
        flutterpi->flutter.paths,
        flutterpi->flutter.engine_argc,
        flutterpi->flutter.engine_argv,
        flutterpi->compositor,
        flutterpi->flutter.aot_data,
        &flutterpi->flutter.procs
    );
    if (engine == NULL) {
        return EINVAL;
    }

    flutterpi->flutter.engine = engine;

    engine_result = procs->RunInitialized(engine);
    if (engine_result != kSuccess) {
        LOG_ERROR("Could not run the flutter engine. FlutterEngineRunInitialized: %s\n", FLUTTER_RESULT_TO_STRING(engine_result));
        ok = EIO;
        goto fail_deinitialize_engine;
    }

    ok = locales_add_to_fl_engine(flutterpi->locales, engine, procs->UpdateLocales);
    if (ok != 0) {
        goto fail_shutdown_engine;
    }

    FlutterEngineDisplay display;
    memset(&display, 0, sizeof(display));

    display.struct_size = sizeof(FlutterEngineDisplay);
    display.display_id = 0;
    display.single_display = true;
    display.refresh_rate = compositor_get_refresh_rate(flutterpi->compositor);

    engine_result = procs->NotifyDisplayUpdate(engine, kFlutterEngineDisplaysUpdateTypeStartup, &display, 1);
    if (engine_result != kSuccess) {
        ok = EINVAL;
        LOG_ERROR(
            "Could not send display update to flutter engine. FlutterEngineNotifyDisplayUpdate: %s\n",
            FLUTTER_RESULT_TO_STRING(engine_result)
        );
        goto fail_shutdown_engine;
    }

    compositor_get_view_geometry(flutterpi->compositor, &geometry);

    // just so we get an error if the window metrics event was expanded without us noticing
    FlutterWindowMetricsEvent window_metrics_event;
    memset(&window_metrics_event, 0, sizeof(window_metrics_event));

    window_metrics_event.struct_size = sizeof(FlutterWindowMetricsEvent);
    window_metrics_event.width = geometry.view_size.x;
    window_metrics_event.height = geometry.view_size.y;
    window_metrics_event.pixel_ratio = geometry.device_pixel_ratio;
    window_metrics_event.left = 0;
    window_metrics_event.top = 0;
    window_metrics_event.physical_view_inset_top = 0;
    window_metrics_event.physical_view_inset_right = 0;
    window_metrics_event.physical_view_inset_bottom = 0;
    window_metrics_event.physical_view_inset_left = 0;

    // update window size
    engine_result = procs->SendWindowMetricsEvent(engine, &window_metrics_event);
    if (engine_result != kSuccess) {
        LOG_ERROR(
            "Could not send window metrics to flutter engine. FlutterEngineSendWindowMetricsEvent: %s\n",
            FLUTTER_RESULT_TO_STRING(engine_result)
        );
        goto fail_shutdown_engine;
    }

    pthread_mutex_lock(&flutterpi->event_loop_mutex);

    ok = sd_event_get_fd(flutterpi->event_loop);
    if (ok < 0) {
        ok = -ok;
        LOG_ERROR("Could not get fd for main event loop. sd_event_get_fd: %s\n", strerror(ok));
        pthread_mutex_unlock(&flutterpi->event_loop_mutex);
        goto fail_shutdown_engine;
    }

    pthread_mutex_unlock(&flutterpi->event_loop_mutex);

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

        pthread_mutex_lock(&flutterpi->event_loop_mutex);

        do {
            state = sd_event_get_state(flutterpi->event_loop);
            switch (state) {
                case SD_EVENT_INITIAL:
                    ok = sd_event_prepare(flutterpi->event_loop);
                    if (ok < 0) {
                        ok = -ok;
                        LOG_ERROR("Could not prepare event loop. sd_event_prepare: %s\n", strerror(ok));
                        goto fail_shutdown_engine;
                    }

                    break;
                case SD_EVENT_ARMED:
                    pthread_mutex_unlock(&flutterpi->event_loop_mutex);

                    do {
                        rfds = const_fds;
                        wfds = const_fds;
                        xfds = const_fds;
                        ok = select(evloop_fd + 1, &rfds, &wfds, &xfds, NULL);
                        if ((ok < 0) && (errno != EINTR)) {
                            ok = errno;
                            LOG_ERROR("Could not wait for event loop events. select: %s\n", strerror(ok));
                            goto fail_shutdown_engine;
                        }
                    } while ((ok < 0) && (errno == EINTR));

                    pthread_mutex_lock(&flutterpi->event_loop_mutex);

                    ok = sd_event_wait(flutterpi->event_loop, 0);
                    if (ok < 0) {
                        ok = -ok;
                        LOG_ERROR("Could not check for event loop events. sd_event_wait: %s\n", strerror(ok));
                        goto fail_shutdown_engine;
                    }

                    break;
                case SD_EVENT_PENDING:
                    ok = sd_event_dispatch(flutterpi->event_loop);
                    if (ok < 0) {
                        ok = -ok;
                        LOG_ERROR("Could not dispatch event loop events. sd_event_dispatch: %s\n", strerror(ok));
                        goto fail_shutdown_engine;
                    }

                    break;
                case SD_EVENT_FINISHED: break;
                default: UNREACHABLE();
            }
        } while (state != SD_EVENT_FINISHED);

        pthread_mutex_unlock(&flutterpi->event_loop_mutex);
    }

    // We deinitialize the plugins here so plugins don't attempt to use the
    // flutter engine anymore.
    // For example, otherwise the gstreamer video player might call
    // texture_push_frame in another thread.
    plugin_registry_ensure_plugins_deinitialized(flutterpi->plugin_registry);

    flutterpi->flutter.procs.Shutdown(engine);
    flutterpi->flutter.engine = NULL;
    return 0;

fail_shutdown_engine:
    flutterpi->flutter.procs.Shutdown(engine);
    return ok;

fail_deinitialize_engine:
    flutterpi->flutter.procs.Deinitialize(engine);
    return ok;
}

void flutterpi_schedule_exit(struct flutterpi *flutterpi) {
    int ok;

    if (pthread_self() != flutterpi->event_loop_thread) {
        pthread_mutex_lock(&flutterpi->event_loop_mutex);
    }

    // There's a race condition here:
    //
    // Other threads can always call flutterpi_post_platform_task(). We can only
    // be sure flutterpi_post_platform_task() will not be called anymore when
    // FlutterEngineShutdown() has returned.
    //
    // However, FlutterEngineShutdown() is blocking and should be called on the
    // platform thread.
    //
    // 1. If we process them all, that's basically just continuing to run the
    //    application.
    //
    // 2. If we don't process them and just error, that could result in memory
    //    leaks.
    //
    // There's not really a nice solution here, but we use the 2nd option here.
    ok = sd_event_exit(flutterpi->event_loop, 0);
    if (ok < 0) {
        LOG_ERROR("Could not schedule application exit. sd_event_exit: %s\n", strerror(-ok));
        if (pthread_self() != flutterpi->event_loop_thread) {
            pthread_mutex_unlock(&flutterpi->event_loop_mutex);
        }
        return;
    }

    if (pthread_self() != flutterpi->event_loop_thread) {
        pthread_mutex_unlock(&flutterpi->event_loop_mutex);
    }

    return;
}

/**************
 * USER INPUT *
 **************/
static void on_flutter_pointer_event(void *userdata, const FlutterPointerEvent *events, size_t n_events) {
    FlutterEngineResult engine_result;
    struct flutterpi *flutterpi;

    ASSERT_NOT_NULL(userdata);
    flutterpi = userdata;

    /// TODO: make this atomic
    flutterpi->flutter.next_frame_request_is_secondary = true;

    engine_result = flutterpi->flutter.procs.SendPointerEvent(flutterpi->flutter.engine, events, n_events);
    if (engine_result != kSuccess) {
        LOG_ERROR(
            "Error sending touchscreen / mouse events to flutter. FlutterEngineSendPointerEvent: %s\n",
            FLUTTER_RESULT_TO_STRING(engine_result)
        );
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

static void
on_gtk_keyevent(void *userdata, uint32_t unicode_scalar_values, uint32_t key_code, uint32_t scan_code, uint32_t modifiers, bool is_down) {
    struct flutterpi *flutterpi;
    int ok;

    flutterpi = userdata;
    (void) flutterpi;

#ifdef BUILD_RAW_KEYBOARD_PLUGIN
    ok = rawkb_send_gtk_keyevent(unicode_scalar_values, key_code, scan_code, modifiers, is_down);
    if (ok != 0) {
        LOG_ERROR("Error handling keyboard event. rawkb_send_gtk_keyevent: %s\n", strerror(ok));
        //flutterpi_schedule_exit(flutterpi);
    }
#endif
}

static void on_switch_vt(void *userdata, int vt) {
    struct flutterpi *flutterpi;

    ASSERT_NOT_NULL(userdata);
    flutterpi = userdata;
    (void) flutterpi;
    (void) vt;

    LOG_DEBUG("on_switch_vt(%d)\n", vt);

    if (flutterpi->libseat != NULL) {
#ifdef HAVE_LIBSEAT
        int ok;

        ok = libseat_switch_session(flutterpi->libseat, vt);
        if (ok < 0) {
            LOG_ERROR("Could not switch session. libseat_switch_session: %s\n", strerror(errno));
        }
#else
        UNREACHABLE();
#endif
    }
}

static void on_set_cursor_enabled(void *userdata, bool enabled) {
    struct flutterpi *flutterpi;

    flutterpi = userdata;
    (void) flutterpi;

    compositor_set_cursor(flutterpi->compositor, true, enabled, false, POINTER_KIND_NONE, false, VEC2F(0, 0));
}

static void on_move_cursor(void *userdata, struct vec2f delta) {
    struct flutterpi *flutterpi;

    flutterpi = userdata;

    compositor_set_cursor(flutterpi->compositor, true, true, false, POINTER_KIND_NONE, true, delta);
}

static int on_user_input_open(const char *path, int flags, void *userdata) {
    struct flutterpi *flutterpi;
    int ok, fd;

    ASSERT_NOT_NULL(path);
    ASSERT_NOT_NULL(userdata);
    flutterpi = userdata;
    (void) flutterpi;

    if (flutterpi->libseat != NULL) {
#ifdef HAVE_LIBSEAT
        struct device_id_and_fd *entry;
        int device_id;

        ok = libseat_open_device(flutterpi->libseat, path, &fd);
        if (ok < 0) {
            ok = errno;
            LOG_ERROR("Couldn't open evdev device. libseat_open_device: %s\n", strerror(ok));
            return -ok;
        }

        device_id = ok;

        entry = malloc(sizeof *entry);
        if (entry == NULL) {
            libseat_close_device(flutterpi->libseat, device_id);
            return -ENOMEM;
        }

        entry->entry = (struct list_head){ NULL, NULL };
        entry->fd = fd;
        entry->device_id = device_id;

        list_add(&entry->entry, &flutterpi->fd_for_device_id);
        return fd;
#else
        UNREACHABLE();
#endif
    } else {
        ok = open(path, flags);
        if (ok < 0) {
            ok = errno;
            LOG_ERROR("Couldn't open evdev device. open: %s\n", strerror(ok));
            return -ok;
        }

        fd = ok;
        return fd;
    }
}

static void on_user_input_close(int fd, void *userdata) {
    struct flutterpi *flutterpi;
    int ok;

    ASSERT_NOT_NULL(userdata);
    flutterpi = userdata;
    (void) flutterpi;

    if (flutterpi->libseat != NULL) {
#ifdef HAVE_LIBSEAT
        struct device_id_and_fd *entry = NULL;

        list_for_each_entry_safe(struct device_id_and_fd, entry_iter, &flutterpi->fd_for_device_id, entry) {
            if (entry_iter->fd == fd) {
                entry = entry_iter;
                break;
            }
        }

        if (entry == NULL) {
            LOG_ERROR("Could not find the device id for the evdev device that should be closed.\n");
            return;
        }

        ok = libseat_close_device(flutterpi->libseat, entry->device_id);
        if (ok < 0) {
            LOG_ERROR("Couldn't close evdev device. libseat_close_device: %s\n", strerror(errno));
        }

        list_del(&entry->entry);
        free(entry);
        return;
#else
        UNREACHABLE();
#endif
    } else {
        ok = close(fd);
        if (ok < 0) {
            LOG_ERROR("Could not close evdev device. close: %s\n", strerror(errno));
        }
    }
}

static int on_user_input_fd_ready(sd_event_source *s, int fd, uint32_t revents, void *userdata) {
    struct user_input *input;

    (void) s;
    (void) fd;
    (void) revents;

    input = userdata;

    return user_input_on_fd_ready(input);
}

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

static bool parse_vec2i(const char *str, struct vec2i *out) {
    int ok;

    ok = sscanf(str, "%d,%d", &out->x, &out->y);
    if (ok != 2) {
        return false;
    }

    return true;
}

bool is_valid_drm_display(const char *display) {
    const char *valid_displays[] = { "HDMI-A-1", "HDMI-A-2", "DSI-1", "DSI-2" };
    for (size_t i = 0; i < sizeof(valid_displays) / sizeof(valid_displays[0]); i++) {
        if (strcmp(display, valid_displays[i]) == 0) {
            return true;
        }
    }
    return false;
}

bool flutterpi_parse_cmdline_args(int argc, char **argv, struct flutterpi_cmdline_args *result_out) {
    bool finished_parsing_options;
    int runtime_mode_int = FLUTTER_RUNTIME_MODE_DEBUG;
    int vulkan_int = false;
    int dummy_display_int = 0;
    int longopt_index = 0;
    int opt, ok;

    // start parsing from the first argument, in case this is called multiple times.
    optind = 1;

    struct option long_options[] = {
        { "release", no_argument, &runtime_mode_int, FLUTTER_RUNTIME_MODE_RELEASE },
        { "profile", no_argument, &runtime_mode_int, FLUTTER_RUNTIME_MODE_PROFILE },
        { "orientation", required_argument, NULL, 'o' },
        { "rotation", required_argument, NULL, 'r' },
        { "dimensions", required_argument, NULL, 'd' },
        { "help", no_argument, 0, 'h' },
        { "pixelformat", required_argument, NULL, 'p' },
        { "vulkan", no_argument, &vulkan_int, true },
        { "videomode", required_argument, NULL, 'v' },
        { "dummy-display", no_argument, &dummy_display_int, 1 },
        { "dummy-display-size", required_argument, NULL, 's' },
        { "drm-vout-display", required_argument, NULL, 'i' },
        { 0, 0, 0, 0 },
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
                if (streq(optarg, "portrait_up")) {
                    result_out->orientation = kPortraitUp;
                    result_out->has_orientation = true;
                } else if (streq(optarg, "landscape_left")) {
                    result_out->orientation = kLandscapeLeft;
                    result_out->has_orientation = true;
                } else if (streq(optarg, "portrait_down")) {
                    result_out->orientation = kPortraitDown;
                    result_out->has_orientation = true;
                } else if (streq(optarg, "landscape_right")) {
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

            case 'd':;
                ok = parse_vec2i(optarg, &result_out->physical_dimensions);
                if (!ok) {
                    LOG_ERROR("ERROR: Invalid argument for --dimensions passed.\n");
                    return false;
                }

                if (result_out->physical_dimensions.x < 0 || result_out->physical_dimensions.y < 0) {
                    LOG_ERROR("ERROR: Invalid argument for --dimensions passed.\n");
                    result_out->physical_dimensions = VEC2I(0, 0);
                    return false;
                }

                result_out->has_physical_dimensions = true;

                break;

            case 'p':
                for (unsigned i = 0; i < n_pixfmt_infos; i++) {
                    if (streq(optarg, pixfmt_infos[i].arg_name)) {
                        result_out->has_pixel_format = true;
                        result_out->pixel_format = pixfmt_infos[i].format;
                        goto valid_format;
                    }
                }

                LOG_ERROR(
                    "ERROR: Invalid argument for --pixelformat passed.\n"
                    "Valid values are: " PIXFMT_LIST(PIXFMT_ARG_NAME
                    ) "\n"
                      "%s",
                    usage
                );
                return false;

valid_format:
                break;

            case 'v':;
                char *vmode_dup = strdup(optarg);
                if (vmode_dup == NULL) {
                    return false;
                }

                result_out->desired_videomode = vmode_dup;
                break;

            case 's':;  // --dummy-display-size
                ok = parse_vec2i(optarg, &result_out->dummy_display_size);
                if (!ok) {
                    LOG_ERROR("ERROR: Invalid argument for --dummy-display-size passed.\n");
                    return false;
                }

                break;

            case 'i':  // --drm-vout-display
                result_out->drm_vout_display = strdup(optarg);
                if (result_out->drm_vout_display == NULL) {
                    return false;
                }
                if (!is_valid_drm_display(result_out->drm_vout_display)) {
                    LOG_ERROR("Invalid DRM display specified: %s. Valid options are HDMI-A-1, HDMI-A-2, DSI-1, DSI-2.\n", optarg);
                    return false;
                }
                break;

            case 'h': printf("%s", usage); return false;

            case '?':
            case ':': LOG_ERROR("Invalid option specified.\n%s", usage); return false;

            case -1: finished_parsing_options = true; break;

            default: break;
        }
    }

    if (optind >= argc) {
        LOG_ERROR("ERROR: Expected asset bundle path after options.\n");
        printf("%s", usage);
        return false;
    }

    result_out->bundle_path = strdup(argv[optind]);
    result_out->runtime_mode = runtime_mode_int;
    result_out->has_runtime_mode = runtime_mode_int != 0;

    argv[optind] = argv[0];
    result_out->engine_argc = argc - optind;
    result_out->engine_argv = argv + optind;

    result_out->use_vulkan = vulkan_int;

    result_out->dummy_display = !!dummy_display_int;

    return true;
}

static int on_drmdev_open(const char *path, int flags, void **fd_metadata_out, void *userdata) {
    int ok, fd, device_id;

    ASSERT_NOT_NULL(path);
    ASSERT_NOT_NULL(fd_metadata_out);
    (void) userdata;

#ifdef HAVE_LIBSEAT
    struct libseat *libseat = userdata;
    if (libseat != NULL) {
        ok = libseat_open_device(libseat, path, &fd);
        if (ok < 0) {
            LOG_ERROR("Couldn't open DRM device. libseat_open_device: %s\n", strerror(errno));
            return -1;
        }

        device_id = ok;
        *(intptr_t *) fd_metadata_out = (intptr_t) device_id;
        return fd;
    }
#else
    ASSERT_EQUALS(userdata, NULL);
#endif

    ok = open(path, flags);
    if (ok < 0) {
        LOG_ERROR("Couldn't open DRM device. open: %s\n", strerror(errno));
        return -1;
    }

    fd = ok;
    device_id = 0;

    *(intptr_t *) fd_metadata_out = (intptr_t) device_id;
    return fd;
}

static void on_drmdev_close(int fd, void *fd_metadata, void *userdata) {
    int ok;

    (void) fd_metadata;
    (void) userdata;

#ifdef HAVE_LIBSEAT
    struct libseat *libseat = userdata;
    if (libseat != NULL) {
        ASSERT_NOT_NULL(fd_metadata);
        int device_id = (intptr_t) fd_metadata;

        ok = libseat_close_device(libseat, device_id);
        if (ok < 0) {
            LOG_ERROR("Couldn't close DRM device. libseat_close_device: %s\n", strerror(errno));
            return;
        }

        return;
    }
#else
    ASSERT_EQUALS(userdata, NULL);
#endif

    ok = close(fd);
    if (ok < 0) {
        LOG_ERROR("Couldn't close DRM device. close: %s\n", strerror(errno));
        return;
    }
}

static const struct drmdev_interface drmdev_interface = { .open = on_drmdev_open, .close = on_drmdev_close };

bool parse_drm_vout_display(const char *display, int *type_out, int *type_id_out) {
    if (strcmp(display, "HDMI-A-1") == 0) {
        *type_out = DRM_MODE_CONNECTOR_HDMIA;
        *type_id_out = 1;
    } else if (strcmp(display, "HDMI-A-2") == 0) {
        *type_out = DRM_MODE_CONNECTOR_HDMIA;
        *type_id_out = 2;
    } else if (strcmp(display, "DSI-1") == 0) {
        *type_out = DRM_MODE_CONNECTOR_DSI;
        *type_id_out = 1;
    } else if (strcmp(display, "DSI-2") == 0) {
        *type_out = DRM_MODE_CONNECTOR_DSI;
        *type_id_out = 2;
    } else {
        return false;
    }
    return true;
}

static struct drmdev *find_drmdev(struct libseat *libseat) {
    struct drm_connector *connector;
    struct drmdev *drmdev;
    drmDevicePtr devices[64];
    int ok, n_devices;

#ifndef HAVE_LIBSEAT
    ASSERT_EQUALS(libseat, NULL);
#endif

    ok = drmGetDevices2(0, devices, sizeof(devices) / sizeof(*devices));
    if (ok < 0) {
        LOG_ERROR("Could not query DRM device list: %s\n", strerror(-ok));
        return NULL;
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

        drmdev = drmdev_new_from_path(device->nodes[DRM_NODE_PRIMARY], &drmdev_interface, libseat);
        if (drmdev == NULL) {
            LOG_ERROR("Could not create drmdev from device at \"%s\". Continuing.\n", device->nodes[DRM_NODE_PRIMARY]);
            continue;
        }

        for_each_connector_in_drmdev(drmdev, connector) {
            if (connector->variable_state.connection_state == kConnected_DrmConnectionState) {
                if (flutterpi->drm_vout_display != NULL) {
                    // We only want to use the display that was specified on the command line.
                    int expected_type, expected_type_id;
                    if (!parse_drm_vout_display(flutterpi->drm_vout_display, &expected_type, &expected_type_id)) {
                        continue;
                    }

                    if (connector->type == expected_type && connector->type_id == expected_type_id) {
                        goto found_connected_connector;
                    } else {
                        continue; 
                    }
                } else {
                    goto found_connected_connector;
                }
            }
        }
        LOG_ERROR("Device \"%s\" doesn't have a display connected. Skipping.\n", device->nodes[DRM_NODE_PRIMARY]);
        drmdev_unref(drmdev);
        continue;

found_connected_connector:
        break;
    }

    drmFreeDevices(devices, n_devices);

    if (drmdev == NULL) {
        LOG_ERROR(
            "flutter-pi couldn't find a usable DRM device.\n"
            "Please make sure you've enabled the Fake-KMS driver in raspi-config.\n"
            "If you're not using a Raspberry Pi, please make sure there's KMS support for your graphics chip.\n"
        );
        goto fail_free_devices;
    }

    return drmdev;

fail_free_devices:
    drmFreeDevices(devices, n_devices);
    return NULL;
}

static struct gbm_device *open_rendernode_as_gbm_device() {
    struct gbm_device *gbm;
    drmDevicePtr devices[64];
    int ok, n_devices;

    ok = drmGetDevices2(0, devices, sizeof(devices) / sizeof(*devices));
    if (ok < 0) {
        LOG_ERROR("Could not query DRM device list: %s\n", strerror(-ok));
        return NULL;
    }

    n_devices = ok;

    // find a GPU that has a primary node
    gbm = NULL;
    for (int i = 0; i < n_devices; i++) {
        drmDevicePtr device;

        device = devices[i];

        if (!(device->available_nodes & (1 << DRM_NODE_RENDER))) {
            // We need a primary node.
            continue;
        }

        int fd = open(device->nodes[DRM_NODE_RENDER], O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            LOG_ERROR("Could not open render node \"%s\". open: %s. Continuing.\n", device->nodes[DRM_NODE_RENDER], strerror(errno));
            continue;
        }

        gbm = gbm_create_device(fd);
        if (gbm == NULL) {
            LOG_ERROR("Could not create gbm device from render node \"%s\". Continuing.\n", device->nodes[DRM_NODE_RENDER]);
            close(fd);
            continue;
        }

        break;
    }

    drmFreeDevices(devices, n_devices);

    if (gbm == NULL) {
        LOG_ERROR(
            "flutter-pi couldn't find a usable render device.\n"
            "Please make sure you have a GPU connected.\n"
        );
        return NULL;
    }

    return gbm;
}

#ifdef HAVE_LIBSEAT
static void on_session_enable(struct libseat *seat, void *userdata) {
    struct flutterpi *fpi;
    int ok;

    ASSERT_NOT_NULL(seat);
    ASSERT_NOT_NULL(userdata);
    fpi = userdata;
    (void) fpi;
    (void) seat;

    /// TODO: Implement
    LOG_DEBUG("on_session_enable\n");

    if (fpi->user_input != NULL) {
        ok = user_input_resume(fpi->user_input);
        if (ok != 0) {
            LOG_ERROR("Couldn't resume user input handling.\n");
        }
    }

    // if (fpi->drmdev != NULL) {
    //     ok = drmdev_resume(fpi->drmdev);
    //     if (ok != 0) {
    //         LOG_ERROR("Couldn't resume drmdev.\n");
    //     }
    // }

    fpi->session_active = true;
}

static void on_session_disable(struct libseat *seat, void *userdata) {
    struct flutterpi *fpi;

    ASSERT_NOT_NULL(seat);
    ASSERT_NOT_NULL(userdata);
    fpi = userdata;
    (void) fpi;

    /// TODO: Implement
    LOG_DEBUG("on_session_disable\n");

    if (fpi->user_input != NULL) {
        user_input_suspend(fpi->user_input);
    }

    // if (fpi->drmdev != NULL) {
    //     drmdev_suspend(fpi->drmdev);
    // }

    libseat_disable_seat(seat);

    fpi->session_active = false;
}

static int on_libseat_fd_ready(sd_event_source *s, int fd, uint32_t revents, void *userdata) {
    struct flutterpi *fpi;
    int ok;

    ASSERT_NOT_NULL(s);
    ASSERT_NOT_NULL(userdata);
    fpi = userdata;
    (void) s;
    (void) fd;
    (void) revents;

    ok = libseat_dispatch(fpi->libseat, 0);
    if (ok < 0) {
        LOG_ERROR("Couldn't dispatch libseat events. libseat_dispatch: %s\n", strerror(errno));
    }

    return 0;
}
#endif

struct flutterpi *flutterpi_new_from_args(int argc, char **argv) {
    enum flutter_runtime_mode runtime_mode;
    enum renderer_type renderer_type;
    struct texture_registry *texture_registry;
    struct plugin_registry *plugin_registry;
    struct frame_scheduler *scheduler;
    struct flutter_paths *paths;
    struct view_geometry geometry;
    FlutterEngineAOTData aot_data;
    FlutterEngineResult engine_result;
    struct gl_renderer *gl_renderer;
    struct vk_renderer *vk_renderer;
    struct gbm_device *gbm_device;
    struct user_input *input;
    struct compositor *compositor;
    struct flutterpi *fpi;
    struct sd_event *event_loop;
    struct flutterpi_cmdline_args cmd_args;
    struct libseat *libseat;
    struct locales *locales;
    struct drmdev *drmdev;
    struct tracer *tracer;
    struct window *window;
    void *engine_handle;
    char *bundle_path, **engine_argv, *desired_videomode;
    int ok, engine_argc, wakeup_fd;

    fpi = malloc(sizeof *fpi);
    if (fpi == NULL) {
        return NULL;
    }

    /// TODO: Remove this
    flutterpi = fpi;

    ok = flutterpi_parse_cmdline_args(argc, argv, &cmd_args);
    if (ok == false) {
        goto fail_free_fpi;
    }

    fpi->drm_vout_display = cmd_args.drm_vout_display ? strdup(cmd_args.drm_vout_display) : NULL;

#ifndef HAVE_VULKAN
    if (cmd_args.use_vulkan == true) {
        LOG_ERROR("ERROR: --vulkan was specified, but flutter-pi was built without vulkan support.\n");
        printf("%s", usage);
        return NULL;
    }
#endif

    runtime_mode = cmd_args.has_runtime_mode ? cmd_args.runtime_mode : FLUTTER_RUNTIME_MODE_DEBUG;
    bundle_path = cmd_args.bundle_path;
    engine_argc = cmd_args.engine_argc;
    engine_argv = cmd_args.engine_argv;

#if defined(HAVE_EGL_GLES2) && defined(HAVE_VULKAN)
    renderer_type = cmd_args.use_vulkan ? kVulkan_RendererType : kOpenGL_RendererType;
#elif defined(HAVE_EGL_GLES2) && !defined(HAVE_VULKAN)
    ASSUME(!cmd_args.use_vulkan);
    renderer_type = kOpenGL_RendererType;
#elif !defined(HAVE_EGL_GLES2) && defined(HAVE_VULKAN)
    renderer_type = kVulkan_RendererType;
#else
    #error "At least one of the Vulkan and OpenGL renderer backends must be built."
#endif

    desired_videomode = cmd_args.desired_videomode;

    if (bundle_path == NULL) {
        LOG_ERROR("ERROR: Bundle path does not exist.\n");
        goto fail_free_cmd_args;
    }

    paths = setup_paths(runtime_mode, bundle_path);
    if (paths == NULL) {
        goto fail_free_cmd_args;
    }

    wakeup_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (wakeup_fd < 0) {
        LOG_ERROR("Could not create fd for waking up the main loop. eventfd: %s\n", strerror(errno));
        goto fail_free_paths;
    }

    ok = sd_event_new(&event_loop);
    if (ok < 0) {
        LOG_ERROR("Could not create main event loop. sd_event_new: %s\n", strerror(-ok));
        goto fail_close_wakeup_fd;
    }

    ok = sd_event_add_io(event_loop, NULL, wakeup_fd, EPOLLIN, on_wakeup_main_loop, NULL);
    if (ok < 0) {
        LOG_ERROR("Error adding wakeup callback to main loop. sd_event_add_io: %s\n", strerror(-ok));
        goto fail_unref_event_loop;
    }

#ifdef HAVE_LIBSEAT
    static const struct libseat_seat_listener libseat_interface = { .enable_seat = on_session_enable, .disable_seat = on_session_disable };

    libseat = libseat_open_seat(&libseat_interface, fpi);
    if (libseat == NULL) {
        LOG_DEBUG("Couldn't open libseat. Flutter-pi will run without session switching support. libseat_open_seat: %s\n", strerror(errno));
    }

    if (libseat != NULL) {
        ok = libseat_get_fd(libseat);
        if (ok < 0) {
            LOG_ERROR(
                "Couldn't get an event fd from libseat. Flutter-pi will run without session switching support. libseat_get_fd: %s\n",
                strerror(errno)
            );
            libseat_close_seat(libseat);
            libseat = NULL;
        }
    }

    if (libseat != NULL) {
        ok = sd_event_add_io(event_loop, NULL, ok, EPOLLIN, on_libseat_fd_ready, fpi);
        if (ok < 0) {
            LOG_ERROR(
                "Couldn't listen for libseat events. Flutter-pi will run without session switching support. sd_event_add_io: %s\n",
                strerror(-ok)
            );
            libseat_close_seat(libseat);
            libseat = NULL;
        }
    }

    if (libseat != NULL) {
        libseat_set_log_level(LIBSEAT_LOG_LEVEL_DEBUG);
    }
#else
    libseat = NULL;
#endif

    locales = locales_new();
    if (locales == NULL) {
        LOG_ERROR("Couldn't setup locales.\n");
        goto fail_destroy_libseat;
    }

    locales_print(locales);

    if (cmd_args.dummy_display) {
        drmdev = NULL;

        // for off-screen rendering, we just open the unprivileged /dev/dri/renderD128 (or whatever)
        // render node as a GBM device.
        // There's other ways to get an offscreen EGL display, but we need the gbm_device for other things
        // (e.g. buffer allocating for the video player, so we just do this.)
        gbm_device = open_rendernode_as_gbm_device();
        if (gbm_device == NULL) {
            goto fail_destroy_locales;
        }
    } else {
        drmdev = find_drmdev(libseat);
        if (drmdev == NULL) {
            goto fail_destroy_locales;
        }

        gbm_device = drmdev_get_gbm_device(drmdev);
        if (gbm_device == NULL) {
            LOG_ERROR("Couldn't create GBM device.\n");
            goto fail_destroy_drmdev;
        }
    }

    tracer = tracer_new_with_stubs();
    if (tracer == NULL) {
        LOG_ERROR("Couldn't create event tracer.\n");
        goto fail_destroy_drmdev;
    }

    scheduler = frame_scheduler_new(false, kDoubleBufferedVsync_PresentMode, NULL, NULL);
    if (scheduler == NULL) {
        LOG_ERROR("Couldn't create frame scheduler.\n");
        goto fail_unref_tracer;
    }

    if (renderer_type == kVulkan_RendererType) {
#ifdef HAVE_VULKAN
        gl_renderer = NULL;
        vk_renderer = vk_renderer_new();
        if (vk_renderer == NULL) {
            LOG_ERROR("Couldn't create vulkan renderer.\n");
            ok = EIO;
            goto fail_unref_scheduler;
        }
#else
        UNREACHABLE();
#endif
    } else if (renderer_type == kOpenGL_RendererType) {
#ifdef HAVE_EGL_GLES2
        vk_renderer = NULL;
        gl_renderer = gl_renderer_new_from_gbm_device(tracer, gbm_device, cmd_args.has_pixel_format, cmd_args.pixel_format);
        if (gl_renderer == NULL) {
            LOG_ERROR("Couldn't create EGL/OpenGL renderer.\n");
            ok = EIO;
            goto fail_unref_scheduler;
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
        }
#else
        UNREACHABLE();
#endif
    } else {
        UNREACHABLE();
        goto fail_unref_scheduler;
    }

    if (cmd_args.dummy_display) {
        window = dummy_window_new(
            tracer,
            scheduler,
            renderer_type,
            gl_renderer,
            vk_renderer,
            cmd_args.dummy_display_size,
            cmd_args.has_physical_dimensions,
            cmd_args.physical_dimensions.x,
            cmd_args.physical_dimensions.y,
            60.0
        );
    } else {
        window = kms_window_new(
            // clang-format off
            tracer,
            scheduler,
            renderer_type,
            gl_renderer,
            vk_renderer,
            cmd_args.has_rotation,
            cmd_args.rotation == 0   ? PLANE_TRANSFORM_ROTATE_0   :
                cmd_args.rotation == 90  ? PLANE_TRANSFORM_ROTATE_90  :
                cmd_args.rotation == 180 ? PLANE_TRANSFORM_ROTATE_180 :
                cmd_args.rotation == 270 ? PLANE_TRANSFORM_ROTATE_270 :
                (assert(0 && "invalid rotation"), PLANE_TRANSFORM_ROTATE_0),
            cmd_args.has_orientation, cmd_args.orientation,
            cmd_args.has_physical_dimensions, cmd_args.physical_dimensions.x, cmd_args.physical_dimensions.y,
            cmd_args.has_pixel_format, cmd_args.pixel_format,
            drmdev,
            desired_videomode
            // clang-format on
        );
        if (window == NULL) {
            LOG_ERROR("Couldn't create KMS window.\n");
            goto fail_unref_renderer;
        }
    }

    compositor = compositor_new(tracer, window);
    if (compositor == NULL) {
        LOG_ERROR("Couldn't create compositor.\n");
        goto fail_unref_window;
    }

    /// TODO: Do we really need the window after this?
    if (drmdev != NULL) {
        ok = sd_event_add_io(event_loop, NULL, drmdev_get_event_fd(drmdev), EPOLLIN | EPOLLHUP | EPOLLPRI, on_drmdev_ready, drmdev);
        if (ok < 0) {
            LOG_ERROR("Could not add DRM pageflip event listener. sd_event_add_io: %s\n", strerror(-ok));
            goto fail_unref_compositor;
        }
    }

    compositor_get_view_geometry(compositor, &geometry);

    static const struct user_input_interface user_input_interface = {
        .on_flutter_pointer_event = on_flutter_pointer_event,
        .on_utf8_character = on_utf8_character,
        .on_xkb_keysym = on_xkb_keysym,
        .on_gtk_keyevent = on_gtk_keyevent,
        .on_set_cursor_enabled = on_set_cursor_enabled,
        .on_move_cursor = on_move_cursor,
        .open = on_user_input_open,
        .close = on_user_input_close,
        .on_switch_vt = on_switch_vt,
        .on_key_event = NULL,
    };

    fpi->libseat = libseat;
    list_inithead(&fpi->fd_for_device_id);

    input = user_input_new(
        &user_input_interface,
        fpi,
        &geometry.display_to_view_transform,
        &geometry.view_to_display_transform,
        geometry.display_size.x,
        geometry.display_size.y
    );
    if (input == NULL) {
        LOG_ERROR("Couldn't initialize user input. flutter-pi will run without user input.\n");
    } else {
        sd_event_source *user_input_event_source;

        ok = sd_event_add_io(
            event_loop,
            &user_input_event_source,
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

        sd_event_source_set_priority(user_input_event_source, SD_EVENT_PRIORITY_IDLE - 10);

        sd_event_source_set_floating(user_input_event_source, true);
        sd_event_source_unref(user_input_event_source);
    }

    engine_handle = load_flutter_engine_lib(paths);
    if (engine_handle == NULL) {
        goto fail_destroy_user_input;
    }

    ok = get_flutter_engine_procs(engine_handle, &fpi->flutter.procs);
    if (ok != 0) {
        goto fail_unload_engine;
    }

    tracer_set_cbs(
        tracer,
        fpi->flutter.procs.TraceEventDurationBegin,
        fpi->flutter.procs.TraceEventDurationEnd,
        fpi->flutter.procs.TraceEventInstant
    );

    plugin_registry = plugin_registry_new(fpi);
    if (plugin_registry == NULL) {
        LOG_ERROR("Could not create plugin registry.\n");
        goto fail_unload_engine;
    }

    ok = plugin_registry_add_plugins_from_static_registry(plugin_registry);
    if (ok != 0) {
        LOG_ERROR("Could not register plugins to plugin registry.\n");
        goto fail_destroy_plugin_registry;
    }

    const struct texture_registry_interface texture_registry_interface = {
        .register_texture = on_register_texture,
        .unregister_texture = on_unregister_texture,
        .mark_frame_available = on_mark_texture_frame_available,
    };

    texture_registry = texture_registry_new(&texture_registry_interface, fpi);
    if (texture_registry == NULL) {
        LOG_ERROR("Could not create texture registry.\n");
        goto fail_destroy_plugin_registry;
    }

    bool engine_is_aot = fpi->flutter.procs.RunsAOTCompiledDartCode();
    if (engine_is_aot == true && !FLUTTER_RUNTIME_MODE_IS_AOT(runtime_mode)) {
        LOG_ERROR(
            "The flutter engine was built for release or profile (AOT) mode, but flutter-pi was not started up in release or profile "
            "mode.\n"
            "Either you swap out the libflutter_engine.so with one that was built for debug mode, or you start"
            "flutter-pi with the --release or --profile flag and make sure a valid \"app.so\" is located inside the asset bundle "
            "directory.\n"
        );
        goto fail_destroy_texture_registry;
    } else if (engine_is_aot == false && FLUTTER_RUNTIME_MODE_IS_AOT(runtime_mode)) {
        LOG_ERROR(
            "The flutter engine was built for debug mode, but flutter-pi was started up in release mode.\n"
            "Either you swap out the libflutter_engine.so with one that was built for release mode,"
            "or you start flutter-pi without the --release flag.\n"
        );
        goto fail_destroy_texture_registry;
    }

    aot_data = NULL;
    if (FLUTTER_RUNTIME_MODE_IS_AOT(runtime_mode)) {
        FlutterEngineAOTDataSource aot_source = { .elf_path = paths->app_elf_path, .type = kFlutterEngineAOTDataSourceTypeElfPath };

        engine_result = fpi->flutter.procs.CreateAOTData(&aot_source, &aot_data);
        if (engine_result != kSuccess) {
            LOG_ERROR("Could not load AOT data. FlutterEngineCreateAOTData: %s\n", FLUTTER_RESULT_TO_STRING(engine_result));
            goto fail_destroy_texture_registry;
        }
    }

    // We don't need these anymore.
    frame_scheduler_unref(scheduler);
    window_unref(window);

    pthread_mutex_init(&fpi->event_loop_mutex, get_default_mutex_attrs());
    fpi->event_loop_thread = pthread_self();
    fpi->wakeup_event_loop_fd = wakeup_fd;
    fpi->event_loop = event_loop;
    fpi->locales = locales;
    fpi->tracer = tracer;
    fpi->compositor = compositor;
    fpi->gl_renderer = gl_renderer;
    fpi->vk_renderer = vk_renderer;
    fpi->user_input = input;
    fpi->flutter.runtime_mode = runtime_mode;
    fpi->flutter.bundle_path = realpath(bundle_path, NULL);
    fpi->flutter.engine_argc = engine_argc;
    fpi->flutter.engine_argv = engine_argv;
    fpi->flutter.paths = paths;
    fpi->flutter.engine_handle = engine_handle;
    fpi->flutter.aot_data = aot_data;
    fpi->drmdev = drmdev;
    fpi->plugin_registry = plugin_registry;
    fpi->texture_registry = texture_registry;
    fpi->libseat = libseat;
    return fpi;

fail_destroy_texture_registry:
    texture_registry_destroy(texture_registry);

fail_destroy_plugin_registry:
    plugin_registry_destroy(plugin_registry);

fail_unload_engine:
    unload_flutter_engine_lib(engine_handle);

fail_destroy_user_input:
    user_input_destroy(input);

fail_unref_compositor:
    compositor_unref(compositor);

fail_unref_window:
    window_unref(window);

fail_unref_renderer:
    if (gl_renderer) {
#ifdef HAVE_EGL_GLES2
        gl_renderer_unref(gl_renderer);
#else
        UNREACHABLE();
#endif
    }
    if (vk_renderer) {
#ifdef HAVE_VULKAN
        vk_renderer_unref(vk_renderer);
#else
        UNREACHABLE();
#endif
    }

fail_unref_scheduler:
    frame_scheduler_unref(scheduler);

fail_unref_tracer:
    tracer_unref(tracer);

fail_destroy_drmdev:
    drmdev_unref(drmdev);

fail_destroy_locales:
    locales_destroy(locales);

fail_destroy_libseat:
    if (libseat != NULL) {
#ifdef HAVE_LIBSEAT
        libseat_close_seat(libseat);
#else
        UNREACHABLE();
#endif
    }

fail_unref_event_loop:
    sd_event_unrefp(&event_loop);

fail_close_wakeup_fd:
    close(wakeup_fd);

fail_free_paths:
    flutter_paths_free(paths);

fail_free_cmd_args:
    free(cmd_args.bundle_path);

fail_free_fpi:
    free(fpi);

    return NULL;
}

void flutterpi_destroy(struct flutterpi *flutterpi) {
    (void) flutterpi;
    LOG_DEBUG("deinit\n");

    pthread_mutex_destroy(&flutterpi->event_loop_mutex);
    texture_registry_destroy(flutterpi->texture_registry);
    plugin_registry_destroy(flutterpi->plugin_registry);
    unload_flutter_engine_lib(flutterpi->flutter.engine_handle);
    user_input_destroy(flutterpi->user_input);
    compositor_unref(flutterpi->compositor);
    if (flutterpi->gl_renderer) {
#ifdef HAVE_EGL_GLES2
        gl_renderer_unref(flutterpi->gl_renderer);
#else
        UNREACHABLE();
#endif
    }
    if (flutterpi->vk_renderer) {
#ifdef HAVE_VULKAN
        vk_renderer_unref(flutterpi->vk_renderer);
#else
        UNREACHABLE();
#endif
    }
    tracer_unref(flutterpi->tracer);
    drmdev_unref(flutterpi->drmdev);
    locales_destroy(flutterpi->locales);
    if (flutterpi->libseat != NULL) {
#ifdef HAVE_LIBSEAT
        libseat_close_seat(flutterpi->libseat);
#else
        UNREACHABLE();
#endif
    }
    sd_event_unrefp(&flutterpi->event_loop);
    close(flutterpi->wakeup_event_loop_fd);
    flutter_paths_free(flutterpi->flutter.paths);
    free(flutterpi->flutter.bundle_path);
    free(flutterpi);
    return;
}

int flutterpi_app_main(int argc, char **argv) {
    struct flutterpi *flutterpi;
    int ok;

#ifdef ENABLE_MTRACE
    mtrace();
#endif

    flutterpi = flutterpi_new_from_args(argc, argv);
    if (flutterpi == NULL) {
        return EXIT_FAILURE;
    }

    ok = flutterpi_run(flutterpi);
    if (ok != 0) {
        flutterpi_destroy(flutterpi);
        return EXIT_FAILURE;
    }

    flutterpi_destroy(flutterpi);

    return EXIT_SUCCESS;
}
