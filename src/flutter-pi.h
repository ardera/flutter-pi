// SPDX-License-Identifier: MIT
/*
 * Flutter-Pi main header
 *
 * Copyright (c) 2023, Hannes Winkler <hanneswinkler2000@web.de>
 */

#ifndef _FLUTTERPI_SRC_FLUTTERPI_H
#define _FLUTTERPI_SRC_FLUTTERPI_H

#define LOG_FLUTTERPI_ERROR(...) fprintf(stderr, "[flutter-pi] " __VA_ARGS__)

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glob.h>

#include <flutter_embedder.h>
#include <libinput.h>
#include <linux/input.h>
#include <systemd/sd-event.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "cursor.h"
#include "pixel_format.h"
#include "util/collection.h"

enum device_orientation { kPortraitUp, kLandscapeLeft, kPortraitDown, kLandscapeRight };

#define ORIENTATION_IS_LANDSCAPE(orientation) ((orientation) == kLandscapeLeft || (orientation) == kLandscapeRight)
#define ORIENTATION_IS_PORTRAIT(orientation) ((orientation) == kPortraitUp || (orientation) == kPortraitDown)
#define ORIENTATION_IS_VALID(orientation) \
    ((orientation) == kPortraitUp || (orientation) == kLandscapeLeft || (orientation) == kPortraitDown || (orientation) == kLandscapeRight)

#define ORIENTATION_ROTATE_CW(orientation)                \
    ((orientation) == kPortraitUp     ? kLandscapeLeft :  \
     (orientation) == kLandscapeLeft  ? kPortraitDown :   \
     (orientation) == kPortraitDown   ? kLandscapeRight : \
     (orientation) == kLandscapeRight ? kPortraitUp :     \
                                        (assert(0 && "invalid device orientation"), 0))

#define ORIENTATION_ROTATE_CCW(orientation)               \
    ((orientation) == kPortraitUp     ? kLandscapeRight : \
     (orientation) == kLandscapeLeft  ? kPortraitUp :     \
     (orientation) == kPortraitDown   ? kLandscapeLeft :  \
     (orientation) == kLandscapeRight ? kPortraitDown :   \
                                        (assert(0 && "invalid device orientation"), 0))

#define ANGLE_FROM_ORIENTATION(o) \
    ((o) == kPortraitUp ? 0 : (o) == kLandscapeLeft ? 90 : (o) == kPortraitDown ? 180 : (o) == kLandscapeRight ? 270 : 0)

#define ANGLE_BETWEEN_ORIENTATIONS(o_start, o_end)                     \
    (ANGLE_FROM_ORIENTATION(o_end) - ANGLE_FROM_ORIENTATION(o_start) + \
     (ANGLE_FROM_ORIENTATION(o_start) > ANGLE_FROM_ORIENTATION(o_end) ? 360 : 0))

#define FLUTTER_RESULT_TO_STRING(result)                               \
    ((result) == kSuccess               ? "Success." :                 \
     (result) == kInvalidLibraryVersion ? "Invalid library version." : \
     (result) == kInvalidArguments      ? "Invalid arguments." :       \
     (result) == kInternalInconsistency ? "Internal inconsistency." :  \
                                          "(?)")

/// TODO: Move this
#define LIBINPUT_EVENT_IS_TOUCH(event_type)                                                            \
    (((event_type) == LIBINPUT_EVENT_TOUCH_DOWN) || ((event_type) == LIBINPUT_EVENT_TOUCH_UP) ||       \
     ((event_type) == LIBINPUT_EVENT_TOUCH_MOTION) || ((event_type) == LIBINPUT_EVENT_TOUCH_CANCEL) || \
     ((event_type) == LIBINPUT_EVENT_TOUCH_FRAME))

#define LIBINPUT_EVENT_IS_POINTER(event_type)                                                                       \
    (((event_type) == LIBINPUT_EVENT_POINTER_MOTION) || ((event_type) == LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE) || \
     ((event_type) == LIBINPUT_EVENT_POINTER_BUTTON) || ((event_type) == LIBINPUT_EVENT_POINTER_AXIS))

#define LIBINPUT_EVENT_IS_KEYBOARD(event_type) (((event_type) == LIBINPUT_EVENT_KEYBOARD_KEY))

