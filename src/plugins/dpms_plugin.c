// SPDX-License-Identifier: MIT
#include "plugins/dpms_plugin.h"

#include <errno.h>

#include "compositor_ng.h"
#include "flutter-pi.h"

int32_t flutterpi_dpms_is_available(void) {
    return flutterpi != NULL && compositor_supports_dpms(flutterpi_get_compositor(flutterpi));
}

int32_t flutterpi_dpms_get(void) {
    if (flutterpi == NULL) {
        return -ENODEV;
    }
    return compositor_get_dpms(flutterpi_get_compositor(flutterpi));
}

int32_t flutterpi_dpms_set(bool enabled) {
    if (flutterpi == NULL) {
        return ENODEV;
    }
    return compositor_set_dpms(flutterpi_get_compositor(flutterpi), enabled);
}
