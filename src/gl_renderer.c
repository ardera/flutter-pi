// SPDX-License-Identifier: MIT
/*
 * Utilities for rendering using OpenGL
 *
 * Copyright (c) 2022, Hannes Winkler <hanneswinkler2000@web.de>
 */

#define _GNU_SOURCE
#include <stdlib.h>
#include <pthread.h>
#include <dlfcn.h>

#include <collection.h>
#include <egl.h>
#include <gles.h>
#include <tracer.h>
#include <pixel_format.h>
#include <gl_renderer.h>

FILE_DESCR("EGL/GL renderer")

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
};

static ATTR_PURE EGLConfig choose_config_with_pixel_format(EGLDisplay display, const EGLint *attrib_list, enum pixfmt pixel_format) {
    EGLConfig *matching;
    EGLBoolean egl_ok;
    EGLint value, n_matched;

    DEBUG_ASSERT(display != EGL_NO_DISPLAY);

    LOG_DEBUG("Choosing EGL config with pixel format %s...\n", get_pixfmt_info(pixel_format)->name);
   
    n_matched = 0;
    egl_ok = eglChooseConfig(display, attrib_list, NULL, 0, &n_matched);
    if (egl_ok != EGL_TRUE) {
        LOG_ERROR("Could not query number of EGL framebuffer configurations with fitting attributes. eglChooseConfig: 0x%08X\n", eglGetError());
        return EGL_NO_CONFIG_KHR;
    }

    matching = alloca(n_matched * sizeof *matching);

    egl_ok = eglChooseConfig(display, attrib_list, matching, n_matched, &n_matched);
    if (egl_ok != EGL_TRUE) {
        LOG_ERROR("Could not query EGL framebuffer configurations with fitting attributes. eglChooseConfig: 0x%08X\n", eglGetError());
        return EGL_NO_CONFIG_KHR;
    }

    for (int i = 0; i < n_matched; i++) {
        egl_ok = eglGetConfigAttrib(display, matching[i], EGL_NATIVE_VISUAL_ID, &value);
        if (egl_ok != EGL_TRUE) {
            LOG_ERROR("Could not query pixel format of EGL framebuffer config. eglGetConfigAttrib: 0x%08X\n", eglGetError());
            return EGL_NO_CONFIG_KHR;
        }

        if (int32_to_uint32(value) == get_pixfmt_info(pixel_format)->gbm_format) {
            // found a config with matching pixel format.
            return matching[i];
        }
    }

    return EGL_NO_CONFIG_KHR;
}

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
    int ok;
    
    renderer = malloc(sizeof *renderer);
    if (renderer == NULL) {
        goto fail_return_null;
    }

    egl_client_exts = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    if (egl_client_exts == NULL) {
        LOG_ERROR("Couldn't query EGL client extensions. eglQueryString: 0x%08X\n", eglGetError());
        goto fail_free_renderer;
    }

    egl_display = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm_device, NULL);
    if (egl_display == EGL_NO_DISPLAY) {
        LOG_ERROR("Could not get EGL display from GBM device. eglGetPlatformDisplay: 0x%08X\n", eglGetError());
        goto fail_free_renderer;
    } 

    egl_ok = eglInitialize(egl_display, &major, &minor);
    if (egl_ok != EGL_TRUE) {
        LOG_ERROR("Failed to initialize EGL! eglInitialize: 0x%08X\n", eglGetError());
        goto fail_free_renderer;
    }   

    egl_display_exts = eglQueryString(egl_display, EGL_EXTENSIONS);
    if (egl_display_exts == NULL) {
        LOG_ERROR("Couldn't query EGL display extensions. eglQueryString: 0x%08X\n", eglGetError());
        goto fail_terminate_display;
    }

    if (!check_egl_extension(egl_client_exts, egl_display_exts, "EGL_KHR_surfaceless_context")) {
        LOG_ERROR("EGL doesn't support the EGL_KHR_surfaceless_context extension, which is required by flutter-pi.\n");
        goto fail_destroy_flutter_resource_uploading_context;
    }

    egl_ok = eglBindAPI(EGL_OPENGL_ES_API);
    if (egl_ok != EGL_TRUE) {
        LOG_ERROR("Couldn't bind OpenGL ES API to EGL. eglBindAPI: 0x%08X\n", eglGetError());
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
            EGL_SURFACE_TYPE,       EGL_WINDOW_BIT,
            EGL_RENDERABLE_TYPE,    EGL_OPENGL_ES2_BIT,
            EGL_SAMPLES,            0,
            EGL_NONE
        };

        if (has_forced_pixel_format == false) {
            has_forced_pixel_format = true;
            pixel_format = kARGB8888;
        }
        
        forced_egl_config = choose_config_with_pixel_format(egl_display, config_attribs, pixel_format);
        if (forced_egl_config == EGL_NO_CONFIG_KHR) {
            LOG_ERROR("No fitting EGL framebuffer configuration found.\n");
            goto fail_terminate_display;
        }
    }

    static const EGLint context_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };

    root_context = eglCreateContext(egl_display, forced_egl_config, EGL_NO_CONTEXT, context_attribs);
    if (root_context == EGL_NO_CONTEXT) {
        LOG_ERROR("Could not create EGL context for OpenGL ES. eglCreateContext: 0x%08X\n", eglGetError());
        goto fail_terminate_display;
    }

    flutter_render_context = eglCreateContext(egl_display, forced_egl_config, root_context, context_attribs);
    if (flutter_render_context == EGL_NO_CONTEXT) {
        LOG_ERROR("Could not create EGL OpenGL ES context for flutter rendering. eglCreateContext: 0x%08X\n", eglGetError());
        goto fail_destroy_root_context;
    }

    flutter_resource_uploading_context = eglCreateContext(egl_display, forced_egl_config, root_context, context_attribs);
    if (flutter_resource_uploading_context == EGL_NO_CONTEXT) {
        LOG_ERROR("Could not create EGL OpenGL ES context for flutter resource uploads. eglCreateContext: 0x%08X\n", eglGetError());
        goto fail_destroy_flutter_render_context;
    }

    flutter_setup_context = eglCreateContext(egl_display, forced_egl_config, root_context, context_attribs);
    if (flutter_setup_context == EGL_NO_CONTEXT) {
        LOG_ERROR("Could not create EGL OpenGL ES context for flutter initialization. eglCreateContext: 0x%08X\n", eglGetError());
        goto fail_destroy_flutter_resource_uploading_context;
    }

    egl_ok = eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, root_context);
    if (egl_ok != EGL_TRUE) {
        LOG_ERROR("Could not make EGL OpenGL ES root context current to query OpenGL information. eglMakeCurrent: 0x%08X\n", eglGetError());
        goto fail_destroy_flutter_setup_context;
    }

    gl_renderer = (const char*) glGetString(GL_RENDERER);
    if (gl_renderer == NULL) {
        LOG_ERROR("Couldn't query OpenGL ES renderer information.\n");
        goto fail_clear_current;
    }

    gl_exts = (const char*) glGetString(GL_EXTENSIONS);
    if (gl_exts == NULL) {
        LOG_ERROR("Couldn't query supported OpenGL ES extensions.\n");
        goto fail_clear_current;
    }

    egl_ok = eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (egl_ok != EGL_TRUE) {
        LOG_ERROR("Could not clear EGL OpenGL ES context. eglMakeCurrent: 0x%08X\n", eglGetError());
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
    DEBUG_ASSERT_NOT_NULL(renderer);
    pthread_mutex_destroy(&renderer->root_context_lock);
    eglDestroyContext(renderer->egl_display, renderer->flutter_resource_uploading_context);
    eglDestroyContext(renderer->egl_display, renderer->flutter_rendering_context);
    eglDestroyContext(renderer->egl_display, renderer->root_context);
    eglTerminate(renderer->egl_display);
    free(renderer);
}

