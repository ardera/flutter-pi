// SPDX-License-Identifier: MIT
/*
 * Utilities for rendering using OpenGL
 *
 * Copyright (c) 2022, Hannes Winkler <hanneswinkler2000@web.de>
 */

#define _GNU_SOURCE
#include "gl_renderer.h"

#include <errno.h>
#include <stdlib.h>

#include <dlfcn.h>
#include <pthread.h>

#include "egl.h"
#include "gles.h"
#include "pixel_format.h"
#include "tracer.h"
#include "util/collection.h"
#include "util/logging.h"
#include "util/refcounting.h"

struct gl_renderer {
    refcount_t n_refs;

    struct tracer *tracer;
    struct gbm_device *gbm_device;
    EGLDisplay egl_display;

    EGLContext root_context, flutter_rendering_context, flutter_resource_uploading_context, flutter_setup_context;
    pthread_mutex_t root_context_lock;

    bool has_forced_pixel_format;
    enum pixfmt pixel_format;

    /**
     * @brief If EGL doesn't support EGL_KHR_no_config_context, we need to specify an EGLConfig (basically the framebuffer format)
     * for the context. And since all shared contexts need to have the same EGL Config, we basically have to choose a single global config
     * for the display.
     *
     * If this field is not EGL_NO_CONFIG_KHR, all contexts we create need to have exactly this EGL Config.
     */
    EGLConfig forced_egl_config;

    EGLint major, minor;
    const char *egl_client_exts;
    const char *egl_display_exts;
    const char *gl_renderer;
    const char *gl_exts;

    bool supports_egl_ext_platform_base;
#ifdef EGL_EXT_platform_base
    PFNEGLGETPLATFORMDISPLAYEXTPROC egl_get_platform_display_ext;
    PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC egl_create_platform_window_surface_ext;
#endif

#ifdef EGL_VERSION_1_5
    PFNEGLGETPLATFORMDISPLAYPROC egl_get_platform_display;
    PFNEGLCREATEPLATFORMWINDOWSURFACEPROC egl_create_platform_window_surface;
#endif
};

static void *try_get_proc_address(const char *name) {
    void *address;

    address = eglGetProcAddress(name);
    if (address) {
        return address;
    }

    address = dlsym(RTLD_DEFAULT, name);
    if (address) {
        return address;
    }

    return NULL;
}

static void *get_proc_address(const char *name) {
    void *address;

    address = try_get_proc_address(name);
    if (address == NULL) {
        LOG_ERROR("Could not resolve EGL/GL symbol \"%s\"\n", name);
        return NULL;
    }

    return address;
}

static ATTR_PURE EGLConfig choose_config_with_pixel_format(EGLDisplay display, const EGLint *attrib_list, enum pixfmt pixel_format) {
    EGLConfig *matching;
    EGLBoolean egl_ok;
    EGLint value, n_matched;

    assert(display != EGL_NO_DISPLAY);

    LOG_DEBUG("Choosing EGL config with pixel format %s...\n", get_pixfmt_info(pixel_format)->name);

    n_matched = 0;
    egl_ok = eglChooseConfig(display, attrib_list, NULL, 0, &n_matched);
    if (egl_ok != EGL_TRUE) {
        LOG_EGL_ERROR(eglGetError(), "Could not query number of EGL framebuffer configurations with fitting attributes. eglChooseConfig");
        return EGL_NO_CONFIG_KHR;
    }

    matching = alloca(n_matched * sizeof *matching);

    egl_ok = eglChooseConfig(display, attrib_list, matching, n_matched, &n_matched);
    if (egl_ok != EGL_TRUE) {
        LOG_EGL_ERROR(eglGetError(), "Could not query EGL framebuffer configurations with fitting attributes. eglChooseConfig");
        return EGL_NO_CONFIG_KHR;
    }

    for (int i = 0; i < n_matched; i++) {
        egl_ok = eglGetConfigAttrib(display, matching[i], EGL_NATIVE_VISUAL_ID, &value);
        if (egl_ok != EGL_TRUE) {
            LOG_EGL_ERROR(eglGetError(), "Could not query pixel format of EGL framebuffer config. eglGetConfigAttrib");
            return EGL_NO_CONFIG_KHR;
        }

        if (int32_to_uint32(value) == get_pixfmt_info(pixel_format)->gbm_format) {
            // found a config with matching pixel format.
            return matching[i];
        }
    }

    return EGL_NO_CONFIG_KHR;
}

