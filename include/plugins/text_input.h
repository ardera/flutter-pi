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

int textin_sync_editing_state(void);
int textin_perform_action(enum text_input_action action);
int textin_on_connection_closed(void);

// TextInput model functions (updating the text editing state)
bool textin_delete_selected(void);
bool textin_add_utf8_char(char *c);
bool textin_backspace(void);
bool textin_delete(void);
bool textin_move_cursor_to_beginning(void);
bool textin_move_cursor_to_end(void);
bool textin_move_cursor_forward(void);
bool textin_move_cursor_back(void);

// parses the input string as linux terminal input and calls the TextInput model functions
// accordingly.
int textin_on_utf8_char(char *c);
int textin_on_key(glfw_key key);

int textin_init(void);
int textin_deinit(void);

#endif