#include <ctype.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <flutter-pi.h>
#include <pluginregistry.h>
#include <plugins/text_input.h>

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

int textin_on_receive(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *responsehandle) {
    struct json_value jsvalue, *temp, *temp2, *state, *config;
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
        
        if ((object->json_arg.type != kJsonArray) || (object->json_arg.size != 2)) {
            return platch_respond_illegal_arg_json(
                responsehandle,
                "Expected `arg` to be an array with length 2."
            );
        }

        if (object->json_arg.array[0].type != kJsonNumber) {
            return platch_respond_illegal_arg_json(
                responsehandle,
                "Expected transaction id to be a number."
            );
        }

        if (object->json_arg.array[1].type != kJsonObject) {
            return platch_respond_illegal_arg_json(
                responsehandle,
                "Expected text input configuration to be a String"
            );
        }

        struct json_value *config = &object->json_arg.array[1];

        if (config->type != kJsonObject) {
            return platch_respond_illegal_arg_json(
                responsehandle,
                "Expected decoded text input configuration to be an Object"
            );
        }

        enum text_input_type input_type;
        bool autocorrect;
        enum text_input_action input_action;

        // AUTOCORRECT
        temp = jsobject_get(config, "autocorrect");
        if (!(temp && ((temp->type == kJsonTrue) || (temp->type == kJsonFalse))))
            goto invalid_config;

        autocorrect = temp->type == kJsonTrue;
        
        // INPUT ACTION
        temp = jsobject_get(config, "inputAction");
        if (!(temp && (temp->type == kJsonString)))
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
        temp = jsobject_get(config, "inputType");

        if (!temp || temp->type != kJsonObject)
            goto invalid_config;


        temp2 = jsobject_get(temp, "name");

        if (!temp2 || temp2->type != kJsonString)
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
        int32_t new_id = (int32_t) object->json_arg.array[0].number_value;

        // everything okay, apply the new text editing config
        text_input.transaction_id = new_id;
        text_input.autocorrect = autocorrect;
        text_input.input_action = input_action;
        text_input.input_type = input_type;

        if (autocorrect && (!text_input.warned_about_autocorrect)) {
            printf("[text_input] warning: flutter requested native autocorrect, which",
                   "is not supported by flutter-pi.\n");
            text_input.warned_about_autocorrect = true;
        }

        return platch_respond(
            responsehandle,
            &(struct platch_obj) {
                .codec = kJSONMethodCallResponse,
                .success = true,
                .json_result = {.type = kJsonNull}
            }
        );

        // invalid config given to setClient
        invalid_config:
            return platch_respond_illegal_arg_json(
                responsehandle,
                "Expected decoded text input configuration to at least contain values for \"autocorrect\""
                " and \"inputAction\""
            );

    } else if STREQ("TextInput.show", object->method) {
        /*
         *  TextInput.show()
         *      Show the keyboard. See [TextInputConnection.show].
         * 
         */

        // do nothing since we use a physical keyboard.
        return platch_respond(
            responsehandle,
            &(struct platch_obj) {
                .codec = kJSONMethodCallResponse,
                .success = true,
                .json_result = {.type = kJsonNull}
            }
        );
    } else if STREQ("TextInput.setEditingState", object->method) {
        /*
         *  TextInput.setEditingState(Map<String, dynamic> textEditingValue)
         *      Update the value in the text editing control. The argument is a
         *      [String] with a JSON-encoded object with seven keys, as
         *      obtained from [TextEditingValue.toJSON].
         *      See [TextInputConnection.setEditingState].
         * 
         */

        state = &object->json_arg;

        if (state->type != kJsonObject) {
            return platch_respond_illegal_arg_json(
                responsehandle,
                "Expected decoded text editing value to be an Object"
            );
        }

        char *text;
        int selection_base, selection_extent, composing_base, composing_extent;
        bool selection_affinity_is_downstream, selection_is_directional;

        temp = jsobject_get(state, "text");
        if (temp && (temp->type == kJsonString))  text = temp->string_value;
        else                                    goto invalid_editing_value;

        temp = jsobject_get(state, "selectionBase");
        if (temp && (temp->type == kJsonNumber))  selection_base = (int) temp->number_value;
        else                                    goto invalid_editing_value;

        temp = jsobject_get(state, "selectionExtent");
        if (temp && (temp->type == kJsonNumber))  selection_extent = (int) temp->number_value;
        else                                    goto invalid_editing_value;

        temp = jsobject_get(state, "selectionAffinity");
        if (temp && (temp->type == kJsonString)) {
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

        temp = jsobject_get(state, "selectionIsDirectional");
        if (temp && (temp->type == kJsonTrue || temp->type == kJsonFalse)) {
            selection_is_directional = temp->type == kJsonTrue;
        } else {
            goto invalid_editing_value;
        }

        temp = jsobject_get(state, "composingBase");
        if (temp && (temp->type == kJsonNumber))  composing_base = (int) temp->number_value;
        else                                    goto invalid_editing_value;

        temp = jsobject_get(state, "composingExtent");
        if (temp && (temp->type == kJsonNumber))  composing_extent = (int) temp->number_value;
        else                                    goto invalid_editing_value;


        snprintf(text_input.text, sizeof(text_input.text), "%s", text);
        text_input.selection_base = selection_base;
        text_input.selection_extent = selection_extent;
        text_input.selection_affinity_is_downstream = selection_affinity_is_downstream;
        text_input.selection_is_directional = selection_is_directional;
        text_input.composing_base = composing_base;
        text_input.composing_extent = composing_extent;

        return platch_respond(
            responsehandle,
            &(struct platch_obj) {
                .codec = kJSONMethodCallResponse,
                .success = true,
                .json_result = {.type = kJsonNull}
            }
        );

        invalid_editing_value:
            return platch_respond_illegal_arg_json(
                responsehandle,
                "Expected decoded text editing value to be a valid"
                " JSON representation of a text editing value"
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

        return platch_respond(
            responsehandle,
            &(struct platch_obj) {
                .codec = kJSONMethodCallResponse,
                .success = true,
                .json_result = {.type = kJsonNull}
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
        return platch_respond(
            responsehandle,
            &(struct platch_obj) {
                .codec = kJSONMethodCallResponse,
                .success = true,
                .json_result = {.type = kJsonNull}
            }
        );
    }

    return platch_respond_not_implemented(responsehandle);
}


int textin_sync_editing_state() {
    return platch_send(
        TEXT_INPUT_CHANNEL,
        &(struct platch_obj) {
            .codec = kJSONMethodCall,
            .method = "TextInputClient.updateEditingState",
            .json_arg = {
                .type = kJsonArray,
                .size = 2,
                .array = (struct json_value[2]) {
                    {.type = kJsonNumber, .number_value = text_input.transaction_id},
                    {.type = kJsonObject, .size = 7,
                        .keys = (char*[7]) {
                            "text", "selectionBase", "selectionExtent", "selectionAffinity",
                            "selectionIsDirectional", "composingBase", "composingExtent"
                        },
                        .values = (struct json_value[7]) {
                            {.type = kJsonString, .string_value = text_input.text},
                            {.type = kJsonNumber, .number_value = text_input.selection_base},
                            {.type = kJsonNumber, .number_value = text_input.selection_extent},
                            {
                                .type = kJsonString,
                                .string_value = text_input.selection_affinity_is_downstream ?
                                    "TextAffinity.downstream" : "TextAffinity.upstream"
                            },
                            {.type = text_input.selection_is_directional? kJsonTrue : kJsonFalse},
                            {.type = kJsonNumber, .number_value = text_input.composing_base},
                            {.type = kJsonNumber, .number_value = text_input.composing_extent}
                        }
                    }
                }
            }
        },
        kJSONMethodCallResponse,
        NULL,
        NULL
    );
}

int textin_perform_action(enum text_input_action action) {

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

    return platch_send(
        TEXT_INPUT_CHANNEL,
        &(struct platch_obj) {
            .codec = kJSONMethodCall,
            .method = "TextInputClient.performAction",
            .json_arg = {
                .type = kJsonArray,
                .size = 2,
                .array = (struct json_value[2]) {
                    {.type = kJsonNumber, .number_value = text_input.transaction_id},
                    {.type = kJsonString, .string_value = action_str}
                }
            }
        },
        0, NULL, NULL
    );
}

int textin_on_connection_closed(void) {
    text_input.transaction_id = -1;

    return platch_send(
        TEXT_INPUT_CHANNEL,
        &(struct platch_obj) {
            .codec = kJSONMethodCall,
            .method = "TextInputClient.onConnectionClosed",
            .json_arg = {.type = kJsonNull}
        },
        kBinaryCodec, NULL, NULL
    );
}

inline int to_byte_index(unsigned int symbol_index) {
    char *cursor = text_input.text;

    while ((*cursor) && (symbol_index--))
        cursor += utf8_symbol_length(cursor);

    if (*cursor)
        return cursor - text_input.text;

    return -1;
}

// start and end index are both inclusive.
int  textin_erase(unsigned int start, unsigned int end) {
    // 0 <= start <= end < len

    char *start_str     = utf8_symbol_at(text_input.text, start);
    char *after_end_str = utf8_symbol_at(text_input.text, end+1);

    if (start_str && after_end_str)
        memmove(start_str, after_end_str, strlen(after_end_str) + 1 /* null byte */);

    return start;
}
bool textin_delete_selected(void) {
    // erase selected text
    text_input.selection_base = textin_erase(text_input.selection_base, text_input.selection_extent-1);
    text_input.selection_extent = text_input.selection_base;
    return true;
}
bool textin_add_utf8_char(char *c) {
    size_t symbol_length;
    char  *to_move;

    if (text_input.selection_base != text_input.selection_extent)
        textin_delete_selected();

    // find out where in our string we need to insert the utf8 symbol

    symbol_length = utf8_symbol_length(c);
    to_move       = utf8_symbol_at(text_input.text, text_input.selection_base);

    if (!to_move || !symbol_length)
        return false;

    // move the string behind the insertion position to
    // make place for the utf8 character

    memmove(to_move + symbol_length, to_move, strlen(to_move) + 1 /* null byte */);

    // after the move, to_move points to the memory
    // where c should be inserted
    for (int i = 0; i < symbol_length; i++)
        to_move[i] = c[i];

    // move our selection to behind the inserted char
    text_input.selection_extent++;
    text_input.selection_base = text_input.selection_extent;

    return true;
}
bool textin_backspace(void) {
    if (text_input.selection_base != text_input.selection_extent)
        return textin_delete_selected();
    
    if (text_input.selection_base != 0) {
        int base = text_input.selection_base - 1;
        text_input.selection_base = textin_erase(base, base);
        text_input.selection_extent = text_input.selection_base;
        return true;
    }

    return false;
}
bool textin_delete(void) {
    if (text_input.selection_base != text_input.selection_extent)
        return textin_delete_selected();
    
    if (text_input.selection_base < strlen(text_input.text)) {
        text_input.selection_base = textin_erase(text_input.selection_base, text_input.selection_base);
        text_input.selection_extent = text_input.selection_base;
        return true;
    }

    return false;
}
bool textin_move_cursor_to_beginning(void) {
    if ((text_input.selection_base != 0) || (text_input.selection_extent != 0)) {
        text_input.selection_base = 0;
        text_input.selection_extent = 0;
        return true;
    }

    return false;
}
bool textin_move_cursor_to_end(void) {
    int end = strlen(text_input.text);

    if (text_input.selection_base != end) {
        text_input.selection_base = end;
        text_input.selection_extent = end;
        return true;
    }

    return false;
}
bool textin_move_cursor_forward(void) {
    if (text_input.selection_base != text_input.selection_extent) {
        text_input.selection_base = text_input.selection_extent;
        return true;
    }

    if (text_input.selection_extent < strlen(text_input.text)) {
        text_input.selection_extent++;
        text_input.selection_base++;
        return true;
    }

    return false;
}
bool textin_move_cursor_back(void) {
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
// a change ocurred, so you don't explicitly need to call textin_sync_editing_state().
// `c` doesn't need to be NULL-terminated, the length of the char will be calculated
// using the start byte.
int textin_on_utf8_char(char *c) {
    if (text_input.transaction_id == -1)
        return 0;

    if (textin_add_utf8_char(c))
        return textin_sync_editing_state();

    return 0;
}

int textin_on_key(glfw_key key) {
    bool needs_sync = false;
    bool perform_action = false;
    int ok;

    if (text_input.transaction_id == -1)
        return 0;

    switch (key) {
        case GLFW_KEY_LEFT:
            needs_sync = textin_move_cursor_back();
            break;
        case GLFW_KEY_RIGHT:
            needs_sync = textin_move_cursor_forward();
            break;
        case GLFW_KEY_END:
            needs_sync = textin_move_cursor_to_end();
            break;
        case GLFW_KEY_HOME:
            needs_sync = textin_move_cursor_to_beginning();
            break;
        case GLFW_KEY_BACKSPACE:
            needs_sync = textin_backspace();
            break;
        case GLFW_KEY_DELETE:
            needs_sync = textin_delete();
            break;
        case GLFW_KEY_ENTER:
            if (text_input.input_type == kInputTypeMultiline)
                needs_sync = textin_add_utf8_char("\n");
            
            perform_action = true;
            break;
        default:
            break;
    }

    if (needs_sync) {
        ok = textin_sync_editing_state();
        if (ok != 0) return ok;
    }

    if (perform_action) {
        ok = textin_perform_action(text_input.input_action);
        if (ok != 0) return ok;
    }

    return 0;
}


int textin_init(void) {
    int ok;

    printf("[test_input] Initializing...\n");

    text_input.text[0] = '\0';
    text_input.warned_about_autocorrect = false;

    ok = plugin_registry_set_receiver(TEXT_INPUT_CHANNEL, kJSONMethodCall, textin_on_receive);
    if (ok != 0) return ok;

    printf("[text_input] Done.\n");

    return 0;
}

int textin_deinit(void) {
    printf("[text_input] deinit.\n");

    return 0;
}