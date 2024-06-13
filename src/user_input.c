#include "user_input.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <libinput.h>
#include <linux/input-event-codes.h>
#include <memory.h>
#include <systemd/sd-event.h>

#include "compositor_ng.h"
#include "flutter-pi.h"
#include "keyboard.h"
#include "util/collection.h"
#include "util/logging.h"

#define LIBINPUT_VER(major, minor, patch) ((((major) & 0xFF) << 16) | (((minor) & 0xFF) << 8) | ((patch) & 0xFF))
#define THIS_LIBINPUT_VER LIBINPUT_VER(LIBINPUT_VERSION_MAJOR, LIBINPUT_VERSION_MINOR, LIBINPUT_VERSION_PATCH)

struct input_device_data {
    struct keyboard_state *keyboard_state;
    int64_t buttons;
    uint64_t timestamp;

    /**
     * @brief Whether libinput has ever emitted any pointer / mouse events
     * for this device.
     *
     * Only applies to devices which have LIBINPUT_DEVICE_CAP_POINTER.
     */
    bool has_emitted_pointer_events;

    /**
     * @brief Only applies to tablets. True if the tablet tool is in contact with the screen right now.
     *
     */
    bool tip;

    /**
     * @brief The current touch positions for each multitouch slot.
     *  
     */
    struct vec2f *positions;

    int64_t touch_device_id_offset;
    int64_t stylus_device_id;
};

struct user_input {
    struct user_input_interface interface;
    void *userdata;

    struct libinput *libinput;
    struct keyboard_config *kbdcfg;
    int64_t next_unused_flutter_device_id;

    /// TODO: Maybe fetch the transform, display dimensions, cursor pos dynamically using a callback instead?

    /**
     * @brief transforms normalized display coordinates (0 .. display_width-1, 0 .. display_height-1) to the coordinates
     * used in the flutter pointer events
     */
    struct mat3f display_to_view_transform;
    struct mat3f view_to_display_transform_nontranslating;
    unsigned int display_width;
    unsigned int display_height;

    /**
     * @brief The number of devices connected that want a mouse cursor.
     * libinput calls them pointer devices, flutter calls them mice.
     */
    unsigned int n_cursor_devices;
    /**
     * @brief The flutter device id of the mouse cursor, if @ref n_cursor_devices > 0.
     */
    int64_t cursor_flutter_device_id;
    /**
     * @brief The current mouse cursor position in floating point display coordinates (0 .. display_width-1, 0 .. display_height-1)
     */
    double cursor_x, cursor_y;

    /**
     * @brief Buffer of collected flutter pointer events, since we can send multiple events at once to flutter.
     */
    FlutterPointerEvent collected_flutter_pointer_events[MAX_COLLECTED_FLUTTER_POINTER_EVENTS];
    /**
     * @brief Number of pointer events currently contained in @ref collected_flutter_pointer_events.
     */
    size_t n_collected_flutter_pointer_events;
};

static inline FlutterPointerEvent make_touch_event(FlutterPointerPhase phase, size_t timestamp, struct vec2f pos, int32_t device_id) {
    FlutterPointerEvent event;
    memset(&event, 0, sizeof(event));

    event.struct_size = sizeof(event);
    event.phase = phase;
    event.timestamp = timestamp;
    event.x = pos.x;
    event.y = pos.y;
    event.device = device_id;
    event.signal_kind = kFlutterPointerSignalKindNone;
    event.scroll_delta_x = 0.0;
    event.scroll_delta_y = 0.0;
    event.device_kind = kFlutterPointerDeviceKindTouch;
    event.buttons = 0;
    event.pan_x = 0.0;
    event.pan_y = 0.0;
    event.scale = 0.0;
    event.rotation = 0.0;

    return event;
}

UNUSED static inline FlutterPointerEvent make_touch_cancel_event(size_t timestamp, struct vec2f pos, int32_t device_id) {
    return make_touch_event(kCancel, timestamp, pos, device_id);
}

static inline FlutterPointerEvent make_touch_up_event(size_t timestamp, struct vec2f pos, int32_t device_id) {
    return make_touch_event(kUp, timestamp, pos, device_id);
}

static inline FlutterPointerEvent make_touch_down_event(size_t timestamp, struct vec2f pos, int32_t device_id) {
    return make_touch_event(kDown, timestamp, pos, device_id);
}

static inline FlutterPointerEvent make_touch_move_event(size_t timestamp, struct vec2f pos, int32_t device_id) {
    return make_touch_event(kMove, timestamp, pos, device_id);
}

static inline FlutterPointerEvent make_touch_add_event(size_t timestamp, struct vec2f pos, int32_t device_id) {
    return make_touch_event(kAdd, timestamp, pos, device_id);
}

static inline FlutterPointerEvent make_touch_remove_event(size_t timestamp, struct vec2f pos, int32_t device_id) {
    return make_touch_event(kRemove, timestamp, pos, device_id);
}

static inline FlutterPointerEvent make_mouse_event(
    FlutterPointerPhase phase,
    size_t timestamp,
    struct vec2f pos,
    int32_t device_id,
    FlutterPointerSignalKind signal_kind,
    struct vec2f scroll_delta,
    int64_t buttons
) {
    FlutterPointerEvent event;
    memset(&event, 0, sizeof(event));

    event.struct_size = sizeof(event);
    event.phase = phase;
    event.timestamp = timestamp;
    event.x = pos.x;
    event.y = pos.y;
    event.device = device_id;
    event.signal_kind = signal_kind;
    event.scroll_delta_x = scroll_delta.x;
    event.scroll_delta_y = scroll_delta.y;
    event.device_kind = kFlutterPointerDeviceKindMouse;
    event.buttons = buttons;
    event.pan_x = 0.0;
    event.pan_y = 0.0;
    event.scale = 0.0;
    event.rotation = 0.0;

    return event;
}

UNUSED static inline FlutterPointerEvent make_mouse_cancel_event(size_t timestamp, struct vec2f pos, int32_t device_id) {
    return make_mouse_event(kCancel, timestamp, pos, device_id, kFlutterPointerSignalKindNone, VEC2F(0, 0), 0);
}

UNUSED static inline FlutterPointerEvent make_mouse_up_event(size_t timestamp, struct vec2f pos, int32_t device_id, int64_t buttons) {
    return make_mouse_event(kUp, timestamp, pos, device_id, kFlutterPointerSignalKindNone, VEC2F(0, 0), buttons);
}

UNUSED static inline FlutterPointerEvent make_mouse_down_event(size_t timestamp, struct vec2f pos, int32_t device_id, int64_t buttons) {
    return make_mouse_event(kDown, timestamp, pos, device_id, kFlutterPointerSignalKindNone, VEC2F(0, 0), buttons);
}

