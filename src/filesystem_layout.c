// SPDX-License-Identifier: MIT
/*
 * Filesystem Layout
 *
 * - implements different filesystem layouts for flutter artifacts
 *   (libflutter_engine, icudtl, asset bundle, etc)
 *
 * Copyright (c) 2022, Hannes Winkler <hanneswinkler2000@web.de>
 */

#define _GNU_SOURCE
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

#include <collection.h>
#include <filesystem_layout.h>
#include <flutter-pi.h>

FILE_DESCR("fs layout")

static bool path_exists(const char *path) {
    return access(path, R_OK) == 0;
}

static struct flutter_paths *resolve(
    const char *app_bundle_path,
    enum flutter_runtime_mode runtime_mode,
    const char *asset_bundle_subpath,
    const char *icudtl_subpath,
    const char *icudtl_system_path,
    const char *icudtl_system_path_fallback,
    const char *kernel_blob_subpath,
    const char *app_elf_subpath,
    const char *app_engine_subpath,
    const char *engine_dlopen_name,
    const char *engine_dlopen_name_fallback
) {
    struct flutter_paths *paths;
    char *dlopen_name_fallback_duped;
    char *app_bundle_path_real;
    char *dlopen_name_duped;
    char *asset_bundle_path;
    char *kernel_blob_path;
    char *app_elf_path;
    char *icudtl_path;
    char *engine_path;
    int ok;

    DEBUG_ASSERT_NOT_NULL(app_bundle_path);
    DEBUG_ASSERT(icudtl_subpath || icudtl_system_path || icudtl_system_path_fallback);
    DEBUG_ASSERT_MSG(!icudtl_system_path_fallback || icudtl_system_path, "icudtl.dat fallback system path is given, but no non-fallback system path.");
    DEBUG_ASSERT_NOT_NULL(asset_bundle_subpath);
    DEBUG_ASSERT_NOT_NULL(kernel_blob_subpath);
    DEBUG_ASSERT_NOT_NULL(app_elf_subpath);
    DEBUG_ASSERT(app_engine_subpath || engine_dlopen_name || engine_dlopen_name_fallback);
    DEBUG_ASSERT_MSG(!engine_dlopen_name_fallback || engine_dlopen_name, "flutter engine fallback dlopen name is given, but no non-fallback dlopen name.");

    paths = malloc(sizeof *paths);
    if (paths == NULL) {
        return NULL;
    }

    if (path_exists(app_bundle_path) == false) {
        LOG_ERROR("App bundle directory \"%s\" does not exist.\n", app_bundle_path);
        goto fail_free_paths;
    }

    // Seems like the realpath will always not end with a slash.
    app_bundle_path_real = realpath(app_bundle_path, NULL);
    if (app_bundle_path_real == NULL) {
        goto fail_free_paths;
    }

    DEBUG_ASSERT(path_exists(app_bundle_path_real));

    // Asset bundle path is the same as the app bundle path in the default filesystem layout,
    // or <appbundle>/flutter_assets in meta-flutter dunfell / kirkstone layout.
    ok = asprintf(&asset_bundle_path, "%s/%s", app_bundle_path_real, asset_bundle_subpath);
    if (ok == -1) {
        goto fail_free_app_bundle_path_real;
    }

    if (path_exists(asset_bundle_path) == false) {
        LOG_ERROR("Asset bundle directory \"%s\" does not exist.\n", asset_bundle_path);
        goto fail_free_asset_bundle_path;
    }

    // Find the icudtl.dat file.
    // Mostly we look in <appbundle>/icudtl.dat or <appbundle>/data/icudtl.dat, /usr/share/flutter/icudtl.dat
    // or /usr/lib/icudtl.dat.
    icudtl_path = NULL;

    if (icudtl_subpath != NULL) {
        ok = asprintf(&icudtl_path, "%s/%s", app_bundle_path_real, icudtl_subpath);
        if (ok == -1) {
            goto fail_free_asset_bundle_path;
        }
    }

    if (icudtl_system_path != NULL && (icudtl_path == NULL || path_exists(icudtl_path) == false)) {
        if (icudtl_path != NULL) {
            LOG_DEBUG("icudtl file not found at %s.\n", icudtl_path);
            free(icudtl_path);
        }

        icudtl_path = strdup(icudtl_system_path);
        if (icudtl_path == NULL) {
            goto fail_free_asset_bundle_path;
        }
    }

    DEBUG_ASSERT_NOT_NULL(icudtl_path);

    if (icudtl_system_path_fallback != NULL && path_exists(icudtl_path) == false) {
        LOG_DEBUG("icudtl file not found at %s.\n", icudtl_path);
        free(icudtl_path);

        icudtl_path = strdup(icudtl_system_path_fallback);
        if (icudtl_path == NULL) {
            goto fail_free_asset_bundle_path;
        }
    }

    DEBUG_ASSERT_NOT_NULL(icudtl_path);

    // We still haven't found it. Fail because we need it to run flutter.
    if (path_exists(icudtl_path) == false) {
        LOG_DEBUG("icudtl file not found at %s.\n", icudtl_path);
        free(icudtl_path);

        LOG_ERROR("icudtl file not found!\n");
        goto fail_free_asset_bundle_path;
    }

    // Find the kernel_blob.bin file. Only necessary for JIT (debug) mode.
    ok = asprintf(&kernel_blob_path, "%s/%s", app_bundle_path_real, kernel_blob_subpath);
    if (ok == -1) {
        goto fail_free_asset_bundle_path;
    }

    if (FLUTTER_RUNTIME_MODE_IS_JIT(runtime_mode) && !path_exists(kernel_blob_path)) {
        LOG_ERROR("kernel blob file \"%s\" does not exist, but is necessary for debug mode.\n", kernel_blob_path);
        goto fail_free_kernel_blob_path;
    }

    // Find the app.so/libapp.so file. Only necessary for AOT (release/profile) mode.
    ok = asprintf(&app_elf_path, "%s/%s", app_bundle_path_real, app_elf_subpath);
    if (ok == -1) {
        goto fail_free_kernel_blob_path;
    }

    if (FLUTTER_RUNTIME_MODE_IS_AOT(runtime_mode) && !path_exists(app_elf_path)) {
        LOG_ERROR("app elf file \"%s\" does not exist, but is necessary for release/profile mode.\n", app_elf_path);
        goto fail_free_app_elf_path;
    }
    
    // Try to find the engine inside the asset bundle. If we don't find it, that's not an error because
    // it could still be inside /usr/lib and we can just dlopen it using the filename.
    ok = asprintf(&engine_path, "%s/%s", app_bundle_path_real, app_engine_subpath);
    if (ok == -1) {
        goto fail_free_app_elf_path;
    }

    if (path_exists(engine_path) == false) {
        if (engine_dlopen_name == NULL && engine_dlopen_name_fallback == NULL) {
            LOG_ERROR("flutter engine file \"%s\" does not exist.\n", engine_path);
            goto fail_maybe_free_engine_path;
        }

        free(engine_path);
        engine_path = NULL;
    }

    if (engine_dlopen_name != NULL) {
        dlopen_name_duped = strdup(engine_dlopen_name);
        if (dlopen_name_duped == NULL) {
            goto fail_maybe_free_engine_path;
        }
    } else {
        dlopen_name_duped = NULL;
    }

    if (engine_dlopen_name_fallback != NULL) {
        dlopen_name_fallback_duped = strdup(engine_dlopen_name_fallback);
        if (dlopen_name_fallback_duped == NULL) {
            goto fail_free_dlopen_name_duped;
        }
    } else {
        dlopen_name_fallback_duped = NULL;
    }


    paths->app_bundle_path = app_bundle_path_real;
    paths->asset_bundle_path = asset_bundle_path;
    paths->icudtl_path = icudtl_path;
    paths->kernel_blob_path = kernel_blob_path;
    paths->app_elf_path = app_elf_path;
    paths->flutter_engine_path = engine_path;
    paths->flutter_engine_dlopen_name = dlopen_name_duped;
    paths->flutter_engine_dlopen_name_fallback = dlopen_name_fallback_duped;
    return paths;


    fail_free_dlopen_name_duped:
    free(dlopen_name_duped);

    fail_maybe_free_engine_path:
    if (engine_path != NULL) {
        free(engine_path);
    }

    fail_free_app_elf_path:
    free(app_elf_path);

    fail_free_kernel_blob_path:
    free(kernel_blob_path);
    
    fail_free_asset_bundle_path:
    free(asset_bundle_path);

    fail_free_app_bundle_path_real:
    free(app_bundle_path_real);

    fail_free_paths:
    free(paths);
    return NULL;
}

