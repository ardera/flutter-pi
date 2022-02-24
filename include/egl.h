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

#endif // _FLUTTERPI_INCLUDE_EGL_H
