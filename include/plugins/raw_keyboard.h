#ifndef _KEY_EVENT_H
#define _KEY_EVENT_H

#include <stdint.h>
#include <linux/input-event-codes.h>

typedef struct {
    union {
        struct {
            bool shift : 1;
            bool capslock : 1;
            bool ctrl : 1;
            bool alt : 1;
            bool numlock : 1;
            int __pad : 23;
            bool meta : 1;
        };
        uint32_t u32;
    };
} key_modifiers_t;


struct key_event_interface {
    void (*send_key_event)(void *userdata, const FlutterKeyEvent *event);
};

struct rawkb;

#define KEY_EVENT_CHANNEL "flutter/keyevent"

int rawkb_send_android_keyevent(
    uint32_t flags,
    uint32_t code_point,
    unsigned int key_code,
    uint32_t plain_code_point,
    uint32_t scan_code,
    uint32_t meta_state,
    uint32_t source,
    uint16_t vendor_id,
    uint16_t product_id,
    uint16_t device_id,
    int repeat_count,
    bool is_down,
    char *character
);

int rawkb_send_gtk_keyevent(
    uint32_t unicode_scalar_values,
    uint32_t key_code,
    uint32_t scan_code,
    uint32_t modifiers,
    bool is_down
);

int rawkb_on_key_event(
    struct rawkb *rawkb,
    uint64_t timestamp_us,
    xkb_keycode_t xkb_keycode,
    xkb_keysym_t xkb_keysym,
    uint32_t plain_codepoint,
    key_modifiers_t modifiers,
    const char *text,
    bool is_down,
    bool is_repeat
);

#endif