static inline FlutterPointerEvent make_mouse_move_event(size_t timestamp, struct vec2f pos, int32_t device_id, int64_t buttons) {
    return make_mouse_event(kMove, timestamp, pos, device_id, kFlutterPointerSignalKindNone, VEC2F(0, 0), buttons);
}

static inline FlutterPointerEvent make_mouse_add_event(size_t timestamp, struct vec2f pos, int32_t device_id, int64_t buttons) {
    return make_mouse_event(kAdd, timestamp, pos, device_id, kFlutterPointerSignalKindNone, VEC2F(0, 0), buttons);
}

static inline FlutterPointerEvent make_mouse_remove_event(size_t timestamp, struct vec2f pos, int32_t device_id) {
    return make_mouse_event(kRemove, timestamp, pos, device_id, kFlutterPointerSignalKindNone, VEC2F(0, 0), 0);
}

static inline FlutterPointerEvent make_mouse_hover_event(size_t timestamp, struct vec2f pos, int32_t device_id, int64_t buttons) {
    return make_mouse_event(kHover, timestamp, pos, device_id, kFlutterPointerSignalKindNone, VEC2F(0, 0), buttons);
}

static inline FlutterPointerEvent make_stylus_event(FlutterPointerPhase phase, size_t timestamp, struct vec2f pos, int32_t device_id) {
    FlutterPointerEvent event;
    memset(&event, 0, sizeof(event));

    event.struct_size = sizeof(event);
    event.phase = phase;
    event.timestamp = timestamp;
    event.x = pos.x;
    event.y = pos.y;
    event.device = device_id;
    event.signal_kind = kFlutterPointerSignalKindNone;
    event.scroll_delta_x = 0.0;
    event.scroll_delta_y = 0.0;
    event.device_kind = kFlutterPointerDeviceKindStylus;
    event.buttons = 0;
    event.pan_x = 0.0;
    event.pan_y = 0.0;
    event.scale = 0.0;
    event.rotation = 0.0;

    return event;
}

UNUSED static inline FlutterPointerEvent make_stylus_cancel_event(size_t timestamp, struct vec2f pos, int32_t device_id) {
    return make_stylus_event(kCancel, timestamp, pos, device_id);
}

static inline FlutterPointerEvent make_stylus_up_event(size_t timestamp, struct vec2f pos, int32_t device_id) {
    return make_stylus_event(kUp, timestamp, pos, device_id);
}

static inline FlutterPointerEvent make_stylus_down_event(size_t timestamp, struct vec2f pos, int32_t device_id) {
    return make_stylus_event(kDown, timestamp, pos, device_id);
}

static inline FlutterPointerEvent make_stylus_move_event(size_t timestamp, struct vec2f pos, int32_t device_id) {
    return make_stylus_event(kMove, timestamp, pos, device_id);
}

static inline FlutterPointerEvent make_stylus_hover_event(size_t timestamp, struct vec2f pos, int32_t device_id) {
    return make_stylus_event(kHover, timestamp, pos, device_id);
}

static inline FlutterPointerEvent make_stylus_add_event(size_t timestamp, struct vec2f pos, int32_t device_id) {
    return make_stylus_event(kAdd, timestamp, pos, device_id);
}

static inline FlutterPointerEvent make_stylus_remove_event(size_t timestamp, struct vec2f pos, int32_t device_id) {
    return make_stylus_event(kRemove, timestamp, pos, device_id);
}

// libinput interface
static int on_open(const char *path, int flags, void *userdata) {
    struct user_input *input;

    ASSERT_NOT_NULL(path);
    ASSERT_NOT_NULL(userdata);
    input = userdata;

    return input->interface.open(path, flags | O_CLOEXEC, input->userdata);
}

static void on_close(int fd, void *userdata) {
    struct user_input *input;

    ASSERT_NOT_NULL(userdata);
    input = userdata;

    return input->interface.close(fd, input->userdata);
}

static const struct libinput_interface libinput_interface = { .open_restricted = on_open, .close_restricted = on_close };

struct user_input *user_input_new(
    const struct user_input_interface *interface,
    void *userdata,
    const struct mat3f *display_to_view_transform,
    const struct mat3f *view_to_display_transform,
    unsigned int display_width,
    unsigned int display_height
) {
    struct keyboard_config *kbdcfg;
    struct user_input *input;
    struct libinput *libinput;
    struct udev *udev;
    int ok;

    input = malloc(sizeof *input);
    if (input == NULL) {
        goto fail_return_null;
    }

    udev = udev_new();
    if (udev == NULL) {
        perror("[flutter-pi] Could not create udev instance. udev_new");
        goto fail_free_input;
    }

    input->interface = *interface;
    input->userdata = userdata;

    libinput = libinput_udev_create_context(&libinput_interface, input, udev);
    if (libinput == NULL) {
        perror("[flutter-pi] Could not create libinput instance. libinput_udev_create_context");
        goto fail_unref_udev;
    }

    udev_unref(udev);

    ok = libinput_udev_assign_seat(libinput, "seat0");
    if (ok < 0) {
        LOG_ERROR("Could not assign udev seat to libinput instance. libinput_udev_assign_seat: %s\n", strerror(-ok));
        goto fail_unref_libinput;
    }

#ifdef BUILD_TEXT_INPUT_PLUGIN
    kbdcfg = keyboard_config_new();
    if (kbdcfg == NULL) {
        LOG_ERROR("Could not initialize keyboard configuration. Flutter-pi will run without text/raw keyboard input.\n");
    }
#else
    kbdcfg = NULL;
#endif

    input->libinput = libinput;
    input->kbdcfg = kbdcfg;
    input->next_unused_flutter_device_id = 0;

    user_input_set_transform(input, display_to_view_transform, view_to_display_transform, display_width, display_height);

    input->n_cursor_devices = 0;
    input->cursor_flutter_device_id = -1;
    input->cursor_x = 0.0;
    input->cursor_y = 0.0;

    input->n_collected_flutter_pointer_events = 0;

    return input;

fail_unref_libinput:
    libinput_unref(libinput);
    goto fail_free_input;

fail_unref_udev:
    udev_unref(udev);

fail_free_input:
    free(input);

fail_return_null:
    return NULL;
}

static int on_device_removed(struct user_input *input, struct libinput_event *event, uint64_t timestamp, bool emit_flutter_events);

