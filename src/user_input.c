#include "user_input.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <klib/khash.h>
#include <klib/kvec.h>
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

#define MAX_N_LISTENERS 8

KHASH_MAP_INIT_INT(metadata_for_fd, void *)

struct device_listener {
    int index;
    void *userdata;
};

struct input_device_data {
    struct libinput_device *device;

    struct keyboard_state *keyboard_state;
    uint8_t buttons;
    uint64_t timestamp;

    int64_t device_id;

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

    int64_t touch_slot_id_offset;
    int64_t stylus_slot_id;

    void *primary_listener_userdata;
};

struct callback {
    void_callback_t cb;
    void *userdata;
};

struct listener {
    enum user_input_event_type filter;
    bool is_primary;

    user_input_event_cb_t cb;
    void *userdata;

    size_t n_events;
    struct user_input_event events[64];
};

struct user_input {
    struct file_interface file_interface;
    void *file_interface_userdata;
    khash_t(metadata_for_fd) * metadata_for_fd;

    struct libinput *libinput;
    struct keyboard_config *kbdcfg;
    int64_t next_device_id;
    int64_t next_slot_id;

    size_t n_cursor_devices;
    size_t cursor_slot_id;

    size_t n_listeners;
    struct listener listeners[MAX_N_LISTENERS];

    kvec_t(struct input_device_data *) deferred_device_cleanups;
};

static struct user_input_device *libinput_dev_to_user_input_dev(struct libinput_device *device) {
    struct input_device_data *data = libinput_device_get_user_data(device);

    if (data == NULL) {
        return NULL;
    }

    return (struct user_input_device *) data;
}

static struct input_device_data *user_input_device_get_device_data(struct user_input_device *device) {
    return (struct input_device_data *) device;
}

static inline struct user_input_event make_device_added_event(struct libinput_device *device, uint64_t timestamp) {
    struct user_input_event event;
    memset(&event, 0, sizeof(event));

    event.type = USER_INPUT_DEVICE_ADDED;
    event.timestamp = timestamp;
    event.device = libinput_dev_to_user_input_dev(device);
    event.global_slot_id = 0;
    event.slot_type = USER_INPUT_SLOT_POINTER;

    return event;
}

static inline struct user_input_event make_device_removed_event(struct libinput_device *device, uint64_t timestamp) {
    struct user_input_event event;
    memset(&event, 0, sizeof(event));

    event.type = USER_INPUT_DEVICE_REMOVED;
    event.timestamp = timestamp;
    event.device = libinput_dev_to_user_input_dev(device);
    event.global_slot_id = 0;
    event.slot_type = USER_INPUT_SLOT_POINTER;

    return event;
}

static inline struct user_input_event
make_slot_added_event(struct libinput_device *device, uint64_t timestamp, int64_t slot_id, enum user_input_slot_type slot_type) {
    struct user_input_event event;
    memset(&event, 0, sizeof(event));

    event.type = USER_INPUT_SLOT_ADDED;
    event.timestamp = timestamp;
    event.device = libinput_dev_to_user_input_dev(device);
    event.global_slot_id = slot_id;
    event.slot_type = slot_type;

    return event;
}

static inline struct user_input_event
make_slot_removed_event(struct libinput_device *device, uint64_t timestamp, int64_t slot_id, enum user_input_slot_type slot_type) {
    struct user_input_event event;
    memset(&event, 0, sizeof(event));

    event.type = USER_INPUT_SLOT_ADDED;
    event.timestamp = timestamp;
    event.device = libinput_dev_to_user_input_dev(device);
    event.global_slot_id = slot_id;
    event.slot_type = slot_type;

    return event;
}

static inline struct user_input_event
make_pointer_motion_event(struct libinput_device *device, uint64_t timestamp, int64_t slot_id, uint8_t buttons, struct vec2f delta) {
    struct user_input_event event;
    memset(&event, 0, sizeof(event));

    event.type = USER_INPUT_POINTER;
    event.timestamp = timestamp;
    event.device = libinput_dev_to_user_input_dev(device);
    event.global_slot_id = slot_id;
    event.slot_type = USER_INPUT_SLOT_POINTER;
    event.pointer.buttons = buttons;
    event.pointer.changed_buttons = 0;
    event.pointer.is_absolute = false;
    event.pointer.delta = delta;
    event.pointer.scroll_delta = VEC2F(0.0, 0.0);

    return event;
}

