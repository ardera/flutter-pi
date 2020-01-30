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
    enum text_input_type input_type;
    bool autocorrect;
    enum text_input_action input_action;
    char text[TEXT_INPUT_MAX_CHARS];
    int  selection_base, selection_extent;
    bool selection_affinity_is_downstream;
    bool selection_is_directional;
    int  composing_base, composing_extent;
    bool warned_about_autocorrect;
} text_input = {
    .transaction_id = -1
};

int TextInput_onReceive(char *channel, struct ChannelObject *object, FlutterPlatformMessageResponseHandle *responsehandle) {
    struct JSONMsgCodecValue jsvalue, *temp, *temp2;
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

        enum text_input_type input_type;
        bool autocorrect;
        enum text_input_action input_action;

        // AUTOCORRECT
        temp = jsobject_get(&jsvalue, "autocorrect");
        if (!(temp && ((temp->type == kJSTrue) || (temp->type == kJSFalse))))
            goto invalid_config;

        autocorrect = temp->type == kJSTrue;
        
        // INPUT ACTION
        temp = jsobject_get(&jsvalue, "inputAction");
        if (!(temp && (temp->type == kJSString)))
            goto invalid_config;
        
        if STREQ("TextInputAction.none", temp->string_value)
            input_action = kTextInputActionNone;
        else if STREQ("TextInputAction.unspecified", temp->string_value)
            input_action = kTextInputActionUnspecified;
        else if STREQ("TextInputAction.done", temp->string_value)
            input_action = kTextInputActionDone;
        else if STREQ("TextInputAction.go", temp->string_value)
            input_action = kTextInputActionGo;
        else if STREQ("TextInputAction.search", temp->string_value)
            input_action = kTextInputActionSearch;
        else if STREQ("TextInputAction.send", temp->string_value)
            input_action = kTextInputActionSend;
        else if STREQ("TextInputAction.next", temp->string_value)
            input_action = kTextInputActionNext;
        else if STREQ("TextInputAction.previous", temp->string_value)
            input_action = kTextInputActionPrevious;
        else if STREQ("TextInputAction.continueAction", temp->string_value)
            input_action = kTextInputActionContinueAction;
        else if STREQ("TextInputAction.join", temp->string_value)
            input_action = kTextInputActionJoin;
        else if STREQ("TextInputAction.route", temp->string_value)
            input_action = kTextInputActionRoute;
        else if STREQ("TextInputAction.emergencyCall", temp->string_value)
            input_action = kTextInputActionEmergencyCall;
        else if STREQ("TextInputAction.newline", temp->string_value)
            input_action = kTextInputActionNewline;
        else
            goto invalid_config;


        // INPUT TYPE
        temp = jsobject_get(&jsvalue, "inputType");

        if (!temp || temp->type != kJSObject)
            goto invalid_config;


        temp2 = jsobject_get(temp, "name");

        if (!temp2 || temp2->type != kJSString)
            goto invalid_config;

        if STREQ("TextInputType.text", temp2->string_value) {
            input_type = kInputTypeText;
        } else if STREQ("TextINputType.multiline", temp2->string_value) {
            input_type = kInputTypeMultiline;
        } else if STREQ("TextInputType.number", temp2->string_value) {
            input_type = kInputTypeNumber;
        } else if STREQ("TextInputType.phone", temp2->string_value) {
            input_type = kInputTypePhone;
        } else if STREQ("TextInputType.datetime", temp2->string_value) {
            input_type = kInputTypeDatetime;
        } else if STREQ("TextInputType.emailAddress", temp2->string_value) {
            input_type = kInputTypeEmailAddress;
        } else if STREQ("TextInputType.url", temp2->string_value) {
            input_type = kInputTypeUrl;
        } else if STREQ("TextInputType.visiblePassword", temp2->string_value) {
            input_type = kInputTypeVisiblePassword;
        } else {
            goto invalid_config;
        }

        // TRANSACTION ID
        int32_t new_id = (int32_t) object->jsarg.values[0].number_value;

        // everything okay, apply the new text editing config
        text_input.transaction_id = (int32_t) object->jsarg.values[0].number_value;
        text_input.autocorrect = autocorrect;
        text_input.input_action = input_action;
        text_input.input_type = input_type;

        if (autocorrect && (!text_input.warned_about_autocorrect)) {
            printf("[text_input] warning: flutter requested native autocorrect, which",
                   "is not supported by flutter-pi.\n");
            text_input.warned_about_autocorrect = true;
        }

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


// start and end index are both inclusive.
int  TextInput_erase(unsigned int start, unsigned int end) {
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


// these two functions automatically sync the editing state with flutter if
// a change ocurred, so you don't explicitly need to call TextInput_syncEditingState().
int TextInput_onChar(char c) {
    if (text_input.transaction_id == -1)
        return 0;

    if (TextInput_addChar(c))
        return TextInput_syncEditingState();

    return 0;
}

int TextInput_onKey(glfw_key key) {
    bool needs_sync = false;
    bool perform_action = false;
    int ok;

    if (text_input.transaction_id == -1)
        return 0;

    switch (key) {
        case GLFW_KEY_LEFT:
            needs_sync = TextInput_moveCursorBack();
            break;
        case GLFW_KEY_RIGHT:
            needs_sync = TextInput_moveCursorForward();
            break;
        case GLFW_KEY_END:
            needs_sync = TextInput_moveCursorToEnd();
            break;
        case GLFW_KEY_HOME:
            needs_sync = TextInput_moveCursorToBeginning();
            break;
        case GLFW_KEY_BACKSPACE:
            needs_sync = TextInput_backspace();
            break;
        case GLFW_KEY_DELETE:
            needs_sync = TextInput_delete();
            break;
        case GLFW_KEY_ENTER:
            if (text_input.input_type == kInputTypeMultiline)
                needs_sync = TextInput_addChar('\n');
            
            perform_action = true;
            break;
        default:
            break;
    }

    if (needs_sync) {
        ok = TextInput_syncEditingState();
        if (ok != 0) return ok;
    }

    if (perform_action) {
        ok = TextInput_performAction(text_input.input_action);
        if (ok != 0) return ok;
    }

    return 0;
}


int TextInput_init(void) {
    text_input.text[0] = '\0';
    text_input.warned_about_autocorrect = false;

    PluginRegistry_setReceiver(TEXT_INPUT_CHANNEL, kJSONMethodCall, TextInput_onReceive);

    printf("[text_input] init.\n");
}

int TextInput_deinit(void) {
    printf("[text_input] deinit.\n");
}