#define _GNU_SOURCE
#include "locales.h"

#include <errno.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <flutter_embedder.h>

#include "flutter-pi.h"
#include "util/asserts.h"
#include "util/collection.h"
#include "util/list.h"
#include "util/logging.h"

#define LOG_LOCALES_ERROR(...) fprintf(stderr, "[locales] " __VA_ARGS__);

struct locale {
    struct list_head entry;

    char *language;
    char *territory;
    char *codeset;
    char *modifier;
    FlutterLocale *flutter_locale;
};

struct locales {
    const FlutterLocale **flutter_locales;
    const FlutterLocale *default_flutter_locale;

    struct list_head locales;
    struct locale *default_locale;

    size_t n_locales;
};

#define for_each_locale_in_locales(_locale, _locales) list_for_each_entry_safe(struct locale, _locale, &(_locales)->locales, entry)

static const char *get_system_locale_string(void) {
    const char *locale;

    locale = getenv("LANGUAGE");
    if (locale != NULL && *locale != '\0') {
        return locale;
    }

    locale = getenv("LC_ALL");
    if (locale != NULL && *locale != '\0') {
        return locale;
    }

    locale = getenv("LC_MESSAGES");
    if (locale != NULL && *locale != '\0') {
        return locale;
    }

    locale = getenv("LANG");
    if (locale != NULL && *locale != '\0') {
        return locale;
    }

    return "C";
}

struct locale *locale_new(const char *language, const char *territory, const char *codeset, const char *modifier) {
    char *language_dup, *territory_dup, *codeset_dup, *modifier_dup;
    struct locale *locale;
    FlutterLocale *fl_locale;

    assert(language != NULL);

    locale = malloc(sizeof *locale);
    if (locale == NULL) {
        return NULL;
    }

    fl_locale = malloc(sizeof *fl_locale);
    if (fl_locale == NULL) {
        goto fail_free_locale;
    }

    language_dup = strdup(language);
    if (language_dup == NULL) {
        goto fail_free_fl_locale;
    }

    if (territory != NULL) {
        territory_dup = strdup(territory);
        if (territory_dup == NULL) {
            goto fail_free_language_dup;
        }
    } else {
        territory_dup = NULL;
    }

    if (codeset != NULL) {
        codeset_dup = strdup(codeset);
        if (codeset_dup == NULL) {
            goto fail_free_territory_dup;
        }
    } else {
        codeset_dup = NULL;
    }

    if (modifier != NULL) {
        modifier_dup = strdup(modifier);
        if (modifier_dup == NULL) {
            goto fail_free_codeset_dup;
        }
    } else {
        modifier_dup = NULL;
    }

    fl_locale->struct_size = sizeof(*fl_locale);
    fl_locale->language_code = language_dup;
    fl_locale->country_code = territory_dup;
    fl_locale->script_code = codeset_dup;
    fl_locale->variant_code = modifier_dup;
    locale->entry = (struct list_head){ NULL, NULL };
    locale->flutter_locale = fl_locale;
    locale->language = language_dup;
    locale->territory = territory_dup;
    locale->codeset = codeset_dup;
    locale->modifier = modifier_dup;

    return locale;

fail_free_codeset_dup:
    free(codeset_dup);

fail_free_territory_dup:
    free(territory_dup);

fail_free_language_dup:
    free(language_dup);

fail_free_fl_locale:
    free(fl_locale);

fail_free_locale:
    free(locale);

    return NULL;
}

const FlutterLocale *locale_get_fl_locale(struct locale *locale) {
    assert(locale != NULL);
    return locale->flutter_locale;
}

void locale_destroy(struct locale *locale) {
    free(locale->language);
    if (locale->territory)
        free(locale->territory);
    if (locale->codeset)
        free(locale->codeset);
    if (locale->modifier)
        free(locale->modifier);
    free(locale->flutter_locale);
    free(locale);
}