static inline struct user_input_event
make_pointer_motion_absolute_event(struct libinput_device *device, uint64_t timestamp, int64_t slot_id, uint8_t buttons, struct vec2f pos) {
    struct user_input_event event;
    memset(&event, 0, sizeof(event));

    event.type = USER_INPUT_POINTER;
    event.timestamp = timestamp;
    event.device = libinput_dev_to_user_input_dev(device);
    event.global_slot_id = slot_id;
    event.slot_type = USER_INPUT_SLOT_POINTER;
    event.pointer.buttons = buttons;
    event.pointer.changed_buttons = 0;
    event.pointer.is_absolute = true;
    event.pointer.position_ndc = pos;
    event.pointer.scroll_delta = VEC2F(0.0, 0.0);
    return event;
}

static inline struct user_input_event
make_pointer_button_event(struct libinput_device *device, uint64_t timestamp, int64_t slot_id, uint8_t buttons, uint8_t changed_buttons) {
    struct user_input_event event;
    memset(&event, 0, sizeof(event));

    event.type = USER_INPUT_POINTER;
    event.timestamp = timestamp;
    event.device = libinput_dev_to_user_input_dev(device);
    event.global_slot_id = slot_id;
    event.slot_type = USER_INPUT_SLOT_POINTER;
    event.pointer.buttons = buttons;
    event.pointer.changed_buttons = changed_buttons;
    event.pointer.is_absolute = false;
    event.pointer.delta = VEC2F(0.0, 0.0);
    event.pointer.scroll_delta = VEC2F(0.0, 0.0);
    return event;
}

static inline struct user_input_event
make_pointer_axis_event(struct libinput_device *device, uint64_t timestamp, int64_t slot_id, uint8_t buttons, struct vec2f scroll_delta) {
    struct user_input_event event;
    memset(&event, 0, sizeof(event));

    event.type = USER_INPUT_POINTER;
    event.timestamp = timestamp;
    event.device = libinput_dev_to_user_input_dev(device);
    event.global_slot_id = slot_id;
    event.slot_type = USER_INPUT_SLOT_POINTER;
    event.pointer.buttons = buttons;
    event.pointer.changed_buttons = 0;
    event.pointer.is_absolute = false;
    event.pointer.delta = VEC2F(0.0, 0.0);
    event.pointer.scroll_delta = scroll_delta;
    return event;
}

static inline struct user_input_event
make_touch_down_event(struct libinput_device *device, uint64_t timestamp, int64_t slot_id, struct vec2f pos_ndc) {
    struct user_input_event event;
    memset(&event, 0, sizeof(event));

    event.type = USER_INPUT_TOUCH;
    event.timestamp = timestamp;
    event.device = libinput_dev_to_user_input_dev(device);
    event.global_slot_id = slot_id;
    event.slot_type = USER_INPUT_SLOT_TOUCH;
    event.touch.down = true;
    event.touch.down_changed = true;
    event.touch.position_ndc = pos_ndc;
    return event;
}

static inline struct user_input_event
make_touch_up_event(struct libinput_device *device, uint64_t timestamp, int64_t slot_id, struct vec2f pos_ndc) {
    struct user_input_event event;
    memset(&event, 0, sizeof(event));

    event.type = USER_INPUT_TOUCH;
    event.timestamp = timestamp;
    event.device = libinput_dev_to_user_input_dev(device);
    event.global_slot_id = slot_id;
    event.slot_type = USER_INPUT_SLOT_TOUCH;
    event.touch.down = false;
    event.touch.down_changed = true;
    event.touch.position_ndc = pos_ndc;
    return event;
}

static inline struct user_input_event
make_touch_move_event(struct libinput_device *device, uint64_t timestamp, int64_t slot_id, struct vec2f pos_ndc) {
    struct user_input_event event;
    memset(&event, 0, sizeof(event));

    event.type = USER_INPUT_TOUCH;
    event.timestamp = timestamp;
    event.device = libinput_dev_to_user_input_dev(device);
    event.global_slot_id = slot_id;
    event.slot_type = USER_INPUT_SLOT_TOUCH;
    event.touch.down = true;
    event.touch.down_changed = false;
    event.touch.position_ndc = pos_ndc;
    return event;
}

static inline struct user_input_event
make_stylus_down_event(struct libinput_device *device, uint64_t timestamp, int64_t slot_id, struct vec2f pos_ndc) {
    struct user_input_event event;
    memset(&event, 0, sizeof(event));

    event.type = USER_INPUT_TABLET_TOOL;
    event.timestamp = timestamp;
    event.device = libinput_dev_to_user_input_dev(device);
    event.global_slot_id = slot_id;
    event.slot_type = USER_INPUT_SLOT_TABLET_TOOL;
    event.tablet.tip = true;
    event.tablet.tip_changed = true;
    event.tablet.tool = LIBINPUT_TABLET_TOOL_TYPE_PEN;
    event.tablet.position_ndc = pos_ndc;
    return event;
}

