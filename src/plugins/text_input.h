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

// parses the input string as linux terminal input and calls the TextInput model functions
// accordingly.
int textin_on_utf8_char(uint8_t *c);
int textin_on_xkb_keysym(xkb_keysym_t keysym);

#endif
