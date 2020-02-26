#ifndef _GPIO_PLUGIN_H
#define _GPIO_PLUGIN_H

#include <gpiod.h>

#ifndef GPIOD_LINE_REQUEST_FLAG_BIAS_DISABLE

#   define GPIOD_LINE_REQUEST_FLAG_BIAS_DISABLE   GPIOD_BIT(2)
#   define GPIOD_LINE_REQUEST_FLAG_BIAS_PULL_DOWN GPIOD_BIT(3)
#   define GPIOD_LINE_REQUEST_FLAG_BIAS_PULL_UP   GPIOD_BIT(4)
enum {
    GPIOD_LINE_BIAS_AS_IS = 1,
    GPIOD_LINE_BIAS_DISABLE,
    GPIOD_LINE_BIAS_PULL_UP,
    GPIOD_LINE_BIAS_PULL_DOWN
};

#endif

#define GPIOD_PLUGIN_METHOD_CHANNEL "plugins.flutter.io/gpiod"
#define GPIOD_PLUGIN_EVENT_CHANNEL  "plugins.flutter.io/gpiod_events"

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

#define LOAD_GPIOD_PROC_OPTIONAL(name) \
    libgpiod.name = dlsym(libgpiod.handle, "gpiod_" #name)

extern int gpiodp_init(void);
extern int gpiodp_deinit(void);

#endif