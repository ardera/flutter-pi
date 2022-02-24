// SPDX-License-Identifier: MIT
/*
 * Just a shim for including EGL headers, and disabling EGL function prototypes if EGL is not present
 *
 * Copyright (c) 2022, Hannes Winkler <hanneswinkler2000@web.de>
 */


#ifndef _FLUTTERPI_INCLUDE_GLES_H
#define _FLUTTERPI_INCLUDE_GLES_H

#ifdef HAS_GL

#   include <GLES2/gl2.h>
#   include <GLES2/gl2ext.h>

#else

// If the system doesn't have EGL installed, we'll clone the official EGL headers,
// but don't declare the function prototypes, so we don't accidentally use one.
#   define GL_GLES_PROTOTYPES 0
#   include <GLES2/gl2.h>
#   include <GLES2/gl2ext.h>

#endif

#endif // _FLUTTERPI_INCLUDE_GLES_H
