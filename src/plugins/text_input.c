#include <ctype.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <xkbcommon/xkbcommon-keysyms.h>
#include <flutter-pi.h>
#include <messenger.h>
#include <platformchannel.h>
#include <collection.h>
#include <plugins/text_input.h>

struct text_input_plugin {
	int64_t connection_id;
	struct flutterpi *flutterpi;
	enum text_input_type input_type;
	bool allow_signs;
	bool has_allow_signs;
	bool allow_decimal;
	bool has_allow_decimal;
	bool autocorrect;
	enum text_input_action input_action;
	uint8_t text[TEXT_INPUT_MAX_CHARS];
	int  selection_base, selection_extent;
	bool selection_affinity_is_downstream;
	bool selection_is_directional;
	int  composing_base, composing_extent;
	bool warned_about_autocorrect;
};


/**
 * UTF8 utility functions
 */
static inline uint8_t utf8_symbol_length(uint8_t c) {
	if (!(c & 0b10000000)) {
		return 1;
	} else if (!(c & 0b01000000)) {
		// we are in a follow byte
		return 0;
	} else if (c & 0b00100000) {
		return 2;
	} else if (c & 0b00010000) {
		return 3;
	} else if (c & 0b00001000) {
		return 4;
	}

	return 0;
}

static inline uint8_t *symbol_at(struct text_input_plugin *textin, unsigned int symbol_index) {
	uint8_t *cursor = textin->text;

	for (; symbol_index && *cursor; symbol_index--)
		cursor += utf8_symbol_length(*cursor);

	return symbol_index? NULL : cursor;
}

static inline int to_byte_index(struct text_input_plugin *textin, unsigned int symbol_index) {
	uint8_t *cursor = textin->text;

	while ((*cursor) && (symbol_index--))
		cursor += utf8_symbol_length(*cursor);

	if (*cursor)
		return cursor - textin->text;

	return -1;
}