void user_input_destroy(struct user_input *input) {
    enum libinput_event_type event_type;
    struct libinput_event *event;
    ASSERTED int ok;

    assert(input != NULL);

    /// TODO: Destroy all the input device data, maybe add an additional
    /// parameter to indicate whether any flutter device removal events should be
    /// emitted.

    libinput_suspend(input->libinput);
    libinput_dispatch(input->libinput);

    // handle all device removal events
    while (libinput_next_event_type(input->libinput) != LIBINPUT_EVENT_NONE) {
        event = libinput_get_event(input->libinput);
        event_type = libinput_event_get_type(event);

        switch (event_type) {
            case LIBINPUT_EVENT_DEVICE_REMOVED:
                ok = on_device_removed(input, event, 0, false);
                ASSERT_ZERO(ok);
                break;
            default: break;
        }

        libinput_event_destroy(event);
    }

    if (input->kbdcfg != NULL) {
        keyboard_config_destroy(input->kbdcfg);
    }
    libinput_unref(input->libinput);
    free(input);
}

void user_input_set_transform(
    struct user_input *input,
    const struct mat3f *display_to_view_transform,
    const struct mat3f *view_to_display_transform,
    unsigned int display_width,
    unsigned int display_height
) {
    assert(input != NULL);
    assert(display_to_view_transform != NULL);
    assert(view_to_display_transform != NULL);

    input->display_to_view_transform = *display_to_view_transform;
    input->view_to_display_transform_nontranslating = *view_to_display_transform;
    input->view_to_display_transform_nontranslating.transX = 0.0;
    input->view_to_display_transform_nontranslating.transY = 0.0;
    input->display_width = display_width;
    input->display_height = display_height;
}

int user_input_get_fd(struct user_input *input) {
    assert(input != NULL);
    return libinput_get_fd(input->libinput);
}

void user_input_suspend(struct user_input *input) {
    ASSERT_NOT_NULL(input);
    libinput_suspend(input->libinput);
}

int user_input_resume(struct user_input *input) {
    int ok;

    ASSERT_NOT_NULL(input);
    ok = libinput_resume(input->libinput);
    if (ok < 0) {
        LOG_ERROR("Couldn't resume libinput event processing. libinput_resume: %s\n", strerror(errno));
        return errno;
    }

    return 0;
}

static void flush_pointer_events(struct user_input *input) {
    assert(input != NULL);

    if (input->n_collected_flutter_pointer_events > 0) {
        input->interface.on_flutter_pointer_event(
            input->userdata,
            input->collected_flutter_pointer_events,
            input->n_collected_flutter_pointer_events
        );

        input->n_collected_flutter_pointer_events = 0;
    }
}

UNUSED static void emit_pointer_events(struct user_input *input, const FlutterPointerEvent *events, size_t n_events) {
    assert(input != NULL);
    assert(events != NULL);

    size_t to_copy;

    while (n_events > 0) {
        // if the internal buffer is full, flush it
        if (input->n_collected_flutter_pointer_events == MAX_COLLECTED_FLUTTER_POINTER_EVENTS) {
            flush_pointer_events(input);
        }

        // how many pointer events we can copy into the internal pointer event buffer?
        to_copy = MIN2(n_events, MAX_COLLECTED_FLUTTER_POINTER_EVENTS - input->n_collected_flutter_pointer_events);

        // copy into the internal pointer event buffer
        memcpy(
            input->collected_flutter_pointer_events + input->n_collected_flutter_pointer_events,
            events,
            to_copy * sizeof(FlutterPointerEvent)
        );

        // advance n_events so it's now the number of remaining unemitted events
        n_events -= to_copy;

        // advance events so it points to the first remaining unemitted event
        events += to_copy;

        // advance the number of stored pointer events
        input->n_collected_flutter_pointer_events += to_copy;
    }
}

static void emit_pointer_event(struct user_input *input, const FlutterPointerEvent event) {
    assert(input != NULL);

    // if the internal buffer is full, flush it
    if (input->n_collected_flutter_pointer_events == MAX_COLLECTED_FLUTTER_POINTER_EVENTS) {
        flush_pointer_events(input);
    }

    memcpy(input->collected_flutter_pointer_events + input->n_collected_flutter_pointer_events, &event, sizeof(event));

    input->n_collected_flutter_pointer_events += 1;
}

/**
 * @brief Called when input->n_cursor_devices was increased to maybe enable the mouse cursor
 * it it isn't yet enabled.
 */
static void maybe_enable_mouse_cursor(struct user_input *input, uint64_t timestamp) {
    assert(input != NULL);

    if (input->n_cursor_devices == 1) {
        if (input->cursor_flutter_device_id == -1) {
            input->cursor_flutter_device_id = input->next_unused_flutter_device_id++;
        }

        emit_pointer_event(
            input,
            make_mouse_add_event(timestamp, VEC2F(input->cursor_x, input->cursor_y), input->cursor_flutter_device_id, 0)
        );
    }
}

/**
 * @brief Called when input->n_cursor_devices was decreased to maybe disable the mouse cursor.
 */
static void maybe_disable_mouse_cursor(struct user_input *input, uint64_t timestamp) {
    assert(input != NULL);

    if (input->n_cursor_devices == 0) {
        emit_pointer_event(
            input,
            make_mouse_remove_event(timestamp, VEC2F(input->cursor_x, input->cursor_y), input->cursor_flutter_device_id)
        );
    }
}

static int on_device_added(struct user_input *input, struct libinput_event *event, uint64_t timestamp) {
    struct input_device_data *data;
    struct libinput_device *device;
    struct vec2f *positions;
    int64_t device_id;

    assert(input != NULL);
    assert(event != NULL);

    device = libinput_event_get_device(event);

    data = malloc(sizeof *data);
    if (data == NULL) {
        return ENOMEM;
    }

    data->touch_device_id_offset = -1;
    data->stylus_device_id = -1;
    data->keyboard_state = NULL;
    data->buttons = 0;
    data->timestamp = timestamp;
    data->has_emitted_pointer_events = false;
    data->tip = false;
    data->positions = NULL;

    libinput_device_set_user_data(device, data);

    if (libinput_device_has_capability(device, LIBINPUT_DEVICE_CAP_POINTER)) {
        // no special things to do here
        // mouse pointer will be added as soon as the device actually sends a
        // mouse event, as some devices will erroneously have a LIBINPUT_DEVICE_CAP_POINTER
        // even though they aren't mice. (My keyboard for example is a mouse smh)

        // reserve one id for the mouse pointer
        // input->next_unused_flutter_device_id++;
    }

    if (libinput_device_has_capability(device, LIBINPUT_DEVICE_CAP_TOUCH)) {
        // add all touch slots as individual touch devices to flutter
        int n_slots = libinput_device_touch_get_touch_count(device);
        if (n_slots == -1) {
            LOG_ERROR("Could not query input device multitouch slot count.\n");
            goto fail_free_data;
        } else if (n_slots == 0) {
            LOG_ERROR("Input devive has unknown number of multitouch slots.\n");
            goto fail_free_data;
        }

        data->touch_device_id_offset = input->next_unused_flutter_device_id;

        for (int i = 0; i < n_slots; i++) {
            device_id = input->next_unused_flutter_device_id++;

            emit_pointer_event(input, make_touch_add_event(timestamp, VEC2F(0, 0), device_id));
        }

        positions = malloc(n_slots * sizeof(struct vec2f));
        if (positions == NULL) {
            goto fail_free_data;
        }

        data->positions = positions;
    }

    if (libinput_device_has_capability(device, LIBINPUT_DEVICE_CAP_KEYBOARD)) {
        // create a new keyboard state for this keyboard
        if (input->kbdcfg) {
            data->keyboard_state = keyboard_state_new(input->kbdcfg, NULL, NULL);
        } else {
            // If we don't have a keyboard config
            data->keyboard_state = NULL;
        }
    }

    if (libinput_device_has_capability(device, LIBINPUT_DEVICE_CAP_TABLET_TOOL)) {
        device_id = input->next_unused_flutter_device_id++;

        data->stylus_device_id = device_id;

        emit_pointer_event(input, make_stylus_add_event(timestamp, VEC2F(0, 0), device_id));
    }

    return 0;

fail_free_data:
    free(data);
    return EINVAL;
}

