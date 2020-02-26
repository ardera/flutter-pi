#ifndef _KEY_EVENT_H
#define _KEY_EVENT_H

#define KEY_EVENT_CHANNEL "flutter/keyevent"

#include <console_keyboard.h>

int rawkb_on_keyevent(glfw_key key, uint32_t scan_code, glfw_key_action action);

int rawkb_init(void);
int rawkb_deinit(void);

#endif