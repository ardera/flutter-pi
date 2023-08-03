// SPDX-License-Identifier: MIT
/*
 * Keyboard / Text Input support
 *
 * Converts key events to text events using the system keyboard config.
 *
 * Copyright (c) 2023, Hannes Winkler <hanneswinkler2000@web.de>
 */

#ifndef _FLUTTERPI_SRC_KEYBOARD_H
#define _FLUTTERPI_SRC_KEYBOARD_H

#include <stdbool.h>

#include <xkbcommon/xkbcommon.h>

struct keyboard_config {
    struct xkb_context *context;
    struct xkb_keymap *default_keymap;
    struct xkb_compose_table *default_compose_table;
};

struct keyboard_state {
    struct keyboard_config *config;
    struct xkb_state *state;
    struct xkb_state *plain_state;
    struct xkb_compose_state *compose_state;
    int n_iso_level2;
    int n_iso_level3;
    int n_iso_level5;
};

struct keyboard_modifier_state {
    bool ctrl : 1;
    bool shift : 1;
    bool alt : 1;
    bool meta : 1;
    bool capslock : 1;
    bool numlock : 1;
    bool scrolllock : 1;
};

#define KEY_RELEASE 0
#define KEY_PRESS 1
#define KEY_REPEAT 2

struct keyboard_config *keyboard_config_new(void);

void keyboard_config_destroy(struct keyboard_config *config);

struct keyboard_state *
keyboard_state_new(struct keyboard_config *config, struct xkb_keymap *keymap_override, struct xkb_compose_table *compose_table_override);

void keyboard_state_destroy(struct keyboard_state *state);

int keyboard_state_process_key_event(
    struct keyboard_state *state,
    uint16_t evdev_keycode,
    int32_t evdev_value,
    xkb_keysym_t *keysym_out,
    uint32_t *codepoint_out
);

uint32_t keyboard_state_get_plain_codepoint(struct keyboard_state *state, uint16_t evdev_keycode, int32_t evdev_value);

static inline bool keyboard_state_is_ctrl_active(struct keyboard_state *state) {
    return xkb_state_mod_name_is_active(state->state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE);
}

static inline bool keyboard_state_is_shift_active(struct keyboard_state *state) {
    return xkb_state_mod_name_is_active(state->state, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE);
}

static inline bool keyboard_state_is_alt_active(struct keyboard_state *state) {
    return xkb_state_mod_name_is_active(state->state, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE);
}

static inline bool keyboard_state_is_meta_active(struct keyboard_state *state) {
    return xkb_state_mod_name_is_active(state->state, XKB_MOD_NAME_LOGO, XKB_STATE_MODS_EFFECTIVE);
}

static inline bool keyboard_state_is_capslock_active(struct keyboard_state *state) {
    return xkb_state_mod_name_is_active(state->state, XKB_MOD_NAME_CAPS, XKB_STATE_MODS_EFFECTIVE);
}

static inline bool keyboard_state_is_numlock_active(struct keyboard_state *state) {
    return xkb_state_mod_name_is_active(state->state, XKB_MOD_NAME_NUM, XKB_STATE_MODS_EFFECTIVE);
}

static inline bool keyboard_state_is_scrolllock_active(struct keyboard_state *state) {
    return xkb_state_mod_name_is_active(state->state, "Mod3", XKB_STATE_MODS_EFFECTIVE);
}

static inline struct keyboard_modifier_state keyboard_state_get_meta_state(struct keyboard_state *state) {
    return (struct keyboard_modifier_state){
        .ctrl = keyboard_state_is_ctrl_active(state),
        .shift = keyboard_state_is_shift_active(state),
        .alt = keyboard_state_is_alt_active(state),
        .meta = keyboard_state_is_meta_active(state),
        .capslock = keyboard_state_is_capslock_active(state),
        .numlock = keyboard_state_is_numlock_active(state),
        .scrolllock = keyboard_state_is_scrolllock_active(state),
    };
}

#endif  // _FLUTTERPI_SRC_KEYBOARD_H
