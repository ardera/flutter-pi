// SPDX-License-Identifier: MIT
/*
 * Logging - Provides debug & error logging macros.
 *
 * Copyright (c) 2023, Hannes Winkler <hanneswinkler2000@web.de>
 */

#ifndef _FLUTTERPI_SRC_UTIL_LOGGING_H
#define _FLUTTERPI_SRC_UTIL_LOGGING_H

#define LOG_ERROR(fmtstring, ...) fprintf(stderr, "%s: " fmtstring, __FILE__, ##__VA_ARGS__)
#define LOG_ERROR_UNPREFIXED(fmtstring, ...) fprintf(stderr, fmtstring, ##__VA_ARGS__)

#ifdef DEBUG
    #define LOG_DEBUG(fmtstring, ...) fprintf(stderr, "%s: " fmtstring, __FILE__, ##__VA_ARGS__)
    #define LOG_DEBUG_UNPREFIXED(fmtstring, ...) fprintf(stderr, fmtstring, ##__VA_ARGS__)
#else
    #define LOG_DEBUG(fmtstring, ...) \
        do {                          \
        } while (0)
    #define LOG_DEBUG_UNPREFIXED(fmtstring, ...) \
        do {                                     \
        } while (0)
#endif

#endif  // _FLUTTERPI_SRC_UTIL_LOGGING_H
