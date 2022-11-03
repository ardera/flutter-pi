#include <stdlib.h>
#include <stdio.h>
#include <memory.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <math.h>

#include <linux/input-event-codes.h>
#include <systemd/sd-event.h>
#include <libinput.h>

#include <flutter-pi.h>
#include <collection.h>
#include <keyboard.h>
#include <user_input.h>
#include <compositor_ng.h>

FILE_DESCR("user input")

#define LIBINPUT_VER(major, minor, patch) ((((major) & 0xFF) << 16) | (((minor) & 0xFF) << 8) | ((patch) & 0xFF))
#define THIS_LIBINPUT_VER LIBINPUT_VER(LIBINPUT_VERSION_MAJOR, LIBINPUT_VERSION_MINOR, LIBINPUT_VERSION_PATCH)

struct input_device_data {
	int64_t flutter_device_id_offset;
	struct keyboard_state *keyboard_state;
	double x, y;
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

// libinput interface
static int open_restricted(const char *path, int flags, void *userdata) {
	(void) userdata;
	return open(path, flags | O_CLOEXEC);
}

static void close_restricted(int fd, void *userdata) {
	(void) userdata;
	close(fd);
}

static const struct libinput_interface libinput_interface = {
	.open_restricted = open_restricted,
	.close_restricted = close_restricted
};

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

	libinput = libinput_udev_create_context(
		&libinput_interface,
		input,
		udev
	);
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

	input->interface = *interface;
    input->userdata = userdata;

	input->libinput = libinput;
	input->kbdcfg = kbdcfg;
	input->next_unused_flutter_device_id = 0;
    
