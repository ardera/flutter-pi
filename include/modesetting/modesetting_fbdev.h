#ifndef _FLUTTERPI_INCLUDE_MODESETTING_FBDEV_H
#define _FLUTTERPI_INCLUDE_MODESETTING_FBDEV_H

#ifndef HAS_FBDEV
#   error "fbdev needs to be present to include modesetting_fbdev.h."
#endif // HAS_FBDEV

#include <stdbool.h>
#include <linux/fb.h>

struct fbdev_display_config {
    bool has_explicit_dimensions;
    int width_mm, height_mm;
};

struct display *fbdev_display_new_from_fd(int fd, const struct fbdev_display_config *config);

struct display *fbdev_display_new_from_path(const char *path, const struct fbdev_display_config *config);

#endif