DEFINE_REF_OPS(gl_renderer, n_refs)

bool gl_renderer_has_forced_pixel_format(struct gl_renderer *renderer) {
    DEBUG_ASSERT_NOT_NULL(renderer);
    return renderer->has_forced_pixel_format;
}

enum pixfmt gl_renderer_get_forced_pixel_format(struct gl_renderer *renderer) {
    DEBUG_ASSERT_NOT_NULL(renderer);
    DEBUG_ASSERT(renderer->has_forced_pixel_format);
    return renderer->pixel_format;
}

bool gl_renderer_has_forced_egl_config(struct gl_renderer *renderer) {
    DEBUG_ASSERT_NOT_NULL(renderer);
    return renderer->forced_egl_config != EGL_NO_CONFIG_KHR;
}

EGLConfig gl_renderer_get_forced_egl_config(struct gl_renderer *renderer) {
    DEBUG_ASSERT_NOT_NULL(renderer);
    return renderer->forced_egl_config;
}

struct gbm_device *gl_renderer_get_gbm_device(struct gl_renderer *renderer) {
    return renderer->gbm_device;
}

int gl_renderer_make_flutter_setup_context_current(struct gl_renderer *renderer) {
    EGLBoolean egl_ok;
    
    DEBUG_ASSERT_NOT_NULL(renderer);

    TRACER_BEGIN(renderer->tracer, "gl_renderer_make_flutter_rendering_context_current");
    egl_ok = eglMakeCurrent(
        renderer->egl_display,
        EGL_NO_SURFACE, EGL_NO_SURFACE,
        renderer->flutter_setup_context
    );
    TRACER_END(renderer->tracer, "gl_renderer_make_flutter_rendering_context_current");

    if (egl_ok != EGL_TRUE) {
        LOG_ERROR("Could not make the flutter setup EGL context current. eglMakeCurrent: 0x%08X\n", eglGetError());
        return EIO;
    }

    return 0;
}