    user_input_set_transform(
        input,
        display_to_view_transform,
        view_to_display_transform,
        display_width,
        display_height
    );

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

void user_input_destroy(struct user_input *input) {
    DEBUG_ASSERT(input != NULL);

    /// TODO: Destroy all the input device data, maybe add an additional
    /// parameter to indicate whether any flutter device removal events should be
    /// emitted.

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
    DEBUG_ASSERT(input != NULL);
    DEBUG_ASSERT(display_to_view_transform != NULL);
    DEBUG_ASSERT(view_to_display_transform != NULL);

    input->display_to_view_transform = *display_to_view_transform;
    input->view_to_display_transform_nontranslating = *view_to_display_transform;
    input->view_to_display_transform_nontranslating.transX = 0.0;
    input->view_to_display_transform_nontranslating.transY = 0.0;
    input->display_width = display_width;
    input->display_height = display_height;
}

int user_input_get_fd(struct user_input *input) {
    DEBUG_ASSERT(input != NULL);
    return libinput_get_fd(input->libinput);
}


static void flush_pointer_events(struct user_input *input) {
    DEBUG_ASSERT(input != NULL);

    if (input->n_collected_flutter_pointer_events > 0) {
        input->interface.on_flutter_pointer_event(
            input->userdata,
            input->collected_flutter_pointer_events,
            input->n_collected_flutter_pointer_events
        );
    
        input->n_collected_flutter_pointer_events = 0;
    }
}

static void emit_pointer_events(struct user_input *input, const FlutterPointerEvent *events, size_t n_events) {
    DEBUG_ASSERT(input != NULL);
    DEBUG_ASSERT(events != NULL);

    size_t to_copy;

    while (n_events > 0) {
        // if the internal buffer is full, flush it
        if (input->n_collected_flutter_pointer_events == MAX_COLLECTED_FLUTTER_POINTER_EVENTS) {
            flush_pointer_events(input);
        }

        // how many pointer events we can copy into the internal pointer event buffer?
        to_copy = min(n_events, MAX_COLLECTED_FLUTTER_POINTER_EVENTS - input->n_collected_flutter_pointer_events);

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

/**
 * @brief Called when input->n_cursor_devices was increased to maybe enable the mouse cursor
 * it it isn't yet enabled.
 */
static void maybe_enable_mouse_cursor(struct user_input *input, uint64_t timestamp) {
    DEBUG_ASSERT(input != NULL);

    if (input->n_cursor_devices == 1) {
        input->cursor_flutter_device_id = input->next_unused_flutter_device_id++;

        emit_pointer_events(
            input,
            &FLUTTER_POINTER_MOUSE_ADD_EVENT(
                timestamp,
                input->cursor_x, input->cursor_y,
                input->cursor_flutter_device_id,
                0
            ),
            1
        );
    }
}

/**
 * @brief Called when input->n_cursor_devices was decreased to maybe disable the mouse cursor.
 */
static void maybe_disable_mouse_cursor(struct user_input *input, uint64_t timestamp) {
    DEBUG_ASSERT(input != NULL);

    if (input->n_cursor_devices == 0) {
        emit_pointer_events(
            input,
            &FLUTTER_POINTER_MOUSE_REMOVE_EVENT(
                timestamp,
                input->cursor_x, input->cursor_y,
                input->cursor_flutter_device_id,
                0
            ),
            1
        );
    }
}


static int on_device_added(struct user_input *input, struct libinput_event *event, uint64_t timestamp) {
    struct input_device_data *data;
    struct libinput_device *device;
    int64_t device_id;

    DEBUG_ASSERT(input != NULL);
    DEBUG_ASSERT(event != NULL);
    
    device = libinput_event_get_device(event);

    data = malloc(sizeof *data);
    if (data == NULL) {
        return ENOMEM;
    }

    data->flutter_device_id_offset = input->next_unused_flutter_device_id;
    data->keyboard_state = NULL;
    data->x = 0.0;
    data->y = 0.0;
    data->buttons = 0;
    data->timestamp = timestamp;
    data->has_emitted_pointer_events = false;
    data->tip = false;

    libinput_device_set_user_data(device, data);

    if (libinput_device_has_capability(device, LIBINPUT_DEVICE_CAP_POINTER)) {
        // no special things to do here
        // mouse pointer will be added as soon as the device actually sends a
        // mouse event, as some devices will erroneously have a LIBINPUT_DEVICE_CAP_POINTER
        // even though they aren't mice. (My keyboard for example is a mouse smh)
    } else if (libinput_device_has_capability(device, LIBINPUT_DEVICE_CAP_TOUCH)) {
        // add all touch slots as individual touch devices to flutter
        for (int i = 0; i < libinput_device_touch_get_touch_count(device); i++) {
            device_id = input->next_unused_flutter_device_id++;

            emit_pointer_events(
                input,
                &FLUTTER_POINTER_TOUCH_ADD_EVENT(
                    timestamp,
                    0.0, 0.0,
                    device_id
                ),
                1
            );
        }
    } else if (libinput_device_has_capability(device, LIBINPUT_DEVICE_CAP_KEYBOARD)) {
        // create a new keyboard state for this keyboard
        if (input->kbdcfg) {
            data->keyboard_state = keyboard_state_new(input->kbdcfg, NULL, NULL);
        } else {
            // If we don't have a keyboard config
            data->keyboard_state = NULL;
        }
    } else if (libinput_device_has_capability(device, LIBINPUT_DEVICE_CAP_TABLET_TOOL)) {
        device_id = input->next_unused_flutter_device_id++;

        /// TODO: Use kFlutterPointerDeviceKindStylus here
        emit_pointer_events(
            input,
            &FLUTTER_POINTER_TOUCH_ADD_EVENT(
                timestamp,
                0.0, 0.0,
                device_id
            ),
            1
        );
    } else {
        // We don't handle this device, so we don't need the data.
        libinput_device_set_user_data(device, NULL);
        free(data);
    }
    
    return 0;
}

static int on_device_removed(struct user_input *input, struct libinput_event *event, uint64_t timestamp) {
    struct input_device_data *data;
    struct libinput_device *device;

    DEBUG_ASSERT(input != NULL);
    DEBUG_ASSERT(event != NULL);
    
    device = libinput_event_get_device(event);
    data = libinput_device_get_user_data(device);

    if (libinput_device_has_capability(device, LIBINPUT_DEVICE_CAP_POINTER)) {
        // if we don't have a mouse cursor added to flutter yet, add one
        if (data->has_emitted_pointer_events) {
            input->n_cursor_devices--;
            maybe_disable_mouse_cursor(input, timestamp);
        }
    } else if (libinput_device_has_capability(device, LIBINPUT_DEVICE_CAP_TOUCH)) {
        // add all touch slots as individual touch devices to flutter
        for (int i = 0; i < libinput_device_touch_get_touch_count(device); i++) {
            emit_pointer_events(
                input,
                &FLUTTER_POINTER_TOUCH_REMOVE_EVENT(
                    timestamp,
                    0.0, 0.0,
                    data->flutter_device_id_offset + i
                ),
                1
            );
        }
    } else if (libinput_device_has_capability(device, LIBINPUT_DEVICE_CAP_KEYBOARD)) {
        // create a new keyboard state for this keyboard
        if (data->keyboard_state != NULL) {
            keyboard_state_destroy(data->keyboard_state);
        }
    }

    if (data != NULL) {
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

    DEBUG_ASSERT(input != NULL);
    DEBUG_ASSERT(event != NULL);

    key_event = libinput_event_get_keyboard_event(event);
    device = libinput_event_get_device(event);
    data = libinput_device_get_user_data(device);

    evdev_keycode = (uint16_t) libinput_event_keyboard_get_key(key_event);
    key_state = libinput_event_keyboard_get_key_state(key_event);

    // If we don't have a keyboard state (for example if we couldn't load /etc/default/keyboard)
    // we just return here.
    if (data->keyboard_state == NULL) {
        return 0;
    }

    // Let the keyboard advance its statemachine.
    // keysym/codepoint are 0 when none were emitted.
    ok = keyboard_state_process_key_event(
        data->keyboard_state,
        evdev_keycode,
        (int32_t) key_state,
        &keysym,
        &codepoint
    );
    if (ok != 0) {
        return ok;
    }

    // GTK keyevent needs the plain codepoint for some reason.
    /// TODO: Maybe remove the evdev_value parameter?
    plain_codepoint = keyboard_state_get_plain_codepoint(data->keyboard_state, evdev_keycode, 1);

    // call the GTK keyevent callback.
    /// TODO: Simplify the meta state construction.
    input->interface.on_gtk_keyevent(
        input->userdata,
        plain_codepoint,
        (uint32_t) keysym,
        evdev_keycode + (uint32_t) 8,
        keyboard_state_is_shift_active(data->keyboard_state)
        | (keyboard_state_is_capslock_active(data->keyboard_state) << 1)
        | (keyboard_state_is_ctrl_active(data->keyboard_state) << 2)
        | (keyboard_state_is_alt_active(data->keyboard_state) << 3)
        | (keyboard_state_is_numlock_active(data->keyboard_state) << 4)
        | (keyboard_state_is_meta_active(data->keyboard_state) << 28),
        key_state
    );

    // Call the UTF8 character callback if we've got a codepoint.
    // Code very similiar to that of the linux kernel in drivers/tty/vt/keyboard.c, to_utf8
    if (codepoint) {
        if (codepoint < 0x80) {
            // we emit UTF8 unconditionally here,
            // maybe we should check if codepoint is a control character?
            if (isprint(codepoint)) {
                input->interface.on_utf8_character(
                    input->userdata,
                    (uint8_t[1]) {codepoint}
                );
            }
        } else if (codepoint < 0x800) {
            input->interface.on_utf8_character(
                input->userdata,
                (uint8_t[2]) {
                    0xc0 | (codepoint >> 6),
                    0x80 | (codepoint & 0x3f)
                }
            );
        } else if (codepoint < 0x10000) {
            // the console keyboard driver of the linux kernel checks
            // at this point whether `codepoint` is a UTF16 high surrogate (U+D800 to U+DFFF)
            // or U+FFFF and returns without emitting UTF8 in that case.
            // don't know whether we should do this here too
            input->interface.on_utf8_character(
                input->userdata,
                (uint8_t[3]) {
                    0xe0 | (codepoint >> 12),
                    0x80 | ((codepoint >> 6) & 0x3f),
                    0x80 | (codepoint & 0x3f)
                }
            );
        } else if (codepoint < 0x110000) {
            input->interface.on_utf8_character(
                input->userdata,
                (uint8_t[4]) {
                    0xf0 | (codepoint >> 18),
                    0x80 | ((codepoint >> 12) & 0x3f),
                    0x80 | ((codepoint >> 6) & 0x3f),
                    0x80 | (codepoint & 0x3f)
                }
            );
        }
    }
    
    // Call the XKB keysym callback if we've got a keysym.
    if (keysym) {
        input->interface.on_xkb_keysym(input->userdata, keysym);
    }

    return 0;
}

static int on_mouse_motion_event(struct user_input *input, struct libinput_event *event) {
    struct libinput_event_pointer *pointer_event;
    struct input_device_data *data;
    struct libinput_device *device;
    struct vec2f delta, pos_display, pos_view;
    uint64_t timestamp;

    DEBUG_ASSERT(input != NULL);
    DEBUG_ASSERT(event != NULL);
    
    pointer_event = libinput_event_get_pointer_event(event);
    device = libinput_event_get_device(event);
    data = libinput_device_get_user_data(device);
    timestamp = libinput_event_pointer_get_time_usec(pointer_event);

    data->timestamp = timestamp;

    delta = transform_point(
        input->view_to_display_transform_nontranslating,
        VEC2F(
            libinput_event_pointer_get_dx(pointer_event),
            libinput_event_pointer_get_dy(pointer_event)
        )
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
    emit_pointer_events(
        input,
        &FLUTTER_POINTER_MOUSE_MOVE_EVENT(
            timestamp,
            pos_view.x,
            pos_view.y,
            data->flutter_device_id_offset,
            data->buttons
        ),
        1
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

    DEBUG_ASSERT(input != NULL);
    DEBUG_ASSERT(event != NULL);

    pointer_event = libinput_event_get_pointer_event(event);
    device = libinput_event_get_device(event);
    data = libinput_device_get_user_data(device);
    timestamp = libinput_event_pointer_get_time_usec(pointer_event);

    // get the new mouse position in display coordinates
    pos_display = VEC2F(
        libinput_event_pointer_get_absolute_x_transformed(pointer_event, input->display_width),
        libinput_event_pointer_get_absolute_y_transformed(pointer_event, input->display_height)
    );

    /// FIXME: Why do we store the coordinates here?
    data->x = pos_display.x;
    data->y = pos_display.y;
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

    emit_pointer_events(
        input,
        &FLUTTER_POINTER_MOUSE_MOVE_EVENT(
            timestamp,
            pos_view.x, pos_view.y,
            data->flutter_device_id_offset,
            data->buttons
        ),
        1
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

    DEBUG_ASSERT(input != NULL);
    DEBUG_ASSERT(event != NULL);

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

        emit_pointer_events(
            input,
            &FLUTTER_POINTER_MOUSE_BUTTON_EVENT(
                pointer_phase,
                timestamp,
                pos_view.x, pos_view.y,
                data->flutter_device_id_offset,
                new_flutter_button_state
            ),
            1
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

    DEBUG_ASSERT(input != NULL);
    DEBUG_ASSERT(event != NULL);

    pointer_event = libinput_event_get_pointer_event(event);
    device = libinput_event_get_device(event);
    data = libinput_device_get_user_data(device);
    timestamp = libinput_event_pointer_get_time_usec(pointer_event);

    // since the stored coords are in display, not view coordinates,
    // we need to transform them again
    pos_view = transform_point(input->display_to_view_transform, VEC2F(input->cursor_x, input->cursor_y));

    double scroll_x = libinput_event_pointer_has_axis(pointer_event, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL)
        ? libinput_event_pointer_get_axis_value(pointer_event, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL)
        : 0.0;

    double scroll_y = libinput_event_pointer_get_axis_value(pointer_event, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL)
        ? libinput_event_pointer_get_axis_value(pointer_event, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL)
        : 0.0;

    emit_pointer_events(
        input,
        &FLUTTER_POINTER_MOUSE_SCROLL_EVENT(
            timestamp,
            pos_view.x,
            pos_view.y,
            data->flutter_device_id_offset,
            scroll_x / 15.0 * 53.0,
            scroll_y / 15.0 * 53.0,
            data->buttons
        ),
        1
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

    DEBUG_ASSERT(input != NULL);
    DEBUG_ASSERT(event != NULL);

    data = libinput_device_get_user_data(libinput_event_get_device(event));
    touch_event = libinput_event_get_touch_event(event);
    timestamp = libinput_event_touch_get_time_usec(touch_event);

    // get the multitouch slot for this event
    // can return -1 when the device is a single touch device
    slot = libinput_event_touch_get_slot(touch_event);
    if (slot == -1) {
        slot = 0;
    }

    device_id = data->flutter_device_id_offset + slot;

    // transform the display coordinates to view (flutter) coordinates
    pos_view = transform_point(
        input->display_to_view_transform,
        VEC2F(
            libinput_event_touch_get_x_transformed(touch_event, input->display_width),
            libinput_event_touch_get_y_transformed(touch_event, input->display_height)
        )
    );

    // emit the flutter pointer event
    emit_pointer_events(input, &FLUTTER_POINTER_TOUCH_DOWN_EVENT(timestamp, pos_view.x, pos_view.y, device_id), 1);

    // alter our device state
    data->x = pos_view.x;
    data->y = pos_view.y;
    data->timestamp = timestamp;

    return 0;
}

static int on_touch_up(struct user_input *input, struct libinput_event *event) {
    struct libinput_event_touch *touch_event;
    struct input_device_data *data;
    uint64_t timestamp;
    int64_t device_id;
    int slot;

    DEBUG_ASSERT(input != NULL);
    DEBUG_ASSERT(event != NULL);

    data = libinput_device_get_user_data(libinput_event_get_device(event));
    touch_event = libinput_event_get_touch_event(event);
    timestamp = libinput_event_touch_get_time_usec(touch_event);

    // get the multitouch slot for this event
    // can return -1 when the device is a single touch device
    slot = libinput_event_touch_get_slot(touch_event);
    if (slot == -1) {
        slot = 0;
    }

    device_id = data->flutter_device_id_offset + slot;

    emit_pointer_events(input, &FLUTTER_POINTER_TOUCH_UP_EVENT(timestamp, data->x, data->y, device_id), 1);

    return 0;
}

static int on_touch_motion(struct user_input *input, struct libinput_event *event) {
    struct libinput_event_touch *touch_event;
    struct input_device_data *data;
    struct vec2f pos_view;
    uint64_t timestamp;
    int64_t device_id;
    int slot;

    DEBUG_ASSERT(input != NULL);
    DEBUG_ASSERT(event != NULL);

    data = libinput_device_get_user_data(libinput_event_get_device(event));
    touch_event = libinput_event_get_touch_event(event);
    timestamp = libinput_event_touch_get_time_usec(touch_event);

    // get the multitouch slot for this event
    // can return -1 when the device is a single touch device
    slot = libinput_event_touch_get_slot(touch_event);
    if (slot == -1) {
        slot = 0;
    }

    device_id = data->flutter_device_id_offset + slot;

    // transform the display coordinates to view (flutter) coordinates
    pos_view = transform_point(
        FLUTTER_TRANSFORM_AS_MAT3F(input->display_to_view_transform),
        VEC2F(
            libinput_event_touch_get_x_transformed(touch_event, input->display_width),
            libinput_event_touch_get_y_transformed(touch_event, input->display_height)
        )
    );

    // emit the flutter pointer event
    emit_pointer_events(input, &FLUTTER_POINTER_TOUCH_MOVE_EVENT(timestamp, pos_view.x, pos_view.y, device_id), 1);

    // alter our device state
    data->x = pos_view.x;
    data->y = pos_view.y;
    data->timestamp = timestamp;

    return 0;
}

static int on_touch_cancel(struct user_input *input, struct libinput_event *event) {
    DEBUG_ASSERT(input != NULL);
    DEBUG_ASSERT(event != NULL);
    
    (void) input;
    (void) event;
    
    /// TODO: Implement touch cancel
    return 0;
}

static int on_touch_frame(struct user_input *input, struct libinput_event *event) {
    DEBUG_ASSERT(input != NULL);
    DEBUG_ASSERT(event != NULL);

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

    DEBUG_ASSERT_NOT_NULL(input);
    DEBUG_ASSERT_NOT_NULL(event);

    data = libinput_device_get_user_data(libinput_event_get_device(event));
    DEBUG_ASSERT_NOT_NULL(data);

    tablet_event = libinput_event_get_tablet_tool_event(event);
    timestamp = libinput_event_tablet_tool_get_time_usec(tablet_event);

    device_id = data->flutter_device_id_offset;

    // Only report down events when the tool is in contact with the tablet.
    /// TODO: Maybe report hover events when it's not in contact?
    /// FIXME: Use kFlutterPointerDeviceKindStylus here
    if (data->tip) {
        pos.x = libinput_event_tablet_tool_get_x_transformed(tablet_event, input->display_width - 1);
        pos.y = libinput_event_tablet_tool_get_y_transformed(tablet_event, input->display_height - 1);

        pos = transform_point(input->display_to_view_transform, pos);
        
        emit_pointer_events(input, &FLUTTER_POINTER_TOUCH_MOVE_EVENT(timestamp, pos.x, pos.y, device_id), 1);
    }

    return 0;
}

static int on_tablet_tool_proximity(struct user_input *input, struct libinput_event *event) {
    DEBUG_ASSERT_NOT_NULL(input);
    DEBUG_ASSERT_NOT_NULL(event);

    (void) input;
    (void) event;

    return 0;
}

static int on_tablet_tool_tip(struct user_input *input, struct libinput_event *event) {
    struct libinput_event_tablet_tool *tablet_event;
    struct input_device_data *data;
    uint64_t timestamp;
    int64_t device_id;
    struct vec2f pos;

    DEBUG_ASSERT_NOT_NULL(input);
    DEBUG_ASSERT_NOT_NULL(event);

    data = libinput_device_get_user_data(libinput_event_get_device(event));
    DEBUG_ASSERT_NOT_NULL(data);

    tablet_event = libinput_event_get_tablet_tool_event(event);
    timestamp = libinput_event_tablet_tool_get_time_usec(tablet_event);

    device_id = data->flutter_device_id_offset;

    pos.x = libinput_event_tablet_tool_get_x_transformed(tablet_event, input->display_width - 1);
    pos.y = libinput_event_tablet_tool_get_y_transformed(tablet_event, input->display_height - 1);

    pos = transform_point(input->display_to_view_transform, pos);

    /// FIXME: Use kFlutterPointerDeviceKindStylus here
    if (libinput_event_tablet_tool_get_tip_state(tablet_event) == LIBINPUT_TABLET_TOOL_TIP_DOWN) {
        data->tip = true;
        emit_pointer_events(input, &FLUTTER_POINTER_TOUCH_DOWN_EVENT(timestamp, pos.x, pos.y, device_id), 1);
    } else {
        data->tip = false;
        emit_pointer_events(input, &FLUTTER_POINTER_TOUCH_UP_EVENT(timestamp, pos.x, pos.y, device_id), 1);
    }

    return 0;
}

static int on_tablet_tool_button(struct user_input *input, struct libinput_event *event) {
    DEBUG_ASSERT_NOT_NULL(input);
    DEBUG_ASSERT_NOT_NULL(event);

    (void) input;
    (void) event;

    return 0;
}

static int on_tablet_pad_button(struct user_input *input, struct libinput_event *event) {
    DEBUG_ASSERT_NOT_NULL(input);
    DEBUG_ASSERT_NOT_NULL(event);

    (void) input;
    (void) event;

    return 0;
}

static int on_tablet_pad_ring(struct user_input *input, struct libinput_event *event) {
    DEBUG_ASSERT_NOT_NULL(input);
    DEBUG_ASSERT_NOT_NULL(event);

    (void) input;
    (void) event;

    return 0;
}

static int on_tablet_pad_strip(struct user_input *input, struct libinput_event *event) {
    DEBUG_ASSERT_NOT_NULL(input);
    DEBUG_ASSERT_NOT_NULL(event);

    (void) input;
    (void) event;

    return 0;
}

#if THIS_LIBINPUT_VER >= LIBINPUT_VER(1, 15, 0)
static int on_tablet_pad_key(struct user_input *input, struct libinput_event *event) {
    DEBUG_ASSERT_NOT_NULL(input);
    DEBUG_ASSERT_NOT_NULL(event);

    (void) input;
    (void) event;

    return 0;
}
#endif

static int process_libinput_events(struct user_input *input, uint64_t timestamp) {
    enum libinput_event_type event_type;
    struct libinput_event *event;
    int ok;

    DEBUG_ASSERT(input != NULL);

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
                ok = on_device_removed(input, event, timestamp);
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
            default:
                break;
        }
        
        libinput_event_destroy(event);
    }

    return 0;


    fail_destroy_event:
    libinput_event_destroy(event);
    return ok;
}

int user_input_on_fd_ready(struct user_input *input) {
    unsigned int cursor_x, cursor_y, cursor_x_before, cursor_y_before;
    uint64_t timestamp; 
    bool cursor_enabled, cursor_enabled_before;
    int ok;

    DEBUG_ASSERT(input != NULL);

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
    cursor_x_before = (unsigned int) round(input->cursor_x);
    cursor_y_before = (unsigned int) round(input->cursor_y);

    // handle all available libinput events
    ok = process_libinput_events(input, timestamp);
    if (ok != 0) {
        LOG_ERROR("Could not process libinput events. process_libinput_events: %s\n", strerror(ok));
        return ok;
    }

    // record cursor state after handling events
    cursor_enabled = input->n_cursor_devices > 0;
    cursor_x = (unsigned int) round(input->cursor_x);
    cursor_y = (unsigned int) round(input->cursor_y);

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
        input->interface.on_move_cursor(input->userdata, cursor_x, cursor_y);
    }

    return 0;
}
