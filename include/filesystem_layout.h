// SPDX-License-Identifier: MIT
/*
 * Filesystem Layout
 *
 * - implements different filesystem layouts for flutter artifacts
 *
 * Copyright (c) 2022, Hannes Winkler <hanneswinkler2000@web.de>
 */


#ifndef _FLUTTERPI_INCLUDE_FILESYSTEM_LAYOUT_H
#define _FLUTTERPI_INCLUDE_FILESYSTEM_LAYOUT_H

#include <flutter-pi.h>

struct flutter_paths {
	char *app_bundle_path;
	char *asset_bundle_path;
	char *app_elf_path;
	char *icudtl_path;
	char *kernel_blob_path;
	char *flutter_engine_path;
	char *flutter_engine_dlopen_name;
	char *flutter_engine_dlopen_name_fallback;
};

typedef struct flutter_paths *(*resolve_paths_t)(const char *app_bundle_path, enum flutter_runtime_mode runtime_mode);

void flutter_paths_free(struct flutter_paths *paths);

struct flutter_paths *fs_layout_flutterpi_resolve(const char *app_bundle_path, enum flutter_runtime_mode runtime_mode);
struct flutter_paths *fs_layout_metaflutter_resolve(const char *app_bundle_path, enum flutter_runtime_mode runtime_mode);

#endif // _FLUTTERPI_INCLUDE_FILESYSTEM_LAYOUT_H