int gl_renderer_make_flutter_rendering_context_current(struct gl_renderer *renderer, EGLSurface surface) {
    EGLBoolean egl_ok;
    
    DEBUG_ASSERT_NOT_NULL(renderer);
    DEBUG_ASSERT(surface != EGL_NO_SURFACE);

    LOG_DEBUG("gl_renderer_make_flutter_rendering_context_current\n");

    TRACER_BEGIN(renderer->tracer, "gl_renderer_make_flutter_rendering_context_current");
    egl_ok = eglMakeCurrent(
        renderer->egl_display,
        surface, surface,
        renderer->flutter_rendering_context
    );
    TRACER_END(renderer->tracer, "gl_renderer_make_flutter_rendering_context_current");

    if (egl_ok != EGL_TRUE) {
        LOG_ERROR("Could not make the flutter rendering EGL context current. eglMakeCurrent: 0x%08X\n", eglGetError());
        return EIO;
    }

    return 0;
}

int gl_renderer_make_flutter_resource_uploading_context_current(struct gl_renderer *renderer) {
    EGLBoolean egl_ok;
    
    DEBUG_ASSERT_NOT_NULL(renderer);

    LOG_DEBUG("gl_renderer_make_flutter_resource_uploading_context_current\n");

    TRACER_BEGIN(renderer->tracer, "gl_renderer_make_flutter_resource_uploading_context_current");
    egl_ok = eglMakeCurrent(
        renderer->egl_display,
        EGL_NO_SURFACE, EGL_NO_SURFACE,
        renderer->flutter_resource_uploading_context
    );
    TRACER_END(renderer->tracer, "gl_renderer_make_flutter_resource_uploading_context_current");

    if (egl_ok != EGL_TRUE) {
        LOG_ERROR("Could not make the flutter resource uploading EGL context current. eglMakeCurrent: 0x%08X\n", eglGetError());
        return EIO;
    }

    return 0;
}

