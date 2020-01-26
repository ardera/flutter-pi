#include <alloca.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <dlfcn.h>
#include <pthread.h>

#include <gpiod.h>

#include <pluginregistry.h>
#include "gpio-plugin.h"



struct {
    bool initialized;
    
    struct gpiod_chip *chips[GPIO_PLUGIN_MAX_CHIPS];
    size_t n_chips;
    
    // complete list of GPIO lines
    struct gpiod_line **lines;
    size_t n_lines;

    // GPIO lines that flutter is currently listening to
    pthread_mutex_t listening_lines_mutex;
    pthread_cond_t  listening_line_added;
    struct gpiod_line_bulk *listening_lines;
} gpio_plugin;

struct {
    void *handle;

    // GPIO chips
    void (*chip_close)(struct gpiod_chip *chip);
    const char *(*chip_name)(struct gpiod_chip *chip);
    const char *(*chip_label)(struct gpiod_chip *chip);
    unsigned int (*chip_num_lines)(struct gpiod_chip *chip);

    // GPIO lines
    unsigned int (*line_offset)(struct gpiod_line *line);
    const char *(*line_name)(struct gpiod_line *line);
    const char *(*line_consumer)(struct gpiod_line *line);
    int (*line_direction)(struct gpiod_line *line);
    int (*line_active_state)(struct gpiod_line *line);
    int (*line_bias)(struct gpiod_line *line);
    bool (*line_is_used)(struct gpiod_line *line);
    bool (*line_is_open_drain)(struct gpiod_line *line);
    bool (*line_is_open_source)(struct gpiod_line *line);
    int (*line_update)(struct gpiod_line *line);
    int (*line_request)(struct gpiod_line *line, const struct gpiod_line_request_config *config, int default_val);
    bool (*line_is_requested)(struct gpiod_line *line);
    bool (*line_is_free)(struct gpiod_line *line);
    int (*line_get_value)(struct gpiod_line *line);
    int (*line_set_value)(struct gpiod_line *line, int value);
    int (*line_set_config)(struct gpiod_line *line, int direction, int flags, int value);
    int (*line_event_wait_bulk)(struct gpiod_line_bulk *bulk, const struct timespec *timeout, struct gpiod_line_bulk *event_bulk);
    int (*line_event_read_multiple)(struct gpiod_line *line, struct gpiod_line_event *events, unsigned int num_events);
    
    // chip iteration
    struct gpiod_chip_iter *(*chip_iter_new)(void);
    void (*chip_iter_free_noclose)(struct gpiod_chip_iter *iter);
    struct gpiod_chip *(*chip_iter_next_noclose)(struct gpiod_chip_iter *iter);

    // line iteration
    struct gpiod_line_iter *(*line_iter_new)(struct gpiod_chip *chip);
    void (*line_iter_free)(struct gpiod_line_iter *iter);
    struct gpiod_line *(*line_iter_next)(struct gpiod_line_iter *iter);
} libgpiod;

//#define gpiod_chip_iter_next_noclose libgpiod.chip_iter_next_noclose
//#define gpiod_line_iter_next libgpiod.line_iter_next

