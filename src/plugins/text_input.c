#include <ctype.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <flutter-pi.h>
#include <pluginregistry.h>
#include "text_input.h"

struct {
    int32_t transaction_id;
    struct text_input_configuration config;
    char text[TEXT_INPUT_MAX_CHARS];
    int  selection_base, selection_extent;
    bool selection_affinity_is_downstream;
    bool selection_is_directional;
    int  composing_base, composing_extent;
} text_input;

int TextInput_onReceive(char *channel, struct ChannelObject *object, FlutterPlatformMessageResponseHandle *responsehandle) {
    struct JSONMsgCodecValue jsvalue, *temp, *autocorrect, *input_action;
    struct text_input_configuration config;
    int ok;

    if STREQ("TextInput.setClient", object->method) {
        /*
         *  TextInput.setClient(List)
         *      Establishes a new transaction. The argument is
         *      a [List] whose first value is an integer representing a previously
         *      unused transaction identifier, and the second is a [String] with a
         *      JSON-encoded object with five keys, as obtained from
         *      [TextInputConfiguration.toJSON]. This method must be invoked before any
         *      others (except `TextInput.hide`). See [TextInput.attach].
         */
        
        if ((object->jsarg.type != kJSArray) || (object->jsarg.size != 2)) {
            return PlatformChannel_respondError(
                responsehandle,
                kJSONMethodCallResponse,
                "illegalargument",
                "Expected JSON Array with length 2 as the argument.",
                NULL
            );
        }

        if (object->jsarg.values[0].type != kJSNumber) {
            return PlatformChannel_respondError(
                responsehandle,
                kJSONMethodCallResponse,
                "illegalargument",
                "Expected transaction id to be a number.",
                NULL
            );
        }

        if (object->jsarg.values[1].type != kJSString) {
            return PlatformChannel_respondError(
                responsehandle,
                kJSONMethodCallResponse,
                "illegalargument",
                "Expected text input configuration to be a String",
                NULL
            );
        }

        ok = PlatformChannel_decodeJSON(object->jsarg.values[1].string_value, &jsvalue);
        if (ok != 0) return ok;

        if (jsvalue.type != kJSObject) {
            return PlatformChannel_respondError(
                responsehandle,
                kJSONMethodCallResponse,
                "illegalargument",
                "Expected decoded text input configuration to be an Object",
                NULL
            );
        }

        // finally parse the text input config
        autocorrect = jsobject_get(&jsvalue, "autocorrect");
        input_action = jsobject_get(&jsvalue, "inputAction");

        // check that the autocorrect property is valid
        if (!(autocorrect && ((autocorrect->type == kJSTrue) || (autocorrect->type == kJSFalse))))
            goto invalid_config;

        config.autocorrect = autocorrect->type == kJSTrue;
        
        // check & parse the input action
        if (!(input_action && (input_action->type == kJSString)))
            goto invalid_config;
        
        if STREQ("TextInputAction.none", input_action->string_value)
            config.input_action = kTextInputActionNone;
        else if STREQ("TextInputAction.unspecified", input_action->string_value)
            config.input_action = kTextInputActionUnspecified;
        else if STREQ("TextInputAction.done", input_action->string_value)
            config.input_action = kTextInputActionDone;
        else if STREQ("TextInputAction.go", input_action->string_value)
            config.input_action = kTextInputActionGo;
        else if STREQ("TextInputAction.search", input_action->string_value)
            config.input_action = kTextInputActionSearch;
        else if STREQ("TextInputAction.send", input_action->string_value)
            config.input_action = kTextInputActionSend;
        else if STREQ("TextInputAction.next", input_action->string_value)
            config.input_action = kTextInputActionNext;
        else if STREQ("TextInputAction.previous", input_action->string_value)
            config.input_action = kTextInputActionPrevious;
        else if STREQ("TextInputAction.continueAction", input_action->string_value)
            config.input_action = kTextInputActionContinueAction;
        else if STREQ("TextInputAction.join", input_action->string_value)
            config.input_action = kTextInputActionJoin;
        else if STREQ("TextInputAction.route", input_action->string_value)
            config.input_action = kTextInputActionRoute;
        else if STREQ("TextInputAction.emergencyCall", input_action->string_value)
            config.input_action = kTextInputActionEmergencyCall;
        else if STREQ("TextInputAction.newline", input_action->string_value)
            config.input_action = kTextInputActionNewline;
        else
            goto invalid_config;

        int32_t new_id = (int32_t) object->jsarg.values[0].number_value;

        text_input.transaction_id = (int32_t) object->jsarg.values[0].number_value;
        text_input.config = config;

        if (config.autocorrect)
            printf("[text_input] warning: flutter requested native autocorrect, which",
                   "is not supported by flutter-pi.\n");

        // success
        PlatformChannel_freeJSONMsgCodecValue(&jsvalue, false);

        return PlatformChannel_respond(
            responsehandle,
            &(struct ChannelObject) {
                .codec = kJSONMethodCallResponse,
                .success = true,
                .jsresult = {.type = kJSNull}
            }
        );

        // invalid config given to setClient
        invalid_config:
            PlatformChannel_freeJSONMsgCodecValue(&jsvalue, false);

            return PlatformChannel_respondError(
                responsehandle,
                kJSONMethodCallResponse,
                "illegalargument",
                "Expected decoded text input configuration to at least contain values for \"autocorrect\""
                " and \"inputAction\"",
                NULL
            );

    } else if STREQ("TextInput.show", object->method) {
        /*
         *  TextInput.show()
         *      Show the keyboard. See [TextInputConnection.show].
         * 
         */

        // do nothing since we use a physical keyboard.
        return PlatformChannel_respond(
            responsehandle,
            &(struct ChannelObject) {
                .codec = kJSONMethodCallResponse,
                .success = true,
                .jsresult = {.type = kJSNull}
            }
        );
    } else if STREQ("TextInput.setEditingState", object->method) {
        /*
         *  TextInput.setEditingState(String textEditingValue)
         *      Update the value in the text editing control. The argument is a
         *      [String] with a JSON-encoded object with seven keys, as
         *      obtained from [TextEditingValue.toJSON].
         *      See [TextInputConnection.setEditingState].
         * 
         */

        if (object->jsarg.type != kJSString) {
            return PlatformChannel_respondError(
                responsehandle,
                kJSONMethodCallResponse,
                "illegalargument",
                "Expected argument to be of type String.",
                NULL
            );
        }

        ok = PlatformChannel_decodeJSON(object->jsarg.string_value, &jsvalue);
        if (ok != 0) return ok;

        if (jsvalue.type != kJSObject) {
            return PlatformChannel_respondError(
                responsehandle,
                kJSONMethodCallResponse,
                "illegalargument",
                "Expected decoded text editing value to be an Object",
                NULL
            );
        }

        char *text;
        int selection_base, selection_extent, composing_base, composing_extent;
        bool selection_affinity_is_downstream, selection_is_directional;

        temp = jsobject_get(&jsvalue, "text");
        if (temp && (temp->type == kJSString))  text = temp->string_value;
        else                                    goto invalid_editing_value;

        temp = jsobject_get(&jsvalue, "selectionBase");
        if (temp && (temp->type == kJSNumber))  selection_base = (int) temp->number_value;
        else                                    goto invalid_editing_value;

        temp = jsobject_get(&jsvalue, "selectionExtent");
        if (temp && (temp->type == kJSNumber))  selection_extent = (int) temp->number_value;
        else                                    goto invalid_editing_value;

        temp = jsobject_get(&jsvalue, "selectionAffinity");
        if (temp && (temp->type == kJSString)) {
            if STREQ("TextAffinity.downstream", temp->string_value) {
                selection_affinity_is_downstream = true;
            } else if STREQ("TextAffinity.upstream", temp->string_value) {
                selection_affinity_is_downstream = false;
            } else {
                goto invalid_editing_value;
            }
        } else {
            goto invalid_editing_value;
        }

        temp = jsobject_get(&jsvalue, "selectionIsDirectional");
        if (temp && (temp->type == kJSTrue || temp->type == kJSFalse)) {
            selection_is_directional = temp->type == kJSTrue;
        } else {
            goto invalid_editing_value;
        }

        temp = jsobject_get(&jsvalue, "composingBase");
        if (temp && (temp->type == kJSNumber))  composing_base = (int) temp->number_value;
        else                                    goto invalid_editing_value;

        temp = jsobject_get(&jsvalue, "composingExtent");
        if (temp && (temp->type == kJSNumber))  composing_extent = (int) temp->number_value;
        else                                    goto invalid_editing_value;


        // text editing value seems to be valid.
        // apply it.
        snprintf(text_input.text, sizeof(text_input.text), "%s", text);
        text_input.selection_base = selection_base;
        text_input.selection_extent = selection_extent;
        text_input.selection_affinity_is_downstream = selection_affinity_is_downstream;
        text_input.selection_is_directional = selection_is_directional;
        text_input.composing_base = composing_base;
        text_input.composing_extent = composing_extent;

        PlatformChannel_freeJSONMsgCodecValue(&jsvalue, false);

        return PlatformChannel_respond(
            responsehandle,
            &(struct ChannelObject) {
                .codec = kJSONMethodCallResponse,
                .success = true,
                .jsresult = {.type = kJSNull}
            }
        );

        invalid_editing_value:
            PlatformChannel_freeJSONMsgCodecValue(&jsvalue, false);

            return PlatformChannel_respondError(
                responsehandle,
                kJSONMethodCallResponse,
                "illegalargument",
                "Expected decoded text editing value to be a valid"
                " JSON representation of a text editing value",
                NULL
            );

    } else if STREQ("TextInput.clearClient", object->method) {
        /* 
         *  TextInput.clearClient()
         *      End the current transaction. The next method called must be
         *      `TextInput.setClient` (or `TextInput.hide`).
         *      See [TextInputConnection.close].
         * 
         */

        text_input.transaction_id = -1;

        return PlatformChannel_respond(
            responsehandle,
            &(struct ChannelObject) {
                .codec = kJSONMethodCallResponse,
                .success = true,
                .jsresult = {.type = kJSNull}
            }
        );
    } else if STREQ("TextInput.hide", object->method) {
        /*
         *  TextInput.hide()
         *      Hide the keyboard. Unlike the other methods, this can be called
         *      at any time. See [TextInputConnection.close].
         * 
         */

        // do nothing since we use a physical keyboard.
        return PlatformChannel_respond(
            responsehandle,
            &(struct ChannelObject) {
                .codec = kJSONMethodCallResponse,
                .success = true,
                .jsresult = {.type = kJSNull}
            }
        );
    }

    return PlatformChannel_respondNotImplemented(responsehandle);
}