static int on_device_removed(struct user_input *input, struct libinput_event *event, uint64_t timestamp, bool emit_flutter_events) {
    struct input_device_data *data;
    struct libinput_device *device;

    assert(input != NULL);
    assert(event != NULL);

    device = libinput_event_get_device(event);

    data = libinput_device_get_user_data(device);

    // on_device_removed is special here because it's also invoked
    // outside the normal user_input_on_fd_ready function as well,
    // in user_input_destroy.
    //
    // user_input_destroy calls libinput_suspend and then handles all events,
    // but only the DEVICE_REMOVED events, so it could be we skip some
    // DEVICE_ADDED events as well
    //
    // so it might be the data is null here because on_device_added
    // was never called for this device.
    if (data == NULL) {
        return 0;
    }

    if (libinput_device_has_capability(device, LIBINPUT_DEVICE_CAP_POINTER)) {
        if (data->has_emitted_pointer_events) {
            input->n_cursor_devices--;
            if (emit_flutter_events) {
                maybe_disable_mouse_cursor(input, timestamp);
            }
        }
    }

    if (libinput_device_has_capability(device, LIBINPUT_DEVICE_CAP_TOUCH)) {
        // add all touch slots as individual touch devices to flutter
        if (emit_flutter_events) {
            for (int i = 0; i < libinput_device_touch_get_touch_count(device); i++) {
                emit_pointer_event(input, make_touch_remove_event(timestamp, VEC2F(0, 0), data->touch_device_id_offset + i));
            }
        }
    }

    if (libinput_device_has_capability(device, LIBINPUT_DEVICE_CAP_KEYBOARD)) {
        // create a new keyboard state for this keyboard
        if (data->keyboard_state != NULL) {
            keyboard_state_destroy(data->keyboard_state);
        }
    }

    if (libinput_device_has_capability(device, LIBINPUT_DEVICE_CAP_TABLET_TOOL)) {
        emit_pointer_event(input, make_stylus_remove_event(timestamp, VEC2F(0, 0), data->stylus_device_id));
    }

    if (data != NULL) {
        if (data->positions != NULL) {
            free(data->positions);
        }
        free(data);
    }

    libinput_device_set_user_data(device, NULL);

    return 0;
}

static int on_key_event(struct user_input *input, struct libinput_event *event) {
    struct libinput_event_keyboard *key_event;
    struct input_device_data *data;
    struct libinput_device *device;
    enum libinput_key_state key_state;
    xkb_keysym_t keysym;
    uint32_t codepoint, plain_codepoint;
    uint16_t evdev_keycode;
    int ok;

    assert(input != NULL);
    assert(event != NULL);

    key_event = libinput_event_get_keyboard_event(event);
    device = libinput_event_get_device(event);
    data = libinput_device_get_user_data(device);

    evdev_keycode = (uint16_t) libinput_event_keyboard_get_key(key_event);
    key_state = libinput_event_keyboard_get_key_state(key_event);

    LOG_DEBUG("on_key_event\n");

    // If we don't have a keyboard state (for example if we couldn't load /etc/default/keyboard)
    // we just return here.
    if (data->keyboard_state == NULL) {
        return 0;
    }

    // Let the keyboard advance its statemachine.
    // keysym/codepoint are 0 when none were emitted.
    keysym = 0;
    codepoint = 0;
    ok = keyboard_state_process_key_event(data->keyboard_state, evdev_keycode, (int32_t) key_state, &keysym, &codepoint);
    if (ok != 0) {
        return ok;
    }

    // GTK keyevent needs the plain codepoint for some reason.
    /// TODO: Maybe remove the evdev_value parameter?
    plain_codepoint = keyboard_state_get_plain_codepoint(data->keyboard_state, evdev_keycode, 1);

    LOG_DEBUG(
        "keyboard state ctrl active: %s, alt active: %s, keysym: 0x%04" PRIx32 "\n",
        keyboard_state_is_ctrl_active(data->keyboard_state) ? "yes" : "no",
        keyboard_state_is_alt_active(data->keyboard_state) ? "yes" : "no",
        keysym
    );

    if (input->interface.on_switch_vt != NULL && keysym >= XKB_KEY_XF86Switch_VT_1 && keysym <= XKB_KEY_XF86Switch_VT_12) {
        // "switch VT" keybind
        input->interface.on_switch_vt(input->userdata, keysym - XKB_KEY_XF86Switch_VT_1 + 1);
    }

    uint8_t utf8_character[5] = { 0 };

    // Call the UTF8 character callback if we've got a codepoint.
    // Code very similiar to that of the linux kernel in drivers/tty/vt/keyboard.c, to_utf8
    if (codepoint) {
        if (codepoint < 0x80) {
            // we emit UTF8 unconditionally here,
            // maybe we should check if codepoint is a control character?
            if (isprint(codepoint)) {
                utf8_character[0] = codepoint;
            }
        } else if (codepoint < 0x800) {
            utf8_character[0] = 0xc0 | (codepoint >> 6);
            utf8_character[1] = 0x80 | (codepoint & 0x3f);
        } else if (codepoint < 0x10000) {
            // the console keyboard driver of the linux kernel checks
            // at this point whether `codepoint` is a UTF16 high surrogate (U+D800 to U+DFFF)
            // or U+FFFF and returns without emitting UTF8 in that case.
            // don't know whether we should do this here too
            utf8_character[0] = 0xe0 | (codepoint >> 12);
            utf8_character[1] = 0x80 | ((codepoint >> 6) & 0x3f);
            utf8_character[2] = 0x80 | (codepoint & 0x3f);
        } else if (codepoint < 0x110000) {
            utf8_character[0] = 0xf0 | (codepoint >> 18);
            utf8_character[1] = 0x80 | ((codepoint >> 12) & 0x3f);
            utf8_character[2] = 0x80 | ((codepoint >> 6) & 0x3f);
            utf8_character[3] = 0x80 | (codepoint & 0x3f);
        }
    }

    if (input->interface.on_key_event) {
        input->interface.on_key_event(
            input->userdata,
            libinput_event_keyboard_get_time_usec(key_event),
            evdev_keycode + 8u,
            keysym,
            plain_codepoint,
            (key_modifiers_t){ .shift = keyboard_state_is_shift_active(data->keyboard_state),
                               .capslock = keyboard_state_is_capslock_active(data->keyboard_state),
                               .ctrl = keyboard_state_is_ctrl_active(data->keyboard_state),
                               .alt = keyboard_state_is_alt_active(data->keyboard_state),
                               .numlock = keyboard_state_is_numlock_active(data->keyboard_state),
                               .__pad = 0,
                               .meta = keyboard_state_is_meta_active(data->keyboard_state) },
            (char *) utf8_character,
            key_state == LIBINPUT_KEY_STATE_PRESSED,
            false
        );
    } else {
        // call the GTK keyevent callback.
        /// TODO: Simplify the meta state construction.
        input->interface.on_gtk_keyevent(
            input->userdata,
            plain_codepoint,
            (uint32_t) keysym,
            evdev_keycode + (uint32_t) 8,
            keyboard_state_is_shift_active(data->keyboard_state) | (keyboard_state_is_capslock_active(data->keyboard_state) << 1) |
                (keyboard_state_is_ctrl_active(data->keyboard_state) << 2) | (keyboard_state_is_alt_active(data->keyboard_state) << 3) |
                (keyboard_state_is_numlock_active(data->keyboard_state) << 4) | (keyboard_state_is_meta_active(data->keyboard_state) << 28),
            key_state
        );

        if (utf8_character[0]) {
            input->interface.on_utf8_character(input->userdata, utf8_character);
        }

        // Call the XKB keysym callback if we've got a keysym.
        if (keysym) {
            input->interface.on_xkb_keysym(input->userdata, keysym);
        }
    }

    return 0;
}