int GpioPlugin_ensureGpiodInitialized() {
    struct gpiod_chip_iter *chipiter;
    struct gpiod_line_iter *lineiter;
    struct gpiod_chip *chip;
    struct gpiod_line *line;
    int ok, i, j;

    if (gpio_plugin.initialized) return 0;
    
    libgpiod.handle = dlopen("libgpiod.so", RTLD_LOCAL | RTLD_LAZY);
    if (!libgpiod.handle) {
        perror("could not load libgpiod.so");
        return errno;
    }

    LOAD_GPIOD_PROC(chip_close);
    LOAD_GPIOD_PROC(chip_name);
    LOAD_GPIOD_PROC(chip_label);
    LOAD_GPIOD_PROC(chip_num_lines);

    LOAD_GPIOD_PROC(line_name);
    LOAD_GPIOD_PROC(line_consumer);
    LOAD_GPIOD_PROC(line_direction);
    LOAD_GPIOD_PROC(line_active_state);
    LOAD_GPIOD_PROC(line_bias);
    LOAD_GPIOD_PROC(line_is_used);
    LOAD_GPIOD_PROC(line_is_open_drain);
    LOAD_GPIOD_PROC(line_is_open_source);
    LOAD_GPIOD_PROC(line_update);
    LOAD_GPIOD_PROC(line_request);
    LOAD_GPIOD_PROC(line_is_requested);
    LOAD_GPIOD_PROC(line_is_free);
    LOAD_GPIOD_PROC(line_get_value);
    LOAD_GPIOD_PROC(line_set_value);
    LOAD_GPIOD_PROC(line_set_config);

    LOAD_GPIOD_PROC(chip_iter_new);
    LOAD_GPIOD_PROC(chip_iter_free_noclose);
    LOAD_GPIOD_PROC(chip_iter_next_noclose);

    LOAD_GPIOD_PROC(line_iter_new);
    LOAD_GPIOD_PROC(line_iter_free);
    LOAD_GPIOD_PROC(line_iter_next);


    // iterate through the GPIO chips
    chipiter = libgpiod.chip_iter_new();
    if (!chipiter) {
        perror("could not create GPIO chip iterator");
        return errno;
    }
    
    for (gpio_plugin.n_chips = 0, gpio_plugin.n_lines = 0, chip = libgpiod.chip_iter_next_noclose(chipiter);
        chip;
        gpio_plugin.n_chips++, chip = libgpiod.chip_iter_next_noclose(chipiter))
    {
        gpio_plugin.chips[gpio_plugin.n_chips] = chip;
        gpio_plugin.n_lines += libgpiod.chip_num_lines(chip);
    }
    libgpiod.chip_iter_free_noclose(chipiter);


    // prepare the GPIO line list
    gpio_plugin.lines = calloc(gpio_plugin.n_lines, sizeof(struct gpiod_line*));
    if (!gpio_plugin.lines) {
        perror("could not allocate memory for GPIO line list");
        return errno;
    }

    // iterate through the chips and put all lines into the list
    for (i = 0, j = 0; i < gpio_plugin.n_chips; i++) {
        lineiter = libgpiod.line_iter_new(gpio_plugin.chips[i]);
        if (!lineiter) {
            perror("could not create new GPIO line iterator");
            return errno;
        }

        for (line = libgpiod.line_iter_next(lineiter); line; line = libgpiod.line_iter_next(lineiter), j++)
            gpio_plugin.lines[j] = line;

        libgpiod.line_iter_free(lineiter);
    }
    
    gpio_plugin.initialized = true;
    return 0;
}

