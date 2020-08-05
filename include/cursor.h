#ifndef _CURSOR_H
#define _CURSOR_H

#include <stdint.h>

struct cursor_icon {
	int rotation;
	int hot_x, hot_y;
	int width, height;
	uint32_t *data;
};

extern const struct cursor_icon cursors[5];
extern int n_cursors;

#endif