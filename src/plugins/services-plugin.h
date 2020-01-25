#ifndef _SERVICES_PLUGIN_H
#define _SERVICES_PLUGIN_H

#include <stdio.h>
#include <string.h>

#define ORIENTATION_FROM_STRING(str) \
    (strcmp(str, "DeviceOrientation.portraitUp") == 0 ? kPortraitUp : \
     strcmp(str, "DeviceOrientation.landscapeLeft") == 0 ? kLandscapeLeft :\
     strcmp(str, "DeviceOrientation.portraitDown") == 0 ? kPortraitDown :\
     strcmp(str, "DeviceOrientation.landscapeRight") == 0 ? kLandscapeRight : -1)

#define KEYEVENT_CHANNEL "flutter/keyevent"
#define TEXTINPUT_CHANNEL "flutter/textinput"

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

struct text_editing_value {
    char *text;
    int   selectionBase;
    int   selectionExtent;
    bool  selectionAffinityIsDownstream;
    bool  selectionIsDirectional;
    int   composingBase, composingExtent;
};

struct text_input_configuration {
    int inputType;
    bool obscureText;
    bool autocorrect;
    int smartDashesType;
    int smartQuotesType;
    bool enableSuggestions;
    char *inputAction;
    int textCapitalization;
    int keyboardAppearance;
};

int Services_init(void);
int Services_deinit(void);

int Services_TextInput_updateEditingState(struct text_editing_value newState);
int Services_TextInput_performAction(enum text_input_action action);
int Services_TextInput_onConnectionClosed(void);

#endif