void flutter_paths_free(struct flutter_paths *paths) {
    free(paths->app_bundle_path);
    free(paths->asset_bundle_path);
    free(paths->icudtl_path);
    free(paths->kernel_blob_path);
    free(paths->app_elf_path);
    if (paths->flutter_engine_path != NULL) {
        free(paths->flutter_engine_path);
    }
    if (paths->flutter_engine_dlopen_name != NULL) {
        free(paths->flutter_engine_dlopen_name);
    }
    if (paths->flutter_engine_dlopen_name_fallback != NULL) {
        free(paths->flutter_engine_dlopen_name_fallback);
    }
    free(paths);
}

struct flutter_paths *fs_layout_flutterpi_resolve(const char *app_bundle_path, enum flutter_runtime_mode runtime_mode) {
    return resolve(
        app_bundle_path,
        runtime_mode,
        /*        asset bundle subpath */ "",
        /*              icudtl subpath */ "icudtl.dat",
        /*          icudtl system path */ "/usr/share/flutter/icudtl.dat",
        /* icudtl system path fallback */ "/usr/lib/icudtl.dat",
        /*         kernel blob subpath */ "kernel_blob.bin",
        /*             app elf subpath */ "app.so",
        /*      flutter engine subpath */ "libflutter_engine.so",
        /*          engine dlopen name */ runtime_mode == kDebug   ? "libflutter_engine.so.debug" :
                                          runtime_mode == kProfile ? "libflutter_engine.so.profile" :
                                                                     "libflutter_engine.so.release",
        /* engine dlopen name fallback */ "libflutter_engine.so"
    );
}

struct flutter_paths *fs_layout_metaflutter_resolve(const char *app_bundle_path, enum flutter_runtime_mode runtime_mode) {
    return resolve(
        app_bundle_path,
        runtime_mode,
        /*        asset bundle subpath */ "data/flutter_assets/",
        /*              icudtl subpath */ "data/icudtl.dat",
        /*          icudtl system path */ "/usr/share/flutter/icudtl.dat",
        /* icudtl system path fallback */ NULL,
        /*         kernel blob subpath */ "data/flutter_assets/kernel_blob.bin",
        /*             app elf subpath */ "lib/libapp.so",
        /*      flutter engine subpath */ "lib/libflutter_engine.so",
        /*          engine dlopen name */ "libflutter_engine.so",
        /* engine dlopen name fallback */ NULL
    );
}