static int on_mouse_motion_event(struct user_input *input, struct libinput_event *event) {
    struct libinput_event_pointer *pointer_event;
    struct input_device_data *data;
    struct libinput_device *device;
    struct vec2f delta, pos_display, pos_view;
    uint64_t timestamp;

    assert(input != NULL);
    assert(event != NULL);

    pointer_event = libinput_event_get_pointer_event(event);
    device = libinput_event_get_device(event);
    data = libinput_device_get_user_data(device);
    timestamp = libinput_event_pointer_get_time_usec(pointer_event);

    data->timestamp = timestamp;

    delta = transform_point(
        input->view_to_display_transform_nontranslating,
        VEC2F(libinput_event_pointer_get_dx(pointer_event), libinput_event_pointer_get_dy(pointer_event))
    );

    pos_display = VEC2F(input->cursor_x + delta.x, input->cursor_y + delta.y);

    // check if we're ran over the display boundaries.
    if (pos_display.x < 0.0) {
        pos_display.x = 0.0;
    } else if (pos_display.x > input->display_width - 1) {
        pos_display.x = input->display_width - 1;
    }

    if (pos_display.y < 0.0) {
        pos_display.y = 0.0;
    } else if (pos_display.y > input->display_height - 1) {
        pos_display.y = input->display_height - 1;
    }

    input->cursor_x = pos_display.x;
    input->cursor_y = pos_display.y;

    // transform the cursor pos to view (flutter) coordinates.
    pos_view = transform_point(input->display_to_view_transform, pos_display);

    if (data->has_emitted_pointer_events == false) {
        data->has_emitted_pointer_events = true;
        input->n_cursor_devices++;
        maybe_enable_mouse_cursor(input, timestamp);
    }

    // send the pointer event to flutter.
    emit_pointer_event(
        input,
        data->buttons & kFlutterPointerButtonMousePrimary ?
            make_mouse_move_event(timestamp, pos_view, input->cursor_flutter_device_id, data->buttons) :
            make_mouse_hover_event(timestamp, pos_view, input->cursor_flutter_device_id, data->buttons)
    );

    // we don't invoke the interfaces' mouse move callback here, since we
    // can have multiple mouse motion events per process_libinput_events
    // and we don't want to invoke the callback each time.
    // instead, we call it in user_input_on_fd_ready if the cursors
    // display coordinates changed.

    return 0;
}

static int on_mouse_motion_absolute_event(struct user_input *input, struct libinput_event *event) {
    struct libinput_event_pointer *pointer_event;
    struct input_device_data *data;
    struct libinput_device *device;
    struct vec2f pos_display, pos_view;
    uint64_t timestamp;

    assert(input != NULL);
    assert(event != NULL);

    pointer_event = libinput_event_get_pointer_event(event);
    device = libinput_event_get_device(event);
    data = libinput_device_get_user_data(device);
    timestamp = libinput_event_pointer_get_time_usec(pointer_event);

    // get the new mouse position in display coordinates
    pos_display = VEC2F(
        libinput_event_pointer_get_absolute_x_transformed(pointer_event, input->display_width),
        libinput_event_pointer_get_absolute_y_transformed(pointer_event, input->display_height)
    );

    data->timestamp = timestamp;

    // update the "global" cursor position
    input->cursor_x = pos_display.x;
    input->cursor_y = pos_display.y;

    // transform x & y to view (flutter) coordinates
    pos_view = transform_point(input->display_to_view_transform, pos_display);

    if (data->has_emitted_pointer_events == false) {
        data->has_emitted_pointer_events = true;
        input->n_cursor_devices++;
        maybe_enable_mouse_cursor(input, timestamp);
    }

    emit_pointer_event(
        input,
        data->buttons & kFlutterPointerButtonMousePrimary ?
            make_mouse_move_event(timestamp, pos_view, input->cursor_flutter_device_id, data->buttons) :
            make_mouse_hover_event(timestamp, pos_view, input->cursor_flutter_device_id, data->buttons)
    );

    return 0;
}

