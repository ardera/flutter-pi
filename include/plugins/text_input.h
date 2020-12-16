#ifndef _TEXT_INPUT_H
#define _TEXT_INPUT_H

#include <xkbcommon/xkbcommon.h>
#include <pluginregistry.h>

#define TEXT_INPUT_PLUGIN_NAME "text_input"
#define TEXT_INPUT_CHANNEL "flutter/textinput"
#define TEXT_INPUT_MAX_CHARS 8192

struct text_input_plugin;

enum text_input_type {
    kInputTypeText,
    kInputTypeMultiline,
    kInputTypeNumber,
    kInputTypePhone,
    kInputTypeDatetime,
    kInputTypeEmailAddress,
    kInputTypeUrl,
    kInputTypeVisiblePassword,
    kInputTypeName,
    kInputTypeAddress
};

enum text_input_action {
    kTextInputActionNone,
    kTextInputActionUnspecified,
    kTextInputActionDone,
    kTextInputActionGo,
    kTextInputActionSearch,
    kTextInputActionSend,
    kTextInputActionNext,
    kTextInputActionPrevious,
    kTextInputActionContinueAction,
    kTextInputActionJoin,
    kTextInputActionRoute,
    kTextInputActionEmergencyCall,
    kTextInputActionNewline
};

// while text input configuration has more values, we only care about these two.
struct text_input_configuration {
    bool autocorrect;
    enum text_input_action input_action;
};

enum floating_cursor_drag_state {
    kFloatingCursorDragStateStart,
    kFloatingCursorDragStateUpdate,
    kFloatingCursorDragStateEnd
};

// parses the input string as linux terminal input and calls the TextInput model functions
// accordingly.
int textin_on_utf8_char(struct text_input_plugin *textin, uint8_t *c);
int textin_on_xkb_keysym(struct text_input_plugin *textin, xkb_keysym_t keysym);

/**
 * @brief Get the text input plugin instance of a specific @ref plugin_registry.
 * 
 * @returns An pointer to @ref text_input_plugin on success, NULL on failure (if there's no text input plugin registered
 * for that @ref registry)
 */
static inline struct text_input_plugin *textin_get_instance_via_plugin_registry(struct plugin_registry *registry) {
    return plugin_registry_get_plugin_userdata(registry, TEXT_INPUT_PLUGIN_NAME);
}

int textin_init(struct flutterpi *flutterpi, void **userdata);
int textin_deinit(struct flutterpi *flutterpi, void **userdata);

#endif