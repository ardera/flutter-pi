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
#include <compositor.h>
#include <keyboard.h>
#include <user_input.h>
#include <locales.h>
#include <platformchannel.h>
#include <pluginregistry.h>
#include <texture_registry.h>
#include <plugins/text_input.h>
#include <plugins/raw_keyboard.h>

#include <termios.h>
#ifdef ENABLE_MTRACE
#   include <mcheck.h>
#endif

FILE_DESCR("flutter-pi")

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
  --pixelformat <format>     Selects the pixel format to use for the framebuffers.\n\
                             Available pixel formats:\n\
                               RGB565, ARGB8888, XRGB8888, BGRA8888, RGBA8888\n\
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

// If this fails, update the accepted value list for --pixelformat above too.
COMPILE_ASSERT(kCount_PixFmt == 5);

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
    EGLint egl_error;

    (void) userdata;

    eglGetError();

    eglMakeCurrent(flutterpi.egl.display, flutterpi.egl.surface, flutterpi.egl.surface, flutterpi.egl.flutter_render_context);
    if (egl_error = eglGetError(), egl_error != EGL_SUCCESS) {
        LOG_ERROR("Could not make the flutter rendering EGL context current. eglMakeCurrent: 0x%08X\n", egl_error);
        return false;
    }

    return true;
}

