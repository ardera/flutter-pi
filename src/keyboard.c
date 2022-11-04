#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <locale.h>
#include <regex.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <linux/input.h>
#include <linux/keyboard.h>
#include <linux/kd.h>

#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-compose.h>

#include <collection.h>
#include <keyboard.h>

FILE_DESCR("keyboard")

static int find_var_offset_in_string(const char *varname, const char *buffer, regmatch_t *match) {
    regmatch_t matches[2];
    char *pattern;
    int ok;

    ok = asprintf(&pattern, "%s=\"([^\"]*)\"", varname);
    if (ok < 0) {
        ok = ENOMEM;
        goto fail_set_match;
    }

    regex_t regex;
    ok = regcomp(&regex, pattern, REG_EXTENDED);
    if (ok != 0) {
        //pregexerr("regcomp", ok, &regex);
        ok = EINVAL;
        goto fail_free_pattern;
    }

    ok = regexec(&regex, buffer, 2, matches, 0);
    if (ok == REG_NOMATCH) {
        ok = EINVAL;
        goto fail_free_regex;
    }

    if (match != NULL) {
        *match = matches[1];
    }

    free(pattern);
    regfree(&regex);

    return 0;

    fail_free_regex:
    regfree(&regex);

    fail_free_pattern:
    free(pattern);

    fail_set_match:
    if (match != NULL) {
        match->rm_so = -1;
        match->rm_eo = -1;
    }
    return ok;
}

static char *get_value_allocated(const char *varname, const char *buffer) {
    regmatch_t match;
    char *allocated;
    int ok, match_length;

    ok = find_var_offset_in_string(varname, buffer, &match);
    if (ok != 0) {
        errno = ok;
        return NULL;
    } else if ((match.rm_so == -1) || (match.rm_eo == -1)) {
        errno = EINVAL;
        return NULL;
    }

    match_length = match.rm_eo - match.rm_so;

    allocated = malloc(match_length + 1);
    if (allocated == NULL) {
        errno = ENOMEM;
        return NULL;
    }

    strncpy(allocated, buffer + match.rm_so, match_length);
    
    allocated[match_length] = '\0';
    
    return allocated;
}

static char *load_file(const char *path) {
    struct stat s;
    int ok, fd;
    
    ok = open(path, O_RDONLY);
    if (ok < 0) {
        goto fail_return_null;
    } else {
        fd = ok;
    }

    ok = fstat(fd, &s);
    if (ok < 0) {
        goto fail_close;
    }

    char *buffer = malloc(s.st_size + 1);
    if (buffer == NULL) {
        errno = ENOMEM;
        goto fail_close;
    }

    int result = read(fd, buffer, s.st_size);
    if (result < 0) {
        goto fail_close;
    } else if (result == 0) {
        errno = EINVAL;
        goto fail_close;
    } else if (result < s.st_size) {
        errno = EINVAL;
        goto fail_close;
    }

    close(fd);

    buffer[s.st_size] = '\0';

    return buffer;


    fail_close:
    close(fd);

    fail_return_null:
    return NULL;
}

static struct xkb_keymap *load_default_keymap(struct xkb_context *context) {
    struct xkb_keymap *keymap;
    char *file, *xkbmodel, *xkblayout, *xkbvariant, *xkboptions; 

    file = load_file("/etc/default/keyboard");
    if (file == NULL) {
        LOG_ERROR("Could not load keyboard configuration from \"/etc/default/keyboard\". Default keyboard config will be used. load_file: %s\n", strerror(errno));
        xkbmodel = NULL;
        xkblayout = NULL;
        xkbvariant = NULL;
        xkboptions = NULL;
    } else {
        // we have a config file, load its properties
        xkbmodel = get_value_allocated("XKBMODEL", file);
        if (xkbmodel == NULL) {
            LOG_ERROR("Could not find \"XKBMODEL\" property inside \"/etc/default/keyboard\". Default value will be used.");
        }

        xkblayout = get_value_allocated("XKBLAYOUT", file);
        if (xkblayout == NULL) {
            LOG_ERROR("Could not find \"XKBLAYOUT\" property inside \"/etc/default/keyboard\". Default value will be used.");
        }

        xkbvariant = get_value_allocated("XKBVARIANT", file);
        if (xkbvariant == NULL) {
            LOG_ERROR("Could not find \"XKBVARIANT\" property inside \"/etc/default/keyboard\". Default value will be used.");
        }

        xkboptions = get_value_allocated("XKBOPTIONS", file);
        if (xkboptions == NULL) {
            LOG_ERROR("Could not find \"XKBOPTIONS\" property inside \"/etc/default/keyboard\". Default value will be used.");
        }

        free(file);
    }

    struct xkb_rule_names names = {
        .rules = NULL,
        .model = xkbmodel,
        .layout = xkblayout,
        .variant = xkbvariant,
        .options = xkboptions
    };

    keymap = xkb_keymap_new_from_names(context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);

    if (xkbmodel != NULL) free(xkbmodel);
    if (xkblayout != NULL) free(xkblayout);
    if (xkbvariant != NULL) free(xkbvariant);
    if (xkboptions != NULL) free(xkboptions);

    if (keymap == NULL) {
        LOG_ERROR("Could not create xkb keymap.");
    }

    return keymap;
}

static struct xkb_compose_table *load_default_compose_table(struct xkb_context *context) {
    struct xkb_compose_table *tbl;

    setlocale(LC_ALL, "");
    