void encode_string_json(char *source, char *target) {
    for (; *source; source++, target++) {
        switch (*source) {
            case '\b':
                *(target++) = '\\';
                *target = 'b';
                break;
            case '\f':
                *(target++) = '\\';
                *target = 'f';
                break;
            case '\n':
                *(target++) = '\\';
                *target = 'n';
                break;
            case '\r':
                *(target++) = '\\';
                *target = 'r';
                break;
            case '\t':
                *(target++) = '\\';
                *target = 't';
                break;
            case '\"':
                *(target++) = '\\';
                *target = 't';
                break;
            case '\\':
                *(target++) = '\\';
                *target = '\\';
                break;
            default:
                *target = *source;
                break;
        }
    }

    *target = '\0';
}

int TextInput_syncEditingState() {
    static char encoded_text[TEXT_INPUT_MAX_CHARS*2];

    encode_string_json(text_input.text, encoded_text);
    
    int buffer_size = strlen(encoded_text) + 256;
    char buffer[buffer_size];

    snprintf(buffer, buffer_size,
        "{"
            "text\":\"%s\","
            "\"selectionBase\":%i,"
            "\"selectionExtent\":%i,"
            "\"selectionAffinity\":%s,"
            "\"selectionIsDirectional\":%s,"
            "\"composingBase\":%d,"
            "\"composingExtent\":%d"
        "}",
        text_input.text,
        text_input.selection_base,
        text_input.selection_extent,
        text_input.selection_affinity_is_downstream ? "TextAffinity.downstream" : "TextAffinity.upstream",
        text_input.selection_is_directional ? "true" : "false",
        text_input.composing_base,
        text_input.composing_extent
    );

    return PlatformChannel_send(
        TEXT_INPUT_CHANNEL,
        &(struct ChannelObject) {
            .codec = kJSONMethodCall,
            .method = "TextInputClient.updateEditingState",
            .jsarg = {
                .type = kJSArray,
                .size = 2,
                .values = (struct JSONMsgCodecValue[2]) {
                    {.type = kJSNumber, .number_value = text_input.transaction_id},
                    {.type = kJSString, .string_value = buffer}
                }
            }
        },
        0, NULL, NULL
    );
}

