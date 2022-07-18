// SPDX-License-Identifier: MIT
/*
 * Just a shim for including EGL headers, and disabling EGL function prototypes if EGL is not present
 *
 * Copyright (c) 2022, Hannes Winkler <hanneswinkler2000@web.de>
 */


#ifndef _FLUTTERPI_INCLUDE_EGL_H
#define _FLUTTERPI_INCLUDE_EGL_H

#include <stdbool.h>
#include <string.h>

#ifdef HAS_EGL

#   include <EGL/egl.h>
#   include <EGL/eglext.h>

#else

// If the system doesn't have EGL installed, we'll clone the official EGL headers and include them,
// but don't declare the function prototypes so we don't accidentally use one.
#   define EGL_EGL_PROTOTYPES 0
#   include <EGL/egl.h>
#   include <EGL/eglext.h>

#endif

static inline bool check_egl_extension(const char *client_ext_string, const char *display_ext_string, const char *extension) {
    size_t len = strlen(extension);

    if (client_ext_string != NULL) {
        const char *result = strstr(client_ext_string, extension);
        if (result != NULL && (result[len] == ' ' || result[len] == '\0')) {
            return true;
        }
    }
    
    if (display_ext_string != NULL) {
        const char *result = strstr(display_ext_string, extension);
        if (result != NULL && (result[len] == ' ' || result[len] == '\0')) {
            return true;
        }
    }

    return false;
}

static inline const char *egl_strerror(EGLenum result) {
    switch (result) {
        case EGL_SUCCESS: return "EGL_SUCCESS";
        case EGL_NOT_INITIALIZED: return "EGL_NOT_INITIALIZED";
        case EGL_BAD_ACCESS: return "EGL_BAD_ACCESS";
        case EGL_BAD_ALLOC: return "EGL_BAD_ALLOC";
        case EGL_BAD_ATTRIBUTE: return "EGL_BAD_ATTRIBUTE";
        case EGL_BAD_CONFIG: return "EGL_BAD_CONFIG";
        case EGL_BAD_CONTEXT: return "EGL_BAD_CONTEXT";
        case EGL_BAD_CURRENT_SURFACE: return "EGL_BAD_CURRENT_SURFACE";
        case EGL_BAD_DISPLAY: return "EGL_BAD_DISPLAY";
        case EGL_BAD_MATCH: return "EGL_BAD_MATCH";
        case EGL_BAD_NATIVE_PIXMAP: return "EGL_BAD_NATIVE_PIXMAP";
        case EGL_BAD_NATIVE_WINDOW: return "EGL_BAD_NATIVE_WINDOW";
        case EGL_BAD_PARAMETER: return "EGL_BAD_PARAMETER";
        case EGL_BAD_SURFACE: return "EGL_BAD_SURFACE";
        case EGL_CONTEXT_LOST: return "EGL_CONTEXT_LOST";
        default: return "<unknown result code>";
    }
}

#define LOG_EGL_ERROR(result, fmt, ...) LOG_ERROR(fmt ": %s\n", __VA_ARGS__ egl_strerror(result))


#endif // _FLUTTERPI_INCLUDE_EGL_H