static int on_mouse_button_event(struct user_input *input, struct libinput_event *event) {
    struct libinput_event_pointer *pointer_event;
    enum libinput_button_state button_state;
    struct input_device_data *data;
    struct libinput_device *device;
    FlutterPointerPhase pointer_phase;
    struct vec2f pos_view;
    uint64_t timestamp;
    uint16_t evdev_code;
    int64_t flutter_button;
    int64_t new_flutter_button_state;

    assert(input != NULL);
    assert(event != NULL);

    pointer_event = libinput_event_get_pointer_event(event);
    device = libinput_event_get_device(event);
    data = libinput_device_get_user_data(device);
    timestamp = libinput_event_pointer_get_time_usec(pointer_event);
    evdev_code = (uint16_t) libinput_event_pointer_get_button(pointer_event);
    button_state = libinput_event_pointer_get_button_state(pointer_event);

    if (data->has_emitted_pointer_events == false) {
        data->has_emitted_pointer_events = true;
        input->n_cursor_devices++;
        maybe_enable_mouse_cursor(input, timestamp);
    }

    // find out the flutter mouse button for this event
    if (evdev_code == BTN_LEFT) {
        flutter_button = kFlutterPointerButtonMousePrimary;
    } else if (evdev_code == BTN_RIGHT) {
        flutter_button = kFlutterPointerButtonMouseSecondary;
    } else if (evdev_code == BTN_MIDDLE) {
        flutter_button = kFlutterPointerButtonMouseMiddle;
    } else if (evdev_code == BTN_BACK) {
        flutter_button = kFlutterPointerButtonMouseBack;
    } else if (evdev_code == BTN_FORWARD) {
        flutter_button = kFlutterPointerButtonMouseForward;
    } else {
        flutter_button = 0;
    }

    // advance our button state, which is just a bitmap
    new_flutter_button_state = data->buttons;
    if (button_state == LIBINPUT_BUTTON_STATE_RELEASED) {
        // remove the released button from the button state
        new_flutter_button_state &= ~flutter_button;
    } else {
        // add the pressed button to the button state.
        // note that libinput doesn't emit key repeat events.
        new_flutter_button_state |= flutter_button;
    }

    // if our button state changed,
    // emit a pointer event
    if (new_flutter_button_state != data->buttons) {
        if (new_flutter_button_state == 0) {
            // no buttons are pressed anymore.
            pointer_phase = kUp;
        } else if (data->buttons == 0) {
            // previously, there were no buttons pressed.
            // now, at least 1 is pressed.
            pointer_phase = kDown;
        } else {
            // some button was pressed or released,
            // but it
            pointer_phase = kMove;
        }

        // since the stored coords are in display, not view coordinates,
        // we need to transform them again
        pos_view = transform_point(input->display_to_view_transform, VEC2F(input->cursor_x, input->cursor_y));

        emit_pointer_event(
            input,
            make_mouse_event(
                pointer_phase,
                timestamp,
                pos_view,
                input->cursor_flutter_device_id,
                kFlutterPointerSignalKindNone,
                VEC2F(0, 0),
                new_flutter_button_state
            )
        );

        // finally apply the new button state
        data->buttons = new_flutter_button_state;
    }

    return 0;
}

static int on_mouse_axis_event(struct user_input *input, struct libinput_event *event) {
    struct libinput_event_pointer *pointer_event;
    struct input_device_data *data;
    struct libinput_device *device;
    struct vec2f pos_view;
    uint64_t timestamp;

    assert(input != NULL);
    assert(event != NULL);

    pointer_event = libinput_event_get_pointer_event(event);
    device = libinput_event_get_device(event);
    data = libinput_device_get_user_data(device);
    timestamp = libinput_event_pointer_get_time_usec(pointer_event);

    // since the stored coords are in display, not view coordinates,
    // we need to transform them again
    pos_view = transform_point(input->display_to_view_transform, VEC2F(input->cursor_x, input->cursor_y));

    double scroll_x = libinput_event_pointer_has_axis(pointer_event, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL) ?
                          libinput_event_pointer_get_axis_value(pointer_event, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL) :
                          0.0;

    double scroll_y = libinput_event_pointer_has_axis(pointer_event, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL) ?
                          libinput_event_pointer_get_axis_value(pointer_event, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL) :
                          0.0;

    emit_pointer_event(
        input,
        make_mouse_event(
            data->buttons & kFlutterPointerButtonMousePrimary ? kMove : kHover,
            timestamp,
            pos_view,
            input->cursor_flutter_device_id,
            kFlutterPointerSignalKindScroll,
            VEC2F(scroll_x / 15.0 * 53.0, scroll_y / 15.0 * 53.0),
            data->buttons
        )
    );

    return 0;
}

static int on_touch_down(struct user_input *input, struct libinput_event *event) {
    struct libinput_event_touch *touch_event;
    struct input_device_data *data;
    struct vec2f pos_view;
    uint64_t timestamp;
    int64_t device_id;
    int slot;

    assert(input != NULL);
    assert(event != NULL);

    data = libinput_device_get_user_data(libinput_event_get_device(event));
    touch_event = libinput_event_get_touch_event(event);
    timestamp = libinput_event_touch_get_time_usec(touch_event);

    // get the multitouch slot for this event
    // can return -1 when the device is a single touch device
    slot = libinput_event_touch_get_slot(touch_event);
    if (slot == -1) {
        slot = 0;
    }

    device_id = data->touch_device_id_offset + slot;

    // transform the display coordinates to view (flutter) coordinates
    pos_view = transform_point(
        input->display_to_view_transform,
        VEC2F(
            libinput_event_touch_get_x_transformed(touch_event, input->display_width),
            libinput_event_touch_get_y_transformed(touch_event, input->display_height)
        )
    );

    // emit the flutter pointer event
    emit_pointer_event(input, make_touch_down_event(timestamp, pos_view, device_id));

    // alter our device state
    data->positions[slot] = pos_view;
    data->timestamp = timestamp;

    return 0;
}

static int on_touch_up(struct user_input *input, struct libinput_event *event) {
    struct libinput_event_touch *touch_event;
    struct input_device_data *data;
    uint64_t timestamp;
    int64_t device_id;
    int slot;

    assert(input != NULL);
    assert(event != NULL);

    data = libinput_device_get_user_data(libinput_event_get_device(event));
    touch_event = libinput_event_get_touch_event(event);
    timestamp = libinput_event_touch_get_time_usec(touch_event);

    // get the multitouch slot for this event
    // can return -1 when the device is a single touch device
    slot = libinput_event_touch_get_slot(touch_event);
    if (slot == -1) {
        slot = 0;
    }

    device_id = data->touch_device_id_offset + slot;

    emit_pointer_event(input, make_touch_up_event(timestamp, data->positions[slot], device_id));

    return 0;
}