    tbl = xkb_compose_table_new_from_locale(context, setlocale(LC_CTYPE, NULL), XKB_COMPOSE_COMPILE_NO_FLAGS);
    if (tbl == NULL) {
        LOG_ERROR("Could not create compose table from locale.\n");
    }

    return tbl;
}


struct keyboard_config *keyboard_config_new(void) {
    struct keyboard_config *cfg;
    struct xkb_compose_table *compose_table;
    struct xkb_context *ctx;
    struct xkb_keymap *keymap;
    
    cfg = malloc(sizeof *cfg);
    if (cfg == NULL) {
        errno = ENOMEM;
        goto fail_return_null;
    }

    ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (ctx == NULL) {
        LOG_ERROR("Could not create XKB context.\n");
        goto fail_free_cfg;
    }

    compose_table = load_default_compose_table(ctx);
    if (compose_table == NULL) {
        goto fail_free_context;
    }

    keymap = load_default_keymap(ctx);
    if (keymap == NULL) {
        goto fail_free_compose_table;
    }

    cfg->context = ctx;
    cfg->default_compose_table = compose_table;
    cfg->default_keymap = keymap;

    return cfg;

    fail_free_compose_table:
    xkb_compose_table_unref(compose_table);

    fail_free_context:
    xkb_context_unref(ctx);

    fail_free_cfg:
    free(cfg);

    fail_return_null:
    return NULL;
}

void keyboard_config_destroy(struct keyboard_config *config) {
    xkb_keymap_unref(config->default_keymap);
    xkb_compose_table_unref(config->default_compose_table);
    xkb_context_unref(config->context);
    free(config);
}


struct keyboard_state *keyboard_state_new(
    struct keyboard_config *config,
    struct xkb_keymap *keymap_override,
    struct xkb_compose_table *compose_table_override
) {
    struct keyboard_state *state;
    struct xkb_compose_state *compose_state;
    struct xkb_state *xkb_state, *plain_xkb_state;

    state = malloc(sizeof *state);
    if (state == NULL) {
        errno = ENOMEM;
        goto fail_return_null;
    }

    xkb_state = xkb_state_new(keymap_override != NULL ? keymap_override : config->default_keymap);
    if (xkb_state == NULL) {
        LOG_ERROR("Could not create new XKB state.\n");
        goto fail_free_state;
    }

    plain_xkb_state = xkb_state_new(keymap_override != NULL ? keymap_override : config->default_keymap);
    if (plain_xkb_state == NULL) {
        LOG_ERROR("Could not create new XKB state.\n");
        goto fail_free_xkb_state;
    }

    compose_state = xkb_compose_state_new(compose_table_override != NULL ? compose_table_override : config->default_compose_table, XKB_COMPOSE_STATE_NO_FLAGS);
    if (compose_state == NULL) {
        LOG_ERROR("Could not create new XKB compose state.\n");
        goto fail_free_plain_xkb_state;
    }

    state->config = config;
    state->state = xkb_state;
    state->plain_state = plain_xkb_state;
    state->compose_state = compose_state;

    return state;

    fail_free_plain_xkb_state:
    xkb_state_unref(plain_xkb_state);

    fail_free_xkb_state:
    xkb_state_unref(xkb_state);

    fail_free_state:
    free(state);

    fail_return_null:
    return NULL;
}

void keyboard_state_destroy(
    struct keyboard_state *state
) {
    xkb_compose_state_unref(state->compose_state);
    xkb_state_unref(state->plain_state);
    xkb_state_unref(state->state);
    free(state);
}

int keyboard_state_process_key_event(
    struct keyboard_state *state,
    uint16_t evdev_keycode,
    int32_t evdev_value,
    xkb_keysym_t *keysym_out,
    uint32_t *codepoint_out
) {
    enum xkb_compose_feed_result feed_result;
    enum xkb_compose_status compose_status;
    xkb_keycode_t xkb_keycode;
    xkb_keysym_t keysym;
    uint32_t codepoint;

    /**
     * evdev_value = 0: release
     * evdev_value = 1: press
     * evdev_value = 2: repeat
     */

    keysym = 0;
    codepoint = 0;
    xkb_keycode = evdev_keycode + 8;

    if (evdev_value) {
        keysym = xkb_state_key_get_one_sym(state->state, xkb_keycode);

        feed_result = xkb_compose_state_feed(state->compose_state, keysym);
        compose_status = xkb_compose_state_get_status(state->compose_state);
        if (feed_result == XKB_COMPOSE_FEED_ACCEPTED && compose_status == XKB_COMPOSE_COMPOSING) {
            keysym = XKB_KEY_NoSymbol;
        }
        
        if (compose_status == XKB_COMPOSE_COMPOSED) {
            keysym = xkb_compose_state_get_one_sym(state->compose_state);
            xkb_compose_state_reset(state->compose_state);
        } else if (compose_status == XKB_COMPOSE_CANCELLED) {
            xkb_compose_state_reset(state->compose_state);
        }

        codepoint = xkb_keysym_to_utf32(keysym);
    }

    xkb_state_update_key(state->state, xkb_keycode, (enum xkb_key_direction) evdev_value);

    if (keysym_out) *keysym_out = keysym;
    if (codepoint_out) *codepoint_out = codepoint;

    return 0;
}

uint32_t keyboard_state_get_plain_codepoint(
    struct keyboard_state *state,
    uint16_t evdev_keycode,
    int32_t evdev_value
) {
    xkb_keycode_t xkb_keycode = evdev_keycode + 8;

    if (evdev_value) {
        return xkb_state_key_get_utf32(state->plain_state, xkb_keycode);
    }

    return 0;
}