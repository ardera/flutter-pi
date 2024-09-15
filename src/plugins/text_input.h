#ifndef _TEXT_INPUT_H
#define _TEXT_INPUT_H

#include <stdbool.h>

#include <xkbcommon/xkbcommon.h>

#define TEXT_INPUT_CHANNEL "flutter/textinput"

#define TEXT_INPUT_MAX_CHARS 8192

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
    kInputTypeAddress,
    kInputTypeNone
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

enum floating_cursor_drag_state { kFloatingCursorDragStateStart, kFloatingCursorDragStateUpdate, kFloatingCursorDragStateEnd };

/**
 * @brief Should be called when text input was received from the keyboard.
 * 
 * @param str The NULL-terminated UTF-8 string that was received.
 */
int textin_on_text(const char *text);

/**
 * @brief Should be called when a key was pressed on the keyboard.
 * 
 * @param keysym The keysym that was pressed.
 */
int textin_on_xkb_keysym(xkb_keysym_t keysym);

#endif