static inline struct user_input_event
make_stylus_up_event(struct libinput_device *device, uint64_t timestamp, int64_t slot_id, struct vec2f pos_ndc) {
    struct user_input_event event;
    memset(&event, 0, sizeof(event));

    event.type = USER_INPUT_TABLET_TOOL;
    event.timestamp = timestamp;
    event.device = libinput_dev_to_user_input_dev(device);
    event.global_slot_id = slot_id;
    event.slot_type = USER_INPUT_SLOT_TABLET_TOOL;
    event.tablet.tip = false;
    event.tablet.tip_changed = true;
    event.tablet.tool = LIBINPUT_TABLET_TOOL_TYPE_PEN;
    event.tablet.position_ndc = pos_ndc;
    return event;
}

static inline struct user_input_event
make_stylus_move_event(struct libinput_device *device, uint64_t timestamp, int64_t slot_id, bool tip, struct vec2f pos_ndc) {
    struct user_input_event event;
    memset(&event, 0, sizeof(event));

    event.type = USER_INPUT_TABLET_TOOL;
    event.timestamp = timestamp;
    event.device = libinput_dev_to_user_input_dev(device);
    event.global_slot_id = slot_id;
    event.slot_type = USER_INPUT_SLOT_TABLET_TOOL;
    event.tablet.tip = tip;
    event.tablet.tip_changed = false;
    event.tablet.tool = LIBINPUT_TABLET_TOOL_TYPE_PEN;
    event.tablet.position_ndc = pos_ndc;
    return event;
}

static inline struct user_input_event make_key_event(
    struct libinput_device *device,
    uint64_t timestamp,
    xkb_keycode_t xkb_keycode,
    xkb_keysym_t xkb_keysym,
    uint32_t plain_codepoint,
    key_modifiers_t modifiers,
    const char text[8],
    bool is_down,
    bool is_repeat
) {
    struct user_input_event event;
    memset(&event, 0, sizeof(event));

    event.type = USER_INPUT_KEY;
    event.timestamp = timestamp;
    event.device = libinput_dev_to_user_input_dev(device);
    event.global_slot_id = 0;
    event.slot_type = USER_INPUT_SLOT_POINTER;
    event.key.xkb_keycode = xkb_keycode;
    event.key.xkb_keysym = xkb_keysym;
    event.key.plain_codepoint = plain_codepoint;
    event.key.modifiers = modifiers;
    memcpy(event.key.text, text, sizeof(event.key.text));
    event.key.is_down = is_down;
    event.key.is_repeat = is_repeat;
    return event;
}

static void flush_listener_events(struct listener *listener) {
    ASSERT_NOT_NULL(listener);

    if (listener->n_events > 0) {
        listener->cb(listener->userdata, listener->n_events, listener->events);
        listener->n_events = 0;
    }
}

static void flush_events(struct user_input *input) {
    ASSERT_NOT_NULL(input);

    for (size_t i = 0; i < input->n_listeners; i++) {
        flush_listener_events(input->listeners + i);
    }
}

static bool emit_event(struct user_input *input, const struct user_input_event event) {
    assert(input != NULL);

    bool emitted = false;

    for (size_t i = 0; i < input->n_listeners; i++) {
        struct listener *listener = input->listeners + i;
        if (!(listener->filter & event.type)) {
            continue;
        }

        if (listener->n_events == ARRAY_SIZE(listener->events)) {
            flush_listener_events(listener);
        }

        memcpy(listener->events + listener->n_events, &event, sizeof(event));
        listener->n_events++;

        emitted = true;
    }

    return emitted;
}

// libinput interface
static int on_libinput_open(const char *path, int flags, void *userdata) {
    struct user_input *input;
    void *fd_metadata;

    ASSERT_NOT_NULL(path);
    ASSERT_NOT_NULL(userdata);
    input = userdata;

    int fd = input->file_interface.open(path, flags | O_CLOEXEC, &fd_metadata, input->file_interface_userdata);
    if (fd < 0) {
        LOG_DEBUG("Could not open input device. open: %s\n", strerror(errno));
        return -1;
    }

    int bucket_status;
    khiter_t k = kh_put(metadata_for_fd, input->metadata_for_fd, fd, &bucket_status);
    if (bucket_status < 0) {
        close(fd);
        errno = ENOMEM;
        return -1;
    }

    kh_val(input->metadata_for_fd, k) = fd_metadata;

    return fd;
}

