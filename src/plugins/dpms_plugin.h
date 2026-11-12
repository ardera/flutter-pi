// SPDX-License-Identifier: MIT
#ifndef _FLUTTERPI_PLUGINS_DPMS_PLUGIN_H
#define _FLUTTERPI_PLUGINS_DPMS_PLUGIN_H

#include <stdbool.h>
#include <stdint.h>

/** FFI ABI: 0 unavailable, 1 available. */
int32_t flutterpi_dpms_is_available(void);

/** FFI ABI: 0 off, 1 on, negative errno on error. */
int32_t flutterpi_dpms_get(void);

/** FFI ABI: 0 success, positive errno on error. May block for a modeset. */
int32_t flutterpi_dpms_set(bool enabled);

#endif  // _FLUTTERPI_PLUGINS_DPMS_PLUGIN_H
