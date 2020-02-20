#include <errno.h>
#include <stdio.h>
#include <dlfcn.h>
#include <poll.h>
#include <pthread.h>
#include <sys/epoll.h>

#include <gpiod.h>

#include <pluginregistry.h>
#include <plugins/gpiod.h>



struct {
    bool initialized;
    bool line_event_listener_should_run;
    
    struct gpiod_chip *chips[GPIO_PLUGIN_MAX_CHIPS];
    size_t n_chips;
    
    // complete list of GPIO lines
    struct gpiod_line **lines;
    size_t n_lines;

    // GPIO lines that flutter is currently listening to
    int epollfd;
    pthread_mutex_t listening_lines_mutex;
    struct gpiod_line_bulk listening_lines;
    pthread_t line_event_listener_thread;
    bool should_emit_events;
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
    int (*line_release)(struct gpiod_line *line);
    bool (*line_is_requested)(struct gpiod_line *line);
    bool (*line_is_free)(struct gpiod_line *line);
    int (*line_get_value)(struct gpiod_line *line);
    int (*line_set_value)(struct gpiod_line *line, int value);
    int (*line_set_config)(struct gpiod_line *line, int direction, int flags, int value);
    int (*line_event_wait_bulk)(struct gpiod_line_bulk *bulk, const struct timespec *timeout, struct gpiod_line_bulk *event_bulk);
    int (*line_event_read)(struct gpiod_line *line, struct gpiod_line_event *event);
    int (*line_event_get_fd)(struct gpiod_line *line);
    struct gpiod_chip *(*line_get_chip)(struct gpiod_line *line);
    
    // chip iteration
    struct gpiod_chip_iter *(*chip_iter_new)(void);
    void (*chip_iter_free_noclose)(struct gpiod_chip_iter *iter);
    struct gpiod_chip *(*chip_iter_next_noclose)(struct gpiod_chip_iter *iter);

    // line iteration
    struct gpiod_line_iter *(*line_iter_new)(struct gpiod_chip *chip);
    void (*line_iter_free)(struct gpiod_line_iter *iter);
    struct gpiod_line *(*line_iter_next)(struct gpiod_line_iter *iter);

    // misc
    const char *(*version_string)(void);
} libgpiod;

struct line_config {
    struct gpiod_line *line;
    int direction;
    int request_type;
    int initial_value;
    uint8_t flags;
};

// because libgpiod doesn't provide it, but it's useful
static inline void gpiod_line_bulk_remove(struct gpiod_line_bulk *bulk, struct gpiod_line *line) {
    struct gpiod_line *linetemp, **cursor;
    struct gpiod_line_bulk new_bulk = GPIOD_LINE_BULK_INITIALIZER;

    gpiod_line_bulk_foreach_line(bulk, linetemp, cursor) {
        if (linetemp != line)
            gpiod_line_bulk_add(&new_bulk, linetemp);
    }

    memcpy(bulk, &new_bulk, sizeof(struct gpiod_line_bulk));
}

static void *gpiodp_io_loop(void *userdata);