int TextInput_performAction(enum text_input_action action) {

    char *action_str =
        (action == kTextInputActionNone) ?          "TextInputAction.none" :
        (action == kTextInputActionUnspecified) ?   "TextInputAction.unspecified" :
        (action == kTextInputActionDone) ?          "TextInputAction.done" :
        (action == kTextInputActionGo) ?            "TextInputAction.go" :
        (action == kTextInputActionSearch) ?        "TextInputAction.search" :
        (action == kTextInputActionSend) ?          "TextInputAction.send" :
        (action == kTextInputActionNext) ?          "TextInputAction.next" :
        (action == kTextInputActionPrevious) ?      "TextInputAction.previous" :
        (action == kTextInputActionContinueAction) ? "TextInputAction.continueAction" :
        (action == kTextInputActionJoin) ?          "TextInputAction.join" :
        (action == kTextInputActionRoute) ?         "TextInputAction.route" :
        (action == kTextInputActionEmergencyCall) ? "TextInputAction.emergencyCall" :
        "TextInputAction.newline";

    return PlatformChannel_send(
        TEXT_INPUT_CHANNEL,
        &(struct ChannelObject) {
            .codec = kJSONMethodCall,
            .method = "TextInputClient.performAction",
            .jsarg = {
                .type = kJSArray,
                .size = 2,
                .values = (struct JSONMsgCodecValue[2]) {
                    {.type = kJSNumber, .number_value = text_input.transaction_id},
                    {.type = kJSString, .string_value = action_str}
                }
            }
        },
        0, NULL, NULL
    );
}