int GpioPlugin_onGpiodMethodCall(char *channel, struct ChannelObject *object, FlutterPlatformMessageResponseHandle *responsehandle) {
    unsigned int chip_index, line_index;
    int ok;

    if STREQ("getNumChips", object->method) {
        // check arg
        if (!STDVALUE_IS_NULL(object->stdarg)) {
            return PlatformChannel_respondError(
                responsehandle,
                kStandardMethodCallResponse,
                "illegalargument",
                "expected null as the argument",
                NULL
            );
        }

        // init GPIO
        ok = GpioPlugin_ensureGpiodInitialized();
        if (ok != 0) {
            return PlatformChannel_respondError(
                responsehandle,
                kStandardMethodCallResponse,
                "couldnotinit",
                "gpio-plugin failed to initialize. see flutter-pi log for details.",
                NULL
            );
        }

        // return num chips
        return PlatformChannel_respond(
            responsehandle,
            &(struct ChannelObject) {
                .codec = kStandardMethodCallResponse,
                .success = true,
                .stdresult = {
                    .type = kInt32,
                    .int32_value = gpio_plugin.n_chips
                }
            }
        );

    } else if STREQ("getChipDetails", object->method) {
        // check arg
        if (!STDVALUE_IS_INT(object->stdarg)) {
            return PlatformChannel_respondError(
                responsehandle,
                kStandardMethodCallResponse,
                "illegalargument",
                "expected chip index as an integer argument.",
                NULL
            );
        }

        // init GPIO
        ok = GpioPlugin_ensureGpiodInitialized();
        if (ok != 0) {
            return PlatformChannel_respondError(
                responsehandle,
                kStandardMethodCallResponse,
                "couldnotinit",
                "gpio-plugin failed to initialize. see flutter-pi log for details.",
                NULL
            );
        }

        chip_index = (unsigned int) STDVALUE_AS_INT(object->stdarg);
        if (chip_index >= gpio_plugin.n_chips) {
            return PlatformChannel_respondError(
                responsehandle,
                kStandardMethodCallResponse,
                "illegalargument",
                "chip index out of range",
                NULL
            );
        }

        // return chip details
        return PlatformChannel_respond(
            responsehandle,
            &(struct ChannelObject) {
                .codec = kStandardMethodCallResponse,
                .success = true,
                .stdresult = {
                    .type = kMap,
                    .size = 3,
                    .keys = (struct StdMsgCodecValue[3]) {
                        {.type = kString, .string_value = "name"},
                        {.type = kString, .string_value = "label"},
                        {.type = kString, .string_value = "numLines"},
                    },
                    .values = (struct StdMsgCodecValue[3]) {
                        {.type = kString, .string_value = libgpiod.chip_name(gpio_plugin.chips[chip_index])},
                        {.type = kString, .string_value = libgpiod.chip_label(gpio_plugin.chips[chip_index])},
                        {.type = kString, .string_value = libgpiod.chip_num_lines(gpio_plugin.chips[chip_index])},
                    }
                }
            }
        );
    } else if STREQ("getLineHandle", object->method) {
        // check arg
        if (!(STDVALUE_IS_SIZED_LIST(object->stdarg, 2) && STDVALUE_IS_INT(object->stdarg.list[0])
              && STDVALUE_IS_INT(object->stdarg.list[0]))) {
            return PlatformChannel_respondError(
                responsehandle,
                kStandardMethodCallResponse,
                "illegalargument",
                "expected list containing two integers as the argument",
                NULL
            );
        }

        // init GPIO
        ok = GpioPlugin_ensureGpiodInitialized();
        if (ok != 0) {
            return PlatformChannel_respondError(
                responsehandle,
                kStandardMethodCallResponse,
                "couldnotinit",
                "gpio-plugin failed to initialize. see flutter-pi log for details.",
                NULL
            );
        }

        chip_index = STDVALUE_AS_INT(object->stdarg.list[0]);
        line_index = STDVALUE_AS_INT(object->stdarg.list[1]);

        if (chip_index >= gpio_plugin.n_chips) {
            return PlatformChannel_respondError(
                responsehandle,
                kStandardMethodCallResponse,
                "illegalargument",
                "chip index out of range",
                NULL
            );
        }

        if (line_index >= libgpiod.chip_num_lines(gpio_plugin.chips[chip_index])) {
            return PlatformChannel_respondError(
                responsehandle,
                kStandardMethodCallResponse,
                "illegalargument",
                "line index out of range",
                NULL
            );
        }

        for (int i = 0; i < chip_index; i++)
            line_index += libgpiod.chip_num_lines(gpio_plugin.chips[i]);

        return PlatformChannel_respond(
            responsehandle,
            &(struct ChannelObject) {
                .codec = kStandardMethodCallResponse,
                .success = true,
                .stdresult = {
                    .type = kInt32,
                    .int32_value = line_index
                }
            }
        );
    } else if STREQ("getLineDetails", object->method) {
        // check arg
        if (!STDVALUE_IS_INT(object->stdarg)) {
            return PlatformChannel_respondError(
                responsehandle,
                kStandardMethodCallResponse,
                "illegalargument",
                "expected line handle (integer) as the argument",
                NULL
            );
        }

        // init GPIO
        ok = GpioPlugin_ensureGpiodInitialized();
        if (ok != 0) {
            return PlatformChannel_respondError(
                responsehandle,
                kStandardMethodCallResponse,
                "couldnotinit",
                "gpio-plugin failed to initialize. see flutter-pi log for details.",
                NULL
            );
        }

        line_index = STDVALUE_AS_INT(object->stdarg);
        if (line_index >= gpio_plugin.n_lines) {
            return PlatformChannel_respondError(
                responsehandle,
                kStandardMethodCallResponse,
                "illegalargument",
                "invalid line handle",
                NULL
            );
        }

        struct gpiod_line *line = gpio_plugin.lines[line_index];

        // if we don't own the line, update it
        if (!libgpiod.line_is_requested(line))
            libgpiod.line_update(line);

        char *direction = libgpiod.line_direction(line) == GPIOD_LINE_DIRECTION_INPUT ?
                              "GpioLineDirection.input" : "GpioLineDirection.output";
        char *activeState = libgpiod.line_active_state(line) == GPIOD_LINE_ACTIVE_STATE_HIGH ?
                            "GpioLineActiveState.high" : "GpioLineActiveState.low";
        
        char *biasStr = "GpioLineBias.asIs";
        if (libgpiod.line_bias) {
            int bias = libgpiod.line_bias(line);
            biasStr = bias == GPIOD_LINE_BIAS_DISABLE ? "GpioLineBias.disable" :
                      bias == GPIOD_LINE_BIAS_PULL_UP ? "GpioLineBias.pullUp" :
                      "GpioLineBias.pullDown";
        }

        // return chip details
        return PlatformChannel_respond(
            responsehandle,
            &(struct ChannelObject) {
                .codec = kStandardMethodCallResponse,
                .success = true,
                .stdresult = {
                    .type = kMap,
                    .size = 10,
                    .keys = (struct StdMsgCodecValue[10]) {
                        {.type = kString, .string_value = "name"},
                        {.type = kString, .string_value = "consumer"},
                        {.type = kString, .string_value = "direction"},
                        {.type = kString, .string_value = "activeState"},
                        {.type = kString, .string_value = "bias"},
                        {.type = kString, .string_value = "isUsed"},
                        {.type = kString, .string_value = "openDrain"},
                        {.type = kString, .string_value = "openSource"},
                        {.type = kString, .string_value = "isRequested"},
                        {.type = kString, .string_value = "isFree"}
                    },
                    .values = (struct StdMsgCodecValue[10]) {
                        {.type = kString, .string_value = libgpiod.line_name(line)},
                        {.type = kString, .string_value = libgpiod.line_consumer(line)},
                        {.type = kString, .string_value = direction},
                        {.type = kString, .string_value = activeState},
                        {.type = kString, .string_value = biasStr},
                        {.type = libgpiod.line_is_used(line) ? kTrue : kFalse},
                        {.type = libgpiod.line_is_open_drain(line) ? kTrue : kFalse},
                        {.type = libgpiod.line_is_open_source(line) ? kTrue : kFalse},
                        {.type = libgpiod.line_is_requested(line) ? kTrue : kFalse},
                        {.type = libgpiod.line_is_free(line) ? kTrue : kFalse}
                    }
                }
            }
        );
    } else if STREQ("requestLine", object->method) {

    } else if STREQ("releaseLine", object->method) {

    } else if STREQ("reconfigureLine", object->method) {

    } else if STREQ("getLineValue", object->method) {

    } else if STREQ("setLineValue", object->method) {

    }
}

int GpioPlugin_init(void) {
    printf("[gpio-plugin] init.\n");

    gpio_plugin.initialized = false;

    PluginRegistry_setReceiver(GPIO_PLUGIN_GPIOD_METHOD_CHANNEL, kStandardMethodCall, GpioPlugin_onGpiodMethodCall);
}

int GpioPlugin_deinit(void) {
    printf("[gpio-plugin] deinit.\n");
}