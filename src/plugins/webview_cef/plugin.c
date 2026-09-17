// SPDX-License-Identifier: MIT
/*
 * CEF powered webview plugin
 *
 * The flutter-pi side of the webview: speaks the `webview_cef` pub package's
 * platform channel and turns the BGRA frames CEF renders off-screen into flutter
 * external textures.
 *
 * Two threads meet in here:
 *   - platform channel handlers run on flutter-pi's platform thread,
 *   - every cef_bridge callback runs on CEF's own UI thread (see cef_bridge.h for
 *     why CEF owns that thread rather than borrowing ours).
 *
 * So the webview list is only ever touched from the platform thread -- anything
 * arriving from CEF that has to change it is posted there -- and everything a
 * frame touches on its way to a texture is behind @ref webview_cef_plugin::gl_mutex.
 * Sending a platform message is the one thing safe to do from either thread:
 * flutterpi_send_platform_message posts to the platform thread when it isn't
 * already on it.
 *
 * Copyright (c) 2026, Bojidar Tonchev <bojidar.tonchev@gmail.com>
 */

#define _GNU_SOURCE

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <pthread.h>

#include "flutter-pi.h"
#include "platformchannel.h"
#include "pluginregistry.h"
#include "texture_registry.h"
#include "util/logging.h"

#include "config.h"

#ifndef HAVE_EGL_GLES2
    #error "The webview_cef plugin requires EGL/OpenGL ES support."
#endif

#include "gl_renderer.h"
#include "gles.h"
#include "plugins/webview_cef.h"
#include "plugins/webview_cef/cef_bridge.h"

/// Where the CEF runtime files (libcef.so, icudtl.dat, *.pak, *.bin, locales/)
/// were installed. Overridable at configure time and at runtime, see
/// resolve_env() below.
#ifndef WEBVIEW_CEF_RUNTIME_DIR
    #define WEBVIEW_CEF_RUNTIME_DIR "/usr/lib/cef"
#endif

/// The CEF subprocess helper executable.
#ifndef WEBVIEW_CEF_HELPER_PATH
    #define WEBVIEW_CEF_HELPER_PATH "/usr/bin/flutter-pi-cef-helper"
#endif

#define MAX_SWITCHES 64
#define DEFAULT_FRAME_RATE 30

/// CEF clamps `windowless_frame_rate` to this range.
#define MIN_FRAME_RATE 1
#define MAX_FRAME_RATE 60

/// Traces the browser and frame lifecycle when FLUTTERPI_CEF_TRACE is set.
///
/// Deliberately not LOG_DEBUG: that is compiled out unless flutter-pi itself was
/// built with DEBUG, and a webview that comes up blank is something you have to
/// explain on a release image, in place, without a rebuild.
#define TRACE(...)                                         \
    do {                                                   \
        if (plugin.trace_level >= 1) {                     \
            fprintf(stderr, "[webview_cef] " __VA_ARGS__); \
        }                                                  \
    } while (0)

/// FLUTTERPI_CEF_TRACE=2: every frame, step by step, unbuffered.
///
/// This exists to find out where a frame stopped, on a device with no debugger and
/// no package feed to put one on. Each step of the upload can block -- taking
/// gl_mutex, making the context current, glFinish -- and which one matters, so the
/// last line printed names the call that didn't come back.
#define TRACE2(...)                                        \
    do {                                                   \
        if (plugin.trace_level >= 2) {                     \
            fprintf(stderr, "[webview_cef] " __VA_ARGS__); \
            fflush(stderr);                                \
        }                                                  \
    } while (0)

/// Chromium switches we pass unless FLUTTERPI_CEF_NO_DEFAULT_SWITCHES is set.
static const char *const default_switches[] = {
    // There is no X server and no wayland compositor to talk to, so Chromium has
    // to use the headless ozone platform.
    "ozone-platform=headless",

    // Render through ANGLE on native EGL/GLES, which on a KMS target means Mesa
    // on a render node -- /dev/dri/renderD128 needs no DRM master, so Chromium
    // gets the GPU while flutter-pi keeps card0.
    //
    // Worth being explicit about why this matters, because the failure is silent
    // and expensive: left to itself Chromium ends up on SwiftShader and
    // rasterises WebGL on the CPU. Measured on a Celeron J6412, a WebGL slot
    // game ran at about 3fps with SwiftShader's four worker threads saturating
    // all four cores, against ~3% CPU in the GPU process once Mesa's iris driver
    // was actually being used. Nothing in the page or the plugin looks wrong
    // either way -- it just renders slowly.
    //
    // Necessary but not sufficient: see the EGL_PLATFORM note in
    // ensure_cef_initialized(), without which Mesa goes looking for an X display,
    // fails, and Chromium quietly falls back to SwiftShader anyway.
    "use-gl=angle",
    "use-angle=gl-egl",

    // An embedded GPU won't be on Chromium's list of known-good configurations,
    // and being absent from that list is not the same as being broken.
    "ignore-gpu-blocklist",

    // Only reached if the native EGL above doesn't come up -- no GL driver, no
    // render node. Chromium has refused software WebGL on its own since around
    // M120, so without this flag the fallback isn't "slow WebGL", it is a page
    // that loads completely and draws nothing. The "unsafe" is about running
    // untrusted shaders through a software rasteriser; a kiosk pointed at a known
    // page is the trusted case the flag exists for.
    "enable-unsafe-swiftshader",

    "disable-dev-shm-usage",
    "autoplay-policy=no-user-gesture-required",
};

struct webview {
    struct webview *next;

    struct texture *texture;
    int64_t texture_id;

    /// CEF's browser identifier. This is what the dart side addresses us by.
    int browser_id;

    struct wvcef_browser *browser;

    /// Logical size & scale, as last reported by `setSize`.
    int logical_width, logical_height;
    double pixel_ratio;

    /// The GL texture the CEF frames are uploaded into. Kept for the lifetime of
    /// the webview and re-uploaded in place on every frame.
    GLuint gl_texture;
    int tex_width, tex_height;

    /// Only used if the driver can't take BGRA pixels directly.
    uint8_t *swizzle_buffer;
    size_t swizzle_buffer_size;