static const EGLint context_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };

struct gl_renderer *gl_renderer_new_from_gbm_device(
    struct tracer *tracer,
    struct gbm_device *gbm_device,
    bool has_forced_pixel_format,
    enum pixfmt pixel_format
) {
    struct gl_renderer *renderer;
    const char *egl_client_exts, *egl_display_exts;
    const char *gl_renderer, *gl_exts;
    EGLContext root_context, flutter_render_context, flutter_resource_uploading_context, flutter_setup_context;
    EGLDisplay egl_display;
    EGLBoolean egl_ok;
    EGLConfig forced_egl_config;
    EGLint major, minor;
    bool supports_egl_ext_platform_base;
    int ok;

    renderer = malloc(sizeof *renderer);
    if (renderer == NULL) {
        goto fail_return_null;
    }

    egl_client_exts = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    if (egl_client_exts == NULL) {
        LOG_EGL_ERROR(eglGetError(), "Couldn't query EGL client extensions. eglQueryString");
        goto fail_free_renderer;
    }

    if (check_egl_extension(egl_client_exts, NULL, "EGL_EXT_platform_base")) {
#ifdef EGL_EXT_platform_base
        supports_egl_ext_platform_base = true;
#else
        LOG_ERROR(
            "EGL supports EGL_EXT_platform_base, but EGL headers didn't contain definitions for EGL_EXT_platform_base."
            "eglGetPlatformDisplayEXT and eglCreatePlatformWindowSurfaceEXT will not be used to create an EGL display.\n"
        );
        supports_egl_ext_platform_base = false;
#endif
    } else {
        supports_egl_ext_platform_base = false;
    }

// PFNEGLGETPLATFORMDISPLAYEXTPROC, PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC
// are defined by EGL_EXT_platform_base.
#ifdef EGL_EXT_platform_base
    PFNEGLGETPLATFORMDISPLAYEXTPROC egl_get_platform_display_ext;
    PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC egl_create_platform_window_surface_ext;
#endif

    if (supports_egl_ext_platform_base) {
#ifdef EGL_EXT_platform_base
        egl_get_platform_display_ext = try_get_proc_address("eglGetPlatformDisplayEXT");
        if (egl_get_platform_display_ext == NULL) {
            LOG_ERROR("Couldn't resolve \"eglGetPlatformDisplayEXT\" even though \"EGL_EXT_platform_base\" was listed as supported.\n");
            supports_egl_ext_platform_base = false;
        }
#else
        UNREACHABLE();
#endif
    }

    if (supports_egl_ext_platform_base) {
#ifdef EGL_EXT_platform_base
        egl_create_platform_window_surface_ext = try_get_proc_address("eglCreatePlatformWindowSurfaceEXT");
        if (egl_create_platform_window_surface_ext == NULL) {
            LOG_ERROR(
                "Couldn't resolve \"eglCreatePlatformWindowSurfaceEXT\" even though \"EGL_EXT_platform_base\" was listed as supported.\n"
            );
            egl_get_platform_display_ext = NULL;
            supports_egl_ext_platform_base = false;
        }
#else
        UNREACHABLE();
#endif
    }

// EGL_PLATFORM_GBM_KHR is defined by EGL_KHR_platform_gbm.
#ifndef EGL_KHR_platform_gbm
    #error "EGL extension EGL_KHR_platform_gbm is required."
#endif

    egl_display = EGL_NO_DISPLAY;
    bool failed_before = false;

#ifdef EGL_VERSION_1_5
    PFNEGLGETPLATFORMDISPLAYPROC egl_get_platform_display = try_get_proc_address("eglGetPlatformDisplay");
    PFNEGLCREATEPLATFORMWINDOWSURFACEPROC egl_create_platform_window_surface = try_get_proc_address("eglCreatePlatformWindowSurface");

    if (egl_display == EGL_NO_DISPLAY && egl_get_platform_display != NULL) {
        egl_display = egl_get_platform_display(EGL_PLATFORM_GBM_KHR, gbm_device, NULL);
        if (egl_display == EGL_NO_DISPLAY) {
            LOG_EGL_ERROR(eglGetError(), "Could not get EGL display from GBM device. eglGetPlatformDisplay");
            failed_before = true;
        }
    }
#endif

#ifdef EGL_EXT_platform_base
    if (egl_display == EGL_NO_DISPLAY && egl_get_platform_display_ext != NULL) {
        if (failed_before) {
            LOG_DEBUG("Attempting eglGetPlatformDisplayEXT...\n");
        }

        egl_display = egl_get_platform_display_ext(EGL_PLATFORM_GBM_KHR, gbm_device, NULL);
        if (egl_display == EGL_NO_DISPLAY) {
            LOG_EGL_ERROR(eglGetError(), "Could not get EGL display from GBM device. eglGetPlatformDisplayEXT");
            failed_before = true;
        }
    }
#endif

    if (egl_display == EGL_NO_DISPLAY) {
        if (failed_before) {
            LOG_DEBUG("Attempting eglGetDisplay...\n");
        }

        egl_display = eglGetDisplay((void *) gbm_device);
        if (egl_display == EGL_NO_DISPLAY) {
            LOG_EGL_ERROR(eglGetError(), "Could not get EGL display from GBM device. eglGetDisplay");
        }
    }

    if (egl_display == EGL_NO_DISPLAY) {
        LOG_ERROR("Could not get EGL Display from any function.\n");
        goto fail_free_renderer;
    }

    egl_ok = eglInitialize(egl_display, &major, &minor);
    if (egl_ok != EGL_TRUE) {
        LOG_EGL_ERROR(eglGetError(), "Failed to initialize EGL! eglInitialize:");
        goto fail_free_renderer;
    }

    egl_display_exts = eglQueryString(egl_display, EGL_EXTENSIONS);
    if (egl_display_exts == NULL) {
        LOG_EGL_ERROR(eglGetError(), "Couldn't query EGL display extensions. eglQueryString");
        goto fail_terminate_display;
    }

    if (!check_egl_extension(egl_client_exts, egl_display_exts, "EGL_KHR_surfaceless_context")) {
        LOG_ERROR("EGL doesn't support the EGL_KHR_surfaceless_context extension, which is required by flutter-pi.\n");
        goto fail_terminate_display;
    }

    egl_ok = eglBindAPI(EGL_OPENGL_ES_API);
    if (egl_ok != EGL_TRUE) {
        LOG_EGL_ERROR(eglGetError(), "Couldn't bind OpenGL ES API to EGL. eglBindAPI");
        goto fail_terminate_display;
    }

    if (check_egl_extension(egl_client_exts, egl_display_exts, "EGL_KHR_no_config_context")) {
        // EGL supports creating contexts without an EGLConfig, which is nice.
        // Just create a context without selecting a config and let the backing stores (when they're created) select
        // the framebuffer config instead.
        forced_egl_config = EGL_NO_CONFIG_KHR;
    } else {
        // choose a config
        const EGLint config_attribs[] = {
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SAMPLES, 0, EGL_NONE,
        };

        if (has_forced_pixel_format == false) {
            has_forced_pixel_format = true;
            pixel_format = PIXFMT_ARGB8888;
        }

        forced_egl_config = choose_config_with_pixel_format(egl_display, config_attribs, pixel_format);
        if (forced_egl_config == EGL_NO_CONFIG_KHR) {
            LOG_ERROR("No fitting EGL framebuffer configuration found.\n");
            goto fail_terminate_display;
        }
    }

    root_context = eglCreateContext(egl_display, forced_egl_config, EGL_NO_CONTEXT, context_attribs);
    if (root_context == EGL_NO_CONTEXT) {
        LOG_EGL_ERROR(eglGetError(), "Could not create EGL context for OpenGL ES. eglCreateContext");
        goto fail_terminate_display;
    }

    flutter_render_context = eglCreateContext(egl_display, forced_egl_config, root_context, context_attribs);
    if (flutter_render_context == EGL_NO_CONTEXT) {
        LOG_EGL_ERROR(eglGetError(), "Could not create EGL OpenGL ES context for flutter rendering. eglCreateContext");
        goto fail_destroy_root_context;
    }

    flutter_resource_uploading_context = eglCreateContext(egl_display, forced_egl_config, root_context, context_attribs);
    if (flutter_resource_uploading_context == EGL_NO_CONTEXT) {
        LOG_EGL_ERROR(eglGetError(), "Could not create EGL OpenGL ES context for flutter resource uploads. eglCreateContext");
        goto fail_destroy_flutter_render_context;
    }

    flutter_setup_context = eglCreateContext(egl_display, forced_egl_config, root_context, context_attribs);
    if (flutter_setup_context == EGL_NO_CONTEXT) {
        LOG_EGL_ERROR(eglGetError(), "Could not create EGL OpenGL ES context for flutter initialization. eglCreateContext");
        goto fail_destroy_flutter_resource_uploading_context;
    }

    egl_ok = eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, root_context);
    if (egl_ok != EGL_TRUE) {
        LOG_EGL_ERROR(eglGetError(), "Could not make EGL OpenGL ES root context current to query OpenGL information. eglMakeCurrent");
        goto fail_destroy_flutter_setup_context;
    }

    gl_renderer = (const char *) glGetString(GL_RENDERER);
    if (gl_renderer == NULL) {
        LOG_ERROR("Couldn't query OpenGL ES renderer information.\n");
        goto fail_clear_current;
    }

    gl_exts = (const char *) glGetString(GL_EXTENSIONS);
    if (gl_exts == NULL) {
        LOG_ERROR("Couldn't query supported OpenGL ES extensions.\n");
        goto fail_clear_current;
    }

    LOG_DEBUG_UNPREFIXED(
        "===================================\n"
        "EGL information:\n"
        "  version: %s\n"
        "  vendor: %s\n"
        "  client extensions: %s\n"
        "  display extensions: %s\n"
        "===================================\n",
        eglQueryString(egl_display, EGL_VERSION),
        eglQueryString(egl_display, EGL_VENDOR),
        egl_client_exts,
        egl_display_exts
    );

    // this needs to be here because the EGL context needs to be current.
    LOG_DEBUG_UNPREFIXED(
        "===================================\n"
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
        LOG_EGL_ERROR(eglGetError(), "Could not clear EGL OpenGL ES context. eglMakeCurrent");
        goto fail_destroy_flutter_resource_uploading_context;
    }

    ok = pthread_mutex_init(&renderer->root_context_lock, NULL);
    if (ok < 0) {
        goto fail_destroy_flutter_resource_uploading_context;
    }

    renderer->n_refs = REFCOUNT_INIT_1;
    renderer->tracer = tracer_ref(tracer);
    renderer->gbm_device = gbm_device;
    renderer->egl_display = egl_display;
    renderer->root_context = root_context;
    renderer->flutter_rendering_context = flutter_render_context;
    renderer->flutter_resource_uploading_context = flutter_resource_uploading_context;
    renderer->flutter_setup_context = flutter_setup_context;
    renderer->has_forced_pixel_format = has_forced_pixel_format;
    renderer->pixel_format = pixel_format;
    renderer->forced_egl_config = forced_egl_config;
    renderer->major = major;
    renderer->minor = minor;
    renderer->egl_client_exts = egl_client_exts;
    renderer->egl_display_exts = egl_display_exts;
    renderer->gl_renderer = gl_renderer;
    renderer->gl_exts = gl_exts;
    renderer->supports_egl_ext_platform_base = supports_egl_ext_platform_base;