int gl_renderer_clear_current(struct gl_renderer *renderer) {
    EGLBoolean egl_ok;

    DEBUG_ASSERT_NOT_NULL(renderer);

    LOG_DEBUG("gl_renderer_clear_current\n");

    TRACER_BEGIN(renderer->tracer, "gl_renderer_clear_current");
    egl_ok = eglMakeCurrent(renderer->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    TRACER_END(renderer->tracer, "gl_renderer_clear_current");
    
    if (egl_ok != EGL_TRUE) {
        LOG_ERROR("Could not clear the flutter EGL context. eglMakeCurrent: 0x%08X\n", eglGetError());
        return EIO;
    }

    return 0;
}

void *gl_renderer_get_proc_address(MAYBE_UNUSED struct gl_renderer *renderer, const char* name) {
    void *address;

    address = eglGetProcAddress(name);
    LOG_DEBUG("eglGetProcAddress(%s): %p\n", name, address);
    if (address) {
        return address;
    }

    address = dlsym(RTLD_DEFAULT, name);
    LOG_DEBUG("dlsym(RTLD_DEFAULT, %s): %p\n", name, address);
    if (address) {
        return address;
    }
    
    LOG_ERROR("Could not resolve EGL/GL symbol \"%s\"\n", name);
    return NULL;
}

EGLDisplay gl_renderer_get_egl_display(struct gl_renderer *renderer) {
    DEBUG_ASSERT_NOT_NULL(renderer);
    return renderer->egl_display;
}

EGLContext gl_renderer_create_context(struct gl_renderer *renderer) {
    EGLContext context;
    
    DEBUG_ASSERT_NOT_NULL(renderer);

    pthread_mutex_lock(&renderer->root_context_lock);
    context = eglCreateContext(renderer->egl_display, renderer->forced_egl_config, renderer->root_context, NULL);
    pthread_mutex_unlock(&renderer->root_context_lock);

    return context;
}

bool gl_renderer_supports_egl_extension(struct gl_renderer *renderer, const char *name) {
    DEBUG_ASSERT_NOT_NULL(renderer);
    DEBUG_ASSERT_NOT_NULL(name);
    return check_egl_extension(renderer->egl_client_exts, renderer->egl_display_exts, name);
}

bool gl_renderer_supports_gl_extension(struct gl_renderer *renderer, const char *name) {
    DEBUG_ASSERT_NOT_NULL(renderer);
    DEBUG_ASSERT_NOT_NULL(name);
    return check_egl_extension(renderer->gl_exts, NULL, name);
}

bool gl_renderer_is_llvmpipe(struct gl_renderer *renderer) {
    DEBUG_ASSERT_NOT_NULL(renderer);
    return strstr(renderer->gl_renderer, "llvmpipe") != NULL;
}

#ifdef DEBUG
static __thread bool is_render_thread = false;
#endif

int gl_renderer_make_this_a_render_thread(struct gl_renderer *renderer) {
    EGLContext context;
    EGLBoolean egl_ok;
    
    DEBUG_ASSERT_NOT_NULL(renderer);
    DEBUG_ASSERT(is_render_thread == false);

    pthread_mutex_lock(&renderer->root_context_lock);
    context = eglCreateContext(renderer->egl_display, renderer->forced_egl_config, renderer->root_context, NULL);
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
    
    DEBUG_ASSERT(is_render_thread);

    context = eglGetCurrentContext();
    DEBUG_ASSERT(context != EGL_NO_CONTEXT);

    display = eglGetCurrentDisplay();
    DEBUG_ASSERT(display != EGL_NO_CONTEXT);

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

ATTR_PURE EGLConfig gl_renderer_choose_config(struct gl_renderer *renderer, bool has_desired_pixel_format, enum pixfmt desired_pixel_format) {
    DEBUG_ASSERT_NOT_NULL(renderer);

    if (renderer->forced_egl_config != EGL_NO_CONFIG_KHR) {
        return renderer->forced_egl_config;
    }

    const EGLint config_attribs[] = {
        EGL_SURFACE_TYPE,       EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE,    EGL_OPENGL_ES2_BIT,
        EGL_SAMPLES,            0,
        EGL_NONE
    };

    return choose_config_with_pixel_format(
        renderer->egl_display,
        config_attribs,
        renderer->has_forced_pixel_format ? renderer->pixel_format :
            has_desired_pixel_format ? desired_pixel_format :
            kARGB8888
    );
}

ATTR_PURE EGLConfig gl_renderer_choose_config_direct(struct gl_renderer *renderer, enum pixfmt pixel_format) {
    DEBUG_ASSERT_NOT_NULL(renderer);
    DEBUG_ASSERT_PIXFMT_VALID(pixel_format);

    const EGLint config_attribs[] = {
        EGL_SURFACE_TYPE,       EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE,    EGL_OPENGL_ES2_BIT,
        EGL_SAMPLES,            0,
        EGL_NONE
    };

    return choose_config_with_pixel_format(
        renderer->egl_display,
        config_attribs,
        pixel_format
    );
}