static void on_libinput_close(int fd, void *userdata) {
    struct user_input *input;
    void *fd_metadata;

    ASSERT_NOT_NULL(userdata);
    input = userdata;

    khiter_t k = kh_get(metadata_for_fd, input->metadata_for_fd, fd);
    if (k == kh_end(input->metadata_for_fd)) {
        LOG_ERROR("Attempted to close an fd that was not previously opened.\n");
        close(fd);
    }

    fd_metadata = kh_val(input->metadata_for_fd, k);
    input->file_interface.close(fd, fd_metadata, input->file_interface_userdata);
}

static const struct libinput_interface libinput_interface = { .open_restricted = on_libinput_open, .close_restricted = on_libinput_close };

struct user_input *user_input_new_suspended(const struct file_interface *interface, void *userdata, struct udev *udev, const char *seat) {
    struct user_input *input;
    int ok;

    ASSERT_NOT_NULL(interface);
    ASSERT_NOT_NULL(udev);

    input = malloc(sizeof *input);
    if (input == NULL) {
        goto fail_return_null;
    }

    input->file_interface = *interface;
    input->file_interface_userdata = userdata;
    input->metadata_for_fd = kh_init(metadata_for_fd);
    if (input->metadata_for_fd == NULL) {
        goto fail_free_input;
    }

    input->libinput = libinput_udev_create_context(&libinput_interface, input, udev);
    if (input->libinput == NULL) {
        perror("[flutter-pi] Could not create libinput instance. libinput_udev_create_context");
        goto fail_destroy_metadata;
    }

    libinput_suspend(input->libinput);

    ok = libinput_udev_assign_seat(input->libinput, seat ? seat : "seat0");
    if (ok < 0) {
        LOG_ERROR("Could not assign udev seat to libinput instance. libinput_udev_assign_seat: %s\n", strerror(-ok));
        goto fail_unref_libinput;
    }

#ifdef BUILD_TEXT_INPUT_PLUGIN
    input->kbdcfg = keyboard_config_new();
    if (input->kbdcfg == NULL) {
        LOG_ERROR("Could not initialize keyboard configuration. Flutter-pi will run without text/raw keyboard input.\n");
    }
#else
    kbdcfg = NULL;
#endif

    input->next_slot_id = 0;
    input->next_device_id = 0;
    input->n_cursor_devices = 0;
    input->cursor_slot_id = -1;
    input->n_listeners = 0;
    memset(input->listeners, 0, sizeof(input->listeners));
    kv_init(input->deferred_device_cleanups);
    return input;

fail_unref_libinput:
    libinput_unref(input->libinput);

fail_destroy_metadata:
    kh_destroy(metadata_for_fd, input->metadata_for_fd);

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

        PRAGMA_DIAGNOSTIC_PUSH
        PRAGMA_DIAGNOSTIC_IGNORED("-Wswitch-enum")
        switch (event_type) {
            case LIBINPUT_EVENT_DEVICE_REMOVED:
                ok = on_device_removed(input, event, 0, false);
                ASSERT_ZERO(ok);
                break;
            default: break;
        }
        PRAGMA_DIAGNOSTIC_POP

        libinput_event_destroy(event);
    }

    if (input->kbdcfg != NULL) {
        keyboard_config_destroy(input->kbdcfg);
    }
    libinput_unref(input->libinput);
    free(input);
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

/**
 * @brief Called when input->n_cursor_devices was increased to maybe enable the mouse cursor
 * it it isn't yet enabled.
 */
static void maybe_enable_mouse_cursor(struct user_input *input, struct libinput_device *device, uint64_t timestamp) {
    assert(input != NULL);

    if (input->n_cursor_devices == 1) {
        if (input->cursor_slot_id == -1) {
            input->cursor_slot_id = input->next_slot_id++;
        }

        emit_event(input, make_slot_added_event(device, timestamp, input->cursor_slot_id, USER_INPUT_SLOT_POINTER));
    }
}

/**
 * @brief Called when input->n_cursor_devices was decreased to maybe disable the mouse cursor.
 */