/// ensures the libgpiod binding and the `gpio_plugin` chips list and line map is initialized.
static int gpiodp_ensure_gpiod_initialized(void) {
    struct gpiod_chip_iter *chipiter;
    struct gpiod_line_iter *lineiter;
    struct gpiod_chip *chip;
    struct gpiod_line *line;
    int ok, i, j, fd;

    if (gpio_plugin.initialized) return 0;
    
    libgpiod.handle = dlopen("libgpiod.so", RTLD_LOCAL | RTLD_LAZY);
    if (!libgpiod.handle) {
        perror("[flutter_gpiod] could not load libgpiod.so. dlopen");
        return errno;
    }

    LOAD_GPIOD_PROC(chip_close);
    LOAD_GPIOD_PROC(chip_name);
    LOAD_GPIOD_PROC(chip_label);
    LOAD_GPIOD_PROC(chip_num_lines);

    LOAD_GPIOD_PROC(line_offset);
    LOAD_GPIOD_PROC(line_name);
    LOAD_GPIOD_PROC(line_consumer);
    LOAD_GPIOD_PROC(line_direction);
    LOAD_GPIOD_PROC(line_active_state);
    LOAD_GPIOD_PROC_OPTIONAL(line_bias);
    LOAD_GPIOD_PROC(line_is_used);
    LOAD_GPIOD_PROC(line_is_open_drain);
    LOAD_GPIOD_PROC(line_is_open_source);
    LOAD_GPIOD_PROC(line_update);
    LOAD_GPIOD_PROC(line_request);
    LOAD_GPIOD_PROC(line_release);
    LOAD_GPIOD_PROC(line_is_requested);
    LOAD_GPIOD_PROC(line_is_free);
    LOAD_GPIOD_PROC(line_get_value);
    LOAD_GPIOD_PROC(line_set_value);
    LOAD_GPIOD_PROC_OPTIONAL(line_set_config);
    LOAD_GPIOD_PROC(line_event_wait_bulk);
    LOAD_GPIOD_PROC(line_event_read);
    LOAD_GPIOD_PROC(line_event_get_fd);
    LOAD_GPIOD_PROC(line_get_chip);

    LOAD_GPIOD_PROC(chip_iter_new);
    LOAD_GPIOD_PROC(chip_iter_free_noclose);
    LOAD_GPIOD_PROC(chip_iter_next_noclose);

    LOAD_GPIOD_PROC(line_iter_new);
    LOAD_GPIOD_PROC(line_iter_free);
    LOAD_GPIOD_PROC(line_iter_next);

    LOAD_GPIOD_PROC(version_string);


    // iterate through the GPIO chips
    chipiter = libgpiod.chip_iter_new();
    if (!chipiter) {
        perror("[flutter_gpiod] could not create GPIO chip iterator. gpiod_chip_iter_new");
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

    fd = epoll_create1(0);
    if (fd == -1) {
        perror("[flutter_gpiod] Could not create line event listen epoll");
        return errno;
    }

    gpio_plugin.listening_lines = (struct gpiod_line_bulk) GPIOD_LINE_BULK_INITIALIZER;
    gpio_plugin.listening_lines_mutex = (pthread_mutex_t) PTHREAD_MUTEX_INITIALIZER;
    gpio_plugin.line_event_listener_should_run = true;

    ok = pthread_create(
        &gpio_plugin.line_event_listener_thread,
        NULL,
        gpiodp_io_loop,
        NULL
    );
    if (ok == -1) {
        perror("[flutter_gpiod] could not create line event listener thread");
        return errno;
    }

    gpio_plugin.initialized = true;
    return 0;
}

/// Sends a platform message to `handle` saying that the libgpiod binding has failed to initialize.
/// Should be called when `gpiodp_ensure_gpiod_initialized()` has failed.
static int gpiodp_respond_init_failed(FlutterPlatformMessageResponseHandle *handle) {
    return platch_respond_error_std(
        handle,
        "couldnotinit",
        "flutter_gpiod failed to initialize libgpiod bindings. See flutter-pi log for details.",
        NULL
    );
}

/// Sends a platform message to `handle` with error code "illegalargument"
/// and error messsage "supplied line handle is not valid".
static int gpiodp_respond_illegal_line_handle(FlutterPlatformMessageResponseHandle *handle) {
    return platch_respond_illegal_arg_std(handle, "supplied line handle is not valid");
}

static int gpiodp_respond_not_supported(FlutterPlatformMessageResponseHandle *handle, char *msg) {
    return platch_respond_error_std(handle, "notsupported", msg, NULL);
}

static int gpiodp_get_config(struct std_value *value,
                      struct line_config *conf_out,
                      FlutterPlatformMessageResponseHandle *responsehandle) {
    struct std_value *temp;
    unsigned int line_handle;
    bool has_bias;
    int ok;

    conf_out->direction = 0;
    conf_out->request_type = 0;
    conf_out->flags = 0;

    if ((!value) || (value->type != kStdMap)) {
        ok = platch_respond_illegal_arg_std(
            responsehandle,
            "Expected `arg` to be a `Map<String, dynamic>`"
        );
        if (ok != 0) return ok;

        return EINVAL;
    }

    // get the line handle from the argument map
    temp = stdmap_get_str(value, "lineHandle");
    if (temp && STDVALUE_IS_INT(*temp)) {
        line_handle = STDVALUE_AS_INT(*temp);
    } else {
        ok = platch_respond_illegal_arg_std(
            responsehandle,
            "Expected `arg['lineHandle']` to be an integer."
        );
        if (ok != 0) return ok;

        return EINVAL;
    }

    // get the corresponding gpiod line
    if (line_handle < gpio_plugin.n_lines) {
        conf_out->line = gpio_plugin.lines[line_handle];
    } else {
        ok = gpiodp_respond_illegal_line_handle(responsehandle);
        if (ok != 0) return ok;

        return EINVAL;
    }

    // get the direction
    temp = stdmap_get_str(value, "direction");
    if (temp && (temp->type == kStdString)) {
        if STREQ("LineDirection.input", temp->string_value) {
            conf_out->direction = GPIOD_LINE_DIRECTION_INPUT;
            conf_out->request_type = GPIOD_LINE_REQUEST_DIRECTION_INPUT;
        } else if STREQ("LineDirection.output", temp->string_value) {
            conf_out->direction = GPIOD_LINE_DIRECTION_OUTPUT;
            conf_out->request_type = GPIOD_LINE_REQUEST_DIRECTION_OUTPUT;
        } else {
            goto invalid_direction;
        }
    } else {
        invalid_direction:

        ok = platch_respond_illegal_arg_std(
            responsehandle, 
            "Expected `arg['direction']` to be a string-ification of `LineDirection`."
        );
        if (ok != 0) return ok;

        return EINVAL;
    }

    // get the output mode
    temp = stdmap_get_str(value, "outputMode");
    if ((!temp) || STDVALUE_IS_NULL(*temp)) {
        if (conf_out->direction == GPIOD_LINE_DIRECTION_OUTPUT) {
            goto invalid_output_mode;
        }
    } else if (temp && temp->type == kStdString) {
        if (conf_out->direction == GPIOD_LINE_DIRECTION_INPUT) {
            goto invalid_output_mode;
        }

        if STREQ("OutputMode.pushPull", temp->string_value) {
            // do nothing
        } else if STREQ("OutputMode.openDrain", temp->string_value) {
            conf_out->flags |= GPIOD_LINE_REQUEST_FLAG_OPEN_DRAIN;
        } else if STREQ("OutputMode.openSource", temp->string_value) {
            conf_out->flags |= GPIOD_LINE_REQUEST_FLAG_OPEN_SOURCE;
        } else {
            goto invalid_output_mode;
        }
    } else {
        invalid_output_mode:

        ok = platch_respond_illegal_arg_std(
            responsehandle, 
            "Expected `arg['outputMode']` to be a string-ification "
            "of [OutputMode] when direction is output, "
            "null when direction is input."
        );
        if (ok != 0) return ok;

        return EINVAL;
    }
    
    // get the bias
    has_bias = false;
    temp = stdmap_get_str(value, "bias");
    if ((!temp) || STDVALUE_IS_NULL(*temp)) {
        // don't need to set any flags
    } else if (temp && temp->type == kStdString) {
        if STREQ("Bias.disable", temp->string_value) {
            conf_out->flags |= GPIOD_LINE_REQUEST_FLAG_BIAS_DISABLE;
            has_bias = true;
        } else if STREQ("Bias.pullUp", temp->string_value) {
            conf_out->flags |= GPIOD_LINE_REQUEST_FLAG_BIAS_PULL_UP;
            has_bias = true;
        } else if STREQ("Bias.pullDown", temp->string_value) {
            conf_out->flags |= GPIOD_LINE_REQUEST_FLAG_BIAS_PULL_DOWN;
            has_bias = true;
        } else {
            goto invalid_bias;
        }
    } else {
        invalid_bias:

        ok = platch_respond_illegal_arg_std(
            responsehandle, 
            "Expected `arg['bias']` to be a stringification of [Bias] or null."
        );
        if (ok != 0) return ok;

        return EINVAL;
    }

    if (has_bias && !libgpiod.line_bias) {
        ok = gpiodp_respond_not_supported(
            responsehandle,
            "Setting line bias is not supported on this platform. "
            "Expected `arg['bias']` to be null."
        );

        if (ok != 0) return ok;
        return ENOTSUP;
    }

    // get the initial value
    conf_out->initial_value = 0;
    temp = stdmap_get_str(value, "initialValue");
    if ((!temp) || STDVALUE_IS_NULL(*temp)) {
        if (conf_out->direction == GPIOD_LINE_DIRECTION_INPUT) {
            // do nothing.
        } else if (conf_out->direction == GPIOD_LINE_DIRECTION_OUTPUT) {
            goto invalid_initial_value;
        }
    } else if (temp && STDVALUE_IS_BOOL(*temp)) {
        if (conf_out->direction == GPIOD_LINE_DIRECTION_INPUT) {
            goto invalid_initial_value;
        } else if (conf_out->direction == GPIOD_LINE_DIRECTION_OUTPUT) {
            conf_out->initial_value = STDVALUE_AS_BOOL(*temp) ? 1 : 0;
        }
    } else {
        invalid_initial_value:

        ok = platch_respond_illegal_arg_std(
            responsehandle, 
            "Expected `arg['initialValue']` to be null if direction is input, "
            "a bool if direction is output."
        );
        if (ok != 0) return ok;

        return EINVAL;
    }

    // get the active state
    temp = stdmap_get_str(value, "activeState");
    if (temp && (temp->type == kStdString)) {
        if STREQ("ActiveState.low", temp->string_value) {
            conf_out->flags |= GPIOD_LINE_REQUEST_FLAG_ACTIVE_LOW;
        } else if STREQ("ActiveState.high", temp->string_value) {
            // do nothing
        } else {
            goto invalid_active_state;
        }
    } else {
        invalid_active_state:

        ok = platch_respond_illegal_arg_std(
            responsehandle, 
            "Expected `arg['activeState']` to be a stringification of [ActiveState]."
        );
        if (ok != 0) return ok;

        return EINVAL;
    }

    return 0;
}

/// Runs on it's own thread. Waits for events
/// on any of the lines in `gpio_plugin.listening_lines`
/// and sends them on to the event channel, if someone
/// is listening on it.
static void *gpiodp_io_loop(void *userdata) {
    struct gpiod_line_event event;
    struct gpiod_line *line, **cursor;
    struct gpiod_chip *chip;
    struct epoll_event fdevents[10];
    unsigned int line_handle;
    bool is_ready;
    int ok, n_fdevents;

    while (gpio_plugin.line_event_listener_should_run) {
        // epoll luckily is concurrent. Other threads can add and remove fd's from this
        // epollfd while we're waiting on it.
        ok = epoll_wait(gpio_plugin.epollfd, fdevents, 10, -1);
        if ((ok == -1) && (errno != EINTR)) {
            perror("[flutter_gpiod] error while waiting for line events, epoll"); 
            continue;
        } else {
            n_fdevents = ok;
        }

        pthread_mutex_lock(&gpio_plugin.listening_lines_mutex);

        // Go through all the lines were listening to right now and find out,
        // check for each line whether an event ocurred on the line's fd.
        // If that's the case, read the events and send them to flutter.
        gpiod_line_bulk_foreach_line(&gpio_plugin.listening_lines, line, cursor) {    
            for (int i = 0, is_ready = false; i < n_fdevents; i++) {
                if (fdevents[i].data.fd == libgpiod.line_event_get_fd(line)) {
                    is_ready = true;
                    break;
                }
            }
            if (!is_ready) continue;

            // read the line events
            ok = libgpiod.line_event_read(line, &event);
            if (ok == -1) {
                perror("[flutter_gpiod] Could not read events from GPIO line. gpiod_line_event_read");
                continue;
            }

            // if currently noone's listening to the
            // flutter_gpiod event channel, we don't send anything
            // to flutter and discard the events.
            if (!gpio_plugin.should_emit_events) continue;

            // convert the gpiod_line to a flutter_gpiod line handle.
            chip = libgpiod.line_get_chip(line);

            line_handle = libgpiod.line_offset(line);
            for (int i = 0; gpio_plugin.chips[i] != chip; i++)
                line_handle += libgpiod.chip_num_lines(chip);
            
            // finally send the event to the event channel.
            ok = platch_send_success_event_std(
                GPIOD_PLUGIN_EVENT_CHANNEL,
                &(struct std_value) {
                    .type = kStdList,
                    .size = 3,
                    .list = (struct std_value[3]) {
                        {.type = kStdInt32, .int32_value = line_handle},
                        {
                            .type = kStdString,
                            .string_value = 
                                event.event_type == GPIOD_LINE_EVENT_FALLING_EDGE?
                                    "SignalEdge.falling" :
                                    "SignalEdge.rising"
                        },
                        {
                            .type = kStdInt64Array, // use int64's here so we don't get any unexpected overflows.
                            .size = 2,
                            .int64array = (int64_t[2]) {event.ts.tv_sec, event.ts.tv_nsec}
                        }
                    }
                }
            );
        }

        pthread_mutex_unlock(&gpio_plugin.listening_lines_mutex);
    }
    
    return NULL;
}


static int gpiodp_get_num_chips(struct platch_obj *object,
                         FlutterPlatformMessageResponseHandle *responsehandle) {
    int ok;
    
    ok = gpiodp_ensure_gpiod_initialized();
    if (ok != 0) {
        return gpiodp_respond_init_failed(responsehandle);
    }

    return platch_respond_success_std(responsehandle, &STDINT32(gpio_plugin.n_chips));
}

static int gpiodp_get_chip_details(struct platch_obj *object,
                            FlutterPlatformMessageResponseHandle *responsehandle) {
    struct gpiod_chip *chip;
    unsigned int chip_index;
    int ok;
    
    // check the argument
    if (STDVALUE_IS_INT(object->std_arg)) {
        chip_index = STDVALUE_AS_INT(object->std_arg);
    } else {
        return platch_respond_illegal_arg_std(
            responsehandle,
            "Expected `arg` to be an integer."
        );
    }

    // init GPIO
    ok = gpiodp_ensure_gpiod_initialized();
    if (ok != 0) {
        return gpiodp_respond_init_failed(responsehandle);
    }

    // get the chip index
    if (chip_index < gpio_plugin.n_chips) {
        chip = gpio_plugin.chips[chip_index];
    } else  {
        return platch_respond_illegal_arg_std(
            responsehandle,
            "Expected `arg` to be valid chip index."
        );
    }

    // return chip details
    return platch_respond_success_std(
        responsehandle,
        &(struct std_value) {
            .type = kStdMap,
            .size = 3,
            .keys = (struct std_value[3]) {
                {.type = kStdString, .string_value = "name"},
                {.type = kStdString, .string_value = "label"},
                {.type = kStdString, .string_value = "numLines"},
            },
            .values = (struct std_value[3]) {
                {.type = kStdString, .string_value = (char*) libgpiod.chip_name(chip)},
                {.type = kStdString, .string_value = (char*) libgpiod.chip_label(chip)},
                {.type = kStdInt32,  .int32_value  = libgpiod.chip_num_lines(chip)},
            }
        }
    );
}

static int gpiodp_get_line_handle(struct platch_obj *object,
                           FlutterPlatformMessageResponseHandle *responsehandle) {
    struct gpiod_chip *chip;
    unsigned int chip_index, line_index;
    int ok;
    
    // check arg
    if (STDVALUE_IS_LIST(object->std_arg)) {
        if (STDVALUE_IS_INT(object->std_arg.list[0])) {
            chip_index = STDVALUE_AS_INT(object->std_arg.list[0]);
        } else {
            return platch_respond_illegal_arg_std(
                responsehandle,
                "Expected `arg[0]` to be an integer."
            );
        }
        
        if (STDVALUE_IS_INT(object->std_arg.list[1])) {
            line_index = STDVALUE_AS_INT(object->std_arg.list[1]);
        } else {
            return platch_respond_illegal_arg_std(
                responsehandle,
                "Expected `arg[1]` to be an integer."
            );
        }
    } else {
        return platch_respond_illegal_arg_std(
            responsehandle,
            "Expected `arg` to be a list with length 2."
        );
    }

    // try to init GPIO
    ok = gpiodp_ensure_gpiod_initialized();
    if (ok != 0) {
        return gpiodp_respond_init_failed(responsehandle);
    }

    // try to get the chip correspondig to the chip index
    if (chip_index < gpio_plugin.n_chips) {
        chip = gpio_plugin.chips[chip_index];
    } else {
        return platch_respond_illegal_arg_std(
            responsehandle,
            "Expected `arg[0]` to be a valid chip index."
        );
    }
    
    // check if the line index is in range
    if (line_index >= libgpiod.chip_num_lines(chip)) {
        return platch_respond_illegal_arg_std(
            responsehandle,
            "Expected `arg[1]` to be a valid line index."
        );
    }

    // transform the line index into a line handle
    for (int i = 0; i < chip_index; i++)
        line_index += libgpiod.chip_num_lines(gpio_plugin.chips[i]);

    return platch_respond_success_std(responsehandle, &STDINT32(line_index));
}

static int gpiodp_get_line_details(struct platch_obj *object,
                            FlutterPlatformMessageResponseHandle *responsehandle) {
    struct gpiod_line *line;
    unsigned int line_handle;
    char *name, *consumer;
    char *direction_str, *bias_str, *output_mode_str, *active_state_str;
    bool open_source, open_drain;
    int direction, bias;
    int ok;
    
    // check arg
    if (STDVALUE_IS_INT(object->std_arg)) {
        line_handle = STDVALUE_AS_INT(object->std_arg);
    } else {
        return platch_respond_illegal_arg_std(
            responsehandle,
            "Expected `arg` to be an integer."
        );
    }

    // init GPIO
    ok = gpiodp_ensure_gpiod_initialized();
    if (ok != 0) {
        return gpiodp_respond_init_failed(responsehandle);
    }

    // try to get the gpiod line corresponding to the line handle
    if (line_handle < gpio_plugin.n_lines) {
        line = gpio_plugin.lines[line_handle];
    } else {
        return gpiodp_respond_illegal_line_handle(responsehandle);
    }    

    // if we don't own the line, update it
    if (!libgpiod.line_is_requested(line)) {
        libgpiod.line_update(line);
    }

    direction = libgpiod.line_direction(line);
    direction_str = direction == GPIOD_LINE_DIRECTION_INPUT ?
                            "LineDirection.input" : "LineDirection.output";

    active_state_str = libgpiod.line_active_state(line) == GPIOD_LINE_ACTIVE_STATE_HIGH ?
                        "ActiveState.high" : "ActiveState.low";
    
    bias_str = NULL;
    if (libgpiod.line_bias) {
        bias = libgpiod.line_bias(line);
        if (bias == GPIOD_LINE_BIAS_DISABLE) {
            bias_str = "Bias.disable";
        } else if (bias == GPIOD_LINE_BIAS_PULL_UP) {
            bias_str = "Bias.pullUp";
        } else {
            bias_str = "Bias.pullDown";
        }
    }

    output_mode_str = NULL;
    if (direction == GPIOD_LINE_DIRECTION_OUTPUT) {
        open_source = libgpiod.line_is_open_source(line);
        open_drain = libgpiod.line_is_open_drain(line);

        if (open_source) {
            output_mode_str = "OutputMode.openSource";
        } else if (open_drain) {
            output_mode_str = "OutputMode.openDrain";
        } else  {
            output_mode_str = "OutputMode.pushPull";
        }
    }
    

    name = (char*) libgpiod.line_name(line);
    consumer = (char*) libgpiod.line_consumer(line);

    // return line details
    return platch_respond_success_std(
        responsehandle,
        &(struct std_value) {
            .type = kStdMap,
            .size = 9,
            .keys = (struct std_value[9]) {
                {.type = kStdString, .string_value = "name"},
                {.type = kStdString, .string_value = "consumer"},
                {.type = kStdString, .string_value = "isUsed"},
                {.type = kStdString, .string_value = "isRequested"},
                {.type = kStdString, .string_value = "isFree"},
                {.type = kStdString, .string_value = "direction"},
                {.type = kStdString, .string_value = "outputMode"},
                {.type = kStdString, .string_value = "bias"},
                {.type = kStdString, .string_value = "activeState"}
            },
            .values = (struct std_value[9]) {
                {.type = name? kStdString : kStdNull, .string_value = name},
                {.type = consumer? kStdString : kStdNull, .string_value = consumer},
                {.type = libgpiod.line_is_used(line) ? kStdTrue : kStdFalse},
                {.type = libgpiod.line_is_requested(line) ? kStdTrue : kStdFalse},
                {.type = libgpiod.line_is_free(line) ? kStdTrue : kStdFalse},
                {.type = kStdString, .string_value = direction_str},
                {
                    .type = output_mode_str? kStdString : kStdNull,
                    .string_value = output_mode_str
                },
                {
                    .type = bias_str? kStdString : kStdNull,
                    .string_value = bias_str
                },
                {.type = kStdString, .string_value = active_state_str}
            }
        }
    );
}

static int gpiodp_request_line(struct platch_obj *object,
                        FlutterPlatformMessageResponseHandle *responsehandle) {
    struct line_config config;
    struct std_value *temp;
    bool is_event_line = false;
    char *consumer;
    int ok;
    
    // check that the arg is a map
    if (object->std_arg.type != kStdMap) {
        return platch_respond_illegal_arg_std(
            responsehandle,
            "Expected `arg` to be a `Map<String, dynamic>`"
        );
    }

    // ensure GPIO is initialized
    ok = gpiodp_ensure_gpiod_initialized();
    if (ok != 0) {
        return gpiodp_respond_init_failed(responsehandle);
    }

    temp = stdmap_get_str(&object->std_arg, "consumer");
    if (!temp || STDVALUE_IS_NULL(*temp)) {
        consumer = NULL;
    } else if (temp && (temp->type == kStdString)) {
        consumer = temp->string_value;
    } else {
        return platch_respond_illegal_arg_std(
            responsehandle, 
            "Expected `arg['consumer']` to be a string or null."
        );
    }

    // get the line config
    ok = gpiodp_get_config(&object->std_arg, &config, responsehandle);
    if (ok != 0) return ok;

    // get the triggers
    temp = stdmap_get_str(&object->std_arg, "triggers");
    if ((!temp) || STDVALUE_IS_NULL(*temp)) {
        if (config.direction == GPIOD_LINE_DIRECTION_INPUT) {
            goto invalid_triggers;
        }
    } else if (temp && STDVALUE_IS_LIST(*temp)) {
        if (config.direction == GPIOD_LINE_DIRECTION_OUTPUT) {
            goto invalid_triggers;
        }

        // iterate through elements in the trigger list.
        for (int i = 0; i < temp->size; i++) {
            if (temp->list[i].type != kStdString) {
                goto invalid_triggers;
            }

            // now update config.request_type accordingly.
            if STREQ("SignalEdge.falling", temp->list[i].string_value) {
                is_event_line = true;
                switch (config.request_type) {
                    case GPIOD_LINE_REQUEST_DIRECTION_INPUT:
                        config.request_type = GPIOD_LINE_REQUEST_EVENT_FALLING_EDGE;
                        break;
                    case GPIOD_LINE_REQUEST_EVENT_FALLING_EDGE:
                        break;
                    case GPIOD_LINE_REQUEST_EVENT_RISING_EDGE:
                    case GPIOD_LINE_REQUEST_EVENT_BOTH_EDGES:
                        config.request_type = GPIOD_LINE_REQUEST_EVENT_BOTH_EDGES;
                        break;
                    default: break;
                }
            } else if STREQ("SignalEdge.rising", temp->list[i].string_value) {
                is_event_line = true;
                switch (config.request_type) {
                    case GPIOD_LINE_REQUEST_DIRECTION_INPUT:
                        config.request_type = GPIOD_LINE_REQUEST_EVENT_RISING_EDGE;
                        break;
                    case GPIOD_LINE_REQUEST_EVENT_RISING_EDGE:
                        break;
                    case GPIOD_LINE_REQUEST_EVENT_FALLING_EDGE:
                    case GPIOD_LINE_REQUEST_EVENT_BOTH_EDGES:
                        config.request_type = GPIOD_LINE_REQUEST_EVENT_BOTH_EDGES;
                        break;
                    default: break;
                }
            } else {
                goto invalid_triggers;
            }
        }
    } else {
        invalid_triggers:
        return platch_respond_illegal_arg_std(
            responsehandle,
            "Expected `arg['triggers']` to be a `List<String>` of "
            "string-ifications of [SignalEdge] when direction is input "
            "(no null values in the list), null when direction is output."
        );
    }

    // finally request the line
    ok = libgpiod.line_request(
        config.line,
        &(struct gpiod_line_request_config) {
            .consumer = consumer,
            .request_type = config.request_type,
            .flags = config.flags
        },
        config.initial_value
    );
    if (ok == -1) {
        return platch_respond_native_error_std(responsehandle, errno);
    }

    if (is_event_line) {
        pthread_mutex_lock(&gpio_plugin.listening_lines_mutex);

        ok = epoll_ctl(gpio_plugin.epollfd,
                       EPOLL_CTL_ADD,
                       libgpiod.line_event_get_fd(config.line),
                       &(struct epoll_event) {.events = EPOLLPRI | EPOLLIN}
        );
        if (ok == -1) {
            perror("[flutter_gpiod] Could not add GPIO line to epollfd. epoll_ctl");
            libgpiod.line_release(config.line);
            return platch_respond_native_error_std(responsehandle, errno);
        }

        gpiod_line_bulk_add(&gpio_plugin.listening_lines, config.line);

        pthread_mutex_unlock(&gpio_plugin.listening_lines_mutex);
    }

    return platch_respond_success_std(responsehandle, NULL);
}

static int gpiodp_release_line(struct platch_obj *object,
                        FlutterPlatformMessageResponseHandle *responsehandle) {
    struct gpiod_line *line;
    unsigned int line_handle;
    int ok, fd;

    // get the line handle
    if (STDVALUE_IS_INT(object->std_arg)) {
        line_handle = STDVALUE_AS_INT(object->std_arg);
    } else {
        return platch_respond_illegal_arg_std(
            responsehandle,
            "Expected `arg` to be an integer."
        );
    }

    // get the corresponding gpiod line
    if (line_handle < gpio_plugin.n_lines) {
        line = gpio_plugin.lines[line_handle];
    } else {
        return gpiodp_respond_illegal_line_handle(responsehandle);
    }

    // Try to get the line associated fd and remove
    // it from the listening thread
    fd = libgpiod.line_event_get_fd(line);
    if (fd != -1) {
        pthread_mutex_lock(&gpio_plugin.listening_lines_mutex);

        gpiod_line_bulk_remove(&gpio_plugin.listening_lines, line);

        ok = epoll_ctl(gpio_plugin.epollfd, EPOLL_CTL_DEL, fd, NULL);
        if (ok == -1) {
            perror("[flutter_gpiod] Could not remove GPIO line from epollfd. epoll_ctl");
            return platch_respond_native_error_std(responsehandle, errno);
        }

        pthread_mutex_unlock(&gpio_plugin.listening_lines_mutex);
    }

    ok = libgpiod.line_release(line);
    if (ok == -1) {
        perror("[flutter_gpiod] Could not release line. gpiod_line_release");
        return platch_respond_native_error_std(responsehandle, errno);
    }

    return platch_respond_success_std(responsehandle, NULL);
}

static int gpiodp_reconfigure_line(struct platch_obj *object,
                            FlutterPlatformMessageResponseHandle *responsehandle) {
    struct line_config config;
    int ok;
    
    // ensure GPIO is initialized
    ok = gpiodp_ensure_gpiod_initialized();
    if (ok != 0) {
        return gpiodp_respond_init_failed(responsehandle);
    }

    ok = gpiodp_get_config(&object->std_arg, &config, responsehandle);
    if (ok != 0) return ok;

    if (!libgpiod.line_set_config) {
        return gpiodp_respond_not_supported(
            responsehandle,
            "Line reconfiguration is not supported on this platform."
        );
    }

    // finally temp the line
    ok = libgpiod.line_set_config(
        config.line,
        config.direction,
        config.flags,
        config.initial_value
    );
    if (ok == -1) {
        return platch_respond_native_error_std(responsehandle, errno);
    }

    return platch_respond_success_std(responsehandle, NULL);
}

static int gpiodp_get_line_value(struct platch_obj *object,
                          FlutterPlatformMessageResponseHandle *responsehandle) {
    struct gpiod_line *line;
    unsigned int line_handle;
    int ok;

    // get the line handle
    if (STDVALUE_IS_INT(object->std_arg)) {
        line_handle = STDVALUE_AS_INT(object->std_arg);
    } else {
        return platch_respond_illegal_arg_std(
            responsehandle,
            "Expected `arg` to be an integer."
        );
    }

    // get the corresponding gpiod line
    if (line_handle < gpio_plugin.n_lines) {
        line = gpio_plugin.lines[line_handle];
    } else {
        return gpiodp_respond_illegal_line_handle(responsehandle);
    }

    // get the line value
    ok = libgpiod.line_get_value(line);
    if (ok == -1) {
        return platch_respond_native_error_std(responsehandle, errno);
    }

    return platch_respond_success_std(responsehandle, &STDBOOL(ok));
}

static int gpiodp_set_line_value(struct platch_obj *object,
                          FlutterPlatformMessageResponseHandle *responsehandle) {
    struct std_value *temp;
    struct gpiod_line *line;
    unsigned int line_handle;
    bool value;
    int ok;

    if (STDVALUE_IS_SIZED_LIST(object->std_arg, 2)) {
        if (STDVALUE_IS_INT(object->std_arg.list[0])) {
            line_handle = STDVALUE_AS_INT(object->std_arg.list[0]);
        } else {
            return platch_respond_illegal_arg_std(
                responsehandle,
                "Expected `arg[0]` to be an integer."
            );
        }

        if (STDVALUE_IS_BOOL(object->std_arg.list[1])) {
            value = STDVALUE_AS_BOOL(object->std_arg.list[1]);
        } else {
            return platch_respond_illegal_arg_std(
                responsehandle,
                "Expected `arg[1]` to be a bool."
            );
        }
    } else {
        return platch_respond_illegal_arg_std(
            responsehandle,
            "Expected `arg` to be a list."
        );
    }

    // get the corresponding gpiod line
    if (line_handle < gpio_plugin.n_lines) {
        line = gpio_plugin.lines[line_handle];
    } else {
        return gpiodp_respond_illegal_line_handle(responsehandle);
    }

    // get the line value
    ok = libgpiod.line_set_value(line, value ? 1 : 0);
    if (ok == -1) {
        return platch_respond_native_error_std(responsehandle, errno);
    }

    return platch_respond_success_std(responsehandle, NULL);
}

static int gpiodp_supports_bias(struct platch_obj *object,
                         FlutterPlatformMessageResponseHandle *responsehandle) {
    int ok;
    
    // ensure GPIO is initialized
    ok = gpiodp_ensure_gpiod_initialized();
    if (ok != 0) {
        return gpiodp_respond_init_failed(responsehandle);
    }
    
    return platch_respond_success_std(responsehandle, &STDBOOL(libgpiod.line_bias));
}

static int gpiodp_supports_reconfiguration(struct platch_obj *object,
                                    FlutterPlatformMessageResponseHandle *responsehandle) {
    int ok;
    
    // ensure GPIO is initialized
    ok = gpiodp_ensure_gpiod_initialized();
    if (ok != 0) {
        return gpiodp_respond_init_failed(responsehandle);
    }
    
    return platch_respond_success_std(responsehandle, &STDBOOL(libgpiod.line_set_config));
}

/// Handles incoming platform messages. Calls the above methods.
int gpiodp_on_receive(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *responsehandle) {
    unsigned int chip_index, line_index;
    bool is_legal_arg;
    int ok;


    if STREQ("getNumChips", object->method) {
        return gpiodp_get_num_chips(object, responsehandle);
    } else if STREQ("getChipDetails", object->method) {
        return gpiodp_get_chip_details(object, responsehandle);
    } else if STREQ("getLineHandle", object->method) {
        return gpiodp_get_line_handle(object, responsehandle);
    } else if STREQ("getLineDetails", object->method) {
        return gpiodp_get_line_details(object, responsehandle);
    } else if STREQ("requestLine", object->method) {
        return gpiodp_request_line(object, responsehandle);
    } else if STREQ("releaseLine", object->method) {
        return gpiodp_release_line(object, responsehandle);
    } else if STREQ("reconfigureLine", object->method) {
        return gpiodp_reconfigure_line(object, responsehandle);
    } else if STREQ("getLineValue", object->method) {
        return gpiodp_get_line_value(object, responsehandle);
    } else if STREQ("setLineValue", object->method) {
        return gpiodp_set_line_value(object, responsehandle);
    } else if STREQ("supportsBias", object->method) {
        return gpiodp_supports_bias(object, responsehandle);
    } else if STREQ("supportsLineReconfiguration", object->method) {
        return gpiodp_supports_reconfiguration(object, responsehandle);
    }

    return platch_respond_not_implemented(responsehandle);
}

int gpiodp_on_receive_evch(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *responsehandle) {
    if STREQ("listen", object->method) {
        gpio_plugin.should_emit_events = true;
        return platch_respond_success_std(responsehandle, NULL);
    } else if STREQ("cancel", object->method) {
        gpio_plugin.should_emit_events = false;
        return platch_respond_success_std(responsehandle, NULL);
    }

    return platch_respond_not_implemented(responsehandle);
}


int gpiodp_init(void) {
    int ok;

    printf("[flutter_gpiod] Initializing...\n");

    gpio_plugin.initialized = false;

    ok = plugin_registry_set_receiver(GPIOD_PLUGIN_METHOD_CHANNEL, kStandardMethodCall, gpiodp_on_receive);
    if (ok != 0) return ok;

    plugin_registry_set_receiver(GPIOD_PLUGIN_EVENT_CHANNEL, kStandardMethodCall, gpiodp_on_receive_evch);
    if (ok != 0) return ok;

    printf("[flutter_gpiod] Done.\n");

    return 0;
}

int gpiodp_deinit(void) {
    printf("[flutter_gpiod] deinit.\n");
    return 0;
}