static int add_locale_variants(struct list_head *locales, const char *locale_description) {
    char *language = NULL;
    char *territory = NULL;
    char *codeset = NULL;
    char *modifier = NULL;
    const char *underscore;
    const char *at;
    const char *dot;
    const char *next_delim;
    int ok = 0;

    // first, split the locale description into its parts
    next_delim = locale_description + strlen(locale_description);

    underscore = strchr(locale_description, '_');
    dot = strchr(underscore ? underscore : locale_description, '.');
    at = strchr(dot ? dot : (underscore ? underscore : locale_description), '@');

    if (at != NULL) {
        //exclude @
        modifier = strdup(at + 1);
        if (modifier == NULL) {
            ok = ENOMEM;
            goto fail_return_ok;
        }

        next_delim = at;
    }

    if (dot != NULL) {
        //exclude . and @
        codeset = strndup(dot + 1, next_delim - dot - 1);
        if (codeset == NULL) {
            ok = ENOMEM;
            goto fail_free_modifier;
        }
        next_delim = dot;
    }

    if (underscore != NULL) {
        //exclude _ and .
        territory = strndup(underscore + 1, next_delim - underscore - 1);
        if (territory == NULL) {
            ok = ENOMEM;
            goto fail_free_codeset;
        }
        next_delim = underscore;
    }

    //nothing to exclude
    language = strndup(locale_description, next_delim - locale_description);
    if (language == NULL) {
        ok = ENOMEM;
        goto fail_free_territory;
    }

    // then append all possible combinations
    for (int i = 7; i >= 0; i--) {
        char *territory_2 = NULL, *codeset_2 = NULL, *modifier_2 = NULL;

        if ((i & 1) != 0) {
            if (codeset == NULL) {
                continue;
            } else {
                codeset_2 = codeset;
            }
        } else if ((i & 2) != 0) {
            if (territory == NULL) {
                continue;
            } else {
                territory_2 = territory;
            }
        } else if ((i & 4) != 0) {
            if (modifier == NULL) {
                continue;
            } else {
                modifier_2 = modifier;
            }
        }

        struct locale *locale = locale_new(language, territory_2, codeset_2, modifier_2);
        if (locale == NULL) {
            goto fail_free_language;
        }

        list_add(&locale->entry, locales);
    }

    free(language);
    if (territory)
        free(territory);
    if (codeset)
        free(codeset);
    if (modifier)
        free(modifier);
    return 0;

fail_free_language:
    free(language);

fail_free_territory:
    if (territory)
        free(territory);

fail_free_codeset:
    if (codeset)
        free(codeset);

fail_free_modifier:
    if (modifier)
        free(modifier);

fail_return_ok:
    return ok;
}

struct locales *locales_new(void) {
    struct locales *locales;
    const char *system_locales;
    char *system_locales_modifiable, *syslocale;
    const FlutterLocale **fl_locales;
    size_t n_locales;

    locales = malloc(sizeof *locales);
    if (locales == NULL) {
        goto fail_return_null;
    }

    list_inithead(&locales->locales);

    // Add our system locales.
    system_locales = get_system_locale_string();

    system_locales_modifiable = strdup(system_locales);
    if (system_locales_modifiable == NULL) {
        goto fail_free_locales;
    }

    syslocale = strtok(system_locales_modifiable, ":");
    while (syslocale != NULL) {
        add_locale_variants(&locales->locales, syslocale);
        syslocale = strtok(NULL, ":");
    }

    free(system_locales_modifiable);

    // Use those to create our flutter locales.
    n_locales = list_length(&locales->locales);
    fl_locales = calloc(n_locales == 0 ? 1 : n_locales, sizeof(const FlutterLocale *));
    if (fl_locales == NULL) {
        goto fail_free_allocated_locales;
    }

    int i = 0;
    for_each_locale_in_locales(locale, locales) {
        fl_locales[i] = locale_get_fl_locale(locale);
        i++;
    }

    // If we have no locales, add a default "C" locale.
    if (i == 0) {
        fl_locales[0] = &(const FlutterLocale){
            .struct_size = sizeof(FlutterLocale),
            .language_code = "C",
            .country_code = NULL,
            .script_code = NULL,
            .variant_code = NULL,
        };
        i++;
    }

    if (streq(fl_locales[0]->language_code, "C")) {
        LOG_LOCALES_ERROR("Warning: The system has no configured locale. The default \"C\" locale may or may not be supported by the app.\n"
        );
    }

    locales->flutter_locales = fl_locales;
    locales->n_locales = n_locales;
    locales->default_locale = NULL;
    locales->default_flutter_locale = fl_locales[0];