static int on_touch_motion(struct user_input *input, struct libinput_event *event) {
    struct libinput_event_touch *touch_event;
    struct input_device_data *data;
    struct vec2f pos_view;
    uint64_t timestamp;
    int64_t device_id;
    int slot;

    assert(input != NULL);
    assert(event != NULL);

    data = libinput_device_get_user_data(libinput_event_get_device(event));
    touch_event = libinput_event_get_touch_event(event);
    timestamp = libinput_event_touch_get_time_usec(touch_event);

    // get the multitouch slot for this event
    // can return -1 when the device is a single touch device
    slot = libinput_event_touch_get_slot(touch_event);
    if (slot == -1) {
        slot = 0;
    }

    device_id = data->touch_device_id_offset + slot;

    // transform the display coordinates to view (flutter) coordinates
    pos_view = transform_point(
        FLUTTER_TRANSFORM_AS_MAT3F(input->display_to_view_transform),
        VEC2F(
            libinput_event_touch_get_x_transformed(touch_event, input->display_width),
            libinput_event_touch_get_y_transformed(touch_event, input->display_height)
        )
    );

    // emit the flutter pointer event
    emit_pointer_event(input, make_touch_move_event(timestamp, pos_view, device_id));

    // alter our device state
    data->positions[slot] = pos_view;
    data->timestamp = timestamp;

    return 0;
}

static int on_touch_cancel(struct user_input *input, struct libinput_event *event) {
    assert(input != NULL);
    assert(event != NULL);

    (void) input;
    (void) event;

    /// TODO: Implement touch cancel
    return 0;
}

static int on_touch_frame(struct user_input *input, struct libinput_event *event) {
    assert(input != NULL);
    assert(event != NULL);

    (void) input;
    (void) event;

    /// TODO: Implement touch frame
    return 0;
}

static int on_tablet_tool_axis(struct user_input *input, struct libinput_event *event) {
    struct libinput_event_tablet_tool *tablet_event;
    struct input_device_data *data;
    struct vec2f pos;
    uint64_t timestamp;
    int64_t device_id;

    ASSERT_NOT_NULL(input);
    ASSERT_NOT_NULL(event);

    data = libinput_device_get_user_data(libinput_event_get_device(event));
    ASSERT_NOT_NULL(data);

    tablet_event = libinput_event_get_tablet_tool_event(event);
    timestamp = libinput_event_tablet_tool_get_time_usec(tablet_event);

    device_id = data->stylus_device_id;

    pos.x = libinput_event_tablet_tool_get_x_transformed(tablet_event, input->display_width - 1);
    pos.y = libinput_event_tablet_tool_get_y_transformed(tablet_event, input->display_height - 1);

    pos = transform_point(input->display_to_view_transform, pos);

    if (data->tip) {
        emit_pointer_event(input, make_stylus_move_event(timestamp, pos, device_id));
    } else {
        emit_pointer_event(input, make_stylus_hover_event(timestamp, pos, device_id));
    }

    return 0;
}

static int on_tablet_tool_proximity(struct user_input *input, struct libinput_event *event) {
    struct libinput_event_tablet_tool *tablet_event;
    struct input_device_data *data;
    struct vec2f pos;
    uint64_t timestamp;
    int64_t device_id;

    ASSERT_NOT_NULL(input);
    ASSERT_NOT_NULL(event);

    data = libinput_device_get_user_data(libinput_event_get_device(event));
    ASSERT_NOT_NULL(data);

    tablet_event = libinput_event_get_tablet_tool_event(event);
    timestamp = libinput_event_tablet_tool_get_time_usec(tablet_event);

    device_id = data->stylus_device_id;

    pos.x = libinput_event_tablet_tool_get_x_transformed(tablet_event, input->display_width - 1);
    pos.y = libinput_event_tablet_tool_get_y_transformed(tablet_event, input->display_height - 1);

    pos = transform_point(input->display_to_view_transform, pos);

    if (!data->tip) {
        emit_pointer_event(input, make_stylus_hover_event(timestamp, pos, device_id));
    }

    return 0;
}

static int on_tablet_tool_tip(struct user_input *input, struct libinput_event *event) {
    struct libinput_event_tablet_tool *tablet_event;
    struct input_device_data *data;
    uint64_t timestamp;
    int64_t device_id;
    struct vec2f pos;

    ASSERT_NOT_NULL(input);
    ASSERT_NOT_NULL(event);

    data = libinput_device_get_user_data(libinput_event_get_device(event));
    ASSERT_NOT_NULL(data);

    tablet_event = libinput_event_get_tablet_tool_event(event);
    timestamp = libinput_event_tablet_tool_get_time_usec(tablet_event);

    device_id = data->stylus_device_id;

    pos.x = libinput_event_tablet_tool_get_x_transformed(tablet_event, input->display_width - 1);
    pos.y = libinput_event_tablet_tool_get_y_transformed(tablet_event, input->display_height - 1);

    pos = transform_point(input->display_to_view_transform, pos);

    if (libinput_event_tablet_tool_get_tip_state(tablet_event) == LIBINPUT_TABLET_TOOL_TIP_DOWN) {
        data->tip = true;
        emit_pointer_event(input, make_stylus_down_event(timestamp, pos, device_id));
    } else {
        data->tip = false;
        emit_pointer_event(input, make_stylus_up_event(timestamp, pos, device_id));
    }

    return 0;
}

static int on_tablet_tool_button(struct user_input *input, struct libinput_event *event) {
    ASSERT_NOT_NULL(input);
    ASSERT_NOT_NULL(event);

    (void) input;
    (void) event;

    return 0;
}

static int on_tablet_pad_button(struct user_input *input, struct libinput_event *event) {
    ASSERT_NOT_NULL(input);
    ASSERT_NOT_NULL(event);

    (void) input;
    (void) event;

    return 0;
}

static int on_tablet_pad_ring(struct user_input *input, struct libinput_event *event) {
    ASSERT_NOT_NULL(input);
    ASSERT_NOT_NULL(event);

    (void) input;
    (void) event;

    return 0;
}

static int on_tablet_pad_strip(struct user_input *input, struct libinput_event *event) {
    ASSERT_NOT_NULL(input);
    ASSERT_NOT_NULL(event);

    (void) input;
    (void) event;

    return 0;
}

#if THIS_LIBINPUT_VER >= LIBINPUT_VER(1, 15, 0)
static int on_tablet_pad_key(struct user_input *input, struct libinput_event *event) {
    ASSERT_NOT_NULL(input);
    ASSERT_NOT_NULL(event);

    (void) input;
    (void) event;

    return 0;
}
#endif