#ifdef EGL_EXT_platform_base
    renderer->egl_get_platform_display_ext = egl_get_platform_display_ext;
    renderer->egl_create_platform_window_surface_ext = egl_create_platform_window_surface_ext;
#endif
#ifdef EGL_VERSION_1_5
    renderer->egl_get_platform_display = egl_get_platform_display;
    renderer->egl_create_platform_window_surface = egl_create_platform_window_surface;
#endif
    return renderer;

fail_clear_current:
    eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

fail_destroy_flutter_setup_context:
    eglDestroyContext(egl_display, flutter_setup_context);

fail_destroy_flutter_resource_uploading_context:
    eglDestroyContext(egl_display, flutter_resource_uploading_context);

fail_destroy_flutter_render_context:
    eglDestroyContext(egl_display, flutter_render_context);

fail_destroy_root_context:
    eglDestroyContext(egl_display, root_context);

fail_terminate_display:
    eglTerminate(egl_display);

fail_free_renderer:
    free(renderer);

fail_return_null:
    return NULL;
}

void gl_renderer_destroy(struct gl_renderer *renderer) {
    ASSERT_NOT_NULL(renderer);
    tracer_unref(renderer->tracer);
    pthread_mutex_destroy(&renderer->root_context_lock);
    eglDestroyContext(renderer->egl_display, renderer->flutter_resource_uploading_context);
    eglDestroyContext(renderer->egl_display, renderer->flutter_rendering_context);
    eglDestroyContext(renderer->egl_display, renderer->root_context);
    eglTerminate(renderer->egl_display);
    free(renderer);
}