static void maybe_disable_mouse_cursor(struct user_input *input, struct libinput_device *device, uint64_t timestamp) {
    assert(input != NULL);

    if (input->n_cursor_devices == 0) {
        emit_event(input, make_slot_removed_event(device, timestamp, input->cursor_slot_id, USER_INPUT_SLOT_POINTER));
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

    data->device = libinput_device_ref(device);
    data->device_id = input->next_device_id++;
    data->touch_slot_id_offset = -1;
    data->stylus_slot_id = -1;
    data->keyboard_state = NULL;
    data->buttons = 0;
    data->timestamp = timestamp;
    data->has_emitted_pointer_events = false;
    data->tip = false;
    data->positions = NULL;
    data->primary_listener_userdata = NULL;

    libinput_device_set_user_data(device, data);

    emit_event(input, make_device_added_event(device, timestamp));

    if (libinput_device_has_capability(device, LIBINPUT_DEVICE_CAP_POINTER)) {
        // no special things to do here
        // mouse pointer will be added as soon as the device actually sends a
        // mouse event, as some devices will erroneously have a LIBINPUT_DEVICE_CAP_POINTER
        // even though they aren't mice. (My keyboard for example is a mouse smh)
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

        data->touch_slot_id_offset = input->next_slot_id;

        for (int i = 0; i < n_slots; i++) {
            emit_event(input, make_slot_added_event(device, timestamp, input->next_slot_id++, USER_INPUT_SLOT_TOUCH));
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
        device_id = input->next_slot_id++;

        emit_event(input, make_slot_added_event(device, timestamp, device_id, USER_INPUT_SLOT_TABLET_TOOL));
        data->stylus_slot_id = device_id;
    }

    return 0;

fail_free_data:
    free(data);
    return EINVAL;
}

static void cleanup_device_data(struct input_device_data *data) {
    libinput_device_set_user_data(data->device, NULL);
    libinput_device_unref(data->device);

    if (data->keyboard_state != NULL) {
        keyboard_state_destroy(data->keyboard_state);
    }

    if (data->positions != NULL) {
        free(data->positions);
    }

    free(data);
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
            maybe_disable_mouse_cursor(input, device, timestamp);
        }
    }

    if (libinput_device_has_capability(device, LIBINPUT_DEVICE_CAP_TOUCH)) {
        // add all touch slots as individual touch devices to flutter
        if (emit_flutter_events) {
            for (int i = 0; i < libinput_device_touch_get_touch_count(device); i++) {
                emit_event(input, make_slot_removed_event(device, timestamp, data->touch_slot_id_offset + i, USER_INPUT_SLOT_TOUCH));
            }
        }
    }

    if (libinput_device_has_capability(device, LIBINPUT_DEVICE_CAP_TABLET_TOOL)) {
        emit_event(input, make_slot_removed_event(device, timestamp, data->stylus_slot_id, USER_INPUT_SLOT_TABLET_TOOL));
    }

    emit_event(input, make_device_removed_event(device, timestamp));

    kv_push(input->deferred_device_cleanups, data);
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

    // if (input->interface.on_switch_vt != NULL && keysym >= XKB_KEY_XF86Switch_VT_1 && keysym <= XKB_KEY_XF86Switch_VT_12) {
    //     // "switch VT" keybind
    //     input->interface.on_switch_vt(input->userdata, keysym - XKB_KEY_XF86Switch_VT_1 + 1);
    // }

    uint8_t utf8_character[8] = { 0 };

    // Call the UTF8 character callback if we've got a codepoint.
    // Code very similiar to that of the linux kernel in drivers/tty/vt/keyboard.c, to_utf8
    if (codepoint) {
        if (codepoint < 0x80) {
            // we emit UTF8 unconditionally here,
            // maybe we should check if codepoint is a control character?
            if (isprint(codepoint)) {
                utf8_character[0] = (uint8_t) codepoint;
            }
        } else if (codepoint < 0x800) {
            utf8_character[0] = 0xc0 | (uint8_t) (codepoint >> 6);
            utf8_character[1] = 0x80 | (codepoint & 0x3f);
        } else if (codepoint < 0x10000) {
            // the console keyboard driver of the linux kernel checks
            // at this point whether `codepoint` is a UTF16 high surrogate (U+D800 to U+DFFF)
            // or U+FFFF and returns without emitting UTF8 in that case.
            // don't know whether we should do this here too
            utf8_character[0] = 0xe0 | (uint8_t) (codepoint >> 12);
            utf8_character[1] = 0x80 | ((codepoint >> 6) & 0x3f);
            utf8_character[2] = 0x80 | (codepoint & 0x3f);
        } else if (codepoint < 0x110000) {
            utf8_character[0] = 0xf0 | (uint8_t) (codepoint >> 18);
            utf8_character[1] = 0x80 | ((codepoint >> 12) & 0x3f);
            utf8_character[2] = 0x80 | ((codepoint >> 6) & 0x3f);
            utf8_character[3] = 0x80 | (codepoint & 0x3f);
        }
    }

    emit_event(
        input,
        make_key_event(
            device,
            libinput_event_keyboard_get_time_usec(key_event),
            evdev_keycode + 8u,
            keysym,
            plain_codepoint,
            (key_modifiers_t){
                .shift = keyboard_state_is_shift_active(data->keyboard_state),
                .capslock = keyboard_state_is_capslock_active(data->keyboard_state),
                .ctrl = keyboard_state_is_ctrl_active(data->keyboard_state),
                .alt = keyboard_state_is_alt_active(data->keyboard_state),
                .numlock = keyboard_state_is_numlock_active(data->keyboard_state),
                .__pad = 0,
                .meta = keyboard_state_is_meta_active(data->keyboard_state),
            },
            (char *) utf8_character,
            key_state == LIBINPUT_KEY_STATE_PRESSED,
            false
        )
    );

    return 0;
}

