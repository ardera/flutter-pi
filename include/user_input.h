#ifndef USER_INPUT_H_
#define USER_INPUT_H_

#include <xkbcommon/xkbcommon.h>
#include <flutter_embedder.h>
#include <dylib_deps.h>

#define LOG_USER_INPUT_ERROR(...) fprintf(stderr, "[user input] " __VA_ARGS__)
#define MAX_COLLECTED_FLUTTER_POINTER_EVENTS 64

#define FLUTTER_POINTER_EVENT(_phase, _timestamp, _x, _y, _device, _signal_kind, _scroll_delta_x, _scroll_delta_y, _device_kind, _buttons) \
    (FlutterPointerEvent) { \
        .struct_size = sizeof(FlutterPointerEvent), \
        .phase = (_phase), \
        .timestamp = (_timestamp), \
        .x = (_x), .y = (_y), \
        .device = (_device), \
        .signal_kind = (_signal_kind), \
        .scroll_delta_x = (_scroll_delta_x), \
        .scroll_delta_y = (_scroll_delta_y), \
        .device_kind = (_device_kind), \
        .buttons = (_buttons) \
    }

#define FLUTTER_POINTER_TOUCH_ADD_EVENT(_timestamp, _x, _y, _device_id) \
    FLUTTER_POINTER_EVENT(kAdd, _timestamp, _x, _y, _device_id, kFlutterPointerSignalKindNone, 0.0, 0.0, kFlutterPointerDeviceKindTouch, 0)

#define FLUTTER_POINTER_TOUCH_REMOVE_EVENT(_timestamp, _x, _y, _device_id) \
    FLUTTER_POINTER_EVENT(kRemove, _timestamp, _x, _y, _device_id, kFlutterPointerSignalKindNone, 0.0, 0.0, kFlutterPointerDeviceKindTouch, 0)

#define FLUTTER_POINTER_TOUCH_MOVE_EVENT(_timestamp, _x, _y, _device_id) \
    FLUTTER_POINTER_EVENT(kMove, _timestamp, _x, _y, _device_id, kFlutterPointerSignalKindNone, 0.0, 0.0, kFlutterPointerDeviceKindTouch, 0)

#define FLUTTER_POINTER_TOUCH_DOWN_EVENT(_timestamp, _x, _y, _device_id) \
    FLUTTER_POINTER_EVENT(kDown, _timestamp, _x, _y, _device_id, kFlutterPointerSignalKindNone, 0.0, 0.0, kFlutterPointerDeviceKindTouch, 0)

#define FLUTTER_POINTER_TOUCH_UP_EVENT(_timestamp, _x, _y, _device_id) \
    FLUTTER_POINTER_EVENT(kUp, _timestamp, _x, _y, _device_id, kFlutterPointerSignalKindNone, 0.0, 0.0, kFlutterPointerDeviceKindTouch, 0)

#define FLUTTER_POINTER_MOUSE_BUTTON_EVENT(_phase, _timestamp, _x, _y, _device_id, _buttons) \
    FLUTTER_POINTER_EVENT(_phase, _timestamp, _x, _y, _device_id, kFlutterPointerSignalKindNone, 0.0, 0.0, kFlutterPointerDeviceKindMouse, _buttons)

#define FLUTTER_POINTER_MOUSE_ADD_EVENT(_timestamp, _x, _y, _device_id, _buttons) \
    FLUTTER_POINTER_EVENT(kAdd, _timestamp, _x, _y, _device_id, kFlutterPointerSignalKindNone, 0.0, 0.0, kFlutterPointerDeviceKindMouse, _buttons)

#define FLUTTER_POINTER_REMOVE_EVENT(_timestamp, _x, _y, _device, _buttons) \
    FLUTTER_POINTER_EVENT(kRemove, _timestamp, _x, _y, _device_id, kFlutterPointerSignalKindNone, 0.0, 0.0, kFlutterPointerDeviceKindMouse, _buttons)

#define FLUTTER_POINTER_MOUSE_MOVE_EVENT(_timestamp, _x, _y, _device_id, _buttons) \
    FLUTTER_POINTER_EVENT( \
        (_buttons) & kFlutterPointerButtonMousePrimary ? kMove : kHover, \
        _timestamp, \
        _x, _y, \
        _device_id, \
        kFlutterPointerSignalKindNone, \
        0.0, 0.0, \
        kFlutterPointerDeviceKindMouse, \
        _buttons\
    )

typedef void (*flutter_pointer_event_callback_t)(void *userdata, const FlutterPointerEvent *events, size_t n_events);

typedef void (*utf8_character_callback_t)(void *userdata, uint8_t *character);

typedef void (*xkb_keysym_callback_t)(void *userdata, xkb_keysym_t keysym);

typedef void (*gtk_keyevent_callback_t)(
	void *userdata,
	uint32_t unicode_scalar_values,
    uint32_t key_code,
    uint32_t scan_code,
    uint32_t modifiers,
    bool is_down
);

typedef void (*set_cursor_enabled_callback_t)(void *userdata, bool enabled);

typedef void (*move_cursor_callback_t)(void *userdata, unsigned int x, unsigned int y);

struct user_input_interface {
    flutter_pointer_event_callback_t on_flutter_pointer_event;
    utf8_character_callback_t on_utf8_character;
    xkb_keysym_callback_t on_xkb_keysym;
    gtk_keyevent_callback_t on_gtk_keyevent;
    set_cursor_enabled_callback_t on_set_cursor_enabled;
    move_cursor_callback_t on_move_cursor;
};

struct user_input;

struct user_input *user_input_new(
    const struct user_input_interface *interface, 
    void *userdata,
    const FlutterTransformation *display_to_view_transform,
	unsigned int display_width,
	unsigned int display_height
);

void user_input_destroy(struct user_input *input);

void user_input_set_transform(
	struct user_input *input,
	const FlutterTransformation *display_to_view_transform,
	unsigned int display_width,
	unsigned int display_height 
);

int user_input_get_fd(struct user_input *input);

int user_input_on_fd_ready(struct user_input *input);

#endif