DEFINE_REF_OPS(gl_renderer, n_refs)

bool gl_renderer_has_forced_pixel_format(struct gl_renderer *renderer) {
    ASSERT_NOT_NULL(renderer);
    return renderer->has_forced_pixel_format;
}

enum pixfmt gl_renderer_get_forced_pixel_format(struct gl_renderer *renderer) {
    ASSERT_NOT_NULL(renderer);
    assert(renderer->has_forced_pixel_format);
    return renderer->pixel_format;
}

bool gl_renderer_has_forced_egl_config(struct gl_renderer *renderer) {
    ASSERT_NOT_NULL(renderer);
    return renderer->forced_egl_config != EGL_NO_CONFIG_KHR;
}

EGLConfig gl_renderer_get_forced_egl_config(struct gl_renderer *renderer) {
    ASSERT_NOT_NULL(renderer);
    return renderer->forced_egl_config;
}

struct gbm_device *gl_renderer_get_gbm_device(struct gl_renderer *renderer) {
    return renderer->gbm_device;
}

int gl_renderer_make_flutter_setup_context_current(struct gl_renderer *renderer) {
    EGLBoolean egl_ok;

    ASSERT_NOT_NULL(renderer);

    TRACER_BEGIN(renderer->tracer, "gl_renderer_make_flutter_setup_context_current");
    egl_ok = eglMakeCurrent(renderer->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, renderer->flutter_setup_context);
    TRACER_END(renderer->tracer, "gl_renderer_make_flutter_setup_context_current");

    if (egl_ok != EGL_TRUE) {
        LOG_EGL_ERROR(eglGetError(), "Could not make the flutter setup EGL context current. eglMakeCurrent");
        return EIO;
    }

    return 0;
}