    /// Frames CEF has handed us, for TRACE only.
    uint64_t n_frames;

    /// `close` was called; the webview is torn down but still waiting for CEF to
    /// confirm the browser is gone.
    bool closing;
};

struct webview_cef_plugin {
    struct flutterpi *flutterpi;

    EGLDisplay egl_display;
    EGLContext egl_context;
    bool supports_bgra;

    /// FLUTTERPI_CEF_TRACE; see TRACE and TRACE2 above.
    int trace_level;

    /// Guards the EGL context and every webview's texture state.
    ///
    /// Frames arrive on CEF's UI thread while the platform thread may be
    /// tearing the same webview down, and the plugin's EGL context is made
    /// current and released within each use, so it is shareable between threads
    /// -- but only one at a time.
    pthread_mutex_t gl_mutex;

    struct webview *webviews;
};

/// There's one webview plugin per process, and both the platform channel
/// receiver and the CEF callbacks need to reach it, so it's a file-scope
/// singleton rather than something passed around as userdata.
static struct webview_cef_plugin plugin;

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

static const char *resolve_env(const char *env_name, const char *fallback) {
    const char *env = getenv(env_name);

    if (env != NULL && env[0] != '\0') {
        return env;
    }

    return fallback;
}

static long resolve_env_long(const char *env_name, long fallback) {
    const char *env = getenv(env_name);
    char *end;
    long value;

    if (env == NULL || env[0] == '\0') {
        return fallback;
    }

    errno = 0;
    value = strtol(env, &end, 10);
    if (errno != 0 || end == env || *end != '\0') {
        LOG_ERROR("Ignoring %s: \"%s\" is not a number.\n", env_name, env);
        return fallback;
    }

    return value;
}

/// `windowless_frame_rate` for new browsers.
static long resolve_frame_rate(void) {
    long frame_rate = resolve_env_long("FLUTTERPI_CEF_FRAME_RATE", DEFAULT_FRAME_RATE);

    if (frame_rate < MIN_FRAME_RATE) {
        return MIN_FRAME_RATE;
    }
    if (frame_rate > MAX_FRAME_RATE) {
        return MAX_FRAME_RATE;
    }
    return frame_rate;
}

/// The webview_cef dart side passes arguments as a positional list, or as a bare
/// value for the single-argument methods.
static struct std_value *arg_at(struct std_value *args, size_t index) {
    if (args == NULL || !STDVALUE_IS_LIST(*args) || index >= args->size) {
        return NULL;
    }
    return args->list + index;
}

static bool arg_int_at(struct std_value *args, size_t index, int64_t *out) {
    struct std_value *value = arg_at(args, index);

    if (value == NULL || !STDVALUE_IS_INT(*value)) {
        return false;
    }

    *out = STDVALUE_AS_INT(*value);
    return true;
}

static bool arg_num_at(struct std_value *args, size_t index, double *out) {
    struct std_value *value = arg_at(args, index);

    if (value == NULL || !STDVALUE_IS_NUM(*value)) {
        return false;
    }

    *out = (double) STDVALUE_AS_NUM(*value);
    return true;
}

static const char *arg_string_at(struct std_value *args, size_t index) {
    struct std_value *value = arg_at(args, index);

    if (value == NULL || !STDVALUE_IS_STRING(*value)) {
        return NULL;
    }

    return STDVALUE_AS_STRING(*value);
}

static bool arg_bool_at(struct std_value *args, size_t index, bool *out) {
    struct std_value *value = arg_at(args, index);

    if (value == NULL || !STDVALUE_IS_BOOL(*value)) {
        return false;
    }

    *out = STDVALUE_AS_BOOL(*value);
    return true;
}

static struct webview *webview_find(int browser_id) {
    for (struct webview *wv = plugin.webviews; wv != NULL; wv = wv->next) {
        if (wv->browser_id == browser_id) {
            return wv;
        }
    }
    return NULL;
}

static void webview_list_add(struct webview *wv) {
    wv->next = plugin.webviews;
    plugin.webviews = wv;
}

static void webview_list_remove(struct webview *wv) {
    struct webview **slot = &plugin.webviews;

    while (*slot != NULL) {
        if (*slot == wv) {
            *slot = wv->next;
            return;
        }
        slot = &(*slot)->next;
    }
}

static void webview_free(struct webview *wv) {
    webview_list_remove(wv);
    free(wv->swizzle_buffer);
    free(wv);
}

