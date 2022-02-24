#include <ctype.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <flutter-pi.h>
#include <pluginregistry.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <plugins/text_input.h>

struct text_input {
    int64_t connection_id;
    enum text_input_type input_type;
    bool allow_signs;
    bool has_allow_signs;
    bool allow_decimal;
    bool has_allow_decimal;
    bool autocorrect;
    enum text_input_action input_action;
    char text[TEXT_INPUT_MAX_CHARS];
    int  selection_base, selection_extent;
    bool selection_affinity_is_downstream;
    bool selection_is_directional;
    int  composing_base, composing_extent;
    bool warned_about_autocorrect;
} text_input = {
    .connection_id = -1
};

/**
 * UTF8 utility functions
 */
static inline uint8_t utf8_symbol_length(uint8_t c) {
    if ((c & 0b11110000) == 0b11110000) {
        return 4;
    }
    if ((c & 0b11100000) == 0b11100000) {
        return 3;
    }
    if ((c & 0b11000000) == 0b11000000) {
        return 2;
    }
    if ((c & 0b10000000) == 0b10000000) {
        // XXX should we return 1 and don't care here?
        DEBUG_ASSERT_MSG(false, "Invalid UTF-8 character");
        return 0;
    }
    return 1;
}

static inline uint8_t *symbol_at(unsigned int symbol_index) {
    uint8_t *cursor = (uint8_t*) text_input.text;

    for (; symbol_index && *cursor; symbol_index--)
        cursor += utf8_symbol_length(*cursor);

    return symbol_index? NULL : cursor;
}

static inline int to_byte_index(unsigned int symbol_index) {
    char *cursor = text_input.text;

    while ((*cursor) && (symbol_index--))
        cursor += utf8_symbol_length(*cursor);

    if (*cursor)
        return cursor - text_input.text;

    return -1;
}

static inline int to_symbol_index(unsigned int byte_index) {
    char *cursor = text_input.text;
    char *target_cursor = cursor + byte_index;
    int symbol_index = 0;

    while ((*cursor) && (cursor < target_cursor)) {
        cursor += utf8_symbol_length(*cursor);
        symbol_index++;
    }

    return cursor < target_cursor? -1 : symbol_index;
}

/**
 * Platform message callbacks
 */