    return locales;

fail_free_allocated_locales:
    for_each_locale_in_locales(locale, locales) {
        list_del(&locale->entry);
        locale_destroy(locale);
    }

fail_free_locales:
    free(locales);

fail_return_null:
    return NULL;
}

void locales_destroy(struct locales *locales) {
    assert(locales != NULL);

    for_each_locale_in_locales(locale, locales) {
        list_del(&locale->entry);
        locale_destroy(locale);
    }
    free(locales->flutter_locales);
    free(locales);
}

int locales_get_flutter_locales(struct locales *locales, const FlutterLocale ***fl_locales_out, size_t *n_fl_locales_out) {
    assert(locales != NULL);
    assert(fl_locales_out != NULL);
    assert(n_fl_locales_out != NULL);
    *fl_locales_out = locales->flutter_locales;
    *n_fl_locales_out = locales->n_locales;
    return 0;
}

const FlutterLocale *locales_get_default_flutter_locale(struct locales *locales) {
    assert(locales != NULL);
    return locales->default_flutter_locale;
}

struct locale *locales_get_default_locale(struct locales *locales);

const char *locale_get_language(struct locale *locale) {
    return locale->language;
}

const char *locale_get_territory(struct locale *locale) {
    return locale->territory;
}

const char *locale_get_codeset(struct locale *locale) {
    return locale->codeset;
}

const char *locale_get_modifier(struct locale *locale) {
    return locale->modifier;
}

int locales_add_to_fl_engine(struct locales *locales, FlutterEngine engine, FlutterEngineUpdateLocalesFnPtr update_locales) {
    FlutterEngineResult engine_result;

    engine_result = update_locales(engine, locales->flutter_locales, locales->n_locales);
    if (engine_result != kSuccess) {
        LOG_LOCALES_ERROR(
            "Couldn't update flutter engine locales. FlutterEngineUpdateLocales: %s\n",
            FLUTTER_RESULT_TO_STRING(engine_result)
        );
        return EINVAL;
    }

    return 0;
}

const FlutterLocale *
locales_on_compute_platform_resolved_locale(struct locales *locales, const FlutterLocale **fl_locales, size_t n_fl_locales) {
    assert(locales != NULL);
    assert(fl_locales != NULL);
    assert(n_fl_locales > 0);

    (void) locales;
    (void) n_fl_locales;

    return fl_locales[0];
}

void locales_print(const struct locales *locales) {
    ASSERT_NOT_NULL(locales);

    LOG_DEBUG_UNPREFIXED("==============Locale==============\n");
    LOG_DEBUG_UNPREFIXED("Flutter locale:\n");
    if (locales->default_flutter_locale != NULL) {
        LOG_DEBUG_UNPREFIXED("  default: %s", locales->default_flutter_locale->language_code);
        if (locales->default_flutter_locale->country_code != NULL) {
            LOG_DEBUG_UNPREFIXED("_%s", locales->default_flutter_locale->country_code);
        }
        if (locales->default_flutter_locale->script_code != NULL) {
            LOG_DEBUG_UNPREFIXED(".%s", locales->default_flutter_locale->script_code);
        }
        if (locales->default_flutter_locale->variant_code != NULL) {
            LOG_DEBUG_UNPREFIXED("@%s", locales->default_flutter_locale->variant_code);
        }

        LOG_DEBUG_UNPREFIXED("\n");
    } else {
        LOG_DEBUG_UNPREFIXED("  default: NULL\n");
    }

    LOG_DEBUG_UNPREFIXED("  locales:");
    for (size_t idx = 0; idx < locales->n_locales; idx++) {
        const FlutterLocale *locale = locales->flutter_locales[idx];
        LOG_DEBUG_UNPREFIXED(" %s", locale->language_code);
        if (locale->country_code != NULL) {
            LOG_DEBUG_UNPREFIXED("_%s", locale->country_code);
        }
        if (locale->script_code != NULL) {
            LOG_DEBUG_UNPREFIXED(".%s", locale->script_code);
        }
        if (locale->variant_code != NULL) {
            LOG_DEBUG_UNPREFIXED("@%s", locale->variant_code);
        }
    }

    LOG_DEBUG_UNPREFIXED("\n===================================\n");
}
