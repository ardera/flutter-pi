#ifndef _KEY_EVENT_H
#define _KEY_EVENT_H

#include <stdint.h>
#include <linux/input-event-codes.h>

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

#endif