int TextInput_onConnectionClosed(void) {
    text_input.transaction_id = -1;

    return PlatformChannel_send(
        TEXT_INPUT_CHANNEL,
        &(struct ChannelObject) {
            .codec = kJSONMethodCall,
            .method = "TextInputClient.onConnectionClosed",
            .jsarg = {.type = kJSNull}
        },
        kBinaryCodec, NULL, NULL
    );
}


/// start and end index are both inclusive.
int TextInput_erase(unsigned int start, unsigned int end) {
    // 0 <= start <= end < len

    size_t len = strlen(text_input.text);
    if (!len) return 0;

    memmove(&text_input.text[start], text_input.text[end + 1], len - end);

    return start;
}
bool TextInput_deleteSelected(void) {
    // erase selected text
    text_input.selection_base = TextInput_erase(text_input.selection_base, text_input.selection_extent-1);
    text_input.selection_extent = text_input.selection_base;
    return true;
}
bool TextInput_addChar(char c) {
    if (text_input.transaction_id == -1) return 1;

    if (text_input.selection_base != text_input.selection_extent) {
        TextInput_deleteSelected();
    }

    int base = text_input.selection_base;
    memmove(&text_input.text[base + 1], &text_input.text[base], strlen(text_input.text) - base + 1);
    text_input.text[base] = c;

    text_input.selection_extent++;
    text_input.selection_base = text_input.selection_extent;

    return true;
}
bool TextInput_backspace(void) {
    if (text_input.selection_base != text_input.selection_extent)
        return TextInput_deleteSelected();
    
    if (text_input.selection_base != 0) {
        int base = text_input.selection_base - 1;
        text_input.selection_base = TextInput_erase(base, base);
        text_input.selection_extent = text_input.selection_base;
        return true;
    }

    return false;
}
bool TextInput_delete(void) {
    if (text_input.selection_base != text_input.selection_extent)
        return TextInput_deleteSelected();
    
    if (text_input.selection_base < (strlen(text_input.text) - 1)) {
        text_input.selection_base = TextInput_erase(text_input.selection_base, text_input.selection_base);
        text_input.selection_extent = text_input.selection_base;
        return true;
    }

    return false;
}
bool TextInput_moveCursorToBeginning(void) {
    if ((text_input.selection_base != 0) || (text_input.selection_extent != 0)) {
        text_input.selection_base = 0;
        text_input.selection_extent = 0;
        return true;
    }

    return false;
}
bool TextInput_moveCursorToEnd(void) {
    int end = strlen(text_input.text) - 1;

    if (text_input.selection_base != end) {
        text_input.selection_base = end;
        text_input.selection_extent = end;
        return true;
    }

    return false;
}
bool TextInput_moveCursorForward(void) {
    if (text_input.selection_base != text_input.selection_extent) {
        text_input.selection_base = text_input.selection_extent;
        return true;
    }

    if ((text_input.selection_extent + 1) < strlen(text_input.text)) {
        text_input.selection_extent++;
        text_input.selection_base++;
        return true;
    }

    return false;
}
bool TextInput_moveCursorBack(void) {
    if (text_input.selection_base != text_input.selection_extent) {
        text_input.selection_extent = text_input.selection_base;
        return true; 
    }

    if (text_input.selection_base > 0) {
        text_input.selection_base--;
        text_input.selection_extent--;
        return true;
    }

    return false;
}