enum flutter_runtime_mode { FLUTTER_RUNTIME_MODE_DEBUG, FLUTTER_RUNTIME_MODE_PROFILE, FLUTTER_RUNTIME_MODE_RELEASE };

#define FLUTTER_RUNTIME_MODE_IS_JIT(runtime_mode) ((runtime_mode) == FLUTTER_RUNTIME_MODE_DEBUG)
#define FLUTTER_RUNTIME_MODE_IS_AOT(runtime_mode) \
    ((runtime_mode) == FLUTTER_RUNTIME_MODE_PROFILE || (runtime_mode) == FLUTTER_RUNTIME_MODE_RELEASE)

struct compositor;
struct plugin_registry;
struct texture_registry;
struct drmdev;
struct locales;
struct vk_renderer;
struct flutterpi;

/// TODO: Remove this
extern struct flutterpi *flutterpi;

struct platform_task {
    int (*callback)(void *userdata);
    void *userdata;
};

struct platform_message {
    bool is_response;
    union {
        const FlutterPlatformMessageResponseHandle *target_handle;
        struct {
            char *target_channel;
            FlutterPlatformMessageResponseHandle *response_handle;
        };
    };
    uint8_t *message;
    size_t message_size;
};

struct flutterpi_cmdline_args {
    bool has_orientation;
    enum device_orientation orientation;

    bool has_rotation;
    int rotation;

    bool has_physical_dimensions;
    struct vec2i physical_dimensions;

    bool has_pixel_format;
    enum pixfmt pixel_format;

    bool has_runtime_mode;
    enum flutter_runtime_mode runtime_mode;

    char *bundle_path;

    int engine_argc;
    char **engine_argv;

    bool use_vulkan;

    char *desired_videomode;

    bool dummy_display;
    struct vec2i dummy_display_size;

    char *drm_vout_display;
};

int flutterpi_fill_view_properties(bool has_orientation, enum device_orientation orientation, bool has_rotation, int rotation);

int flutterpi_post_platform_task(int (*callback)(void *userdata), void *userdata);

int flutterpi_post_platform_task_with_time(int (*callback)(void *userdata), void *userdata, uint64_t target_time_usec);

int flutterpi_sd_event_add_io(sd_event_source **source_out, int fd, uint32_t events, sd_event_io_handler_t callback, void *userdata);

int flutterpi_send_platform_message(
    struct flutterpi *flutterpi,
    const char *channel,
    const uint8_t *restrict message,
    size_t message_size,
    FlutterPlatformMessageResponseHandle *responsehandle
);

int flutterpi_respond_to_platform_message(
    const FlutterPlatformMessageResponseHandle *handle,
    const uint8_t *restrict message,
    size_t message_size
);

bool flutterpi_parse_cmdline_args(int argc, char **argv, struct flutterpi_cmdline_args *result_out);

struct texture_registry *flutterpi_get_texture_registry(struct flutterpi *flutterpi);

struct plugin_registry *flutterpi_get_plugin_registry(struct flutterpi *flutterpi);

FlutterPlatformMessageResponseHandle *
flutterpi_create_platform_message_response_handle(struct flutterpi *flutterpi, FlutterDataCallback data_callback, void *userdata);

void flutterpi_release_platform_message_response_handle(struct flutterpi *flutterpi, FlutterPlatformMessageResponseHandle *handle);

struct texture *flutterpi_create_texture(struct flutterpi *flutterpi);

const char *flutterpi_get_asset_bundle_path(struct flutterpi *flutterpi);

void flutterpi_schedule_exit(struct flutterpi *flutterpi);

struct gbm_device *flutterpi_get_gbm_device(struct flutterpi *flutterpi);

bool flutterpi_has_gl_renderer(struct flutterpi *flutterpi);

struct gl_renderer *flutterpi_get_gl_renderer(struct flutterpi *flutterpi);

void flutterpi_set_pointer_kind(struct flutterpi *flutterpi, enum pointer_kind kind);

void flutterpi_trace_event_instant(struct flutterpi *flutterpi, const char *name);

void flutterpi_trace_event_begin(struct flutterpi *flutterpi, const char *name);

void flutterpi_trace_event_end(struct flutterpi *flutterpi, const char *name);

#endif  // _FLUTTERPI_SRC_FLUTTERPI_H
