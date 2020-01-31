#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <ctype.h>
#include <termios.h>
#include <unistd.h>
#include <errno.h>

#include <console_keyboard.h>

char *glfw_key_control_sequence[GLFW_KEY_LAST+1] = {
    NULL,
    [GLFW_KEY_ENTER] = "\n",
    [GLFW_KEY_TAB] = "\t",
    [GLFW_KEY_BACKSPACE] = "\x7f",
    NULL,
    [GLFW_KEY_DELETE] = "\e[3~",
    [GLFW_KEY_RIGHT] = "\e[C",
    [GLFW_KEY_LEFT] = "\e[D",
    NULL,
    [GLFW_KEY_PAGE_UP] = "\e[5~",
    [GLFW_KEY_PAGE_DOWN] = "\e[6~",
    [GLFW_KEY_HOME] = "\e[1~",
    [GLFW_KEY_END] = "\e[4~",
    NULL,

    // function keys
    [GLFW_KEY_F1] = "\eOP", "\eOQ", "\eOR", "\eOS",
    "\e[15~", "\e[17~", "\e[18~", "\e[19~",
    "\e[20~", "\e[21~", "\e[23~", "\e[24~"
};

int console_flush_stdin(void) {
    int ok;
    
    ok = tcflush(STDIN_FILENO, TCIFLUSH);
    if (ok == -1) {
        perror("could not flush stdin");
        return errno;
    }

    return 0;
}

struct termios original_config;
bool           is_raw = false;

int console_make_raw(void) {
    struct termios config;
    int ok;

    if (is_raw) return 0;

    ok = tcgetattr(STDIN_FILENO, &config);
    if (ok == -1) {
        perror("could not get terminal attributes");
        return errno;
    }

    original_config = config;

    config.c_lflag &= ~(ECHO | ICANON);

    //config.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    //config.c_oflag &= ~OPOST;
    //config.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    //config.c_cflag &= ~(CSIZE | PARENB);
    //config.c_cflag |= CS8;

    ok = tcsetattr(STDIN_FILENO, TCSANOW, &config);
    if (ok == -1) {
        perror("could not set terminal attributes");
        return errno;
    }

    return 0;
}

int console_restore(void) {
    int ok;

    if (!is_raw) return 0;

    ok = tcsetattr(STDIN_FILENO, TCSANOW, &original_config);
    if (ok == -1) {
        perror("could not set terminal attributes");
        return errno;
    }

    is_raw = false;
}

size_t   utf8_symbol_length(char *c) {
    uint8_t first = ((uint8_t*) c)[0];
    uint8_t second = ((uint8_t*) c)[1];
    uint8_t third = ((uint8_t*) c)[2];
    uint8_t fourth = ((uint8_t*) c)[3];

    if (first <= 0b01111111) {
        // ASCII
        return 1;
    } else if (((first >> 5) == 0b110) && ((second >> 6) == 0b10)) {
        // 2-byte UTF8
        return 2;
    } else if (((first >> 4) == 0b1110) && ((second >> 6) == 0b10) && ((third >> 6) == 0b10)) {
        // 3-byte UTF8
        return 3;
    } else if (((first >> 3) == 0b11110) && ((second >> 6) == 0b10) && ((third >> 6) == 0b10) && ((fourth >> 6) == 0b10)) {
        // 4-byte UTF8
        return 4;
    }

    return 0;
}

glfw_key console_try_get_key(char *input, char **input_out) {
    if (input_out)
        *input_out = input;

    for (glfw_key key = 0; key <= GLFW_KEY_LAST; key++) {
        if (glfw_key_control_sequence[key] == NULL)
            continue;

        if (strcmp(input, glfw_key_control_sequence[key]) == 0) {
            if (input_out)
                *input_out += strlen(glfw_key_control_sequence[key]);
            
            return key;
        }
    }

    return GLFW_KEY_UNKNOWN;
}

char    *console_try_get_utf8char(char *input, char **input_out) {
    if (input_out)
        *input_out = input;
    
    size_t length = utf8_symbol_length(input);

    if ((length == 1) && !isprint(*input))
        return NULL;
    
    if (length == 0)
        return NULL;

    *input_out += length;

    return input;
}