static int on_mouse_motion_event(struct user_input *input, struct libinput_event *event) {
    struct libinput_event_pointer *pointer_event;
    struct input_device_data *data;
    struct libinput_device *device;
    uint64_t timestamp;

    assert(input != NULL);
    assert(event != NULL);

    pointer_event = libinput_event_get_pointer_event(event);
    device = libinput_event_get_device(event);
    data = libinput_device_get_user_data(device);
    timestamp = libinput_event_pointer_get_time_usec(pointer_event);

    data->timestamp = timestamp;

    struct vec2f delta = VEC2F(libinput_event_pointer_get_dx(pointer_event), libinput_event_pointer_get_dy(pointer_event));

    if (data->has_emitted_pointer_events == false) {
        data->has_emitted_pointer_events = true;
        input->n_cursor_devices++;
        maybe_enable_mouse_cursor(input, device, timestamp);
    }

    emit_event(input, make_pointer_motion_event(device, timestamp, input->cursor_slot_id, data->buttons, delta));
    return 0;
}

static int on_mouse_motion_absolute_event(struct user_input *input, struct libinput_event *event) {
    struct libinput_event_pointer *pointer_event;
    struct input_device_data *data;
    struct libinput_device *device;
    uint64_t timestamp;

    assert(input != NULL);
    assert(event != NULL);

    pointer_event = libinput_event_get_pointer_event(event);
    device = libinput_event_get_device(event);
    data = libinput_device_get_user_data(device);
    timestamp = libinput_event_pointer_get_time_usec(pointer_event);

    data->timestamp = timestamp;

    // transform x & y to view (flutter) coordinates
    struct vec2f pos_ndc = VEC2F(
        libinput_event_pointer_get_absolute_x_transformed(pointer_event, 1.0),
        libinput_event_pointer_get_absolute_y_transformed(pointer_event, 1.0)
    );

    if (data->has_emitted_pointer_events == false) {
        data->has_emitted_pointer_events = true;
        input->n_cursor_devices++;
        maybe_enable_mouse_cursor(input, device, timestamp);
    }

    emit_event(input, make_pointer_motion_absolute_event(device, timestamp, input->cursor_slot_id, data->buttons, pos_ndc));

    return 0;
}