/// Releases the flutter texture and the GL texture. Safe to call twice.
///
/// Platform thread. Takes gl_mutex because a frame may be arriving on CEF's UI
/// thread at the same moment, and it must not find a half-released webview or the
/// EGL context current on another thread.
static void webview_release_textures(struct webview *wv) {
    pthread_mutex_lock(&plugin.gl_mutex);

    if (wv->texture != NULL) {
        texture_destroy(wv->texture);
        wv->texture = NULL;
    }

    if (wv->gl_texture != 0) {
        if (eglMakeCurrent(plugin.egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, plugin.egl_context) == EGL_TRUE) {
            glDeleteTextures(1, &wv->gl_texture);
            eglMakeCurrent(plugin.egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        } else {
            LOG_ERROR("Could not make the webview EGL context current to delete a texture. eglMakeCurrent: 0x%04X\n", eglGetError());
        }
        wv->gl_texture = 0;
    }

    pthread_mutex_unlock(&plugin.gl_mutex);
}

// ---------------------------------------------------------------------------
// frame upload
// ---------------------------------------------------------------------------

static void on_texture_frame_destroy(const struct texture_frame *frame, void *userdata) {
    // The GL texture belongs to the webview and is reused for every frame, so
    // there is nothing to release per frame.
    (void) frame;
    (void) userdata;
}

/// Converts BGRA to RGBA into a scratch buffer, for drivers without
/// GL_EXT_texture_format_BGRA8888.
static const void *swizzle_bgra_to_rgba(struct webview *wv, const void *buffer, int width, int height) {
    const uint8_t *src;
    uint8_t *dst;
    size_t n_pixels, needed;

    n_pixels = (size_t) width * (size_t) height;
    needed = n_pixels * 4;

    if (wv->swizzle_buffer_size < needed) {
        uint8_t *new_buffer = realloc(wv->swizzle_buffer, needed);
        if (new_buffer == NULL) {
            return NULL;
        }
        wv->swizzle_buffer = new_buffer;
        wv->swizzle_buffer_size = needed;
    }

    src = buffer;
    dst = wv->swizzle_buffer;

    for (size_t i = 0; i < n_pixels; i++) {
        dst[i * 4 + 0] = src[i * 4 + 2];
        dst[i * 4 + 1] = src[i * 4 + 1];
        dst[i * 4 + 2] = src[i * 4 + 0];
        dst[i * 4 + 3] = src[i * 4 + 3];
    }

    return dst;
}

/// A frame from CEF, on CEF's UI thread.
///
/// Everything from here to texture_push_frame runs under gl_mutex: the platform
/// thread may be releasing this very webview's textures, and the plugin's EGL
/// context can only be current on one thread at a time.
static void on_paint(void *userdata, const void *buffer, int width, int height) {
    struct webview *wv;
    const void *pixels;
    GLenum gl_format, gl_error;
    EGLBoolean egl_ok;
    bool uploaded = false;

    wv = userdata;

    wv->n_frames++;

    pthread_mutex_lock(&plugin.gl_mutex);

    // The first frames are what you want to see; after that a heartbeat is enough,
    // since 30fps of this would drown out everything else. Under the lock because
    // of wv->texture, which the platform thread clears from under us on close.
    if (wv->n_frames <= 3 || wv->n_frames % 100 == 0) {
        TRACE(
            "paint %" PRIu64 ": browser %d, %dx%d px, texture %" PRId64 "%s\n",
            wv->n_frames,
            wv->browser_id,
            width,
            height,
            wv->texture_id,
            wv->texture == NULL ? " -- DROPPED, no texture" : ""
        );
    }

    // Closed while a frame was in flight.
    if (wv->texture == NULL || width <= 0 || height <= 0) {
        pthread_mutex_unlock(&plugin.gl_mutex);
        return;
    }

    if (plugin.supports_bgra) {
        gl_format = GL_BGRA_EXT;
        pixels = buffer;
    } else {
        gl_format = GL_RGBA;
        pixels = swizzle_bgra_to_rgba(wv, buffer, width, height);
        if (pixels == NULL) {
            LOG_ERROR("Out of memory while converting a webview frame.\n");
            pthread_mutex_unlock(&plugin.gl_mutex);
            return;
        }
    }

    TRACE2("paint %" PRIu64 ": making context current\n", wv->n_frames);

    egl_ok = eglMakeCurrent(plugin.egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, plugin.egl_context);
    if (egl_ok == EGL_FALSE) {
        LOG_ERROR("Could not make the webview EGL context current. eglMakeCurrent: 0x%04X\n", eglGetError());
        pthread_mutex_unlock(&plugin.gl_mutex);
        return;
    }

    TRACE2("paint %" PRIu64 ": context current, uploading %dx%d\n", wv->n_frames, width, height);

    if (wv->gl_texture == 0) {
        glGenTextures(1, &wv->gl_texture);
        if (wv->gl_texture == 0) {
            LOG_ERROR("Could not create a GL texture for the webview. glGenTextures: 0x%04X\n", glGetError());
            goto clear_context;
        }

        glBindTexture(GL_TEXTURE_2D, wv->gl_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        wv->tex_width = 0;
        wv->tex_height = 0;
    } else {
        glBindTexture(GL_TEXTURE_2D, wv->gl_texture);
    }

    if (wv->tex_width != width || wv->tex_height != height) {
        glTexImage2D(GL_TEXTURE_2D, 0, gl_format, width, height, 0, gl_format, GL_UNSIGNED_BYTE, pixels);

        gl_error = glGetError();
        if (gl_error != GL_NO_ERROR) {
            LOG_ERROR("Could not allocate the webview GL texture. glTexImage2D: 0x%04X\n", gl_error);
            glBindTexture(GL_TEXTURE_2D, 0);
            goto clear_context;
        }

        wv->tex_width = width;
        wv->tex_height = height;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, gl_format, GL_UNSIGNED_BYTE, pixels);

        gl_error = glGetError();
        if (gl_error != GL_NO_ERROR) {
            LOG_ERROR("Could not upload a webview frame. glTexSubImage2D: 0x%04X\n", gl_error);
            glBindTexture(GL_TEXTURE_2D, 0);
            goto clear_context;
        }
    }

    glBindTexture(GL_TEXTURE_2D, 0);

    // The flutter rasterizer samples this texture from a different thread and a
    // different (shared) context, so the upload has to be complete before we
    // hand the frame over. glFlush() is the minimum the GLES spec asks for, but
    // in practice only glFinish() is reliable across drivers. If this ever shows
    // up in a profile, an EGL fence sync is the way to relax it.
    TRACE2("paint %" PRIu64 ": uploaded, glFinish\n", wv->n_frames);
    glFinish();
    TRACE2("paint %" PRIu64 ": glFinish returned\n", wv->n_frames);
    uploaded = true;

clear_context:
    eglMakeCurrent(plugin.egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    if (!uploaded) {
        pthread_mutex_unlock(&plugin.gl_mutex);
        return;
    }

    TRACE2("paint %" PRIu64 ": pushing frame\n", wv->n_frames);

    texture_push_frame(
        wv->texture,
        &(struct texture_frame){
            .gl = {
                .target = GL_TEXTURE_2D,
                .name = wv->gl_texture,
                .format = GL_RGBA8_OES,
                .width = (size_t) width,
                .height = (size_t) height,
            },
            .destroy = on_texture_frame_destroy,
            .userdata = NULL,
        }
    );

    pthread_mutex_unlock(&plugin.gl_mutex);
}

// ---------------------------------------------------------------------------
// events towards dart
//
// The webview_cef dart side keys everything off the CEF browser id and reads the
// arguments out of a map.
//
// These all run on CEF's UI thread. That is fine because they pass no response
// callback: platch_send then only encodes into a local buffer and hands it to
// flutterpi_send_platform_message, which copies the message and posts it to the
// platform thread. Passing a callback would take platch_send's response-handle
// path instead, which is documented as not working off the platform thread.
// ---------------------------------------------------------------------------

static void send_event(int browser_id, const char *method, struct std_value *extra_keys, struct std_value *extra_values, size_t n_extra) {
    struct std_value keys[6];
    struct std_value values[6];
    struct std_value event;

    if (n_extra > 5) {
        n_extra = 5;
    }

    keys[0] = STDSTRING("browserId");
    values[0] = STDINT32(browser_id);

    for (size_t i = 0; i < n_extra; i++) {
        keys[i + 1] = extra_keys[i];
        values[i + 1] = extra_values[i];
    }

    event.type = kStdMap;
    event.size = n_extra + 1;
    event.keys = keys;
    event.values = values;

    platch_call_std(WEBVIEW_CEF_CHANNEL, (char *) method, &event, NULL, NULL);
}

static void send_string_event(int browser_id, const char *method, const char *key, const char *value) {
    struct std_value keys[1] = { STDSTRING((char *) key) };
    struct std_value values[1] = { STDSTRING((char *) (value != NULL ? value : "")) };

    send_event(browser_id, method, keys, values, 1);
}

static void on_url_changed(void *userdata, const char *url) {
    struct webview *wv = userdata;
    send_string_event(wv->browser_id, "urlChanged", "url", url);
}

static void on_title_changed(void *userdata, const char *title) {
    struct webview *wv = userdata;
    send_string_event(wv->browser_id, "titleChanged", "title", title);
}

static void on_tooltip(void *userdata, const char *text) {
    struct webview *wv = userdata;
    send_string_event(wv->browser_id, "onTooltip", "text", text);
}

static void on_load_start(void *userdata, const char *url) {
    struct webview *wv = userdata;
    // The dart side really does call this key "urlId".
    send_string_event(wv->browser_id, "onLoadStart", "urlId", url);
}

static void on_load_end(void *userdata, const char *url, int http_status_code) {
    struct webview *wv = userdata;

    (void) http_status_code;
    send_string_event(wv->browser_id, "onLoadEnd", "urlId", url);
}

static void on_load_error(void *userdata, int error_code, const char *error_text, const char *failed_url) {
    struct webview *wv = userdata;

    // The webview_cef dart API has no load error callback, so this is only
    // useful in the log. CEF still calls OnLoadEnd afterwards, which the app
    // does see.
    LOG_ERROR(
        "Webview %d could not load %s: %s (%d)\n",
        wv->browser_id,
        failed_url != NULL ? failed_url : "(?)",
        error_text != NULL ? error_text : "(?)",
        error_code
    );
}

static void on_cursor_changed(void *userdata, int cursor_type) {
    struct webview *wv = userdata;

    struct std_value keys[1] = { STDSTRING("type") };
    struct std_value values[1] = { STDINT32(cursor_type) };

    send_event(wv->browser_id, "onCursorChanged", keys, values, 1);
}

static void on_console_message(void *userdata, int level, const char *message, const char *source, int line) {
    struct webview *wv = userdata;

    struct std_value keys[4] = { STDSTRING("level"), STDSTRING("message"), STDSTRING("source"), STDSTRING("line") };
    struct std_value values[4] = {
        STDINT32(level),
        STDSTRING((char *) (message != NULL ? message : "")),
        STDSTRING((char *) (source != NULL ? source : "")),
        STDINT32(line),
    };

    send_event(wv->browser_id, "onConsoleMessage", keys, values, 4);
}

/// The browser is really gone now, so the webview can be freed. Runs on the
/// platform thread, posted by on_browser_closed.
static int on_browser_closed_on_platform(void *userdata) {
    struct webview *wv = userdata;

    // Releasing the handle is this thread's job precisely because this thread is
    // the only one that reads wv->browser: doing it back on CEF's thread would
    // invalidate the pointer under whatever channel call happens to be in flight.
    if (wv->browser != NULL) {
        wvcef_browser_destroy(wv->browser);
        wv->browser = NULL;
    }

    if (!wv->closing) {
        // CEF closed the browser on its own -- a crashed renderer, most likely.
        // Keep the texture around showing the last frame; the dart side still
        // holds a controller for it and would only get a black rectangle
        // otherwise.
        LOG_ERROR("Webview %d was closed by CEF.\n", wv->browser_id);
        return 0;
    }

    webview_free(wv);
    return 0;
}

/// CEF's UI thread, from inside the browser's own teardown.
///
/// Everything this has to touch -- the webview list, wv->browser, the webview
/// itself -- belongs to the platform thread, so nothing is done here beyond
/// getting the work over there. The webview stays readable until then; the
/// bridge has already made every call on the handle a no-op.
static void on_browser_closed(void *userdata) {
    struct webview *wv = userdata;
    int ok;

    ok = flutterpi_post_platform_task(on_browser_closed_on_platform, wv);
    if (ok != 0) {
        // Nothing sane left to do: freeing it here would race the platform
        // thread, so leak it and say so.
        LOG_ERROR("Could not post webview teardown to the platform thread: %s\n", strerror(ok));
    }
}

static const struct wvcef_host_callbacks host_callbacks = {
    .on_paint = on_paint,
    .on_load_start = on_load_start,
    .on_load_end = on_load_end,
    .on_load_error = on_load_error,
    .on_url_changed = on_url_changed,
    .on_title_changed = on_title_changed,
    .on_tooltip = on_tooltip,
    .on_console_message = on_console_message,
    .on_cursor_changed = on_cursor_changed,
    .on_closed = on_browser_closed,
};

// ---------------------------------------------------------------------------
// CEF initialization
// ---------------------------------------------------------------------------

/// Splits a comma separated switch list into `switches`, writing into `list`
/// (which is modified in place). Returns the new switch count.
static size_t parse_switch_list(char *list, const char **switches, size_t n_switches, size_t max_switches) {
    char *cursor = list;

    while (cursor != NULL && *cursor != '\0' && n_switches < max_switches) {
        char *comma = strchr(cursor, ',');

        if (comma != NULL) {
            *comma = '\0';
        }

        if (*cursor != '\0') {
            switches[n_switches++] = cursor;
        }

        cursor = comma != NULL ? comma + 1 : NULL;
    }

    return n_switches;
}

/**
 * @brief Starts CEF, if it isn't running yet.
 *
 * The webview_cef channel only carries a user agent, so everything else is
 * configured through the environment -- see the README.
 */
static int ensure_cef_initialized(const char *user_agent) {
    struct wvcef_init_options options;
    const char *switches[MAX_SWITCHES];
    char *env_switches_copy = NULL;
    char locales_dir[PATH_MAX];
    const char *runtime_dir;
    const char *env_switches;
    size_t n_switches = 0;
    int written;
    int ok;

    if (wvcef_is_initialized()) {
        return 0;
    }

    memset(&options, 0, sizeof options);

    runtime_dir = resolve_env("FLUTTERPI_CEF_RUNTIME_DIR", WEBVIEW_CEF_RUNTIME_DIR);

    written = snprintf(locales_dir, sizeof(locales_dir), "%s/locales", runtime_dir);
    if (written < 0 || (size_t) written >= sizeof(locales_dir)) {
        LOG_ERROR("The CEF runtime directory path is too long: %s\n", runtime_dir);
        return ENAMETOOLONG;
    }

    options.subprocess_path = resolve_env("FLUTTERPI_CEF_HELPER", WEBVIEW_CEF_HELPER_PATH);
    // The resources sit in the runtime directory itself, so there is nothing to
    // build here and nothing to copy.
    options.resources_dir = runtime_dir;
    options.locales_dir = locales_dir;
    options.cache_path = resolve_env("FLUTTERPI_CEF_CACHE_PATH", NULL);
    options.user_agent = user_agent;
    options.log_file = resolve_env("FLUTTERPI_CEF_LOG_FILE", NULL);
    options.log_severity = (int) resolve_env_long("FLUTTERPI_CEF_LOG_SEVERITY", 0);

    if (getenv("FLUTTERPI_CEF_NO_DEFAULT_SWITCHES") == NULL) {
        for (size_t i = 0; i < ARRAY_SIZE(default_switches) && n_switches < MAX_SWITCHES; i++) {
            switches[n_switches++] = default_switches[i];
        }
    }

    // FLUTTERPI_CEF_SWITCHES=disable-web-security,enable-logging=stderr
    env_switches = getenv("FLUTTERPI_CEF_SWITCHES");
    if (env_switches != NULL && env_switches[0] != '\0') {
        env_switches_copy = strdup(env_switches);
        if (env_switches_copy != NULL) {
            n_switches = parse_switch_list(env_switches_copy, switches, n_switches, MAX_SWITCHES);
        }
    }

    options.switches = switches;
    options.n_switches = n_switches;

    // Mesa's EGL defaults to the X11 platform, and there is no X server here. The
    // CEF helper processes then fail eglInitialize with "Could not open the default
    // X display" and Chromium falls back to SwiftShader, saying so nowhere except
    // its own log -- so `use-gl=angle` above only reaches the GPU with this set.
    //
    // It goes in *our* environment because CEF has no hook for a subprocess's: the
    // helpers are forked from this process and inherit it. flutter-pi's own display
    // is not affected. It already exists by the time any plugin is initialized, and
    // it is created with an explicit platform -- eglGetPlatformDisplay with
    // EGL_PLATFORM_GBM_KHR -- which EGL_PLATFORM does not override; the variable
    // only steers the legacy eglGetDisplay().
    //
    // Not overwritten, so a target that wants a different EGL platform (or plain
    // X11, if someone runs this under one) can just say so in the environment.
    setenv("EGL_PLATFORM", "surfaceless", 0);

    LOG_DEBUG("Initializing CEF. runtime dir: %s, helper: %s\n", runtime_dir, options.subprocess_path);

    ok = wvcef_initialize(&options);

    free(env_switches_copy);

    if (ok != 0) {
        return ok;
    }

    return 0;
}

// ---------------------------------------------------------------------------
// method handlers
// ---------------------------------------------------------------------------

/// `init` takes the user agent directly, or nothing at all.
static int on_init(struct std_value *args, FlutterPlatformMessageResponseHandle *response_handle) {
    const char *user_agent = NULL;
    int ok;

    if (args != NULL && STDVALUE_IS_STRING(*args)) {
        user_agent = STDVALUE_AS_STRING(*args);
    }

    ok = ensure_cef_initialized(user_agent);
    if (ok != 0) {
        return platch_respond_error_std(response_handle, "cef-init-failed", "Could not initialize CEF. See the flutter-pi log.", &STDNULL);
    }

    return platch_respond_success_std(response_handle, &STDNULL);
}

/// `create` takes the url directly and answers with [browserId, textureId].
static int on_create(struct std_value *args, FlutterPlatformMessageResponseHandle *response_handle) {
    struct webview *wv;
    const char *url = NULL;
    long frame_rate;
    int ok;

    ok = ensure_cef_initialized(NULL);
    if (ok != 0) {
        return platch_respond_error_std(response_handle, "cef-init-failed", "Could not initialize CEF. See the flutter-pi log.", &STDNULL);
    }

    if (args != NULL && STDVALUE_IS_STRING(*args)) {
        url = STDVALUE_AS_STRING(*args);
    }

    frame_rate = resolve_frame_rate();

    wv = calloc(1, sizeof *wv);
    if (wv == NULL) {
        return platch_respond_native_error_std(response_handle, ENOMEM);
    }

    // The dart side doesn't know the widget size yet at this point -- it calls
    // `setSize` once the webview has been laid out. Until then CEF renders at
    // the smallest size it accepts.
    wv->logical_width = 1;
    wv->logical_height = 1;
    wv->pixel_ratio = 1.0;

    wv->texture = flutterpi_create_texture(plugin.flutterpi);
    if (wv->texture == NULL) {
        free(wv);
        return platch_respond_error_std(response_handle, "texture-failed", "Could not create a flutter texture.", &STDNULL);
    }

    wv->texture_id = texture_get_id(wv->texture);

    wv->browser = wvcef_browser_create(url, wv->logical_width, wv->logical_height, wv->pixel_ratio, (int) frame_rate, &host_callbacks, wv);
    if (wv->browser == NULL) {
        // Not logged as well: the bridge has already said why on its way out, and
        // the dart side is being told right here.
        texture_destroy(wv->texture);
        free(wv);
        return platch_respond_error_std(response_handle, "browser-failed", "Could not create the CEF browser.", &STDNULL);
    }

    wv->browser_id = wvcef_browser_get_id(wv->browser);
    webview_list_add(wv);

    TRACE("created: browser %d, texture %" PRId64 ", url %s\n", wv->browser_id, wv->texture_id, url != NULL ? url : "about:blank");

    return platch_respond_success_std(
        response_handle,
        &(struct std_value){
            .type = kStdList,
            .size = 2,
            .list = (struct std_value[2]){ STDINT32(wv->browser_id), STDINT64(wv->texture_id) },
        }
    );
}

/// For `close`, `reload`, `goBack` and `goForward`, which take the browser id as
/// a bare integer.
static struct webview *
webview_from_bare_id(struct std_value *args, FlutterPlatformMessageResponseHandle *response_handle, int *response_out) {
    struct webview *wv;

    if (args == NULL || !STDVALUE_IS_INT(*args)) {
        *response_out = platch_respond_illegal_arg_std(response_handle, "Expected the browser id to be an integer.");
        return NULL;
    }

    wv = webview_find((int) STDVALUE_AS_INT(*args));
    if (wv == NULL || wv->closing) {
        *response_out = platch_respond_error_std(response_handle, "no-such-webview", "There is no webview with that browser id.", &STDNULL);
        return NULL;
    }

    return wv;
}

/// For everything else: the browser id is the first element of the argument list.
static struct webview *webview_from_list(struct std_value *args, FlutterPlatformMessageResponseHandle *response_handle, int *response_out) {
    struct webview *wv;
    int64_t browser_id;

    if (!arg_int_at(args, 0, &browser_id)) {
        *response_out = platch_respond_illegal_arg_std(response_handle, "Expected a list with the browser id as its first element.");
        return NULL;
    }

    wv = webview_find((int) browser_id);
    if (wv == NULL || wv->closing) {
        *response_out = platch_respond_error_std(response_handle, "no-such-webview", "There is no webview with that browser id.", &STDNULL);
        return NULL;
    }

    return wv;
}

static int on_close(struct std_value *args, FlutterPlatformMessageResponseHandle *response_handle) {
    struct webview *wv;
    int response = 0;

    wv = webview_from_bare_id(args, response_handle, &response);
    if (wv == NULL) {
        return response;
    }

    wv->closing = true;
    webview_release_textures(wv);

    if (wv->browser != NULL) {
        // `wv` is freed in on_browser_closed.
        wvcef_browser_close(wv->browser);
    } else {
        webview_free(wv);
    }

    return platch_respond_success_std(response_handle, &STDNULL);
}

static int on_load_url(struct std_value *args, FlutterPlatformMessageResponseHandle *response_handle) {
    struct webview *wv;
    const char *url;
    int response = 0;

    wv = webview_from_list(args, response_handle, &response);
    if (wv == NULL) {
        return response;
    }

    url = arg_string_at(args, 1);
    if (url == NULL) {
        return platch_respond_illegal_arg_std(response_handle, "Expected the url to be a string.");
    }

    wvcef_browser_load_url(wv->browser, url);

    return platch_respond_success_std(response_handle, &STDNULL);
}

static int on_set_size(struct std_value *args, FlutterPlatformMessageResponseHandle *response_handle) {
    struct webview *wv;
    double dpi = 1.0, width = 0.0, height = 0.0;
    int response = 0;

    wv = webview_from_list(args, response_handle, &response);
    if (wv == NULL) {
        return response;
    }

    if (!arg_num_at(args, 1, &dpi) || !arg_num_at(args, 2, &width) || !arg_num_at(args, 3, &height)) {
        return platch_respond_illegal_arg_std(response_handle, "Expected [browserId, dpi, width, height].");
    }

    if (dpi <= 0.0) {
        dpi = 1.0;
    }

    wv->pixel_ratio = dpi;
    wv->logical_width = (int) (width > 1.0 ? width : 1.0);
    wv->logical_height = (int) (height > 1.0 ? height : 1.0);

    TRACE("setSize: browser %d, %dx%d logical, dpi %.2f\n", wv->browser_id, wv->logical_width, wv->logical_height, wv->pixel_ratio);

    wvcef_browser_resize(wv->browser, wv->logical_width, wv->logical_height, wv->pixel_ratio);

    return platch_respond_success_std(response_handle, &STDNULL);
}

/// cursorMove, cursorDragging, cursorClickDown and cursorClickUp all take
/// [browserId, x, y].
static int on_cursor_event(const char *method, struct std_value *args, FlutterPlatformMessageResponseHandle *response_handle) {
    struct webview *wv;
    int64_t x = 0, y = 0;
    int response = 0;

    wv = webview_from_list(args, response_handle, &response);
    if (wv == NULL) {
        return response;
    }

    if (!arg_int_at(args, 1, &x) || !arg_int_at(args, 2, &y)) {
        return platch_respond_illegal_arg_std(response_handle, "Expected [browserId, x, y].");
    }

    if (streq(method, "cursorMove")) {
        wvcef_browser_send_mouse_move(wv->browser, (int) x, (int) y, false);
    } else if (streq(method, "cursorDragging")) {
        wvcef_browser_send_mouse_move(wv->browser, (int) x, (int) y, true);
    } else if (streq(method, "cursorClickDown")) {
        wvcef_browser_send_mouse_click(wv->browser, (int) x, (int) y, false);
    } else {
        wvcef_browser_send_mouse_click(wv->browser, (int) x, (int) y, true);
    }

    return platch_respond_success_std(response_handle, &STDNULL);
}

static int on_set_scroll_delta(struct std_value *args, FlutterPlatformMessageResponseHandle *response_handle) {
    struct webview *wv;
    int64_t x = 0, y = 0, delta_x = 0, delta_y = 0;
    int response = 0;

    wv = webview_from_list(args, response_handle, &response);
    if (wv == NULL) {
        return response;
    }

    if (!arg_int_at(args, 1, &x) || !arg_int_at(args, 2, &y) || !arg_int_at(args, 3, &delta_x) || !arg_int_at(args, 4, &delta_y)) {
        return platch_respond_illegal_arg_std(response_handle, "Expected [browserId, x, y, deltaX, deltaY].");
    }

    wvcef_browser_send_mouse_wheel(wv->browser, (int) x, (int) y, (int) delta_x, (int) delta_y);

    return platch_respond_success_std(response_handle, &STDNULL);
}

static int on_set_client_focus(struct std_value *args, FlutterPlatformMessageResponseHandle *response_handle) {
    struct webview *wv;
    bool focused = true;
    int response = 0;

    wv = webview_from_list(args, response_handle, &response);
    if (wv == NULL) {
        return response;
    }

    arg_bool_at(args, 1, &focused);

    wvcef_browser_set_focus(wv->browser, focused);

    return platch_respond_success_std(response_handle, &STDNULL);
}

/// imeSetComposition and imeCommitText both take [browserId, text]. This is how
/// the dart side delivers typed text -- see the README about what it takes for
/// the page to ask for it by itself.
static int on_ime_text(const char *method, struct std_value *args, FlutterPlatformMessageResponseHandle *response_handle) {
    struct webview *wv;
    const char *text;
    int response = 0;

    wv = webview_from_list(args, response_handle, &response);
    if (wv == NULL) {
        return response;
    }

    text = arg_string_at(args, 1);
    if (text == NULL) {
        return platch_respond_illegal_arg_std(response_handle, "Expected [browserId, text].");
    }

    if (streq(method, "imeCommitText")) {
        wvcef_browser_ime_commit_text(wv->browser, text);
    } else {
        wvcef_browser_ime_set_composition(wv->browser, text);
    }

    return platch_respond_success_std(response_handle, &STDNULL);
}

static int on_execute_javascript(struct std_value *args, FlutterPlatformMessageResponseHandle *response_handle) {
    struct webview *wv;
    const char *code;
    int response = 0;

    wv = webview_from_list(args, response_handle, &response);
    if (wv == NULL) {
        return response;
    }

    code = arg_string_at(args, 1);
    if (code == NULL) {
        return platch_respond_illegal_arg_std(response_handle, "Expected the javascript code to be a string.");
    }

    wvcef_browser_execute_javascript(wv->browser, code);

    return platch_respond_success_std(response_handle, &STDNULL);
}

static int on_navigation(const char *method, struct std_value *args, FlutterPlatformMessageResponseHandle *response_handle) {
    struct webview *wv;
    int response = 0;

    wv = webview_from_bare_id(args, response_handle, &response);
    if (wv == NULL) {
        return response;
    }

    if (streq(method, "reload")) {
        wvcef_browser_reload(wv->browser, false);
    } else if (streq(method, "goBack")) {
        wvcef_browser_go_back(wv->browser);
    } else {
        wvcef_browser_go_forward(wv->browser);
    }

    return platch_respond_success_std(response_handle, &STDNULL);
}

/**
 * @brief `quit` only releases the browsers.
 *
 * CEF cannot be initialized twice in a process, so actually calling CefShutdown
 * here would make every later webview fail. flutter-pi tears CEF down in the
 * plugin's deinit anyway, which is the only point where nothing can come back.
 */
static int on_quit(FlutterPlatformMessageResponseHandle *response_handle) {
    for (struct webview *wv = plugin.webviews, *next = NULL; wv != NULL; wv = next) {
        next = wv->next;

        wv->closing = true;
        webview_release_textures(wv);

        if (wv->browser != NULL) {
            wvcef_browser_close(wv->browser);
        } else {
            webview_free(wv);
        }
    }

    return platch_respond_success_std(response_handle, &STDNULL);
}

static int on_receive(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *response_handle) {
    struct std_value *args;
    const char *method;

    (void) channel;

    method = object->method;
    args = &object->std_arg;

    if (streq(method, "init")) {
        return on_init(args, response_handle);
    } else if (streq(method, "create")) {
        return on_create(args, response_handle);
    } else if (streq(method, "close")) {
        return on_close(args, response_handle);
    } else if (streq(method, "loadUrl")) {
        return on_load_url(args, response_handle);
    } else if (streq(method, "setSize")) {
        return on_set_size(args, response_handle);
    } else if (streq(method, "setScrollDelta")) {
        return on_set_scroll_delta(args, response_handle);
    } else if (streq(method, "setClientFocus")) {
        return on_set_client_focus(args, response_handle);
    } else if (streq(method, "executeJavaScript")) {
        return on_execute_javascript(args, response_handle);
    } else if (streq(method, "imeCommitText") || streq(method, "imeSetComposition")) {
        return on_ime_text(method, args, response_handle);
    } else if (streq(method, "quit")) {
        return on_quit(response_handle);
    } else if (streq(method, "cursorMove") || streq(method, "cursorDragging") || streq(method, "cursorClickDown") ||
               streq(method, "cursorClickUp")) {
        return on_cursor_event(method, args, response_handle);
    } else if (streq(method, "reload") || streq(method, "goBack") || streq(method, "goForward")) {
        return on_navigation(method, args, response_handle);
    }

    // Not implemented, on purpose:
    //   evaluateJavascript, setJavaScriptChannels, sendJavaScriptChannelCallBack
    //     -- need a CefRenderProcessHandler in the subprocess helper plus IPC to
    //        get values back out of V8.
    //   openDevTools -- needs a real window to put the inspector in.
    //   setCookie, deleteCookie, visitAllCookies, visitUrlCookies
    //     -- straightforward to add on top of CefCookieManager, just not done.
    return platch_respond_not_implemented(response_handle);
}

// ---------------------------------------------------------------------------
// plugin lifecycle
// ---------------------------------------------------------------------------

enum plugin_init_result webview_cef_init(struct flutterpi *flutterpi, void **userdata_out) {
    struct gl_renderer *renderer;
    EGLDisplay display;
    EGLContext context;
    int ok;

    if (!flutterpi_has_gl_renderer(flutterpi)) {
        LOG_ERROR("The webview plugin needs EGL/OpenGL ES rendering, which is not available. Webviews will not work.\n");
        return PLUGIN_INIT_RESULT_NOT_APPLICABLE;
    }

    renderer = flutterpi_get_gl_renderer(flutterpi);

    display = gl_renderer_get_egl_display(renderer);
    if (display == EGL_NO_DISPLAY) {
        LOG_ERROR("The GL renderer has no EGL display.\n");
        return PLUGIN_INIT_RESULT_NOT_APPLICABLE;
    }

    // Shares the flutter root context, so the textures we fill here can be
    // sampled by the flutter rasterizer.
    context = gl_renderer_create_context(renderer);
    if (context == EGL_NO_CONTEXT) {
        LOG_ERROR("Could not create an EGL context for the webview plugin. eglCreateContext: 0x%04X\n", eglGetError());
        return PLUGIN_INIT_RESULT_ERROR;
    }

    memset(&plugin, 0, sizeof plugin);

    plugin.flutterpi = flutterpi;
    plugin.egl_display = display;
    plugin.egl_context = context;
    // Any value means on; a number picks the level. Spelling it this way keeps
    // FLUTTERPI_CEF_TRACE=1 and a bare FLUTTERPI_CEF_TRACE= both meaning "trace".
    plugin.trace_level = getenv("FLUTTERPI_CEF_TRACE") == NULL ? 0 : (int) resolve_env_long("FLUTTERPI_CEF_TRACE", 1);

    // The BGRA path uploads with GL_BGRA_EXT as the internal format, but the
    // frame is handed to the engine as GL_RGBA8_OES, which is what every other
    // texture source in flutter-pi reports. If a driver's Skia backend refuses
    // that combination the webview goes silently blank -- no GL error, nothing
    // in the log -- so keep a way to fall back to the CPU conversion in place
    // rather than having to rebuild to find out.
    plugin.supports_bgra = gl_renderer_supports_gl_extension(renderer, "GL_EXT_texture_format_BGRA8888") &&
                           getenv("FLUTTERPI_CEF_FORCE_RGBA") == NULL;

    TRACE("plugin up. BGRA textures: %s\n", plugin.supports_bgra ? "yes" : "no, converting on the CPU");

    if (!plugin.supports_bgra) {
        LOG_ERROR("GL_EXT_texture_format_BGRA8888 is missing; webview frames will be converted on the CPU, which is slow.\n");
    }

    pthread_mutex_init(&plugin.gl_mutex, NULL);

    ok = plugin_registry_set_receiver_locked(WEBVIEW_CEF_CHANNEL, kStandardMethodCall, on_receive);
    if (ok != 0) {
        LOG_ERROR("Could not set the webview platform channel receiver: %s\n", strerror(ok));
        pthread_mutex_destroy(&plugin.gl_mutex);
        eglDestroyContext(display, context);
        return PLUGIN_INIT_RESULT_ERROR;
    }

    // CEF itself is only started on the first `init`/`create` call, so an app
    // that never opens a webview doesn't pay for Chromium's ~150MB of RAM.
    //
    // Plugins are initialized before the flutter engine is created though, so
    // starting CEF here means its helper processes are forked out of a process
    // that isn't heavily threaded yet, and Chromium's signal handlers are
    // installed before the engine's. If lazy initialization ever turns out to be
    // flaky, set FLUTTERPI_CEF_EAGER_INIT=1.
    if (getenv("FLUTTERPI_CEF_EAGER_INIT") != NULL) {
        ok = ensure_cef_initialized(NULL);
        if (ok != 0) {
            LOG_ERROR("Eager CEF initialization failed: %s. Webviews will not work.\n", strerror(ok));
        }
    }

    *userdata_out = &plugin;
    return PLUGIN_INIT_RESULT_INITIALIZED;
}

void webview_cef_deinit(struct flutterpi *flutterpi, void *userdata) {
    (void) flutterpi;
    (void) userdata;

    plugin_registry_remove_receiver_locked(WEBVIEW_CEF_CHANNEL);

    // Close every browser and let CEF finish the teardown. Without this,
    // CefShutdown() aborts.
    for (struct webview *wv = plugin.webviews; wv != NULL; wv = wv->next) {
        wv->closing = true;
        webview_release_textures(wv);

        if (wv->browser != NULL) {
            wvcef_browser_close(wv->browser);
        }
    }

    if (wvcef_is_initialized()) {
        // CEF closes browsers on its own thread, so this only has to wait.
        // 200 * 10ms = 2s worth of patience.
        for (int i = 0; i < 200 && wvcef_n_live_browsers() > 0; i++) {
            nanosleep(&(struct timespec){ .tv_sec = 0, .tv_nsec = 10 * 1000 * 1000 }, NULL);
        }

        if (wvcef_n_live_browsers() > 0) {
            LOG_ERROR("%d webview(s) did not close in time; skipping CEF shutdown.\n", wvcef_n_live_browsers());
        }
    }

    // Everything is torn down here rather than in on_browser_closed_on_platform,
    // because that runs as a platform task and the event loop has already stopped
    // by the time plugins are deinitialized -- the tasks CEF's thread just posted
    // will never be dispatched. Which also means nothing else is going to touch
    // these webviews, so freeing them here is safe; the undispatched tasks hold
    // pointers into them, but the process is on its way out.
    //
    // Before wvcef_shutdown(), so no handle outlives the CEF runtime.
    while (plugin.webviews != NULL) {
        struct webview *wv = plugin.webviews;

        plugin.webviews = wv->next;

        if (wv->browser != NULL) {
            wvcef_browser_destroy(wv->browser);
        }

        free(wv->swizzle_buffer);
        free(wv);
    }

    if (wvcef_is_initialized() && wvcef_n_live_browsers() == 0) {
        wvcef_shutdown();
    }

    if (plugin.egl_context != EGL_NO_CONTEXT) {
        eglDestroyContext(plugin.egl_display, plugin.egl_context);
        plugin.egl_context = EGL_NO_CONTEXT;
    }

    pthread_mutex_destroy(&plugin.gl_mutex);
}

FLUTTERPI_PLUGIN("webview_cef", webview_cef_plugin, webview_cef_init, webview_cef_deinit)
