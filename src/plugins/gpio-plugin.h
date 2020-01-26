#ifndef _GPIO_PLUGIN_H
#define _GPIO_PLUGIN_H

#include <gpiod.h>

#define GPIO_PLUGIN_GPIOD_METHOD_CHANNEL "flutter-pi/gpio/gpiod"
#define GPIO_PLUGIN_GPIOD_EVENT_CHANNEL  "flutter-pi/gpio/gpiod_events"

#define GPIO_PLUGIN_MAX_CHIPS 8

// a basic macro that loads a symbol from libgpiod, puts it into the libgpiod struct
// and returns the errno if an error ocurred

#define LOAD_GPIOD_PROC(name) \
    do { \
        libgpiod.name = dlsym(libgpiod.handle, "gpiod_" #name); \
        if (!libgpiod.name) {\
            perror("could not resolve libgpiod procedure gpiod_" #name); \
            return errno; \
        } \
    } while (false)

// because libgpiod doesn't provide it, but it's useful
static inline void gpiod_line_bulk_remove(struct gpiod_line_bulk *bulk, unsigned int index) {
    memmove(&bulk->lines[index], &bulk->lines[index+1], sizeof(struct gpiod_line*) * (bulk->num_lines - index - 1));
    bulk->num_lines--;
}

extern int GpioPlugin_init(void);
extern int GpioPlugin_deinit(void);

#endif