// parses the input string as linux terminal input and calls the TextInput model functions
// accordingly. Also automatically syncs state with flutter side.
// only parses a single console code / character
int TextInput_onTerminalInput(char *input) {
    char *control_sequence = NULL;

    // do nothing if there's no model to edit.
    if (text_input.transaction_id == -1) return 0;

    // first check control characters
    // linux console control characters: BEL 0x07, BS 0x08, HT 0x09, LF 0x0A, VT, FF, CR 0x0D,
    //                                   SO 0x0E, SI 0x0F, CAN 0x18, SUB 0x1A, ESC 0x1B,
    //                                   DEL 0x7F, CSI 0x9B

    // handled here: backspace, enter
    switch (*input) {
        case 0x07:  // BEL, beep, not implemented
        case 0x08:  // BS, backspace, but seems to be unused
        case 0x09:  // HT, tab, not implemented
        case 0x0A:  // LF, line-feed, not implemented
        case 0x0E:  // SO, activates G1 charset, not implemented
        case 0x0F:  // SI, activates G0 charset, not implemented
        case 0x18:  // CAN and
        case 0x1A:  // SUB, interrupt escape sequences
            break;

        case 0x0D:  // CR, carriage-return (enter key was pressed)
            // if the text-input type is multiline, should add a new line.
            return TextInput_performAction(text_input.config.input_action);

        case 0x7F:  // DEL, backspace
            if (TextInput_backspace())
                return TextInput_syncEditingState();

        case 0x1B:  // ESC
            if (input[1] == '[') {
                // we found a control sequence introducer.
                control_sequence = &input[2];
            }
            break;

        case 0x9B:
            control_sequence = &input[1];
            break;

        default:
            if (isprint(*input))
                TextInput_addChar(*input);
            break;
    }

    if (!control_sequence) {
        return 0;
    }

    // control sequence introducer found.
    // handled here: left, right, end, home, delete

    char *action;
}


int TextInput_init(void) {
    text_input.transaction_id = -1;

    PluginRegistry_setReceiver(TEXT_INPUT_CHANNEL, kJSONMethodCall, TextInput_onReceive);

    printf("[text_input] init.\n");
}

int TextInput_deinit(void) {
    printf("[text_input] deinit.\n");
}