static int on_set_client(
    struct platch_obj *object,
    FlutterPlatformMessageResponseHandle *responsehandle
) {
    enum text_input_action input_action;
    enum text_input_type input_type;
    struct json_value *temp, *temp2, *config;
    bool autocorrect, allow_signs, allow_decimal, has_allow_signs, has_allow_decimal;

    (void) allow_signs;
    (void) allow_decimal;
    (void) has_allow_signs;
    (void) has_allow_decimal;

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
            "Expected `arg[0]` to be a number"
        );
    }

    if (object->json_arg.array[1].type != kJsonObject) {
        return platch_respond_illegal_arg_json(
            responsehandle,
            "Expected `arg[1]` to be an map."
        );
    }

    config = &object->json_arg.array[1];

    // AUTOCORRECT
    temp = jsobject_get(config, "autocorrect");
    if (temp == NULL || (temp->type != kJsonTrue && temp->type != kJsonFalse)) {
        return platch_respond_illegal_arg_json(
            responsehandle,
            "Expected `arg[1]['autocorrect']` to be a boolean."
        );
    } else {
        autocorrect = temp->type == kJsonTrue;
    }
    
    // INPUT ACTION
    temp = jsobject_get(config, "inputAction");
    if (temp == NULL || temp->type != kJsonString) {
        return platch_respond_illegal_arg_json(
            responsehandle,
            "Expected `arg[1]['inputAction']` to be a string-ification of `TextInputAction`."
        );
    }
    
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
        return platch_respond_illegal_arg_json(
            responsehandle,
            "Expected `arg[1]['inputAction']` to be a string-ification of `TextInputAction`."
        );

    // INPUT TYPE
    temp = jsobject_get(config, "inputType");
    if (temp == NULL || temp->type != kJsonObject) {
        return platch_respond_illegal_arg_json(
            responsehandle,
            "Expected `arg[1]['inputType']` to be a map."
        );
    }

    temp2 = jsobject_get(temp, "signed");
    if (temp2 == NULL || temp2->type == kJsonNull) {
        has_allow_signs = false;
    } else if (temp2->type == kJsonTrue || temp2->type == kJsonFalse) {
        has_allow_signs = true;
        allow_signs = temp2->type == kJsonTrue;
    } else {
        return platch_respond_illegal_arg_json(
            responsehandle,
            "Expected `arg[1]['inputType']['signed']` to be a boolean or null."
        );
    }

    temp2 = jsobject_get(temp, "decimal");
    if (temp2 == NULL || temp2->type == kJsonNull) {
        has_allow_decimal = false;
    } else if (temp2->type == kJsonTrue || temp2->type == kJsonFalse) {
        has_allow_decimal = true;
        allow_decimal = temp2->type == kJsonTrue;
    } else {
        return platch_respond_illegal_arg_json(
            responsehandle,
            "Expected `arg[1]['inputType']['decimal']` to be a boolean or null."
        );
    }

    temp2 = jsobject_get(temp, "name");
    if (temp2 == NULL || temp2->type != kJsonString) {
        return platch_respond_illegal_arg_json(
            responsehandle,
            "Expected `arg[1]['inputType']['name']` to be a string-ification of `TextInputType`."
        );
    }

    if STREQ("TextInputType.text", temp2->string_value) {
        input_type = kInputTypeText;
    } else if STREQ("TextInputType.multiline", temp2->string_value) {
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
    } else if STREQ("TextInputType.name", temp2->string_value) {
        input_type = kInputTypeName;
    } else if STREQ("TextInputType.address", temp2->string_value) {
        input_type = kInputTypeAddress;
    } else {
        return platch_respond_illegal_arg_json(
            responsehandle,
            "Expected `arg[1]['inputType']['name']` to be a string-ification of `TextInputType`."
        );
    }

    // TRANSACTION ID
    int32_t new_id = (int32_t) object->json_arg.array[0].number_value;

    // everything okay, apply the new text editing config
    text_input.connection_id = new_id;
    text_input.autocorrect = autocorrect;
    text_input.input_action = input_action;
    text_input.input_type = input_type;

    if (autocorrect && !text_input.warned_about_autocorrect) {
        printf("[text_input] warning: flutter requested native autocorrect, which"
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
}

static int on_hide(
    struct platch_obj *object,
    FlutterPlatformMessageResponseHandle *responsehandle
) {
    /*
     *  TextInput.hide()
     *      Hide the keyboard. Unlike the other methods, this can be called
     *      at any time. See [TextInputConnection.close].
     * 
     */

    (void) object;

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

static int on_clear_client(
    struct platch_obj *object,
    FlutterPlatformMessageResponseHandle *responsehandle
) {
    /* 
     *  TextInput.clearClient()
     *      End the current transaction. The next method called must be
     *      `TextInput.setClient` (or `TextInput.hide`).
     *      See [TextInputConnection.close].
     * 
     */

    (void) object;

    text_input.connection_id = -1;

    return platch_respond(
        responsehandle,
        &(struct platch_obj) {
            .codec = kJSONMethodCallResponse,
            .success = true,
            .json_result = {.type = kJsonNull}
        }
    );
}

static int on_set_editing_state(
    struct platch_obj *object,
    FlutterPlatformMessageResponseHandle *responsehandle
) {
    struct json_value *temp, *state;
    char *text;
    bool selection_affinity_is_downstream, selection_is_directional;
    int selection_base, selection_extent, composing_base, composing_extent;

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
            "Expected `arg` to be a map."
        );
    }

    temp = jsobject_get(state, "text");
    if (temp == NULL || temp->type != kJsonString) {
        return platch_respond_illegal_arg_json(
            responsehandle,
            "Expected `arg['text']` to be a string."
        );
    } else {
        text = temp->string_value;
    }

    temp = jsobject_get(state, "selectionBase");
    if (temp == NULL || temp->type != kJsonNumber) {
        return platch_respond_illegal_arg_json(
            responsehandle,
            "Expected `arg['selectionBase']` to be a number."
        );
    } else {
        selection_base = (int) temp->number_value;
    }

    temp = jsobject_get(state, "selectionExtent");
    if (temp == NULL || temp->type != kJsonNumber) {
        return platch_respond_illegal_arg_json(
            responsehandle,
            "Expected `arg['selectionExtent']` to be a number."
        );
    } else {
        selection_extent = (int) temp->number_value;
    }

    temp = jsobject_get(state, "selectionAffinity");
    if (temp == NULL || temp->type != kJsonString) {
        return platch_respond_illegal_arg_json(
            responsehandle,
            "Expected `arg['selectionAffinity']` to be a string-ification of `TextAffinity`."
        );
    } else {
        if STREQ("TextAffinity.downstream", temp->string_value) {
            selection_affinity_is_downstream = true;
        } else if STREQ("TextAffinity.upstream", temp->string_value) {
            selection_affinity_is_downstream = false;
        } else {
            return platch_respond_illegal_arg_json(
                responsehandle,
                "Expected `arg['selectionAffinity']` to be a string-ification of `TextAffinity`."
            );
        }
    }

    temp = jsobject_get(state, "selectionIsDirectional");
    if (temp == NULL || (temp->type != kJsonTrue && temp->type != kJsonFalse)) {
        return platch_respond_illegal_arg_json(
            responsehandle,
            "Expected `arg['selectionIsDirectional']` to be a bool."
        );
    } else {
        selection_is_directional = temp->type == kJsonTrue;
    }

    temp = jsobject_get(state, "composingBase");
    if (temp == NULL || temp->type != kJsonNumber) {
        return platch_respond_illegal_arg_json(
            responsehandle,
            "Expected `arg['composingBase']` to be a number."
        );
    } else {
        composing_base = (int) temp->number_value;
    }

    temp = jsobject_get(state, "composingExtent");
    if (temp == NULL || temp->type != kJsonNumber) {
        return platch_respond_illegal_arg_json(
            responsehandle,
            "Expected `arg['composingExtent']` to be a number."
        );
    } else {
        composing_extent = (int) temp->number_value;
    }

    strncpy(text_input.text, text, TEXT_INPUT_MAX_CHARS - 1);
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
}

