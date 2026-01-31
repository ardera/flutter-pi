#include "plugins/dpms_plugin.h"

int32_t flutterpi_dpms_is_available() {
    return compositor_is_available_dpms(flutterpi->compositor);
}

// 0 => success, non-zero value => errno-style error
int32_t flutterpi_dpms_set(bool value) {
    return compositor_set_dpms(flutterpi->compositor, value);
}

// 0 => off, 1 => on, negative value => errno-style error
int32_t flutterpi_dpms_get() {
    return compositor_get_dpms(flutterpi->compositor);
}