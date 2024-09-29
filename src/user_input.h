// SPDX-License-Identifier: MIT
/*
 * User Input
 *
 * Collects user input from libinput, transforms it (partially into
 * more flutter compatible forms), and calls configured handler callbacks.
 *
 * Copyright (c) 2023, Hannes Winkler <hanneswinkler2000@web.de>
 */

#ifndef _FLUTTERPI_SRC_USER_INPUT_H
#define _FLUTTERPI_SRC_USER_INPUT_H

#include <flutter_embedder.h>
#include <xkbcommon/xkbcommon.h>

#include "libinput.h"
#include "plugins/raw_keyboard.h"
#include "util/asserts.h"
#include "util/collection.h"
#include "util/file_interface.h"
#include "util/geometry.h"

#define MAX_COLLECTED_FLUTTER_POINTER_EVENTS 64

typedef void (*utf8_character_cb_t)(void *userdata, uint8_t *character);

typedef void (*xkb_keysym_cb_t)(void *userdata, xkb_keysym_t keysym);

// clang-format off
typedef void (*gtk_keyevent_callback_t)(
    void *userdata,
    uint32_t unicode_scalar_values,
    uint32_t key_code,
    uint32_t scan_code,
    uint32_t modifiers,
    bool is_down
);
// clang-format on

// clang-format off
typedef void (*keyevent_callback_t)(
    void *userdata,
    uint64_t timestamp_us,
    xkb_keycode_t xkb_keycode,
    xkb_keysym_t xkb_keysym,
    uint32_t plain_codepoint,
    key_modifiers_t modifiers,
    const char *text,
    bool is_down,
    bool is_repeat
);
// clang-format on

struct user_input_device;

void user_input_device_set_primary_listener_userdata(struct user_input_device *device, void *userdata);

void *user_input_device_get_primary_listener_userdata(struct user_input_device *device);

int64_t user_input_device_get_id(struct user_input_device *device);

struct libinput_device *user_input_device_get_libinput_device(struct user_input_device *device);

struct udev_device *user_input_device_get_udev_device(struct user_input_device *device);

enum user_input_event_type {
    USER_INPUT_DEVICE_ADDED = 1 << 0,
    USER_INPUT_DEVICE_REMOVED = 1 << 1,
    USER_INPUT_SLOT_ADDED = 1 << 2,
    USER_INPUT_SLOT_REMOVED = 1 << 3,
    USER_INPUT_POINTER = 1 << 4,
    USER_INPUT_TOUCH = 1 << 5,
    USER_INPUT_TABLET_TOOL = 1 << 6,
    USER_INPUT_KEY = 1 << 7,
};

enum user_input_slot_type {
    USER_INPUT_SLOT_POINTER,
    USER_INPUT_SLOT_TOUCH,
    USER_INPUT_SLOT_TABLET_TOOL,
};

struct user_input_event {
    enum user_input_event_type type;
    uint64_t timestamp;

    struct user_input_device *device;
    int64_t global_slot_id;
    enum user_input_slot_type slot_type;

    union {
        struct {
            uint8_t buttons;
            uint8_t changed_buttons;
            bool is_absolute;
            union {
                struct vec2f delta;
                struct vec2f position_ndc;
            };
            struct vec2f scroll_delta;
        } pointer;

        struct {
            bool down;
            bool down_changed;
            struct vec2f position_ndc;
        } touch;

        struct {
            bool tip;
            bool tip_changed;
            enum libinput_tablet_tool_type tool;
            struct vec2f position_ndc;
        } tablet;

        struct {
            xkb_keycode_t xkb_keycode;
            xkb_keysym_t xkb_keysym;
            uint32_t plain_codepoint;
            key_modifiers_t modifiers;
            char text[8];
            bool is_down;
            bool is_repeat;
        } key;
    };
};

typedef void (*user_input_event_cb_t)(void *userdata, size_t n_events, const struct user_input_event *events);

struct user_input;

/**
 * @brief Create a new user input instance. Will try to load the default keyboard config from /etc/default/keyboard
 * and create a udev-backed libinput instance.
 */
struct user_input *user_input_new_suspended(const struct file_interface *interface, void *userdata, struct udev *udev, const char *seat);

/**
 * @brief Destroy this user input instance and free all allocated memory. This will not remove any input devices
 * added to flutter and won't invoke any callbacks in the user input interface at all.
 */
void user_input_destroy(struct user_input *input);

/**
 * @brief Returns a filedescriptor used for input event notification. The returned
 * filedescriptor should be listened to with EPOLLIN | EPOLLRDHUP | EPOLLPRI or equivalent.
 * When the fd becomes ready, @ref user_input_on_fd_ready should be called not long after it
 * became ready. (libinput somehow relies on that)
 */
int user_input_get_fd(struct user_input *input);

/**
 * @brief Should be called when the fd returned by @ref user_input_get_fd becomes ready.
 * The user_input_interface callbacks will be called inside this function.
 */
int user_input_on_fd_ready(struct user_input *input);

void user_input_suspend(struct user_input *input);

int user_input_resume(struct user_input *input);

void user_input_add_listener(struct user_input *input, enum user_input_event_type events, user_input_event_cb_t cb, void *userdata);

void user_input_add_primary_listener(struct user_input *input, enum user_input_event_type events, user_input_event_cb_t cb, void *userdata);

#endif  // _FLUTTERPI_SRC_USER_INPUT_H