static int on_show(
    struct platch_obj *object,
    FlutterPlatformMessageResponseHandle *responsehandle
) {
    /*
     *  TextInput.show()
     *      Show the keyboard. See [TextInputConnection.show].
     * 
     */

    (void) object;

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

static int on_request_autofill(
    struct platch_obj *object,
    FlutterPlatformMessageResponseHandle *responsehandle
) {
    (void) object;
    return platch_respond(
        responsehandle,
        &(struct platch_obj) {
            .codec = kJSONMethodCallResponse,
            .success = true,
            .json_result = {.type = kJsonNull}
        }
    );
}

MAYBE_UNUSED static int on_set_editable_size_and_transform(
    struct platch_obj *object,
    FlutterPlatformMessageResponseHandle *responsehandle
) {
    (void) object;
    return platch_respond(
        responsehandle,
        &(struct platch_obj) {
            .codec = kJSONMethodCallResponse,
            .success = true,
            .json_result = {.type = kJsonNull}
        }
    );
}

static int on_set_style(
    struct platch_obj *object,
    FlutterPlatformMessageResponseHandle *responsehandle
) {
    (void) object;
    return platch_respond(
        responsehandle,
        &(struct platch_obj) {
            .codec = kJSONMethodCallResponse,
            .success = true,
            .json_result = {.type = kJsonNull}
        }
    );
}

static int on_finish_autofill_context(
    struct platch_obj *object,
    FlutterPlatformMessageResponseHandle *responsehandle
) {
    (void) object;
    return platch_respond(
        responsehandle,
        &(struct platch_obj) {
            .codec = kJSONMethodCallResponse,
            .success = true,
            .json_result = {.type = kJsonNull}
        }
    );
}

static int on_receive(
    char *channel,
    struct platch_obj *object,
    FlutterPlatformMessageResponseHandle *responsehandle
) {
    (void) channel;
    (void) object;

    if STREQ("TextInput.setClient", object->method) {
        return on_set_client(object, responsehandle);
    } else if STREQ("TextInput.hide", object->method) {
        return on_hide(object, responsehandle);
    } else if STREQ("TextInput.clearClient", object->method) {
        return on_clear_client(object, responsehandle);
    } else if STREQ("TextInput.setEditingState", object->method) {
        return on_set_editing_state(object, responsehandle);
    } else if STREQ("TextInput.show", object->method) {
        return on_show(object, responsehandle);
    } else if STREQ("TextInput.requestAutofill", object->method) {
        return on_request_autofill(object, responsehandle);
    } else if STREQ("TextInput.setEditableSizeAndTransform", object->method) {
        return on_set_style(object, responsehandle);
    } else if STREQ("TextInput.setStyle", object->method) {
        return on_set_style(object, responsehandle);
    } else if STREQ("TextInput.finishAutofillContext", object->method) {
        return on_finish_autofill_context(object, responsehandle);
    }

    return platch_respond_not_implemented(responsehandle);
}

static int client_update_editing_state(
    double connection_id,
    char *text,
    double selection_base,
    double selection_extent,
    bool selection_affinity_is_downstream,
    bool selection_is_directional,
    double composing_base,
    double composing_extent
) {
    return platch_call_json(
        TEXT_INPUT_CHANNEL,
        "TextInputClient.updateEditingState",
        &JSONARRAY2(
            JSONNUM(connection_id),
            JSONOBJECT7(
                "text", JSONSTRING(text),
                "selectionBase", JSONNUM(selection_base),
                "selectionExtent", JSONNUM(selection_extent),
                "selectionAffinity", JSONSTRING(selection_affinity_is_downstream ? "TextAffinity.downstream" : "TextAffinity.upstream"),
                "selectionIsDirectional", JSONBOOL(selection_is_directional),
                "composingBase", JSONNUM(composing_base),
                "composingExtent", JSONNUM(composing_extent)
            )
        ),
        NULL,
        NULL
    );
}

int client_perform_action(
    double connection_id,
    enum text_input_action action
) {
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

    return platch_call_json(
        TEXT_INPUT_CHANNEL,
        "TextInputClient.performAction",
        &JSONARRAY2(
            JSONNUM(connection_id),
            JSONSTRING(action_str)
        ),
        NULL,
        NULL
    );
}

int client_perform_private_command(
    double connection_id,
    char *action,
    struct json_value *data
) {
    if (data != NULL && data->type != kJsonNull && data->type != kJsonObject) {
        return EINVAL;
    }

    return platch_call_json(
        TEXT_INPUT_CHANNEL,
        "TextInputClient.performPrivateCommand",
        &JSONARRAY2(
            JSONNUM(connection_id),
            JSONOBJECT2(
                "action", JSONSTRING(action),
                "data", *data
            )
        ),
        NULL,
        NULL
    );
}

int client_update_floating_cursor(
    double connection_id,
    enum floating_cursor_drag_state text_cursor_action,
    double x,
    double y
) {
    return platch_call_json(
        TEXT_INPUT_CHANNEL,
        "TextInputClient.updateFloatingCursor",
        &JSONARRAY3(
            JSONNUM(connection_id),
            JSONSTRING(
                text_cursor_action == kFloatingCursorDragStateStart ? "FloatingCursorDragState.start" :
                text_cursor_action == kFloatingCursorDragStateUpdate ? "FloatingCursorDragState.update" :
                    "FloatingCursorDragState.end"
            ),
            JSONOBJECT2(
                "X", JSONNUM(x),
                "Y", JSONNUM(y)
            )
        ),
        NULL,
        NULL
    );
}

int client_on_connection_closed(double connection_id) {
    return platch_call_json(
        TEXT_INPUT_CHANNEL,
        "TextInputClient.onConnectionClosed",
        &JSONARRAY1(
            JSONNUM(connection_id)
        ),
        NULL,
        NULL
    );
}

int client_show_autocorrection_prompt_rect(
    double connection_id,
    double start,
    double end
) {
    return platch_call_json(
        TEXT_INPUT_CHANNEL,
        "TextInputClient.showAutocorrectionPromptRect",
        &JSONARRAY3(
            JSONNUM(connection_id),
            JSONNUM(start),
            JSONNUM(end)
        ),
        NULL,
        NULL
    );
}

/**
 * Text Input Model functions.
 */
static inline int selection_start(void) {
    return min(text_input.selection_base, text_input.selection_extent);
}

static inline int selection_end(void) {
    return max(text_input.selection_base, text_input.selection_extent);
}

/**
 * Erases the characters between `start` and `end` (both inclusive) and returns
 * `start`.
 */
static int  model_erase(unsigned int start, unsigned int end) {
    // 0 <= start <= end < len

    uint8_t *start_str     = symbol_at(start);
    uint8_t *after_end_str = symbol_at(end+1);

    if (start_str && after_end_str)
        memmove(start_str, after_end_str, strlen((char*) after_end_str) + 1 /* null byte */);

    return start;
}

static bool model_delete_selected(void) {
    // erase selected text
    text_input.selection_base = model_erase(selection_start(), selection_end()-1);
    text_input.selection_extent = text_input.selection_base;
    return true;
}

static bool model_add_utf8_char(uint8_t *c) {
    size_t symbol_length;
    uint8_t *to_move;

    if (text_input.selection_base != text_input.selection_extent)
        model_delete_selected();

    // find out where in our string we need to insert the utf8 symbol

    symbol_length = utf8_symbol_length(*c);
    to_move       = symbol_at(text_input.selection_base);

    if (!to_move || !symbol_length)
        return false;

    // move the string behind the insertion position to
    // make place for the utf8 charactercursor

    memmove(to_move + symbol_length, to_move, strlen((char*) to_move) + 1 /* null byte */);

    // after the move, to_move points to the memory
    // where c should be inserted
    for (int i = 0; i < symbol_length; i++)
        to_move[i] = c[i];

    // move our selection to behind the inserted char
    text_input.selection_extent++;
    text_input.selection_base = text_input.selection_extent;

    return true;
}

static bool model_backspace(void) {
    if (text_input.selection_base != text_input.selection_extent)
        return model_delete_selected();
    
    if (text_input.selection_base != 0) {
        int base = text_input.selection_base - 1;
        text_input.selection_base = model_erase(base, base);
        text_input.selection_extent = text_input.selection_base;
        return true;
    }

    return false;
}

static bool model_delete(void) {
    if (text_input.selection_base != text_input.selection_extent)
        return model_delete_selected();
    
    if (selection_start() < strlen(text_input.text)) {
        text_input.selection_base = model_erase(selection_start(), selection_end());
        text_input.selection_extent = text_input.selection_base;
        return true;
    }

    return false;
}

static bool model_move_cursor_to_beginning(void) {
    if ((text_input.selection_base != 0) || (text_input.selection_extent != 0)) {
        text_input.selection_base = 0;
        text_input.selection_extent = 0;
        return true;
    }

    return false;
}

static bool model_move_cursor_to_end(void) {
    int end = to_symbol_index(strlen(text_input.text));

    if (text_input.selection_base != end) {
        text_input.selection_base = end;
        text_input.selection_extent = end;
        return true;
    }

    return false;
}

MAYBE_UNUSED static bool model_move_cursor_forward(void) {
    if (text_input.selection_base != text_input.selection_extent) {
        text_input.selection_base = text_input.selection_extent;
        return true;
    }

    if (text_input.selection_extent < to_symbol_index(strlen(text_input.text))) {
        text_input.selection_extent++;
        text_input.selection_base++;
        return true;
    }

    return false;
}

MAYBE_UNUSED static bool model_move_cursor_back(void) {
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



static int sync_editing_state(void) {
    return client_update_editing_state(
        text_input.connection_id,
        text_input.text,
        text_input.selection_base,
        text_input.selection_extent,
        text_input.selection_affinity_is_downstream,
        text_input.selection_is_directional,
        text_input.composing_base,
        text_input.composing_extent
    );
}

/**
 * `c` doesn't need to be NULL-terminated, the length of the char will be calculated
 * using the start byte.
 */
int textin_on_utf8_char(uint8_t *c) {
    if (text_input.connection_id == -1)
        return 0;

    if (model_add_utf8_char(c))
        return sync_editing_state();

    return 0;
}

int textin_on_xkb_keysym(xkb_keysym_t keysym) {
    bool needs_sync = false;
    bool perform_action = false;
    int ok;

    if (text_input.connection_id == -1)
        return 0;

    switch (keysym) {
        case XKB_KEY_BackSpace:
            needs_sync = model_backspace();
            break;
        case XKB_KEY_Delete:
        case XKB_KEY_KP_Delete:
            needs_sync = model_delete();
            break;
        case XKB_KEY_End:
        case XKB_KEY_KP_End:
            needs_sync = model_move_cursor_to_end();
            break;
        case XKB_KEY_Return:
        case XKB_KEY_KP_Enter:
        case XKB_KEY_ISO_Enter:
            if (text_input.input_type == kInputTypeMultiline)
                needs_sync = model_add_utf8_char((uint8_t*) "\n");
            
            perform_action = true;
            break;
        case XKB_KEY_Home:
        case XKB_KEY_KP_Home:
            needs_sync = model_move_cursor_to_beginning();
            break;
        case XKB_KEY_Left:
        case XKB_KEY_KP_Left:
            // handled inside of flutter
            // needs_sync = model_move_cursor_back();
            break;
        case XKB_KEY_Right:
        case XKB_KEY_KP_Right:
            // handled inside of flutter
            // needs_sync = model_move_cursor_forward();
            break;
        default:
            break;
    }

    if (needs_sync) {
        ok = sync_editing_state();
        if (ok != 0) return ok;
    }

    if (perform_action) {
        ok = client_perform_action(text_input.connection_id, text_input.input_action);
        if (ok != 0) return ok;
    }

    return 0;
}

enum plugin_init_result textin_init(struct flutterpi *flutterpi, void **userdata_out) {
    struct text_input *textin;
    int ok;

    (void) flutterpi;

    textin = malloc(sizeof *textin);
    if (textin == NULL) {
        return kError_PluginInitResult;
    }

    ok = plugin_registry_set_receiver(TEXT_INPUT_CHANNEL, kJSONMethodCall, on_receive);
    if (ok != 0) {
        free(textin);
        return kError_PluginInitResult;
    }

    textin->connection_id = -1;
    textin->input_type = kInputTypeText;
    textin->allow_signs = false;
    textin->has_allow_signs = false;
    textin->allow_decimal = false;
    textin->has_allow_decimal = false;
    textin->autocorrect = false;
    textin->input_action = kTextInputActionNone;
    textin->text[0] = '\0';
    textin->selection_base = 0;
    textin->selection_extent = 0;
    textin->selection_affinity_is_downstream = false;
    textin->selection_is_directional = false;
    textin->composing_base = 0;
    textin->composing_extent = 0;
    textin->warned_about_autocorrect = false;
    *userdata_out = textin;
    return kInitialized_PluginInitResult;
}

void textin_deinit(struct flutterpi *flutterpi, void *userdata) {
    (void) flutterpi;
    plugin_registry_remove_receiver(TEXT_INPUT_CHANNEL);
    free(userdata);
}

FLUTTERPI_PLUGIN(
    "text input", text_input,
    textin_init,
    textin_deinit
)