/// Called on some flutter internal thread to
/// clear the EGLContext.
static bool on_clear_current(void* userdata) {
    EGLint egl_error;

    (void) userdata;

    eglGetError();

    eglMakeCurrent(flutterpi.egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (egl_error = eglGetError(), egl_error != EGL_SUCCESS) {
        LOG_ERROR("Could not clear the flutter EGL context. eglMakeCurrent: 0x%08X\n", egl_error);
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
    (void) userdata;
    return 0;
}

/// Called on some flutter internal thread when the flutter
/// resource uploading EGLContext should be made current.
static bool on_make_resource_current(void *userdata) {
    EGLint egl_error;

    (void) userdata;

    eglGetError();

    eglMakeCurrent(flutterpi.egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, flutterpi.egl.flutter_resource_uploading_context);
    if (egl_error = eglGetError(), egl_error != EGL_SUCCESS) {
        LOG_ERROR("Could not make the flutter resource uploading EGL context current. eglMakeCurrent: 0x%08X\n", egl_error);
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

    (void) userdata;

    /*
     * The mesa V3D driver reports some OpenGL ES extensions as supported and working
     * even though they aren't. hacked_glGetString is a workaround for this, which will
     * cut out the non-working extensions from the list of supported extensions.
     */

    if (name == NULL)
        return NULL;

    // first detect if we're running on a VideoCore 4 / using the VC4 driver.
    if ((is_VC4 == -1) && (is_VC4 = strcmp(flutterpi.egl.renderer, "VC4 V3D 2.1") == 0)) {
        printf( "detected VideoCore IV as underlying graphics chip, and VC4 as the driver.\n"
                "Reporting modified GL_EXTENSIONS string that doesn't contain non-working extensions.\n");
        is_VC4 = 0;
    }

    // if we do, and the symbol to resolve is glGetString, we return our hacked_glGetString.
    if (is_VC4 && (strcmp(name, "glGetString") == 0))
        return hacked_glGetString;

    if ((address = dlsym(RTLD_DEFAULT, name)) || (address = eglGetProcAddress(name)))
        return address;

    LOG_ERROR("proc_resolver: Could not resolve symbol \"%s\"\n", name);

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

/// Called on the main thread when a new frame request may have arrived.
/// Uses [drmCrtcGetSequence] or [FlutterEngineGetCurrentTime] to complete
/// the frame request.
static int on_execute_frame_request(
    void *userdata
) {
    FlutterEngineResult result;
    struct frame *peek;
    int ok;

    (void) userdata;
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
                ns = flutterpi.flutter.libflutter_engine.FlutterEngineGetCurrentTime();
            }

            result = flutterpi.flutter.libflutter_engine.FlutterEngineOnVsync(
                flutterpi.flutter.engine,
                peek->baton,
                ns,
                ns + (1000000000 / flutterpi.display.refresh_rate)
            );
            if (result != kSuccess) {
                LOG_ERROR("Could not reply to frame request. FlutterEngineOnVsync: %s\n", FLUTTER_RESULT_TO_STRING(result));
                cqueue_unlock(&flutterpi.frame_queue);
                return EIO;
            }

            peek->state = kFrameRendering;
        }
    } else if (ok == EAGAIN) {
        // do nothing
    } else if (ok != 0) {
        LOG_ERROR("Could not get peek of frame queue. cqueue_peek_locked: %s\n", strerror(ok));
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
    struct frame *peek;
    int ok;

    (void) userdata;
    cqueue_lock(&flutterpi.frame_queue);

    ok = cqueue_peek_locked(&flutterpi.frame_queue, (void**) &peek);
    if ((ok == 0) || (ok == EAGAIN)) {
        bool reply_instantly = ok == EAGAIN;

        ok = cqueue_try_enqueue_locked(&flutterpi.frame_queue, &(struct frame) {
            .state = kFramePending,
            .baton = baton
        });
        if (ok != 0) {
            LOG_ERROR("Could not enqueue frame request. cqueue_try_enqueue_locked: %s\n", strerror(ok));
            cqueue_unlock(&flutterpi.frame_queue);
            return;
        }

        if (reply_instantly) {
            flutterpi_post_platform_task(
                on_execute_frame_request,
                NULL
            );
        }
    } else if (ok != 0) {
        LOG_ERROR("Could not get peek of frame queue. cqueue_peek_locked: %s\n", strerror(ok));
    }

    cqueue_unlock(&flutterpi.frame_queue);
}

static FlutterTransformation on_get_transformation(void *userdata) {
    (void) userdata;
    return flutterpi.view.view_to_display_transform;
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

    if (runs_platform_tasks_on_current_thread(NULL)) {
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
    return flutterpi->flutter.asset_bundle_path;
}

/// TODO: Make this refcounted if we're gonna use it from multiple threads.
struct gbm_device *flutterpi_get_gbm_device(struct flutterpi *flutterpi) {
    return flutterpi->gbm.device;
}

EGLDisplay flutterpi_get_egl_display(struct flutterpi *flutterpi) {
    return flutterpi->egl.display;
}

EGLContext flutterpi_create_egl_context(struct flutterpi *flutterpi) {
    EGLContext context;

    pthread_mutex_lock(&flutterpi->egl.temp_context_lock);

    context = eglCreateContext(
        flutterpi->egl.display,
        flutterpi->egl.config,
        flutterpi->egl.temp_context,
        (EGLint[]) {
            EGL_CONTEXT_CLIENT_VERSION, 2,
            EGL_NONE
        }
    );
    if (context == EGL_NO_CONTEXT) {
        LOG_ERROR("Could not create new EGL context from temp context. eglCreateContext: %" PRId32 "\n", eglGetError());
        goto fail_unlock_mutex;
    }

    pthread_mutex_unlock(&flutterpi->egl.temp_context_lock);

    return context;

    fail_unlock_mutex:
    pthread_mutex_unlock(&flutterpi->egl.temp_context_lock);
    return EGL_NO_CONTEXT;
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
    (void) userdata;
    return pthread_equal(pthread_self(), flutterpi.event_loop_thread) != 0;
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
/// Called on the main thread when a pageflip ocurred.
void on_pageflip_event(
    int fd,
    unsigned int frame,
    unsigned int sec,
    unsigned int usec,
    void *userdata
) {
    FlutterEngineResult result;
    struct frame presented_frame, *peek;
    int ok;

    (void) fd;
    (void) frame;
    (void) userdata;

    flutterpi.flutter.libflutter_engine.FlutterEngineTraceEventInstant("pageflip");

    cqueue_lock(&flutterpi.frame_queue);

    ok = cqueue_try_dequeue_locked(&flutterpi.frame_queue, &presented_frame);
    if (ok != 0) {
        LOG_ERROR("Could not dequeue completed frame from frame queue: %s\n", strerror(ok));
        goto fail_unlock_frame_queue;
    }

    ok = cqueue_peek_locked(&flutterpi.frame_queue, (void**) &peek);
    if (ok == EAGAIN) {
        // no frame queued after the one that was completed right now.
        // do nothing here.
    } else if (ok != 0) {
        LOG_ERROR("Could not get frame queue peek. cqueue_peek_locked: %s\n", strerror(ok));
        goto fail_unlock_frame_queue;
    } else {
        if (peek->state == kFramePending) {
            uint64_t ns = (sec * 1000000000ll) + (usec * 1000ll);

            result = flutterpi.flutter.libflutter_engine.FlutterEngineOnVsync(
                flutterpi.flutter.engine,
                peek->baton,
                ns,
                ns + (1000000000ll / flutterpi.display.refresh_rate)
            );
            if (result != kSuccess) {
                LOG_ERROR("Could not reply to frame request. FlutterEngineOnVsync: %s\n", FLUTTER_RESULT_TO_STRING(result));
                goto fail_unlock_frame_queue;
            }

            peek->state = kFrameRendering;
        } else {
            LOG_ERROR("frame queue in inconsistent state. aborting\n");
            abort();
        }
    }

    cqueue_unlock(&flutterpi.frame_queue);

    ok = compositor_on_page_flip(sec, usec);
    if (ok != 0) {
        LOG_ERROR("Error notifying compositor about page flip. compositor_on_page_flip: %s\n", strerror(ok));
    }

    return;


    fail_unlock_frame_queue:
    cqueue_unlock(&flutterpi.frame_queue);
}

static int on_drm_fd_ready(sd_event_source *s, int fd, uint32_t revents, void *userdata) {
    int ok;

    (void) s;
    (void) revents;
    (void) userdata;

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

    if (flutterpi.view.rotation == 0) {
        flutterpi.view.view_to_display_transform = FLUTTER_TRANSLATION_TRANSFORMATION(0, 0);

        flutterpi.view.display_to_view_transform = FLUTTER_TRANSLATION_TRANSFORMATION(0, 0);
    } else if (flutterpi.view.rotation == 90) {
        flutterpi.view.view_to_display_transform = FLUTTER_ROTZ_TRANSFORMATION(90);
        flutterpi.view.view_to_display_transform.transX = flutterpi.display.width;

        flutterpi.view.display_to_view_transform = FLUTTER_ROTZ_TRANSFORMATION(-90);
        flutterpi.view.display_to_view_transform.transY = flutterpi.display.width;
    } else if (flutterpi.view.rotation == 180) {
        flutterpi.view.view_to_display_transform = FLUTTER_ROTZ_TRANSFORMATION(180);
        flutterpi.view.view_to_display_transform.transX = flutterpi.display.width;
        flutterpi.view.view_to_display_transform.transY = flutterpi.display.height;

        flutterpi.view.display_to_view_transform = FLUTTER_ROTZ_TRANSFORMATION(-180);
        flutterpi.view.display_to_view_transform.transX = flutterpi.display.width;
        flutterpi.view.display_to_view_transform.transY = flutterpi.display.height;
    } else if (flutterpi.view.rotation == 270) {
        flutterpi.view.view_to_display_transform = FLUTTER_ROTZ_TRANSFORMATION(270);
        flutterpi.view.view_to_display_transform.transY = flutterpi.display.height;

        flutterpi.view.display_to_view_transform = FLUTTER_ROTZ_TRANSFORMATION(-270);
        flutterpi.view.display_to_view_transform.transX = flutterpi.display.height;
    }

    if (flutterpi.user_input != NULL) {
        // update the user input with the new transforms
        user_input_set_transform(
            flutterpi.user_input,
            &flutterpi.view.display_to_view_transform,
            &flutterpi.view.view_to_display_transform,
            flutterpi.display.width,
            flutterpi.display.height
        );
    }

    return 0;
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

static int load_egl_gl_procs(void) {
	LOAD_EGL_PROC(flutterpi, getPlatformDisplay, eglGetPlatformDisplayEXT);
	LOAD_EGL_PROC(flutterpi, createPlatformWindowSurface, eglCreatePlatformWindowSurface);
	LOAD_EGL_PROC(flutterpi, createPlatformPixmapSurface, eglCreatePlatformPixmapSurface);
	flutterpi.egl.createDRMImageMESA = (PFNEGLCREATEDRMIMAGEMESAPROC) eglGetProcAddress("eglCreateDRMImageMESA");
	flutterpi.egl.exportDRMImageMESA = (PFNEGLEXPORTDRMIMAGEMESAPROC) eglGetProcAddress("eglExportDRMImageMESA");
	flutterpi.gl.EGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC) eglGetProcAddress("glEGLImageTargetTexture2DOES");
	flutterpi.gl.EGLImageTargetRenderbufferStorageOES = (PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC) eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES");
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
    int ok, num_devices;

    /**********************
     * DRM INITIALIZATION *
     **********************/

    num_devices = drmGetDevices2(0, devices, sizeof(devices)/sizeof(*devices));
    if (num_devices < 0) {
        LOG_ERROR("Could not query DRM device list: %s\n", strerror(-num_devices));
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
            LOG_ERROR("Could not create drmdev from device at \"%s\". Continuing.\n", device->nodes[DRM_NODE_PRIMARY]);
            continue;
        }

        break;
    }

    if (flutterpi.drm.drmdev == NULL) {
        LOG_ERROR("flutter-pi couldn't find a usable DRM device.\n"
                  "Please make sure you've enabled the Fake-KMS driver in raspi-config.\n"
                  "If you're not using a Raspberry Pi, please make sure there's KMS support for your graphics chip.\n");
        return ENOENT;
    }

    // find a connected connector
    for_each_connector_in_drmdev(flutterpi.drm.drmdev, connector) {
        if (connector->connector->connection == DRM_MODE_CONNECTED) {
            // only update the physical size of the display if the values
            //   are not yet initialized / not set with a commandline option
            if ((flutterpi.display.width_mm == 0) || (flutterpi.display.height_mm == 0)) {
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
            }

            break;
        }
    }

    if (connector == NULL) {
        LOG_ERROR("Could not find a connected connector!\n");
        return EINVAL;
    }

    // Find the preferred mode (GPU drivers _should_ always supply a preferred mode, but of course, they don't)
    // Alternatively, find the mode with the highest width*height. If there are multiple modes with the same w*h,
    // prefer higher refresh rates. After that, prefer progressive scanout modes.
    mode = NULL;
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
        LOG_ERROR("Could not find a preferred output mode!\n");
        return EINVAL;
    }

    flutterpi.display.width = mode->hdisplay;
    flutterpi.display.height = mode->vdisplay;
    flutterpi.display.refresh_rate = mode->vrefresh;

    if ((flutterpi.display.width_mm == 0) || (flutterpi.display.height_mm == 0)) {
        LOG_ERROR("WARNING: display didn't provide valid physical dimensions. The device-pixel ratio will default to 1.0, which may not be the fitting device-pixel ratio for your display.\n");
        flutterpi.display.pixel_ratio = 1.0;
    } else {
        flutterpi.display.pixel_ratio = (10.0 * flutterpi.display.width) / (flutterpi.display.width_mm * 38.0);

        int horizontal_dpi = (int) (flutterpi.display.width / (flutterpi.display.width_mm / 25.4));
        int vertical_dpi = (int) (flutterpi.display.height / (flutterpi.display.height_mm / 25.4));

        if (horizontal_dpi != vertical_dpi) {
                // See https://github.com/flutter/flutter/issues/71865 for current status of this issue.
            LOG_ERROR("WARNING: display has non-square pixels. Non-square-pixels are not supported by flutter.\n");
        }
    }

    for_each_encoder_in_drmdev(flutterpi.drm.drmdev, encoder) {
        if (encoder->encoder->encoder_id == connector->connector->encoder_id) {
            break;
        }
    }

    if (encoder == NULL) {
        for (int i = 0; i < connector->connector->count_encoders; i++, encoder = NULL) {
            for_each_encoder_in_drmdev(flutterpi.drm.drmdev, encoder) {
                if (encoder->encoder->encoder_id == connector->connector->encoders[i]) {
                    break;
                }
            }

            if (encoder->encoder->possible_crtcs) {
                // only use this encoder if there's a crtc we can use with it
                break;
            }
        }
    }

    if (encoder == NULL) {
        LOG_ERROR("Could not find a suitable DRM encoder.\n");
        return EINVAL;
    }

    for_each_crtc_in_drmdev(flutterpi.drm.drmdev, crtc) {
        if (crtc->crtc->crtc_id == encoder->encoder->crtc_id) {
            break;
        }
    }

    if (crtc == NULL) {
        for_each_crtc_in_drmdev(flutterpi.drm.drmdev, crtc) {
            if (encoder->encoder->possible_crtcs & crtc->bitmask) {
                // find a CRTC that is possible to use with this encoder
                break;
            }
        }
    }

    if (crtc == NULL) {
        LOG_ERROR("Could not find a suitable DRM CRTC.\n");
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
                LOG_ERROR("WARNING: Error getting last vblank timestamp. drmCrtcGetSequence: %s\n", strerror(_errno));
            } else {
                LOG_ERROR("WARNING: Kernel didn't return a valid vblank timestamp. (timestamp == 0)\n");
            }
            LOG_ERROR("VSync will be disabled.\nSee https://github.com/ardera/flutter-pi/issues/38 for more info.\n");
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
        LOG_ERROR("Could not add DRM pageflip event listener. sd_event_add_io: %s\n", strerror(-ok));
        return -ok;
    }

    locales_print(flutterpi.locales);
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
        LOG_ERROR("Could not load EGL / GL ES procedure addresses! error: %s\n", strerror(ok));
        return ok;
    }

    eglGetError();

#ifdef EGL_KHR_platform_gbm
    flutterpi.egl.display = flutterpi.egl.getPlatformDisplay(EGL_PLATFORM_GBM_KHR, flutterpi.gbm.device, NULL);
    if ((egl_error = eglGetError()) != EGL_SUCCESS) {
        LOG_ERROR("Could not get EGL display! eglGetPlatformDisplay: 0x%08X\n", egl_error);
        return EIO;
    }
#else
    flutterpi.egl.display = eglGetDisplay((void*) flutterpi.gbm.device);
    if ((egl_error = eglGetError()) != EGL_SUCCESS) {
        LOG_ERROR("Could not get EGL display! eglGetDisplay: 0x%08X\n", egl_error);
        return EIO;
    }
#endif

    eglInitialize(flutterpi.egl.display, &major, &minor);
    if ((egl_error = eglGetError()) != EGL_SUCCESS) {
        LOG_ERROR("Failed to initialize EGL! eglInitialize: 0x%08X\n", egl_error);
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
        LOG_ERROR("Failed to bind OpenGL ES API! eglBindAPI: 0x%08X\n", egl_error);
        return EIO;
    }

    EGLint count = 0, matched = 0;
    EGLConfig *configs;
    bool _found_matching_config = false;

    eglGetConfigs(flutterpi.egl.display, NULL, 0, &count);
    if ((egl_error = eglGetError()) != EGL_SUCCESS) {
        LOG_ERROR("Could not get the number of EGL framebuffer configurations. eglGetConfigs: 0x%08X\n", egl_error);
        return EIO;
    }

    configs = malloc(count * sizeof(EGLConfig));
    if (!configs) return ENOMEM;

    eglChooseConfig(flutterpi.egl.display, config_attribs, configs, count, &matched);
    if ((egl_error = eglGetError()) != EGL_SUCCESS) {
        LOG_ERROR("Could not query EGL framebuffer configurations with fitting attributes. eglChooseConfig: 0x%08X\n", egl_error);
        return EIO;
    }

    if (matched == 0) {
        LOG_ERROR("No fitting EGL framebuffer configuration found.\n");
        return EIO;
    }

    for (int i = 0; i < count; i++) {
        EGLint native_visual_id;

        eglGetConfigAttrib(flutterpi.egl.display, configs[i], EGL_NATIVE_VISUAL_ID, &native_visual_id);
        if ((egl_error = eglGetError()) != EGL_SUCCESS) {
            LOG_ERROR("Could not query native visual ID of EGL config. eglGetConfigAttrib: 0x%08X\n", egl_error);
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
        LOG_ERROR("Could not find EGL framebuffer configuration with appropriate attributes & native visual ID.\n");
        return EIO;
    }

    /****************************
     * OPENGL ES INITIALIZATION *
     ****************************/
    flutterpi.egl.root_context = eglCreateContext(flutterpi.egl.display, flutterpi.egl.config, EGL_NO_CONTEXT, context_attribs);
    if (flutterpi.egl.root_context == EGL_NO_CONTEXT) {
        LOG_ERROR("Could not create OpenGL ES root context. eglCreateContext: 0x%08X\n", egl_error);
        return EIO;
    }

    flutterpi.egl.flutter_render_context = eglCreateContext(flutterpi.egl.display, flutterpi.egl.config, flutterpi.egl.root_context, context_attribs);
    if (flutterpi.egl.flutter_render_context == EGL_NO_CONTEXT) {
        LOG_ERROR("Could not create OpenGL ES context for flutter rendering. eglCreateContext: 0x%08X\n", egl_error);
        return EIO;
    }

    flutterpi.egl.flutter_resource_uploading_context = eglCreateContext(flutterpi.egl.display, flutterpi.egl.config, flutterpi.egl.root_context, context_attribs);
    if (flutterpi.egl.flutter_resource_uploading_context == EGL_NO_CONTEXT) {
        LOG_ERROR("Could not create OpenGL ES context for flutter resource uploads. eglCreateContext: 0x%08X\n", egl_error);
        return EIO;
    }

    flutterpi.egl.compositor_context = eglCreateContext(flutterpi.egl.display, flutterpi.egl.config, flutterpi.egl.root_context, context_attribs);
    if (flutterpi.egl.compositor_context == EGL_NO_CONTEXT) {
        LOG_ERROR("Could not create OpenGL ES context for compositor. eglCreateContext: 0x%08X\n", egl_error);
        return EIO;
    }

    pthread_mutex_init(&flutterpi.egl.temp_context_lock, NULL);

    flutterpi.egl.temp_context = eglCreateContext(flutterpi.egl.display, flutterpi.egl.config, flutterpi.egl.root_context, context_attribs);
    if (flutterpi.egl.temp_context == EGL_NO_CONTEXT) {
        LOG_ERROR("Could not create OpenGL ES context for creating new contexts. eglCreateContext: 0x%08X\n", egl_error);
        return EIO;
    }

    flutterpi.egl.surface = eglCreateWindowSurface(flutterpi.egl.display, flutterpi.egl.config, (EGLNativeWindowType) flutterpi.gbm.surface, NULL);
    if ((egl_error = eglGetError()) != EGL_SUCCESS) {
        LOG_ERROR("Could not create EGL window surface. eglCreateWindowSurface: 0x%08X\n", egl_error);
        return EIO;
    }

    eglMakeCurrent(flutterpi.egl.display, flutterpi.egl.surface, flutterpi.egl.surface, flutterpi.egl.root_context);
    if ((egl_error = eglGetError()) != EGL_SUCCESS) {
        LOG_ERROR("Could not make OpenGL ES root context current to get OpenGL information. eglMakeCurrent: 0x%08X\n", egl_error);
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
        LOG_ERROR("Could not clear OpenGL ES context. eglMakeCurrent: 0x%08X\n", egl_error);
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
    FlutterEngineAOTDataSource aot_source;
    struct libflutter_engine *libflutter_engine;
    struct texture_registry *texture_registry;
    struct plugin_registry *plugin_registry;
    FlutterRendererConfig renderer_config = {0};
    FlutterEngineAOTData aot_data;
    FlutterEngineResult engine_result;
    FlutterProjectArgs project_args = {0};
    void *libflutter_engine_handle;
    int ok;

    libflutter_engine_handle = NULL;
    if (flutterpi.flutter.runtime_mode == kRelease) {
        libflutter_engine_handle = dlopen("libflutter_engine.so.release", RTLD_LOCAL | RTLD_NOW);
        if (libflutter_engine_handle == NULL) {
            LOG_ERROR("[flutter-pi] Warning: Could not load libflutter_engine.so.release: %s. Trying to open libflutter_engine.so...\n", dlerror());
        }
    } else if (flutterpi.flutter.runtime_mode == kDebug) {
        libflutter_engine_handle = dlopen("libflutter_engine.so.debug", RTLD_LOCAL | RTLD_NOW);
        if (libflutter_engine_handle == NULL) {
            LOG_ERROR("[flutter-pi] Warning: Could not load libflutter_engine.so.debug: %s. Trying to open libflutter_engine.so...\n", dlerror());
        }
    }

    if (libflutter_engine_handle == NULL) {
        libflutter_engine_handle = dlopen("libflutter_engine.so", RTLD_LOCAL | RTLD_NOW);
        if (libflutter_engine_handle == NULL) {
            LOG_ERROR("Could not load libflutter_engine.so. dlopen: %s", dlerror());
            LOG_ERROR("Could not find a fitting libflutter_engine.\n");
            return EINVAL;
        }
    }

    libflutter_engine = &flutterpi.flutter.libflutter_engine;

#	define LOAD_LIBFLUTTER_ENGINE_PROC(name) \
        do { \
            libflutter_engine->name = dlsym(libflutter_engine_handle, #name); \
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
        .vsync_callback = NULL /* on_frame_request - broken since 2.2 */,
        .custom_dart_entrypoint = NULL,
        .custom_task_runners = &(FlutterCustomTaskRunners) {
            .struct_size = sizeof(FlutterCustomTaskRunners),
            .platform_task_runner = &(FlutterTaskRunnerDescription) {
                .struct_size = sizeof(FlutterTaskRunnerDescription),
                .user_data = NULL,
                .runs_task_on_current_thread_callback = runs_platform_tasks_on_current_thread,
                .post_task_callback = on_post_flutter_task
            },
            .render_task_runner = NULL,
            .thread_priority_setter = NULL
        },
        .shutdown_dart_vm_when_done = true,
        .compositor = &flutter_compositor,
        .dart_old_gen_heap_size = -1,
        .compute_platform_resolved_locale_callback = NULL,
        .dart_entrypoint_argc = 0,
        .dart_entrypoint_argv = NULL,
        .log_message_callback = NULL,
        .log_tag = NULL
    };

    bool engine_is_aot = libflutter_engine->FlutterEngineRunsAOTCompiledDartCode();
    if ((engine_is_aot == true) && (flutterpi.flutter.runtime_mode != kRelease)) {
        LOG_ERROR(
            "The flutter engine was built for release (AOT) mode, but flutter-pi was not started up in release mode.\n"
            "Either you swap out the libflutter_engine.so with one that was built for debug mode, or you start"
            "flutter-pi with the --release flag and make sure a valid \"app.so\" is located inside the asset bundle directory.\n"
        );
        return EINVAL;
    } else if ((engine_is_aot == false) && (flutterpi.flutter.runtime_mode != kDebug)) {
        LOG_ERROR(
            "The flutter engine was built for debug mode, but flutter-pi was started up in release mode.\n"
            "Either you swap out the libflutter_engine.so with one that was built for release mode,"
            "or you start flutter-pi without the --release flag.\n"
        );
        return EINVAL;
    }

    if (flutterpi.flutter.runtime_mode == kRelease) {
        aot_source = (FlutterEngineAOTDataSource) {
            .elf_path = flutterpi.flutter.app_elf_path,
            .type = kFlutterEngineAOTDataSourceTypeElfPath
        };

        engine_result = libflutter_engine->FlutterEngineCreateAOTData(&aot_source, &aot_data);
        if (engine_result != kSuccess) {
            LOG_ERROR("Could not load AOT data. FlutterEngineCreateAOTData: %s\n", FLUTTER_RESULT_TO_STRING(engine_result));
            return EIO;
        }

        project_args.aot_data = aot_data;
    }

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
            .refresh_rate = flutterpi.display.refresh_rate
        },
        1
    );
    if (engine_result != kSuccess) {
        LOG_ERROR("Could not send display update to flutter engine. FlutterEngineNotifyDisplayUpdate: %s\n", FLUTTER_RESULT_TO_STRING(engine_result));
        return EINVAL;
    }

    // update window size
    engine_result = libflutter_engine->FlutterEngineSendWindowMetricsEvent(
        flutterpi.flutter.engine,
        &(FlutterWindowMetricsEvent) {
            .struct_size = sizeof(FlutterWindowMetricsEvent),
            .width = flutterpi.view.width,
            .height = flutterpi.view.height,
            .pixel_ratio = flutterpi.display.pixel_ratio,
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

    flutterpi = userdata;

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

    ok = compositor_apply_cursor_state(
        enabled,
        flutterpi->view.rotation,
        flutterpi->display.pixel_ratio
    );
    if (ok != 0) {
        LOG_ERROR("Error enabling / disabling mouse cursor. compositor_apply_cursor_state: %s\n", strerror(ok));
    }
}

static void on_move_cursor(void *userdata, unsigned int x, unsigned int y) {
    struct flutterpi *flutterpi;
    int ok;

    flutterpi = userdata;
    (void) flutterpi;

    ok = compositor_set_cursor_pos(x, y);
    if (ok != 0) {
        LOG_ERROR("Error moving mouse cursor. compositor_set_cursor_pos: %s\n", strerror(ok));
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

static int on_user_input_fd_ready(sd_event_source *s, int fd, uint32_t revents, void *userdata) {
    struct user_input *input;

    (void) s;
    (void) fd;
    (void) revents;

    input = userdata;

    return user_input_on_fd_ready(input);
}

static int init_user_input(void) {
    struct user_input *input;
    sd_event_source *event_source;
    int ok;

    event_source = NULL;

    input = user_input_new(
        &user_input_interface,
        &flutterpi,
        &flutterpi.view.display_to_view_transform,
        &flutterpi.view.view_to_display_transform,
        flutterpi.display.width,
        flutterpi.display.height
    );
    if (input == NULL) {
        LOG_ERROR("Couldn't initialize user input. flutter-pi will run without user input.\n");
    } else {
        ok = sd_event_add_io(
            flutterpi.event_loop,
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

    flutterpi.user_input = input;
    flutterpi.user_input_event_source = event_source;

    return 0;
}


static bool setup_paths(void) {
    char *kernel_blob_path, *icu_data_path, *app_elf_path;
    #define PATH_EXISTS(path) (access((path),R_OK)==0)

    if (!PATH_EXISTS(flutterpi.flutter.asset_bundle_path)) {
        LOG_ERROR("Asset Bundle Directory \"%s\" does not exist\n", flutterpi.flutter.asset_bundle_path);
        return false;
    }

    asprintf(&kernel_blob_path, "%s/kernel_blob.bin", flutterpi.flutter.asset_bundle_path);
    asprintf(&app_elf_path, "%s/app.so", flutterpi.flutter.asset_bundle_path);

    if (flutterpi.flutter.runtime_mode == kDebug) {
        if (!PATH_EXISTS(kernel_blob_path)) {
            LOG_ERROR("Could not find \"kernel.blob\" file inside \"%s\", which is required for debug mode.\n", flutterpi.flutter.asset_bundle_path);
            return false;
        }
    } else if (flutterpi.flutter.runtime_mode == kRelease) {
        if (!PATH_EXISTS(app_elf_path)) {
            LOG_ERROR("Could not find \"app.so\" file inside \"%s\", which is required for release and profile mode.\n", flutterpi.flutter.asset_bundle_path);
            return false;
        }
    }

    asprintf(&icu_data_path, "/usr/lib/icudtl.dat");
    if (!PATH_EXISTS(icu_data_path)) {
        LOG_ERROR("Could not find \"icudtl.dat\" file inside \"/usr/lib/\".\n");
        return false;
    }

    flutterpi.flutter.kernel_blob_path = kernel_blob_path;
    flutterpi.flutter.icu_data_path = icu_data_path;
    flutterpi.flutter.app_elf_path = app_elf_path;

    return true;

    #undef PATH_EXISTS
}

static bool parse_cmd_args(int argc, char **argv) {
    bool finished_parsing_options;
    int runtime_mode_int = kDebug;
    int longopt_index = 0;
    int opt, ok;

    struct option long_options[] = {
        {"release", no_argument, &runtime_mode_int, kRelease},
        {"input", required_argument, NULL, 'i'},
        {"orientation", required_argument, NULL, 'o'},
        {"rotation", required_argument, NULL, 'r'},
        {"dimensions", required_argument, NULL, 'd'},
        {"help", no_argument, 0, 'h'},
        {"pixelformat", required_argument, NULL, 'p'},
        {0, 0, 0, 0}
    };

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

                flutterpi.view.rotation = rotation;
                flutterpi.view.has_rotation = true;
                break;

            case 'd': ;
                unsigned int width_mm, height_mm;

                ok = sscanf(optarg, "%u,%u", &width_mm, &height_mm);
                if ((ok == 0) || (ok == EOF)) {
                    LOG_ERROR("ERROR: Invalid argument for --dimensions passed.\n%s", usage);
                    return false;
                }

                flutterpi.display.width_mm = width_mm;
                flutterpi.display.height_mm = height_mm;

                break;

            case 'p':
                for (int i = 0; i < n_pixfmt_infos; i++) {
                    if (strcmp(optarg, pixfmt_infos[i].arg_name) == 0) {
                        flutterpi.gbm.format = pixfmt_infos[i].gbm_format;
                        goto valid_format;
                    }
                }

                LOG_ERROR(
                    "ERROR: Invalid argument for --pixelformat passed.\n"
                    "Valid values are: RGB565, ARGB8888, XRGB8888, BGRA8888, RGBA8888\n"
                    "%s",
                    usage
                );

                // Just so we get a compile error when we update the pixel format list
                // but don't update the valid values above.
                COMPILE_ASSERT(kCount_PixFmt == 5);

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

    flutterpi.flutter.asset_bundle_path = realpath(argv[optind], NULL);
    flutterpi.flutter.runtime_mode = runtime_mode_int;

    argv[optind] = argv[0];
    flutterpi.flutter.engine_argc = argc - optind;
    flutterpi.flutter.engine_argv = argv + optind;

    return true;
}

int init(int argc, char **argv) {
    int ok;

#ifdef ENABLE_MTRACE
    mtrace();
#endif

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

    flutterpi.locales = locales_new();
    if (flutterpi.locales == NULL) {
        LOG_ERROR("Couldn't setup locales.\n");
        return EINVAL;
    }

    ok = init_user_input();
    if (ok != 0) {
        return ok;
    }

    ok = init_display();
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

    tcflush(STDIN_FILENO, TCIOFLUSH);
    return EXIT_SUCCESS;
}
