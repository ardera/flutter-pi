#ifndef _DPMS_PLUGIN_H
#define _DPMS_PLUGIN_H

#include <stdint.h>
#include "flutter-pi.h"

uint32_t flutterpi_dpms_is_available();

void flutterpi_dpms_set(uint64_t value);

uint64_t flutterpi_dpms_get();

#endif
