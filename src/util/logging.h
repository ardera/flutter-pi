#ifndef _FLUTTERPI_SRC_UTIL_LOGGING_H
#define _FLUTTERPI_SRC_UTIL_LOGGING_H

//#define FILE_DESCR(_logging_name) static const char *__attribute__((unused)) __file_logging_name = _logging_name;

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

#endif
