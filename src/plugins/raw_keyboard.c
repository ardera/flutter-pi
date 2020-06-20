#include <ctype.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <flutter-pi.h>
#include <pluginregistry.h>
#include <console_keyboard.h>

#include <plugins/raw_keyboard.h>

static struct {
    // same as mods, just that it differentiates between left and right-sided modifiers.
    uint16_t leftright_mods;
    glfw_keymod_map mods;
    bool initialized;
} raw_keyboard = {0};

static int send_glfw_keyevent(uint32_t code_point, glfw_key key_code, uint32_t scan_code, glfw_keymod_map mods, bool is_down) {
    return platch_send(
        KEY_EVENT_CHANNEL,
        &(struct platch_obj) {
            .codec = kJSONMessageCodec,
            .json_value = {
                .type = kJsonObject,
                .size = 7,
                .keys = (char*[7]) {
                    "keymap", "toolkit", "unicodeScalarValues", "keyCode", "scanCode",
                    "modifiers", "type"
                },
                .values = (struct json_value[7]) {
                    {.type = kJsonString, .string_value = "linux"},
                    {.type = kJsonString, .string_value = "glfw"},
                    {.type = kJsonNumber, .number_value = code_point},
                    {.type = kJsonNumber, .number_value = key_code},
                    {.type = kJsonNumber, .number_value = scan_code},
                    {.type = kJsonNumber, .number_value = mods},
                    {.type = kJsonString, .string_value = is_down? "keydown" : "keyup"}
                }
            }
        },
        kJSONMessageCodec,
        NULL,
        NULL
    );
}

int rawkb_on_keyevent(glfw_key key, uint32_t scan_code, glfw_key_action action) {
    glfw_keymod_map mods_after = raw_keyboard.mods;
    uint16_t        lrmods_after = raw_keyboard.leftright_mods;
    glfw_keymod     mod;
    bool send;

    if (!raw_keyboard.initialized) return 0;

    // flutter's glfw key adapter does not distinguish between left- and right-sided modifier keys.
    // so we implicitly combine the state of left and right-sided keys
    mod = GLFW_KEYMOD_FOR_KEY(key);
    send = !mod;

    if (mod && ((action == GLFW_PRESS) || (action == GLFW_RELEASE))) {
        lrmods_after = raw_keyboard.leftright_mods;
        
        switch (mod) {
            case GLFW_MOD_SHIFT:
            case GLFW_MOD_CONTROL:
            case GLFW_MOD_ALT:
            case GLFW_MOD_SUPER: ;
                uint16_t sided_mod = mod;

                if (GLFW_KEY_IS_RIGHTSIDED(key))
                    sided_mod = sided_mod << 8;

                if (action == GLFW_PRESS) {
                    lrmods_after |= sided_mod;
                } else if (action == GLFW_RELEASE) {
                    lrmods_after &= ~sided_mod;
                }
                break;
            case GLFW_MOD_CAPS_LOCK:
            case GLFW_MOD_NUM_LOCK:
                if (action == GLFW_PRESS)
                    lrmods_after ^= mod;
                break;
            default:
                break;
        }

        mods_after = lrmods_after | (lrmods_after >> 8);
        if (mods_after != raw_keyboard.mods)
            send = true;
    }

    switch (key) {
        case GLFW_KEY_RIGHT_SHIFT:
            key = GLFW_KEY_LEFT_SHIFT;
            break;
        case GLFW_KEY_RIGHT_CONTROL:
            key = GLFW_KEY_LEFT_CONTROL;
            break;
        case GLFW_KEY_RIGHT_ALT:
            key = GLFW_KEY_LEFT_ALT;
            break;
        case GLFW_KEY_RIGHT_SUPER:
            key = GLFW_KEY_LEFT_SUPER;
            break;
        default: break;
    }

    if (send) {
        send_glfw_keyevent(0, key, scan_code, raw_keyboard.mods, action != GLFW_RELEASE);
    }

    raw_keyboard.leftright_mods = lrmods_after;
    raw_keyboard.mods = mods_after;

    return 0;
}

int rawkb_init(void) {
    raw_keyboard.leftright_mods = 0;
    raw_keyboard.mods = 0;
    raw_keyboard.initialized = true;
    
    return 0;
}

int rawkb_deinit(void) {
    raw_keyboard.initialized = false;

    return 0;
}