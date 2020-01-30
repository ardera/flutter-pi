#ifndef _CONSOLE_KEYBOARD_H
#define _CONSOLE_KEYBOARD_H

#include <stdlib.h>

// small subset of the GLFW key ids.
// (only the ones needed for text input)
typedef enum {
    GLFW_KEY_UNKNOWN = -1,
    GLFW_KEY_ENTER = 257,
    GLFW_KEY_BACKSPACE = 259,
    GLFW_KEY_DELETE = 261,
    GLFW_KEY_RIGHT = 262,
    GLFW_KEY_LEFT = 263,
    GLFW_KEY_HOME = 268,
    GLFW_KEY_END = 269
} glfw_key;

#define GLFW_KEY_LAST 348

char *glfw_key_control_sequence[GLFW_KEY_LAST+1] = {
    NULL,
    [GLFW_KEY_ENTER] = "\r",
    NULL,
    [GLFW_KEY_BACKSPACE] = "\x7f",
    NULL,
    [GLFW_KEY_DELETE] = "\e[3~",
    [GLFW_KEY_RIGHT] = "\e[C",
    [GLFW_KEY_LEFT] = "\e[D",
    [GLFW_KEY_HOME] = "\e[1~",
    [GLFW_KEY_END] = "\e[4~",
    NULL
};


int console_flush_stdin(void);
int console_make_raw(void);
int console_restore(void);

/// tries to parse the console input represented by the string `input`
/// as a keycode ()
glfw_key console_try_get_key(char *input, char **input_out);
char     console_try_get_char(char *input, char **input_out);

#endif