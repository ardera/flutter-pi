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

#define EVDEV_TO_GLFW(keyname) [KEY_##keyname] = GLFW_KEY_##keyname
#define EVDEV_TO_GLFW_RENAME(linux_keyname, glfw_keyname) [KEY_##linux_keyname] = GLFW_KEY_##glfw_keyname

glfw_key evdev_code_glfw_key[KEY_CNT] = {
	EVDEV_TO_GLFW(SPACE),
	EVDEV_TO_GLFW(APOSTROPHE),
	EVDEV_TO_GLFW(COMMA),
	EVDEV_TO_GLFW(MINUS),
	EVDEV_TO_GLFW_RENAME(DOT, PERIOD),
	EVDEV_TO_GLFW(SLASH),
	EVDEV_TO_GLFW(0),
	EVDEV_TO_GLFW(1),
	EVDEV_TO_GLFW(2),
	EVDEV_TO_GLFW(3),
	EVDEV_TO_GLFW(4),
	EVDEV_TO_GLFW(5),
	EVDEV_TO_GLFW(6),
	EVDEV_TO_GLFW(7),
	EVDEV_TO_GLFW(8),
	EVDEV_TO_GLFW(9),
	EVDEV_TO_GLFW(SEMICOLON),
	EVDEV_TO_GLFW(EQUAL),
	EVDEV_TO_GLFW(A),
	EVDEV_TO_GLFW(B),
	EVDEV_TO_GLFW(C),
	EVDEV_TO_GLFW(D),
	EVDEV_TO_GLFW(E),
	EVDEV_TO_GLFW(F),
	EVDEV_TO_GLFW(G),
	EVDEV_TO_GLFW(H),
	EVDEV_TO_GLFW(I),
	EVDEV_TO_GLFW(J),
	EVDEV_TO_GLFW(K),
	EVDEV_TO_GLFW(L),
	EVDEV_TO_GLFW(M),
	EVDEV_TO_GLFW(N),
	EVDEV_TO_GLFW(O),
	EVDEV_TO_GLFW(P),
	EVDEV_TO_GLFW(Q),
	EVDEV_TO_GLFW(R),
	EVDEV_TO_GLFW(S),
	EVDEV_TO_GLFW(T),
	EVDEV_TO_GLFW(U),
	EVDEV_TO_GLFW(V),
	EVDEV_TO_GLFW(W),
	EVDEV_TO_GLFW(X),
	EVDEV_TO_GLFW(Y),
	EVDEV_TO_GLFW(Z),
	EVDEV_TO_GLFW_RENAME(LEFTBRACE, LEFT_BRACKET),
	EVDEV_TO_GLFW(BACKSLASH),
	EVDEV_TO_GLFW_RENAME(RIGHTBRACE, RIGHT_BRACKET),
	EVDEV_TO_GLFW_RENAME(GRAVE, GRAVE_ACCENT),
	EVDEV_TO_GLFW_RENAME(ESC, ESCAPE),
	EVDEV_TO_GLFW(ENTER),
	EVDEV_TO_GLFW(TAB),
	EVDEV_TO_GLFW(BACKSPACE),
	EVDEV_TO_GLFW(INSERT),
	EVDEV_TO_GLFW(DELETE),
	EVDEV_TO_GLFW(RIGHT),
	EVDEV_TO_GLFW(LEFT),
	EVDEV_TO_GLFW(DOWN),
	EVDEV_TO_GLFW(UP),
	EVDEV_TO_GLFW_RENAME(PAGEUP, PAGE_UP),
	EVDEV_TO_GLFW_RENAME(PAGEDOWN, PAGE_DOWN),
	EVDEV_TO_GLFW(HOME),
	EVDEV_TO_GLFW(END),
	EVDEV_TO_GLFW_RENAME(CAPSLOCK, CAPS_LOCK),
	EVDEV_TO_GLFW_RENAME(SCROLLLOCK, SCROLL_LOCK),
	EVDEV_TO_GLFW_RENAME(NUMLOCK, NUM_LOCK),
	EVDEV_TO_GLFW_RENAME(SYSRQ, PRINT_SCREEN),
	EVDEV_TO_GLFW(PAUSE),
	EVDEV_TO_GLFW(F1),
	EVDEV_TO_GLFW(F2),
	EVDEV_TO_GLFW(F3),
	EVDEV_TO_GLFW(F4),
	EVDEV_TO_GLFW(F5),
	EVDEV_TO_GLFW(F6),
	EVDEV_TO_GLFW(F7),
	EVDEV_TO_GLFW(F8),
	EVDEV_TO_GLFW(F9),
	EVDEV_TO_GLFW(F10),
	EVDEV_TO_GLFW(F11),
	EVDEV_TO_GLFW(F12),
	EVDEV_TO_GLFW(F13),
	EVDEV_TO_GLFW(F14),
	EVDEV_TO_GLFW(F15),
	EVDEV_TO_GLFW(F16),
	EVDEV_TO_GLFW(F17),
	EVDEV_TO_GLFW(F18),
	EVDEV_TO_GLFW(F19),
	EVDEV_TO_GLFW(F20),
	EVDEV_TO_GLFW(F21),
	EVDEV_TO_GLFW(F22),
	EVDEV_TO_GLFW(F23),
	EVDEV_TO_GLFW(F24),
	EVDEV_TO_GLFW_RENAME(KP0, KP_0),
	EVDEV_TO_GLFW_RENAME(KP1, KP_1),
	EVDEV_TO_GLFW_RENAME(KP2, KP_2),
	EVDEV_TO_GLFW_RENAME(KP3, KP_3),
	EVDEV_TO_GLFW_RENAME(KP4, KP_4),
	EVDEV_TO_GLFW_RENAME(KP5, KP_5),
	EVDEV_TO_GLFW_RENAME(KP6, KP_6),
	EVDEV_TO_GLFW_RENAME(KP7, KP_7),
	EVDEV_TO_GLFW_RENAME(KP8, KP_8),
	EVDEV_TO_GLFW_RENAME(KP9, KP_9),
	EVDEV_TO_GLFW_RENAME(KPDOT, KP_DECIMAL),
	EVDEV_TO_GLFW_RENAME(KPSLASH, KP_DIVIDE),
	EVDEV_TO_GLFW_RENAME(KPASTERISK, KP_MULTIPLY),
	EVDEV_TO_GLFW_RENAME(KPMINUS, KP_SUBTRACT),
	EVDEV_TO_GLFW_RENAME(KPPLUS, KP_ADD),
	EVDEV_TO_GLFW_RENAME(KPENTER, KP_ENTER),
    //CONVERSION(KP_EQUAL), // what is the equivalent of KP_EQUAL? is it 
	EVDEV_TO_GLFW_RENAME(LEFTSHIFT, LEFT_SHIFT),
	EVDEV_TO_GLFW_RENAME(LEFTCTRL, LEFT_CONTROL),
	EVDEV_TO_GLFW_RENAME(LEFTALT, LEFT_ALT),
	EVDEV_TO_GLFW_RENAME(LEFTMETA, LEFT_SUPER),
	EVDEV_TO_GLFW_RENAME(RIGHTSHIFT, RIGHT_SHIFT),
	EVDEV_TO_GLFW_RENAME(RIGHTCTRL, RIGHT_CONTROL),
	EVDEV_TO_GLFW_RENAME(RIGHTALT, RIGHT_ALT),
	EVDEV_TO_GLFW_RENAME(RIGHTMETA, RIGHT_SUPER),
	EVDEV_TO_GLFW(MENU), // could also be that the linux equivalent is KEY_COMPOSE
};

#undef EVDEV_TO_GLFW
#undef EVDEV_TO_GLFW_RENAME

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