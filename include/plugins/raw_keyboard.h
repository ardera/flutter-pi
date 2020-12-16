#ifndef _KEY_EVENT_H
#define _KEY_EVENT_H

#include <stdint.h>
#include <linux/input-event-codes.h>

#define RAW_KEYBOARD_PLUGIN_NAME "raw_keyboard"
#define KEY_EVENT_CHANNEL "flutter/keyevent"

struct raw_keyboard_plugin;

/**
 * @brief Send a raw key event in android format to the flutter `RawKeyboard` interface.
 * For best performance, should be called on the platform thread.
 * 
 * @returns An error code returned by @ref fm_send_blocking if something goes
 * wrong while sending the platform message.
 */
int rawkb_send_android_keyevent(
    struct raw_keyboard_plugin *rawkb,
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

/**
 * @brief Send a raw key event in GTK format to the flutter RawKeyboard interface.
 * For best performance, should be called on the platform thread.
 * 
 * @returns An error code returned by @ref fm_send_blocking if something goes
 * wrong while sending the platform message.
 */
int rawkb_send_gtk_keyevent(
    struct raw_keyboard_plugin *rawkb,
    uint32_t unicode_scalar_values,
    uint32_t key_code,
    uint32_t scan_code,
    uint32_t modifiers,
    bool is_down
);

/**
 * @brief Get the raw keyboard plugin instance of a specific @ref plugin_registry.
 * 
 * @returns An pointer to @ref raw_keyboard_plugin on success, NULL on failure.
 */
static inline struct raw_keyboard_plugin *rawkb_get_instance_via_plugin_registry(struct plugin_registry *registry) {
	return (struct raw_keyboard_plugin *) plugin_registry_get_plugin_userdata(registry, RAW_KEYBOARD_PLUGIN_NAME);
}

int rawkb_init(struct flutterpi *flutterpi, void **userdata);

int rawkb_deinit(struct flutterpi *flutterpi, void **userdata);

#endif