static inline int to_symbol_index(struct text_input_plugin *textin, unsigned int byte_index) {
	uint8_t *cursor = textin->text;
	uint8_t *target_cursor = cursor + byte_index;
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
	struct text_input_plugin *textin,
	const struct platch_obj *object,
	struct flutter_message_response_handle *responsehandle
) {
	const struct json_value *temp, *temp2, *config;
	enum text_input_action input_action;
	enum text_input_type input_type;
	bool autocorrect, has_allow_signs, allow_signs, has_allow_decimal, allow_decimal;

	/*
	 *  TextInput.setClient(List)
	 *      Establishes a new transaction. The argument is
	 *      a [List] whose first value is an integer representing a previously
	 *      unused transaction identifier, and the second is a [String] with a
	 *      JSON-encoded object with five keys, as obtained from
	 *      [TextInputConfiguration.toJSON]. This method must be invoked before any
	 *      others (except `TextInput.hide`). See [TextInput.attach].
	 */
	
	if (JSONVALUE_IS_SIZED_ARRAY(object->json_arg, 2) == false) {
		return fm_respond_illegal_arg_json(
			responsehandle,
			"Expected `arg` to be an array with length 2."
		);
	}

	if (JSONVALUE_IS_NUM(object->json_arg.array[0]) == false) {
		return fm_respond_illegal_arg_json(
			responsehandle,
			"Expected `arg[0]` to be a number"
		);
	}

	if (JSONVALUE_IS_OBJECT(object->json_arg.array[1]) == false) {
		return fm_respond_illegal_arg_json(
			responsehandle,
			"Expected `arg[1]` to be an map."
		);
	}

	config = object->json_arg.array + 1;

	// AUTOCORRECT
	temp = jsobject_get_const(config, "autocorrect");
	if (temp == NULL || !JSONVALUE_IS_BOOL(*temp)) {
		return fm_respond_illegal_arg_json(
			responsehandle,
			"Expected `arg[1]['autocorrect']` to be a boolean."
		);
	} else {
		autocorrect = temp->type == kJsonTrue;
	}
	
	// INPUT ACTION
	temp = jsobject_get_const(config, "inputAction");
	if (temp == NULL || !JSONVALUE_IS_STRING(*temp)) {
		return fm_respond_illegal_arg_json(
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
		return fm_respond_illegal_arg_json(
			responsehandle,
			"Expected `arg[1]['inputAction']` to be a string-ification of `TextInputAction`."
		);

	// INPUT TYPE
	temp = jsobject_get_const(config, "inputType");
	if (temp == NULL || JSONVALUE_IS_OBJECT(*temp)) {
		return fm_respond_illegal_arg_json(
			responsehandle,
			"Expected `arg[1]['inputType']` to be a map."
		);
	}

	temp2 = jsobject_get_const(temp, "signed");
	if (temp2 == NULL || JSONVALUE_IS_NULL(*temp2)) {
		has_allow_signs = false;
	} else if (JSONVALUE_IS_BOOL(*temp2)) {
		has_allow_signs = true;
		allow_signs = JSONVALUE_AS_BOOL(*temp2);
	} else {
		return fm_respond_illegal_arg_json(
			responsehandle,
			"Expected `arg[1]['inputType']['signed']` to be a boolean or null."
		);
	}

	temp2 = jsobject_get_const(temp, "decimal");
	if (temp2 == NULL || JSONVALUE_IS_NULL(*temp2)) {
		has_allow_decimal = false;
	} else if (JSONVALUE_IS_BOOL(*temp2)) {
		has_allow_decimal = true;
		allow_decimal = JSONVALUE_AS_BOOL(*temp2);
	} else {
		return fm_respond_illegal_arg_json(
			responsehandle,
			"Expected `arg[1]['inputType']['decimal']` to be a boolean or null."
		);
	}

	temp2 = jsobject_get_const(temp, "name");
	if (temp2 == NULL || !JSONVALUE_IS_STRING(*temp2)) {
		return fm_respond_illegal_arg_json(
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
		return fm_respond_illegal_arg_json(
			responsehandle,
			"Expected `arg[1]['inputType']['name']` to be a string-ification of `TextInputType`."
		);
	}

	// TRANSACTION ID
	int32_t new_id = (int32_t) object->json_arg.array[0].number_value;

	// everything okay, apply the new text editing config
	textin->connection_id = new_id;
	textin->autocorrect = autocorrect;
	textin->input_action = input_action;
	textin->input_type = input_type;
	textin->has_allow_signs = has_allow_signs;
	textin->allow_signs = allow_signs;
	textin->has_allow_decimal = has_allow_decimal;
	textin->allow_decimal = allow_decimal;

	if (autocorrect && !textin->warned_about_autocorrect) {
		fprintf(stderr, "[text input plugin] warning: flutter requested native autocorrect, which "
				"is not supported by flutter-pi.\n");
		textin->warned_about_autocorrect = true;
	}

	return fm_respond_success_json(responsehandle, &JSONNULL);
}

static int on_hide(
	struct text_input_plugin *textin,
	const struct platch_obj *object,
	struct flutter_message_response_handle *responsehandle
) {
	/*
	 *  TextInput.hide()
	 *      Hide the keyboard. Unlike the other methods, this can be called
	 *      at any time. See [TextInputConnection.close].
	 * 
	 */

	(void) textin;
	(void) object;

	// do nothing since we use a physical keyboard.
	return fm_respond_success_json(
		responsehandle,
		&JSONNULL
	);
}

static int on_clear_client(
	struct text_input_plugin *textin,
	const struct platch_obj *object,
	struct flutter_message_response_handle *responsehandle
) {
	/* 
	 *  TextInput.clearClient()
	 *      End the current transaction. The next method called must be
	 *      `TextInput.setClient` (or `TextInput.hide`).
	 *      See [TextInputConnection.close].
	 * 
	 */

	(void) object;

	textin->connection_id = -1;

	return fm_respond_success_json(
		responsehandle,
		&JSONNULL
	);
}

static int on_set_editing_state(
	struct text_input_plugin *textin,
	const struct platch_obj *object,
	struct flutter_message_response_handle *responsehandle
) {
	const struct json_value *temp, *state;
	const char *text;
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
		return fm_respond_illegal_arg_json(
			responsehandle,
			"Expected `arg` to be a map."
		);
	}

	temp = jsobject_get_const(state, "text");
	if (temp == NULL || temp->type != kJsonString) {
		return fm_respond_illegal_arg_json(
			responsehandle,
			"Expected `arg['text']` to be a string."
		);
	} else {
		text = temp->string_value;
	}

	temp = jsobject_get_const(state, "selectionBase");
	if (temp == NULL || temp->type != kJsonNumber) {
		return fm_respond_illegal_arg_json(
			responsehandle,
			"Expected `arg['selectionBase']` to be a number."
		);
	} else {
		selection_base = (int) temp->number_value;
	}

	temp = jsobject_get_const(state, "selectionExtent");
	if (temp == NULL || temp->type != kJsonNumber) {
		return fm_respond_illegal_arg_json(
			responsehandle,
			"Expected `arg['selectionExtent']` to be a number."
		);
	} else {
		selection_extent = (int) temp->number_value;
	}

	temp = jsobject_get_const(state, "selectionAffinity");
	if (temp == NULL || temp->type != kJsonString) {
		return fm_respond_illegal_arg_json(
			responsehandle,
			"Expected `arg['selectionAffinity']` to be a string-ification of `TextAffinity`."
		);
	} else {
		if STREQ("TextAffinity.downstream", temp->string_value) {
			selection_affinity_is_downstream = true;
		} else if STREQ("TextAffinity.upstream", temp->string_value) {
			selection_affinity_is_downstream = false;
		} else {
			return fm_respond_illegal_arg_json(
				responsehandle,
				"Expected `arg['selectionAffinity']` to be a string-ification of `TextAffinity`."
			);
		}
	}

	temp = jsobject_get_const(state, "selectionIsDirectional");
	if (temp == NULL || (temp->type != kJsonTrue && temp->type != kJsonFalse)) {
		return fm_respond_illegal_arg_json(
			responsehandle,
			"Expected `arg['selectionIsDirectional']` to be a bool."
		);
	} else {
		selection_is_directional = temp->type == kJsonTrue;
	}

	temp = jsobject_get_const(state, "composingBase");
	if (temp == NULL || temp->type != kJsonNumber) {
		return fm_respond_illegal_arg_json(
			responsehandle,
			"Expected `arg['composingBase']` to be a number."
		);
	} else {
		composing_base = (int) temp->number_value;
	}

	temp = jsobject_get_const(state, "composingExtent");
	if (temp == NULL || temp->type != kJsonNumber) {
		return fm_respond_illegal_arg_json(
			responsehandle,
			"Expected `arg['composingExtent']` to be a number."
		);
	} else {
		composing_extent = (int) temp->number_value;
	}

	strncpy((char*) textin->text, text, TEXT_INPUT_MAX_CHARS);
	textin->selection_base = selection_base;
	textin->selection_extent = selection_extent;
	textin->selection_affinity_is_downstream = selection_affinity_is_downstream;
	textin->selection_is_directional = selection_is_directional;
	textin->composing_base = composing_base;
	textin->composing_extent = composing_extent;

	return fm_respond_success_json(
		responsehandle,
		&JSONNULL
	);
}

static int on_show(
	struct text_input_plugin *textin,
	const struct platch_obj *object,
	struct flutter_message_response_handle *responsehandle
) {
	/*
	 *  TextInput.show()
	 *      Show the keyboard. See [TextInputConnection.show].
	 * 
	 */

	(void) textin;
	(void) object;

	// do nothing since we use a physical keyboard.
	return fm_respond_success_json(
		responsehandle,
		&JSONNULL
	);
}

static int on_request_autofill(
	struct text_input_plugin *textin,
	const struct platch_obj *object,
	struct flutter_message_response_handle *responsehandle
) {
	(void) textin;
	(void) object;

	return fm_respond_success_json(responsehandle, &JSONNULL);
}

/*
static int on_set_editable_size_and_transform(
	struct text_input_plugin *textin,
	const struct platch_obj *object,
	struct flutter_message_response_handle *responsehandle
) {
	(void) textin;
	(void) object;

	return fm_respond_success_json(responsehandle, &JSONNULL);
}
*/

static int on_set_style(
	struct text_input_plugin *textin,
	const struct platch_obj *object,
	struct flutter_message_response_handle *responsehandle
) {
	(void) textin;
	(void) object;
	
	return fm_respond_success_json(responsehandle, &JSONNULL);
}

static int on_finish_autofill_context(
	struct text_input_plugin *textin,
	const struct platch_obj *object,
	struct flutter_message_response_handle *responsehandle
) {
	(void) textin;
	(void) object;
	
	return fm_respond_success_json(responsehandle, &JSONNULL);
}

static void on_receive(
	bool success,
	struct flutter_message_response_handle *responsehandle,
	const char *channel,
	const struct platch_obj *object,
	void *userdata
) {
	struct text_input_plugin *textin;

	(void) success;
	(void) channel;
	
	textin = userdata;

	if STREQ("TextInput.setClient", object->method) {
		on_set_client(textin, object, responsehandle);
	} else if STREQ("TextInput.hide", object->method) {
		on_hide(textin, object, responsehandle);
	} else if STREQ("TextInput.clearClient", object->method) {
		on_clear_client(textin, object, responsehandle);
	} else if STREQ("TextInput.setEditingState", object->method) {
		on_set_editing_state(textin, object, responsehandle);
	} else if STREQ("TextInput.show", object->method) {
		on_show(textin, object, responsehandle);
	} else if STREQ("TextInput.requestAutofill", object->method) {
		on_request_autofill(textin, object, responsehandle);
	} else if STREQ("TextInput.setEditableSizeAndTransform", object->method) {
		on_set_style(textin, object, responsehandle);
	} else if STREQ("TextInput.setStyle", object->method) {
		on_set_style(textin, object, responsehandle);
	} else if STREQ("TextInput.finishAutofillContext", object->method) {
		on_finish_autofill_context(textin, object, responsehandle);
	} else {
		fm_respond_not_implemented(responsehandle);
	}
}

static int client_update_editing_state(
	struct text_input_plugin *textin, 
	double connection_id,
	uint8_t *text,
	double selection_base,
	double selection_extent,
	bool selection_affinity_is_downstream,
	bool selection_is_directional,
	double composing_base,
	double composing_extent
) {
	return fm_call_json(
		textin->flutterpi->flutter_messenger,
		TEXT_INPUT_CHANNEL,
		"TextInputClient.updateEditingState",
		&(struct json_value) {
			.type = kJsonArray,
			.size = 2,
			.array = (struct json_value[2]) {
				JSONNUM(connection_id),
				JSONOBJECT7(
					"text",						JSONSTRING((char*) text),
					"selectionBase",			JSONNUM(selection_base),
					"selectionExtent",			JSONNUM(selection_extent),
					"selectionAffinity",		JSONSTRING(selection_affinity_is_downstream ? "TextAffinity.downstream" : "TextAffinity.upstream"),
					"selectionIsDirectional",	JSONBOOL(selection_is_directional),
					"composingBase",			JSONNUM(composing_base),
					"composingExtent",			JSONNUM(composing_extent)
				)
			}
		},
		NULL,
		NULL,
		NULL
	);
}

int client_perform_action(
	struct text_input_plugin *textin,
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

	return fm_call_json(
		textin->flutterpi->flutter_messenger,
		TEXT_INPUT_CHANNEL,
		"TextInputClient.performAction",
		&(struct json_value) {
			.type = kJsonArray,
			.size = 2,
			.array = (struct json_value[2]) {
				JSONNUM(connection_id),
				JSONSTRING(action_str)
			}
		},
		NULL,
		NULL,
		NULL
	);
}

int client_perform_private_command(
	struct text_input_plugin *textin,
	double connection_id,
	char *action,
	struct json_value *data
) {
	if (data != NULL && data->type != kJsonNull && data->type != kJsonObject) {
		return EINVAL;
	}

	return fm_call_json(
		textin->flutterpi->flutter_messenger,
		TEXT_INPUT_CHANNEL,
		"TextInputClient.performPrivateCommand",
		&(struct json_value) {
			.type = kJsonArray,
			.size = 2,
			.array = (struct json_value[2]) {
				JSONNUM(connection_id),
				JSONOBJECT2(
					"action", 	JSONSTRING(action),
					"data",		*data
				)
			}
		},
		NULL,
		NULL,
		NULL
	);
}

int client_update_floating_cursor(
	struct text_input_plugin *textin,
	double connection_id,
	enum floating_cursor_drag_state text_cursor_action,
	double x,
	double y
) {
	return fm_call_json(
		textin->flutterpi->flutter_messenger,
		TEXT_INPUT_CHANNEL,
		"TextInputClient.updateFloatingCursor",
		&(struct json_value) {
			.type = kJsonArray,
			.size = 3,
			.array = (struct json_value[3]) {
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
			}
		},
		NULL,
		NULL,
		NULL
	);
}

int client_on_connection_closed(
	struct text_input_plugin *textin,
	double connection_id
) {
	return fm_call_json(
		textin->flutterpi->flutter_messenger,
		TEXT_INPUT_CHANNEL,
		"TextInputClient.onConnectionClosed",
		&(struct json_value) {
			.type = kJsonArray,
			.size = 1,
			.array = (struct json_value[1]) {
				JSONNUM(connection_id)
			}
		},
		NULL,
		NULL,
		NULL
	);
}

int client_show_autocorrection_prompt_rect(
	struct text_input_plugin *textin,
	double connection_id,
	double start,
	double end
) {
	return fm_call_json(
		textin->flutterpi->flutter_messenger,
		TEXT_INPUT_CHANNEL,
		"TextInputClient.showAutocorrectionPromptRect",
		&(struct json_value) {
			.type = kJsonArray,
			.size = 3,
			.array = (struct json_value[3]) {
				JSONNUM(connection_id),
				JSONNUM(start),
				JSONNUM(end)
			}
		},
		NULL,
		NULL,
		NULL
	);
}

/**
 * Text Input Model functions.
 */

static inline unsigned int selection_start(struct text_input_plugin *textin) {
	return min(textin->selection_base, textin->selection_extent);
}

static inline int selection_end(struct text_input_plugin *textin) {
	return max(textin->selection_base, textin->selection_extent);
}

/**
 * Erases the characters between `start` and `end` (both inclusive) and returns
 * `start`.
 */
static int  model_erase(struct text_input_plugin *textin, unsigned int start, unsigned int end) {
	// 0 <= start <= end < len

	uint8_t *start_str     = symbol_at(textin, start);
	uint8_t *after_end_str = symbol_at(textin, end+1);

	if (start_str && after_end_str)
		memmove(start_str, after_end_str, strlen((char*) after_end_str) + 1 /* null byte */);

	return start;
}

static bool model_delete_selected(struct text_input_plugin *textin) {
	// erase selected text
	textin->selection_base = model_erase(textin, selection_start(textin), selection_end(textin)-1);
	textin->selection_extent = textin->selection_base;
	return true;
}

static bool model_add_utf8_char(struct text_input_plugin *textin, uint8_t *c) {
	size_t symbol_length;
	uint8_t *to_move;

	if (textin->selection_base != textin->selection_extent)
		model_delete_selected(textin);

	// find out where in our string we need to insert the utf8 symbol

	symbol_length = utf8_symbol_length(*c);
	to_move       = symbol_at(textin, textin->selection_base);

	if (!to_move || !symbol_length)
		return false;

	// move the string behind the insertion position to
	// make place for the utf8 charactercursor

	memmove(to_move + symbol_length, to_move, strlen((char*) to_move) + 1 /* null byte */);

	// after the move, to_move points to the memory
	// where c should be inserted
	for (unsigned int i = 0; i < symbol_length; i++)
		to_move[i] = c[i];

	// move our selection to behind the inserted char
	textin->selection_extent++;
	textin->selection_base = textin->selection_extent;

	return true;
}

static bool model_backspace(struct text_input_plugin *textin) {
	if (textin->selection_base != textin->selection_extent)
		return model_delete_selected(textin);
	
	if (textin->selection_base != 0) {
		int base = textin->selection_base - 1;
		textin->selection_base = model_erase(textin, base, base);
		textin->selection_extent = textin->selection_base;
		return true;
	}

	return false;
}

static bool model_delete(struct text_input_plugin *textin) {
	if (textin->selection_base != textin->selection_extent)
		return model_delete_selected(textin);
	
	if (selection_start(textin) < strlen((char*) textin->text)) {
		textin->selection_base = model_erase(textin, selection_start(textin), selection_end(textin));
		textin->selection_extent = textin->selection_base;
		return true;
	}

	return false;
}

static bool model_move_cursor_to_beginning(struct text_input_plugin *textin) {
	if ((textin->selection_base != 0) || (textin->selection_extent != 0)) {
		textin->selection_base = 0;
		textin->selection_extent = 0;
		return true;
	}

	return false;
}

static bool model_move_cursor_to_end(struct text_input_plugin *textin) {
	int end = to_symbol_index(textin, strlen((char*) textin->text));

	if (textin->selection_base != end) {
		textin->selection_base = end;
		textin->selection_extent = end;
		return true;
	}

	return false;
}

/*
static bool model_move_cursor_forward(struct text_input_plugin *textin) {
	if (textin->selection_base != textin->selection_extent) {
		textin->selection_base = textin->selection_extent;
		return true;
	}

	if (textin->selection_extent < to_symbol_index(textin, strlen((char*) textin->text))) {
		textin->selection_extent++;
		textin->selection_base++;
		return true;
	}

	return false;
}

static bool model_move_cursor_back(struct text_input_plugin *textin) {
	if (textin->selection_base != textin->selection_extent) {
		textin->selection_extent = textin->selection_base;
		return true; 
	}

	if (textin->selection_base > 0) {
		textin->selection_base--;
		textin->selection_extent--;
		return true;
	}

	return false;
}
*/

static int sync_editing_state(struct text_input_plugin *textin) {
	return client_update_editing_state(
		textin,
		textin->connection_id,
		textin->text,
		textin->selection_base,
		textin->selection_extent,
		textin->selection_affinity_is_downstream,
		textin->selection_is_directional,
		textin->composing_base,
		textin->composing_extent
	);
}

/**
 * `c` doesn't need to be NULL-terminated, the length of the char will be calculated
 * using the start byte.
 */
int textin_on_utf8_char(struct text_input_plugin *textin, uint8_t *c) {
	if (textin->connection_id == -1)
		return 0;
	
	switch (textin->input_type) {
		case kInputTypeNumber:
			if (isdigit(*c)) {
				break;
			} else {
				return 0;
			}
		case kInputTypePhone:
			if (isdigit(*c) || *c == '*' || *c == '#' || *c == '+') {
				break;
			} else {
				return 0;
			}
		default:
			break;
	}

	if (model_add_utf8_char(textin, c))
		return sync_editing_state(textin);

	return 0;
}

int textin_on_xkb_keysym(struct text_input_plugin *textin, xkb_keysym_t keysym) {
	bool needs_sync = false;
	bool perform_action = false;
	int ok;

	if (textin->connection_id == -1)
		return 0;

	switch (keysym) {
		case XKB_KEY_BackSpace:
			needs_sync = model_backspace(textin);
			break;
		case XKB_KEY_Delete:
		case XKB_KEY_KP_Delete:
			needs_sync = model_delete(textin);
			break;
		case XKB_KEY_End:
		case XKB_KEY_KP_End:
			needs_sync = model_move_cursor_to_end(textin);
			break;
		case XKB_KEY_Return:
		case XKB_KEY_KP_Enter:
		case XKB_KEY_ISO_Enter:
			if (textin->input_type == kInputTypeMultiline)
				needs_sync = model_add_utf8_char(textin, (uint8_t*) "\n");
			
			perform_action = true;
			break;
		case XKB_KEY_Home:
		case XKB_KEY_KP_Home:
			needs_sync = model_move_cursor_to_beginning(textin);
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
		ok = sync_editing_state(textin);
		if (ok != 0) return ok;
	}

	if (perform_action) {
		ok = client_perform_action(textin, textin->connection_id, textin->input_action);
		if (ok != 0) return ok;
	}

	return 0;
}


int textin_init(struct flutterpi *flutterpi, void **userdata) {
	struct text_input_plugin *plugin;
	int ok;

	plugin = malloc(sizeof *plugin);
	if (plugin == NULL) {
		return ENOMEM;
	}

	plugin->connection_id = -1;
	plugin->flutterpi = flutterpi;
	plugin->input_type = kInputTypeText;
	plugin->allow_signs = false;
	plugin->has_allow_signs = false;
	plugin->allow_decimal = false;
	plugin->has_allow_decimal = false;
	plugin->autocorrect = false;
	plugin->input_action = kTextInputActionNone;
	plugin->text[0] = '\0'; // no need to null the other chars
	plugin->selection_base = 0;
	plugin->selection_extent = 0;
	plugin->selection_affinity_is_downstream = false;
	plugin->selection_is_directional = false;
	plugin->composing_base = 0;
	plugin->composing_extent = 0;
	plugin->warned_about_autocorrect = false;

	ok = fm_set_listener(
		flutterpi->flutter_messenger,
		TEXT_INPUT_CHANNEL,
		kJSONMethodCall,
		on_receive,
		NULL,
		plugin
	);
	if (ok != 0) {
		free(plugin);
		return ok;
	}
	
	*userdata = plugin;

	return 0;
}

int textin_deinit(struct flutterpi *flutterpi, void **userdata) {
	struct text_input_plugin *plugin = *userdata;

	fm_remove_listener(flutterpi->flutter_messenger, TEXT_INPUT_CHANNEL);
	free(plugin);

	return 0;
}