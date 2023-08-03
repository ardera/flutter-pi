// SPDX-License-Identifier: MIT
/*
 * Locales
 *
 * Provides the configured system locales in a flutter-friendly form.
 *
 * Copyright (c) 2023, Hannes Winkler <hanneswinkler2000@web.de>
 */

#ifndef _FLUTTERPI_SRC_LOCALES_H
#define _FLUTTERPI_SRC_LOCALES_H

#include <flutter_embedder.h>

struct locale;
struct locales;
struct concurrent_pointer_set;

struct locale *locale_new(const char *language, const char *territory, const char *codeset, const char *modifier);

void locale_destroy(struct locale *locale);

const FlutterLocale *locale_get_fl_locale(struct locale *locale);

const char *locale_get_language(struct locale *locale);

const char *locale_get_territory(struct locale *locale);

const char *locale_get_codeset(struct locale *locale);

const char *locale_get_modifier(struct locale *locale);

struct locales *locales_new(void);

void locales_destroy(struct locales *locales);

int locales_get_flutter_locales(struct locales *locales, const FlutterLocale ***fl_locales_out, size_t *n_fl_locales_out);

const FlutterLocale *locales_get_default_flutter_locale(struct locales *locales);

int locales_add_to_fl_engine(struct locales *locales, FlutterEngine engine, FlutterEngineUpdateLocalesFnPtr update_locales);

const FlutterLocale *
locales_on_compute_platform_resolved_locale(struct locales *locales, const FlutterLocale **fl_locales, size_t n_fl_locales);

void locales_print(const struct locales *locales);

#endif  // _FLUTTERPI_SRC_LOCALES_H