int gl_renderer_make_flutter_rendering_context_current(struct gl_renderer *renderer, EGLSurface surface) {
    EGLBoolean egl_ok;

    ASSERT_NOT_NULL(renderer);
    /// NOTE: Allow this for now
    /// assert(surface != EGL_NO_SURFACE);

    TRACER_BEGIN(renderer->tracer, "gl_renderer_make_flutter_rendering_context_current");
    egl_ok = eglMakeCurrent(renderer->egl_display, surface, surface, renderer->flutter_rendering_context);
    TRACER_END(renderer->tracer, "gl_renderer_make_flutter_rendering_context_current");

    if (egl_ok != EGL_TRUE) {
        LOG_EGL_ERROR(eglGetError(), "Could not make the flutter rendering EGL context current. eglMakeCurrent");
        return EIO;
    }

    return 0;
}

int gl_renderer_make_flutter_resource_uploading_context_current(struct gl_renderer *renderer) {
    EGLBoolean egl_ok;

    ASSERT_NOT_NULL(renderer);

    TRACER_BEGIN(renderer->tracer, "gl_renderer_make_flutter_resource_uploading_context_current");
    egl_ok = eglMakeCurrent(renderer->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, renderer->flutter_resource_uploading_context);
    TRACER_END(renderer->tracer, "gl_renderer_make_flutter_resource_uploading_context_current");

    if (egl_ok != EGL_TRUE) {
        LOG_EGL_ERROR(eglGetError(), "Could not make the flutter resource uploading EGL context current. eglMakeCurrent");
        return EIO;
    }

    return 0;
}

