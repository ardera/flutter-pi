#ifndef _CONSOLE_KEYBOARD_H
#define _CONSOLE_KEYBOARD_H

#include <stdlib.h>

// small subset of the GLFW key ids.
// (only the ones needed for text input)
typedef enum {
    GLFW_KEY_UNKNOWN = -1,
    GLFW_KEY_ENTER = 257,
    GLFW_KEY_TAB = 258,
    GLFW_KEY_BACKSPACE = 259,
    GLFW_KEY_DELETE = 261,
    GLFW_KEY_RIGHT = 262,
    GLFW_KEY_LEFT = 263,
    GLFW_KEY_PAGE_UP = 266,
    GLFW_KEY_PAGE_DOWN = 267,
    GLFW_KEY_HOME = 268,
    GLFW_KEY_END = 269,
    GLFW_KEY_F1 = 290,
    GLFW_KEY_F2 = 291,
    GLFW_KEY_F3 = 292,
    GLFW_KEY_F4 = 293,
    GLFW_KEY_F5 = 294,
    GLFW_KEY_F6 = 295,
    GLFW_KEY_F7 = 296,
    GLFW_KEY_F8 = 297,
    GLFW_KEY_F9 = 298,
    GLFW_KEY_F10 = 299,
    GLFW_KEY_F11 = 300,
    GLFW_KEY_F12 = 301
} glfw_key;

#define GLFW_KEY_LAST 348

extern char *glfw_key_control_sequence[GLFW_KEY_LAST+1];

int console_flush_stdin(void);
int console_make_raw(void);
int console_restore(void);

/// tries to parse the console input represented by the string `input`
/// as a keycode ()
size_t   utf8_symbol_length(char *c);

static inline char *utf8_symbol_at(char *utf8str, unsigned int symbol_index) {
    for (; symbol_index && *utf8str; symbol_index--)
        utf8str += utf8_symbol_length(utf8str);

    return symbol_index? NULL : utf8str;
}

glfw_key console_try_get_key(char *input, char **input_out);
char    *console_try_get_utf8char(char *input, char **input_out);

#endif