#include <ctype.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <flutter-pi.h>
#include <pluginregistry.h>
#include <keyboard.h>
#include <messenger.h>

#include <plugins/raw_keyboard.h>

int rawkb_send_android_keyevent(
	struct raw_keyboard_plugin *rawkb,
	uint32_t flags,
	uint32_t code_point,
	unsigned int key_code,
	uint32_t plain_code_point,
	uint32_t scan_code,
	uint32_t meta_state,
	uint32_t source,
	uint16_t vendor_id,
	uint16_t product_id,
	uint16_t device_id,
	int repeat_count,
	bool is_down,
	char *character
) {
	struct flutterpi *flutterpi;

	/**
	 * keymap: android
	 * flags: flags
	 * codePoint: code_point
	 * keyCode: key_code
	 * plainCodePoint: plain_code_point
	 * scanCode: scan_code
	 * metaState: meta_state
	 * source: source
	 * vendorId: vendor_id
	 * productId: product_id
	 * deviceId: device_id
	 * repeatCount: repeatCount,
	 * type: is_down? "keydown" : "keyup"
	 * character: character
	 */

	// for now, our userdata is just the flutterpi instance.
	// in the future we may use our own struct here
	flutterpi = (struct flutterpi *) rawkb;

	return fm_send_blocking(
		flutterpi->flutter_messenger,
		KEY_EVENT_CHANNEL,
		&PLATCH_OBJ_JSON_MSG(
			JSONOBJECT14(
				"keymap", 			JSONSTRING("android"),
				"flags", 			JSONNUM(flags),
				"codePoint", 		JSONNUM(code_point),
				"keyCode", 			JSONNUM(key_code),
				"plainCodePoint", 	JSONNUM(plain_code_point),
				"scanCode", 		JSONNUM(scan_code),
				"metaState", 		JSONNUM(meta_state),
				"source", 			JSONNUM(source),
				"vendorId", 		JSONNUM(vendor_id),
				"productId", 		JSONNUM(product_id),
				"deviceId", 		JSONNUM(device_id),
				"repeatCount", 		JSONNUM(repeat_count),
				"type", 			JSONSTRING(is_down? "keydown" : "keyup"),
				"character", 		character? JSONSTRING(character) : JSONNULL
			)
		),
		kNotImplemented,
		NULL,
		NULL,
		NULL
	);
}

int rawkb_send_gtk_keyevent(
	struct raw_keyboard_plugin *rawkb,
	uint32_t unicode_scalar_values,
	uint32_t key_code,
	uint32_t scan_code,
	uint32_t modifiers,
	bool is_down
) {
	struct flutterpi *flutterpi;

	/**
	 * keymap: linux
	 * toolkit: glfw
	 * unicodeScalarValues: code_point
	 * keyCode: key_code
	 * scanCode: scan_code
	 * modifiers: mods
	 * type: is_down? "keydown" : "keyup"
	 */

	flutterpi = (struct flutterpi *) rawkb;

	// We can use _blocking here because this rawkb_send_gtk_keyevent should be called on the platform thread anyway.
	return fm_send_blocking(
		flutterpi->flutter_messenger,
		KEY_EVENT_CHANNEL,
		&PLATCH_OBJ_JSON_MSG(
			JSONOBJECT7(
				"keymap",               JSONSTRING("linux"),
				"toolkit",              JSONSTRING("gtk"),
				"unicodeScalarValues",  JSONNUM(unicode_scalar_values),
				"keyCode",              JSONNUM(key_code),
				"scanCode",             JSONNUM(scan_code),
				"modifiers",            JSONNUM(modifiers),
				"type",                 JSONSTRING(is_down? "keydown" : "keyup")
			)
		),
		kNotImplemented,
		NULL,
		NULL,
		NULL
	);
}

int rawkb_init(struct flutterpi *flutterpi, void **userdata) {
	*userdata = flutterpi;
	return 0;
}

int rawkb_deinit(struct flutterpi *flutterpi, void **userdata) {
	(void) flutterpi;
	(void) userdata;
	return 0;
}