int gl_renderer_clear_current(struct gl_renderer *renderer) {
    EGLBoolean egl_ok;

    ASSERT_NOT_NULL(renderer);

    TRACER_BEGIN(renderer->tracer, "gl_renderer_clear_current");
    egl_ok = eglMakeCurrent(renderer->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    TRACER_END(renderer->tracer, "gl_renderer_clear_current");

    if (egl_ok != EGL_TRUE) {
        LOG_EGL_ERROR(eglGetError(), "Could not clear the flutter EGL context. eglMakeCurrent");
        return EIO;
    }

    return 0;
}

void *gl_renderer_get_proc_address(ASSERTED struct gl_renderer *renderer, const char *name) {
    ASSERT_NOT_NULL(renderer);
    ASSERT_NOT_NULL(name);
    return get_proc_address(name);
}

EGLDisplay gl_renderer_get_egl_display(struct gl_renderer *renderer) {
    ASSERT_NOT_NULL(renderer);
    return renderer->egl_display;
}

EGLContext gl_renderer_create_context(struct gl_renderer *renderer) {
    EGLContext context;

    ASSERT_NOT_NULL(renderer);

    pthread_mutex_lock(&renderer->root_context_lock);
    context = eglCreateContext(renderer->egl_display, renderer->forced_egl_config, renderer->root_context, context_attribs);
    pthread_mutex_unlock(&renderer->root_context_lock);

    if (context == EGL_NO_CONTEXT) {
        LOG_ERROR("Couldn't create a new EGL context.\n");
        return EGL_NO_CONTEXT;
    }

    return context;
}

bool gl_renderer_supports_egl_extension(struct gl_renderer *renderer, const char *name) {
    ASSERT_NOT_NULL(renderer);
    ASSERT_NOT_NULL(name);
    return check_egl_extension(renderer->egl_client_exts, renderer->egl_display_exts, name);
}

bool gl_renderer_supports_gl_extension(struct gl_renderer *renderer, const char *name) {
    ASSERT_NOT_NULL(renderer);
    ASSERT_NOT_NULL(name);
    return check_egl_extension(renderer->gl_exts, NULL, name);
}

bool gl_renderer_is_llvmpipe(struct gl_renderer *renderer) {
    ASSERT_NOT_NULL(renderer);
    return strstr(renderer->gl_renderer, "llvmpipe") != NULL;
}

#ifdef DEBUG
static __thread bool is_render_thread = false;
#endif

int gl_renderer_make_this_a_render_thread(struct gl_renderer *renderer) {
    EGLContext context;
    EGLBoolean egl_ok;

    ASSERT_NOT_NULL(renderer);
    assert(is_render_thread == false);

    pthread_mutex_lock(&renderer->root_context_lock);
    context = eglCreateContext(renderer->egl_display, renderer->forced_egl_config, renderer->root_context, context_attribs);
    pthread_mutex_unlock(&renderer->root_context_lock);

    if (context == EGL_NO_CONTEXT) {
        LOG_ERROR("Couldn't create a new EGL context.\n");
        return EIO;
    }

    egl_ok = eglMakeCurrent(renderer->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, context);
    if (egl_ok == EGL_FALSE) {
        LOG_ERROR("Couldn't make EGL context current to make this an EGL thread.\n");
        return EIO;
    }

#ifdef DEBUG
    is_render_thread = true;
#endif

    return 0;
}

void gl_renderer_cleanup_this_render_thread() {
    EGLDisplay display;
    EGLContext context;
    EGLBoolean egl_ok;

    assert(is_render_thread);

    context = eglGetCurrentContext();
    assert(context != EGL_NO_CONTEXT);

    display = eglGetCurrentDisplay();
    assert(display != EGL_NO_CONTEXT);

    egl_ok = eglMakeCurrent(EGL_NO_DISPLAY, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (egl_ok == EGL_FALSE) {
        LOG_ERROR("Couldn't clear EGL context to cleanup this EGL thread.\n");
        return;
    }

    egl_ok = eglDestroyContext(display, context);
    if (egl_ok == EGL_FALSE) {
        LOG_ERROR("Couldn't destroy EGL context to cleanup this EGL thread.\n");
        return;
    }
}

ATTR_PURE EGLConfig
gl_renderer_choose_config(struct gl_renderer *renderer, bool has_desired_pixel_format, enum pixfmt desired_pixel_format) {
    ASSERT_NOT_NULL(renderer);

    if (renderer->forced_egl_config != EGL_NO_CONFIG_KHR) {
        return renderer->forced_egl_config;
    }

    const EGLint config_attribs[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SAMPLES, 0, EGL_NONE };

    return choose_config_with_pixel_format(
        renderer->egl_display,
        config_attribs,
        renderer->has_forced_pixel_format ? renderer->pixel_format :
        has_desired_pixel_format          ? desired_pixel_format :
                                            PIXFMT_ARGB8888
    );
}

ATTR_PURE EGLConfig gl_renderer_choose_config_direct(struct gl_renderer *renderer, enum pixfmt pixel_format) {
    ASSERT_NOT_NULL(renderer);
    ASSUME_PIXFMT_VALID(pixel_format);

    const EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SAMPLES, 0, EGL_NONE,
    };

    return choose_config_with_pixel_format(renderer->egl_display, config_attribs, pixel_format);
}

EGLSurface gl_renderer_create_gbm_window_surface(
    struct gl_renderer *renderer,
    EGLConfig config,
    struct gbm_surface *gbm_surface,
    const EGLAttribKHR *attrib_list,
    const EGLint *int_attrib_list
) {
    EGLSurface surface = EGL_NO_SURFACE;
    bool failed_before = false;

#ifdef EGL_VERSION_1_5
    if (renderer->egl_create_platform_window_surface != NULL) {
        surface = renderer->egl_create_platform_window_surface(renderer->egl_display, config, gbm_surface, attrib_list);
        if (surface == EGL_NO_SURFACE) {
            LOG_EGL_ERROR(eglGetError(), "Couldn't create gbm_surface backend window surface. eglCreatePlatformWindowSurface");
            failed_before = true;
        }
    }
#endif

    if (surface == EGL_NO_SURFACE && renderer->supports_egl_ext_platform_base) {
#ifdef EGL_EXT_platform_base
        ASSUME(renderer->egl_create_platform_window_surface_ext);

        if (failed_before) {
            LOG_DEBUG("Attempting eglCreatePlatformWindowSurfaceEXT...\n");
        }

        surface = renderer->egl_create_platform_window_surface_ext(renderer->egl_display, config, gbm_surface, int_attrib_list);
        if (surface == EGL_NO_SURFACE) {
            LOG_EGL_ERROR(eglGetError(), "Couldn't create gbm_surface backend window surface. eglCreatePlatformWindowSurfaceEXT");
            failed_before = true;
        }
#else
        UNREACHABLE();
#endif
    }

    if (surface == EGL_NO_SURFACE) {
        if (failed_before) {
            LOG_DEBUG("Attempting eglCreateWindowSurface...\n");
        }

        surface = eglCreateWindowSurface(renderer->egl_display, config, (EGLNativeWindowType) gbm_surface, int_attrib_list);
        if (surface == EGL_NO_SURFACE) {
            LOG_EGL_ERROR(eglGetError(), "Couldn't create gbm_surface backend window surface. eglCreateWindowSurface");
        }
    }

    return surface;
}