static int on_mouse_button_event(struct user_input *input, struct libinput_event *event) {
    struct libinput_event_pointer *pointer_event;
    enum libinput_button_state button_state;
    struct input_device_data *data;
    struct libinput_device *device;
    uint64_t timestamp;
    uint16_t evdev_code;
    uint8_t flutter_button;
    uint8_t new_flutter_button_state;

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
        maybe_enable_mouse_cursor(input, device, timestamp);
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
        emit_event(
            input,
            make_pointer_button_event(
                device,
                timestamp,
                input->cursor_slot_id,
                new_flutter_button_state,
                new_flutter_button_state ^ data->buttons
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
    uint64_t timestamp;

    assert(input != NULL);
    assert(event != NULL);

    pointer_event = libinput_event_get_pointer_event(event);
    device = libinput_event_get_device(event);
    data = libinput_device_get_user_data(device);
    timestamp = libinput_event_pointer_get_time_usec(pointer_event);

    // since the stored coords are in display, not view coordinates,
    // we need to transform them again
    double scroll_x = libinput_event_pointer_has_axis(pointer_event, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL) ?
                          libinput_event_pointer_get_axis_value(pointer_event, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL) :
                          0.0;

    double scroll_y = libinput_event_pointer_has_axis(pointer_event, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL) ?
                          libinput_event_pointer_get_axis_value(pointer_event, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL) :
                          0.0;

    emit_event(
        input,
        make_pointer_axis_event(
            device,
            timestamp,
            input->cursor_slot_id,
            data->buttons,
            VEC2F(scroll_x / 15.0 * 53.0, scroll_y / 15.0 * 53.0)
        )

    );

    return 0;
}

static int on_touch_down(struct user_input *input, struct libinput_event *event) {
    struct libinput_event_touch *touch_event;
    struct input_device_data *data;
    uint64_t timestamp;
    int64_t device_id;
    int slot;

    assert(input != NULL);
    assert(event != NULL);

    struct libinput_device *device = libinput_event_get_device(event);
    data = libinput_device_get_user_data(libinput_event_get_device(event));
    touch_event = libinput_event_get_touch_event(event);
    timestamp = libinput_event_touch_get_time_usec(touch_event);

    // get the multitouch slot for this event
    // can return -1 when the device is a single touch device
    slot = libinput_event_touch_get_slot(touch_event);
    if (slot == -1) {
        slot = 0;
    }

    device_id = data->touch_slot_id_offset + slot;

    // transform the display coordinates to view (flutter) coordinates
    struct vec2f pos_ndc =
        VEC2F(libinput_event_touch_get_x_transformed(touch_event, 1.0), libinput_event_touch_get_y_transformed(touch_event, 1.0));

    emit_event(input, make_touch_down_event(device, timestamp, device_id, pos_ndc));

    // alter our device state
    data->positions[slot] = pos_ndc;
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

    struct libinput_device *device = libinput_event_get_device(event);
    data = libinput_device_get_user_data(device);
    touch_event = libinput_event_get_touch_event(event);
    timestamp = libinput_event_touch_get_time_usec(touch_event);

    // get the multitouch slot for this event
    // can return -1 when the device is a single touch device
    slot = libinput_event_touch_get_slot(touch_event);
    if (slot == -1) {
        slot = 0;
    }

    device_id = data->touch_slot_id_offset + slot;

    emit_event(input, make_touch_up_event(device, timestamp, device_id, data->positions[slot]));

    return 0;
}

static int on_touch_motion(struct user_input *input, struct libinput_event *event) {
    struct libinput_event_touch *touch_event;
    struct input_device_data *data;
    uint64_t timestamp;
    int64_t device_id;
    int slot;

    assert(input != NULL);
    assert(event != NULL);

    struct libinput_device *device = libinput_event_get_device(event);
    data = libinput_device_get_user_data(device);
    touch_event = libinput_event_get_touch_event(event);
    timestamp = libinput_event_touch_get_time_usec(touch_event);

    // get the multitouch slot for this event
    // can return -1 when the device is a single touch device
    slot = libinput_event_touch_get_slot(touch_event);
    if (slot == -1) {
        slot = 0;
    }

    device_id = data->touch_slot_id_offset + slot;

    // transform the display coordinates to view (flutter) coordinates
    struct vec2f pos_ndc =
        VEC2F(libinput_event_touch_get_x_transformed(touch_event, 1.0), libinput_event_touch_get_y_transformed(touch_event, 1.0));

    // emit the flutter pointer event
    emit_event(input, make_touch_move_event(device, timestamp, device_id, pos_ndc));

    // alter our device state
    data->positions[slot] = pos_ndc;
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
    uint64_t timestamp;
    int64_t device_id;

    ASSERT_NOT_NULL(input);
    ASSERT_NOT_NULL(event);

    struct libinput_device *device = libinput_event_get_device(event);
    data = libinput_device_get_user_data(device);
    ASSERT_NOT_NULL(data);

    tablet_event = libinput_event_get_tablet_tool_event(event);
    timestamp = libinput_event_tablet_tool_get_time_usec(tablet_event);

    device_id = data->stylus_slot_id;

    struct vec2f pos_ndc;
    pos_ndc.x = libinput_event_tablet_tool_get_x_transformed(tablet_event, 1);
    pos_ndc.y = libinput_event_tablet_tool_get_y_transformed(tablet_event, 1);

    emit_event(input, make_stylus_move_event(device, timestamp, device_id, data->tip, pos_ndc));
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

    struct libinput_device *device = libinput_event_get_device(event);
    data = libinput_device_get_user_data(device);
    ASSERT_NOT_NULL(data);

    tablet_event = libinput_event_get_tablet_tool_event(event);
    timestamp = libinput_event_tablet_tool_get_time_usec(tablet_event);

    device_id = data->stylus_slot_id;

    pos.x = libinput_event_tablet_tool_get_x_transformed(tablet_event, 1);
    pos.y = libinput_event_tablet_tool_get_y_transformed(tablet_event, 1);

    if (!data->tip) {
        emit_event(input, make_stylus_move_event(device, timestamp, device_id, data->tip, pos));
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

    struct libinput_device *device = libinput_event_get_device(event);
    data = libinput_device_get_user_data(device);
    ASSERT_NOT_NULL(data);

    tablet_event = libinput_event_get_tablet_tool_event(event);
    timestamp = libinput_event_tablet_tool_get_time_usec(tablet_event);

    device_id = data->stylus_slot_id;

    pos.x = libinput_event_tablet_tool_get_x_transformed(tablet_event, 1);
    pos.y = libinput_event_tablet_tool_get_y_transformed(tablet_event, 1);

    if (libinput_event_tablet_tool_get_tip_state(tablet_event) == LIBINPUT_TABLET_TOOL_TIP_DOWN) {
        data->tip = true;
        emit_event(input, make_stylus_down_event(device, timestamp, device_id, pos));
    } else {
        data->tip = false;
        emit_event(input, make_stylus_up_event(device, timestamp, device_id, pos));
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

        // We explicitly don't want to handle every event type here.
        // Otherwise we'd need to add a new `case` every libinput introduces
        // a new event.
        PRAGMA_DIAGNOSTIC_PUSH
        PRAGMA_DIAGNOSTIC_IGNORED("-Wswitch-enum")
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
        PRAGMA_DIAGNOSTIC_POP

        libinput_event_destroy(event);
    }

    return 0;

fail_destroy_event:
    libinput_event_destroy(event);
    return ok;
}

int user_input_on_fd_ready(struct user_input *input) {
    uint64_t timestamp;
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

    // handle all available libinput events
    ok = process_libinput_events(input, timestamp);
    if (ok != 0) {
        LOG_ERROR("Could not process libinput events. process_libinput_events: %s\n", strerror(ok));
        return ok;
    }

    // make sure we've dispatched all the flutter pointer events
    flush_events(input);

    // now we can cleanup all devices that were removed
    while (kv_size(input->deferred_device_cleanups) > 0) {
        struct input_device_data *data = kv_pop(input->deferred_device_cleanups);
        cleanup_device_data(data);
    }

    return 0;
}

static void
add_listener(struct user_input *input, bool is_primary, enum user_input_event_type filter, user_input_event_cb_t cb, void *userdata) {
    ASSERT_NOT_NULL(input);
    ASSERT_NOT_NULL(cb);

    if (input->n_listeners >= ARRAY_SIZE(input->listeners)) {
        LOG_ERROR("Could not add input listener, too many listeners.\n");
        return;
    }

    if (is_primary) {
        for (size_t i = 0; i < input->n_listeners; i++) {
            if (input->listeners[i].is_primary) {
                LOG_ERROR("Could not add primary input listener, another primary listener was already added.\n");
                return;
            }
        }
    }

    input->listeners[input->n_listeners].filter = filter;
    input->listeners[input->n_listeners].is_primary = is_primary;
    input->listeners[input->n_listeners].cb = cb;
    input->listeners[input->n_listeners].userdata = userdata;
    input->n_listeners++;
}

void user_input_add_primary_listener(struct user_input *input, enum user_input_event_type filter, user_input_event_cb_t cb, void *userdata) {
    add_listener(input, true, filter, cb, userdata);
}

void user_input_add_listener(struct user_input *input, enum user_input_event_type filter, user_input_event_cb_t cb, void *userdata) {
    add_listener(input, false, filter, cb, userdata);
}

void user_input_device_set_primary_listener_userdata(struct user_input_device *device, void *userdata) {
    ASSERT_NOT_NULL(device);

    struct input_device_data *data = user_input_device_get_device_data(device);
    ASSERT_NOT_NULL(data);

    data->primary_listener_userdata = userdata;
}

void *user_input_device_get_primary_listener_userdata(struct user_input_device *device) {
    ASSERT_NOT_NULL(device);

    struct input_device_data *data = user_input_device_get_device_data(device);
    ASSERT_NOT_NULL(data);

    return data->primary_listener_userdata;
}

int64_t user_input_device_get_id(struct user_input_device *device) {
    ASSERT_NOT_NULL(device);

    struct input_device_data *data = user_input_device_get_device_data(device);
    return data->device_id;
}

struct libinput_device *user_input_device_get_libinput_device(struct user_input_device *device) {
    ASSERT_NOT_NULL(device);

    struct input_device_data *data = user_input_device_get_device_data(device);
    return data->device;
}

struct udev_device *user_input_device_get_udev_device(struct user_input_device *device) {
    ASSERT_NOT_NULL(device);

    struct input_device_data *data = user_input_device_get_device_data(device);
    return libinput_device_get_udev_device(data->device);
}
