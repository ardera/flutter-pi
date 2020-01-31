#ifndef _KEY_EVENT_H
#define _KEY_EVENT_H

#define KEY_EVENT_CHANNEL "flutter/keyevent"

int RawKeyboard_onKeyEvent(glfw_key key, uint32_t scan_code, glfw_key_action action);

int RawKeyboard_init(void);
int RawKeyboard_deinit(void);

#endif