#include <ctype.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <flutter-pi.h>
#include <pluginregistry.h>
#include <keyboard.h>

#include <plugins/raw_keyboard.h>

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
) {
    /**
     * keymap: android
     * flags: flags
     * codePoint: code_point
     * keyCode: key_code
     * plainCodePoint: plain_code_point
     * scanCode: scan_code
     * metaState: meta_state
     * source: source
     * vendorId: vendor_id
     * productId: product_id
     * deviceId: device_id
     * repeatCount: repeatCount,
     * type: is_down? "keydown" : "keyup"
     * character: character
     */

    (void) plain_code_point;
    
    return platch_send(
        KEY_EVENT_CHANNEL,
        &(struct platch_obj) {
            .codec = kJSONMessageCodec,
            .json_value = {
                .type = kJsonObject,
                .size = 14,
                .keys = (char*[14]) {
                    "keymap",
                    "flags",
                    "codePoint",
                    "keyCode",
                    "plainCodePoint",
                    "scanCode",
                    "metaState",
                    "source",
                    "vendorId",
                    "productId",
                    "deviceId",
                    "repeatCount",
                    "type",
                    "character"
                },
                .values = (struct json_value[14]) {
                    /* keymap */            {.type = kJsonString, .string_value = "android"},
                    /* flags */             {.type = kJsonNumber, .number_value = flags},
                    /* codePoint */         {.type = kJsonNumber, .number_value = code_point},
                    /* keyCode */           {.type = kJsonNumber, .number_value = key_code},
                    /* plainCodePoint */    {.type = kJsonNumber, .number_value = code_point},
                    /* scanCode */          {.type = kJsonNumber, .number_value = scan_code},
                    /* metaState */         {.type = kJsonNumber, .number_value = meta_state},
                    /* source */            {.type = kJsonNumber, .number_value = source},
                    /* vendorId */          {.type = kJsonNumber, .number_value = vendor_id},
                    /* productId */         {.type = kJsonNumber, .number_value = product_id},
                    /* deviceId */          {.type = kJsonNumber, .number_value = device_id},
                    /* repeatCount */       {.type = kJsonNumber, .number_value = repeat_count},
                    /* type */              {.type = kJsonString, .string_value = is_down? "keydown" : "keyup"},
                    /* character */         {.type = character? kJsonString : kJsonNull, .string_value = character}
                }
            }
        },
        kJSONMessageCodec,
        NULL,
        NULL
    );
}

int rawkb_send_gtk_keyevent(
    uint32_t unicode_scalar_values,
    uint32_t key_code,
    uint32_t scan_code,
    uint32_t modifiers,
    bool is_down
) {
    /**
     * keymap: linux
     * toolkit: glfw
     * unicodeScalarValues: code_point
     * keyCode: key_code
     * scanCode: scan_code
     * modifiers: mods
     * type: is_down? "keydown" : "keyup"
     */

    return platch_send(
        KEY_EVENT_CHANNEL,
        &(struct platch_obj) {
            .codec = kJSONMessageCodec,
            .json_value = {
                .type = kJsonObject,
                .size = 7,
                .keys = (char*[7]) {
                    "keymap",
                    "toolkit",
                    "unicodeScalarValues",
                    "keyCode",
                    "scanCode",
                    "modifiers",
                    "type"
                },
                .values = (struct json_value[7]) {
                    /* keymap */                {.type = kJsonString, .string_value = "linux"},
                    /* toolkit */               {.type = kJsonString, .string_value = "gtk"},
                    /* unicodeScalarValues */   {.type = kJsonNumber, .number_value = unicode_scalar_values},
                    /* keyCode */               {.type = kJsonNumber, .number_value = key_code},
                    /* scanCode */              {.type = kJsonNumber, .number_value = scan_code},
                    /* modifiers */             {.type = kJsonNumber, .number_value = modifiers},
                    /* type */                  {.type = kJsonString, .string_value = is_down? "keydown" : "keyup"}
                }
            }
        },
        kJSONMessageCodec,
        NULL,
        NULL
    );
}

int rawkb_init(void) {
    return 0;
}

int rawkb_deinit(void) {
    return 0;
}
