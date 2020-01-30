#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <termios.h>
#include <unistd.h>
#include <errno.h>
#include <console_keyboard.h>

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

    config.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    config.c_oflag &= ~OPOST;
    config.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    config.c_cflag &= ~(CSIZE | PARENB);
    config.c_cflag |= CS8;

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

char     console_try_get_char(char *input, char **input_out) {
    if (input_out)
        *input_out = input;

    if (isprint(*input)) {
        if (input_out)
            (*input_out)++;
        
        return *input;
    }

    return '\0';
}