static int process_libinput_events(struct user_input *input, uint64_t timestamp) {
    enum libinput_event_type event_type;
    struct libinput_event *event;
    int ok;

    assert(input != NULL);

    while (libinput_next_event_type(input->libinput) != LIBINPUT_EVENT_NONE) {
        event = libinput_get_event(input->libinput);
        event_type = libinput_event_get_type(event);

        switch (event_type) {
            case LIBINPUT_EVENT_DEVICE_ADDED:
                ok = on_device_added(input, event, timestamp);
                if (ok != 0) {
                    goto fail_destroy_event;
                }
                break;
            case LIBINPUT_EVENT_DEVICE_REMOVED:
                ok = on_device_removed(input, event, timestamp, true);
                if (ok != 0) {
                    goto fail_destroy_event;
                }
                break;
            case LIBINPUT_EVENT_KEYBOARD_KEY:
                ok = on_key_event(input, event);
                if (ok != 0) {
                    goto fail_destroy_event;
                }
                break;
            case LIBINPUT_EVENT_POINTER_MOTION:
                ok = on_mouse_motion_event(input, event);
                if (ok != 0) {
                    goto fail_destroy_event;
                }
                break;
            case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE:
                ok = on_mouse_motion_absolute_event(input, event);
                if (ok != 0) {
                    goto fail_destroy_event;
                }
                break;
            case LIBINPUT_EVENT_POINTER_BUTTON:
                ok = on_mouse_button_event(input, event);
                if (ok != 0) {
                    goto fail_destroy_event;
                }
                break;
            case LIBINPUT_EVENT_POINTER_AXIS:
                ok = on_mouse_axis_event(input, event);
                if (ok != 0) {
                    goto fail_destroy_event;
                }
                break;
            case LIBINPUT_EVENT_TOUCH_DOWN:
                ok = on_touch_down(input, event);
                if (ok != 0) {
                    goto fail_destroy_event;
                }
                break;
            case LIBINPUT_EVENT_TOUCH_UP:
                ok = on_touch_up(input, event);
                if (ok != 0) {
                    goto fail_destroy_event;
                }
                break;
            case LIBINPUT_EVENT_TOUCH_MOTION:
                ok = on_touch_motion(input, event);
                if (ok != 0) {
                    goto fail_destroy_event;
                }
                break;
            case LIBINPUT_EVENT_TOUCH_CANCEL:
                ok = on_touch_cancel(input, event);
                if (ok != 0) {
                    goto fail_destroy_event;
                }
                break;
            case LIBINPUT_EVENT_TOUCH_FRAME:
                ok = on_touch_frame(input, event);
                if (ok != 0) {
                    goto fail_destroy_event;
                }
                break;
            case LIBINPUT_EVENT_TABLET_TOOL_AXIS:
                ok = on_tablet_tool_axis(input, event);
                if (ok != 0) {
                    goto fail_destroy_event;
                }
                break;
            case LIBINPUT_EVENT_TABLET_TOOL_PROXIMITY:
                ok = on_tablet_tool_proximity(input, event);
                if (ok != 0) {
                    goto fail_destroy_event;
                }
                break;
            case LIBINPUT_EVENT_TABLET_TOOL_TIP:
                ok = on_tablet_tool_tip(input, event);
                if (ok != 0) {
                    goto fail_destroy_event;
                }
                break;
            case LIBINPUT_EVENT_TABLET_TOOL_BUTTON:
                ok = on_tablet_tool_button(input, event);
                if (ok != 0) {
                    goto fail_destroy_event;
                }
                break;
            case LIBINPUT_EVENT_TABLET_PAD_BUTTON:
                ok = on_tablet_pad_button(input, event);
                if (ok != 0) {
                    goto fail_destroy_event;
                }
                break;
            case LIBINPUT_EVENT_TABLET_PAD_RING:
                ok = on_tablet_pad_ring(input, event);
                if (ok != 0) {
                    goto fail_destroy_event;
                }
                break;
            case LIBINPUT_EVENT_TABLET_PAD_STRIP:
                ok = on_tablet_pad_strip(input, event);
                if (ok != 0) {
                    goto fail_destroy_event;
                }
                break;
#if THIS_LIBINPUT_VER >= LIBINPUT_VER(1, 15, 0)
            case LIBINPUT_EVENT_TABLET_PAD_KEY:
                ok = on_tablet_pad_key(input, event);
                if (ok != 0) {
                    goto fail_destroy_event;
                }
                break;
#endif
            default: break;
        }

        libinput_event_destroy(event);
    }

    return 0;

fail_destroy_event:
    libinput_event_destroy(event);
    return ok;
}

int user_input_on_fd_ready(struct user_input *input) {
    int cursor_x, cursor_y, cursor_x_before, cursor_y_before;
    uint64_t timestamp;
    bool cursor_enabled, cursor_enabled_before;
    int ok;

    assert(input != NULL);

    // get a timestamp because some libinput events don't provide one
    // needs to be in milliseconds, since that's what the other libinput events
    // use and what flutter pointer events require
    timestamp = get_monotonic_time() / 1000;

    // tell libinput about new events
    ok = libinput_dispatch(input->libinput);
    if (ok < 0) {
        LOG_ERROR("Could not notify libinput about new input events. libinput_dispatch: %s\n", strerror(-ok));
        return -ok;
    }

    // record cursor state before handling events
    cursor_enabled_before = input->n_cursor_devices > 0;
    cursor_x_before = round(input->cursor_x);
    cursor_y_before = round(input->cursor_y);

    // handle all available libinput events
    ok = process_libinput_events(input, timestamp);
    if (ok != 0) {
        LOG_ERROR("Could not process libinput events. process_libinput_events: %s\n", strerror(ok));
        return ok;
    }

    // record cursor state after handling events
    cursor_enabled = input->n_cursor_devices > 0;
    cursor_x = round(input->cursor_x);
    cursor_y = round(input->cursor_y);

    // make sure we've dispatched all the flutter pointer events
    flush_pointer_events(input);

    // call the interface callback if the cursor has been enabled or disabled
    if (cursor_enabled && !cursor_enabled_before) {
        input->interface.on_set_cursor_enabled(input->userdata, true);
    } else if (!cursor_enabled && cursor_enabled_before) {
        input->interface.on_set_cursor_enabled(input->userdata, false);
    }

    // only move the pointer if the cursor is enabled now
    if (cursor_enabled && ((cursor_x != cursor_x_before) || (cursor_y != cursor_y_before))) {
        input->interface.on_move_cursor(input->userdata, VEC2F(cursor_x - cursor_x_before, cursor_y - cursor_y_before));
    }

    return 0;
}
