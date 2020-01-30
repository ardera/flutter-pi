#ifndef _TEXT_INPUT_H
#define _TEXT_INPUT_H

#include <console_keyboard.h>

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

int TextInput_syncEditingState(void);
int TextInput_performAction(enum text_input_action action);
int TextInput_onConnectionClosed(void);

// TextInput model functions (updating the text editing state)
bool TextInput_deleteSelected(void);
bool TextInput_addChar(char c);
bool TextInput_backspace(void);
bool TextInput_delete(void);
bool TextInput_moveCursorToBeginning(void);
bool TextInput_moveCursorToEnd(void);
bool TextInput_moveCursorForward(void);
bool TextInput_moveCursorBack(void);

// parses the input string as linux terminal input and calls the TextInput model functions
// accordingly.
int TextInput_onChar(char c);
int TextInput_onKey(glfw_key key);

#endif