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

typedef struct flutter_paths *(*resolve_paths_t)(const char *app_bundle_path, enum flutter_runtime_mode runtime_mode);

struct flutter_paths *fs_layout_flutterpi_resolve(const char *app_bundle_path, enum flutter_runtime_mode runtime_mode);
struct flutter_paths *fs_layout_dunfell_resolve(const char *app_bundle_path, enum flutter_runtime_mode runtime_mode);
struct flutter_paths *fs_layout_kirkstone_resolve(const char *app_bundle_path, enum flutter_runtime_mode runtime_mode);

#endif // _FLUTTERPI_INCLUDE_FILESYSTEM_LAYOUT_H
