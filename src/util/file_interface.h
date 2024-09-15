#ifndef _FLUTTERPI_SRC_UTIL_FILE_INTERFACE_H
#define _FLUTTERPI_SRC_UTIL_FILE_INTERFACE_H

/**
 * @brief Interface that will be used to open and close files.
 */
struct file_interface {
    int (*open)(const char *path, int flags, void **fd_metadata_out, void *userdata);
    void (*close)(int fd, void *fd_metadata, void *userdata);
};

#endif  // _FLUTTERPI_SRC_UTIL_FILE_INTERFACE_H
