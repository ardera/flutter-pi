#define _POSIX_C_SOURCE 200809L
#include <unistd.h>

#include <sentry.h>

#include "flutter-pi.h"
#include "platformchannel.h"
#include "pluginregistry.h"
#include "util/logging.h"

#define SENTRY_PLUGIN_METHOD_CHANNEL "sentry_flutter"

#define SENTRY_PLUGIN_DEBUG 1
#define LOG_SENTRY_DEBUG(fmt, ...)         \
    do {                                   \
        if (SENTRY_PLUGIN_DEBUG)           \
            LOG_DEBUG(fmt, ##__VA_ARGS__); \
    } while (0)

struct sentry_plugin {
    bool sentry_initialized;
};

UNUSED static int sentry_configure_bundled_crashpad_handler(sentry_options_t *options) {
    char *path = malloc(PATH_MAX);
    if (path == NULL) {
        return ENOMEM;
    }

    ssize_t n_read = readlink("/proc/self/exe", path, PATH_MAX);
    if (n_read == -1) {
        free(path);
        return errno;
    }

    sentry_options_set_handler_path_n(options, path, n_read);

    free(path);

    sentry_options_add_attachment(options, "FlutterpiCrashpadHandlerMode");

    return 0;
}

static void on_init_native_sdk(
    struct sentry_plugin *plugin,
    const struct raw_std_value *arg,
    const FlutterPlatformMessageResponseHandle *responsehandle
) {
    int ok;

    (void) plugin;
    (void) arg;
    (void) responsehandle;

    if (!raw_std_value_is_map(arg) || raw_std_map_get_size(arg) == 0) {
        platch_respond_error_std(responsehandle, "4", "Expected `arg` to be a Map or null.", &STDNULL);
        return;
    }

    sentry_options_t *options = sentry_options_new();

    const struct raw_std_value *dsn = raw_std_map_find_str(arg, "dsn");
    if (raw_std_value_is_string(dsn)) {
        sentry_options_set_dsn_n(options, raw_std_string_get_nonzero_terminated(dsn), raw_std_string_get_length(dsn));
    } else if (!raw_std_value_is_null(dsn)) {
        platch_respond_error_std(responsehandle, "4", "Expected `arg['dsn']` to be a string or null.", &STDNULL);
        return;
    }

    const struct raw_std_value *debug = raw_std_map_find_str(arg, "debug");
    if (raw_std_value_is_bool(debug)) {
        sentry_options_set_debug(options, raw_std_value_as_bool(debug) ? 1 : 0);
    } else if (!raw_std_value_is_null(debug)) {
        platch_respond_error_std(responsehandle, "4", "Expected `arg['debug']` to be a bool or null.", &STDNULL);
        return;
    }

    const struct raw_std_value *environment = raw_std_map_find_str(arg, "environment");
    if (raw_std_value_is_string(environment)) {
        sentry_options_set_environment_n(
            options,
            raw_std_string_get_nonzero_terminated(environment),
            raw_std_string_get_length(environment)
        );
    } else if (!raw_std_value_is_null(environment)) {
        platch_respond_error_std(responsehandle, "4", "Expected `arg['environment']` to be a string or null.", &STDNULL);
        return;
    }

    const struct raw_std_value *release = raw_std_map_find_str(arg, "release");
    if (raw_std_value_is_string(release)) {
        sentry_options_set_release_n(options, raw_std_string_get_nonzero_terminated(release), raw_std_string_get_length(release));
    } else if (!raw_std_value_is_null(release)) {
        platch_respond_error_std(responsehandle, "4", "Expected `arg['release']` to be a string or null.", &STDNULL);
        return;
    }

    const struct raw_std_value *dist = raw_std_map_find_str(arg, "dist");
    if (raw_std_value_is_string(dist)) {
        sentry_options_set_dist_n(options, raw_std_string_get_nonzero_terminated(dist), raw_std_string_get_length(dist));
    } else if (!raw_std_value_is_null(dist)) {
        platch_respond_error_std(responsehandle, "4", "Expected `arg['dist']` to be a string or null.", &STDNULL);
        return;
    }

    const struct raw_std_value *auto_session_tracking = raw_std_map_find_str(arg, "enableAutoSessionTracking");
    if (raw_std_value_is_bool(auto_session_tracking)) {
        sentry_options_set_auto_session_tracking(options, raw_std_value_as_bool(auto_session_tracking) ? 1 : 0);
    } else if (!raw_std_value_is_null(dist)) {
        platch_respond_error_std(responsehandle, "4", "Expected `arg['enableAutoSessionTracking']` to be a bool or null.", &STDNULL);
        return;
    }

    /// TODO: Handle these as well or find out if they don't apply
    ///   autoSessionTrackingIntervalMillis
    ///   anrTimeoutIntervalMillis
    ///   attachThreads
    ///   attachStacktrace
    ///   enableAutoNativeBreadcrambs
    ///   maxBreadcrumbs
    ///   maxCacheItems
    ///   diagnosticLevel
    ///   anrEnabled
    ///   sendDefaultPii
    ///   enableNdkScopeSync
    ///   proguardUuid
    ///   enableNativeCrashHandling
    ///   enableAutoPerformanceTracing
    ///   sendClientReports
    ///   maxAttachmentSize
    ///   connectionTimeoutMillis
    ///   readTimeoutMillis

    /// TODO: Set the correct path of the crashpad handler here

    LOG_SENTRY_DEBUG(
        "initNativeSdk(), dsn: %s, debug: %s, environment: %s, release: %s, dist: %s, auto_session_tracking: %s\n",
        sentry_options_get_dsn(options),
        sentry_options_get_debug(options) ? "true" : "false",
        sentry_options_get_environment(options),
        sentry_options_get_release(options),
        sentry_options_get_dist(options),
        sentry_options_get_auto_session_tracking(options) ? "true" : "false"
    );

#ifdef HAVE_BUNDLED_CRASHPAD_HANDLER
    ok = sentry_configure_bundled_crashpad_handler(options);
    if (ok != 0) {
        platch_respond_error_std(responsehandle, "1", "Failed to configure bundled Crashpad handler.", &STDNULL);
        return;
    }
#endif

    ok = sentry_init(options);
    if (ok != 0) {
        platch_respond_error_std(responsehandle, "1", "Failed to initialize Sentry.", &STDNULL);
        return;
    }

    plugin->sentry_initialized = true;

    platch_respond_success_std(responsehandle, &STDNULL);
}

static void on_capture_envelope(
    struct sentry_plugin *plugin,
    const struct raw_std_value *arg,
    const FlutterPlatformMessageResponseHandle *responsehandle
) {
    (void) plugin;
    (void) arg;

    LOG_SENTRY_DEBUG("captureEnvelope()\n");

    /// TODO: Implement
    platch_respond_not_implemented(responsehandle);
}

static void on_load_image_list(
    struct sentry_plugin *plugin,
    const struct raw_std_value *arg,
    const FlutterPlatformMessageResponseHandle *responsehandle
) {
    (void) plugin;
    (void) arg;

    LOG_SENTRY_DEBUG("loadImageList()\n");

    /// TODO: Implement
    platch_respond_not_implemented(responsehandle);
}

static void on_close_native_sdk(
    struct sentry_plugin *plugin,
    const struct raw_std_value *arg,
    const FlutterPlatformMessageResponseHandle *responsehandle
) {
    if (!raw_std_value_is_null(arg)) {
        platch_respond_error_std(responsehandle, "4", "Expected `arg` to be null.", &STDNULL);
        return;
    }

    LOG_SENTRY_DEBUG("closeNativeSdk()\n");

    if (plugin->sentry_initialized) {
        sentry_close();
        plugin->sentry_initialized = false;
    }

    platch_respond_success_std(responsehandle, &STDNULL);
}

static void on_fetch_native_app_start(
    struct sentry_plugin *plugin,
    const struct raw_std_value *arg,
    const FlutterPlatformMessageResponseHandle *responsehandle
) {
    (void) plugin;
    (void) arg;

    if (!raw_std_value_is_null(arg)) {
        platch_respond_error_std(responsehandle, "4", "Expected `arg` to be null.", &STDNULL);
        return;
    }

    LOG_SENTRY_DEBUG("fetchNativeAppStart()\n");

    // clang-format off
    platch_respond_success_std(
        responsehandle,
        &STDMAP2(
            STDSTRING("appStartTime"), STDFLOAT64(0.0),
            STDSTRING("isColdStart"), STDBOOL(true)
        )
    );
    // clang-format on
}

static void on_begin_native_frames(
    struct sentry_plugin *plugin,
    const struct raw_std_value *arg,
    const FlutterPlatformMessageResponseHandle *responsehandle
) {
    (void) plugin;
    (void) arg;

    if (!raw_std_value_is_null(arg)) {
        platch_respond_error_std(responsehandle, "4", "Expected `arg` to be null.", &STDNULL);
        return;
    }

    LOG_SENTRY_DEBUG("beginNativeFrames()\n");

    /// TODO: Implement
    platch_respond_not_implemented(responsehandle);
}

static void on_end_native_frames(
    struct sentry_plugin *plugin,
    const struct raw_std_value *arg,
    const FlutterPlatformMessageResponseHandle *responsehandle
) {
    (void) plugin;
    (void) arg;

    if (!raw_std_value_is_map(arg)) {
        platch_respond_error_std(responsehandle, "4", "Expected `arg` to be a Map.", &STDNULL);
        return;
    }

    const struct raw_std_value *id = raw_std_map_find_str(arg, "id");
    if (id == NULL || !raw_std_value_is_string(id)) {
        platch_respond_error_std(responsehandle, "4", "Expected `arg['id']` to be a string.", &STDNULL);
        return;
    }

    LOG_SENTRY_DEBUG("endNativeFrames(), id: %.*s\n", (int) raw_std_string_get_length(id), raw_std_string_get_nonzero_terminated(id));

    // clang-format off
    platch_respond_success_std(
        responsehandle,
        &STDMAP3(
            STDSTRING("totalFrames"), STDINT64(0),
            STDSTRING("slowFrames"), STDINT64(0),
            STDSTRING("frozenFrames"), STDINT64(0)
        )
    );
    // clang-format on
}

static sentry_value_t raw_std_value_as_sentry_value(const struct raw_std_value *arg) {
    if (arg == NULL) {
        return sentry_value_new_null();
    }

    switch (raw_std_value_get_type(arg)) {
        case kStdNull: return sentry_value_new_null();
        case kStdFalse: return sentry_value_new_bool(0);
        case kStdTrue: return sentry_value_new_bool(1);
        case kStdInt32: return sentry_value_new_int32(raw_std_value_as_int32(arg));
        case kStdInt64: return sentry_value_new_int32((int32_t) raw_std_value_as_int64(arg));
        case kStdFloat64: return sentry_value_new_double(raw_std_value_as_float64(arg));
        case kStdString: return sentry_value_new_string_n(raw_std_string_get_nonzero_terminated(arg), raw_std_string_get_length(arg));
        case kStdMap: {
            sentry_value_t map = sentry_value_new_object();
            for_each_entry_in_raw_std_map(key, value, arg) {
                if (!raw_std_value_is_string(key)) {
                    continue;
                }

                sentry_value_set_by_key_n(
                    map,
                    raw_std_string_get_nonzero_terminated(key),
                    raw_std_string_get_length(key),
                    raw_std_value_as_sentry_value(value)
                );
            }

            return map;
        }
        case kStdList: {
            sentry_value_t list = sentry_value_new_list();

            for_each_element_in_raw_std_list(element, arg) {
                sentry_value_append(list, raw_std_value_as_sentry_value(element));
            }

            return list;
        }
        default: return sentry_value_new_null();
    }
}

static void
on_set_user(struct sentry_plugin *plugin, const struct raw_std_value *arg, const FlutterPlatformMessageResponseHandle *responsehandle) {
    (void) plugin;
    (void) arg;

    if (!raw_std_value_is_map(arg)) {
        platch_respond_error_std(responsehandle, "4", "Expected `arg` to be a Map.", &STDNULL);
        return;
    }

    const struct raw_std_value *user = raw_std_map_find_str(arg, "user");
    if (user == NULL || !raw_std_value_is_map(user)) {
        platch_respond_error_std(responsehandle, "4", "Expected `arg['user']` to be a Map.", &STDNULL);
        return;
    }

    sentry_value_t sentry_user = raw_std_value_as_sentry_value(user);

    char *user_json = sentry_value_to_json(sentry_user);
    LOG_SENTRY_DEBUG("setUser(), arg: %s\n", user_json);
    free(user_json);

    sentry_set_user(sentry_user);

    platch_respond_success_std(responsehandle, &STDNULL);
}

static void
on_add_breadcrumb(struct sentry_plugin *plugin, const struct raw_std_value *arg, const FlutterPlatformMessageResponseHandle *responsehandle) {
    if (!plugin->sentry_initialized) {
        platch_respond_error_std(responsehandle, "1", "Sentry is not initialized.", &STDNULL);
        return;
    }

    if (!raw_std_value_is_map(arg)) {
        platch_respond_error_std(responsehandle, "4", "Expected `arg` to be a Map.", &STDNULL);
        return;
    }

    const struct raw_std_value *breadcrumb = raw_std_map_find_str(arg, "breadcrumb");
    if (breadcrumb == NULL || !raw_std_value_is_map(breadcrumb)) {
        platch_respond_error_std(responsehandle, "4", "Expected `arg['breadcrumb']` to be a Map.", &STDNULL);
        return;
    }

    sentry_value_t sentry_breadcrumb = raw_std_value_as_sentry_value(breadcrumb);

    char *breadcrumb_json = sentry_value_to_json(sentry_breadcrumb);
    LOG_SENTRY_DEBUG("addBreadcrumb(), arg: %s\n", breadcrumb_json);
    free(breadcrumb_json);

    sentry_add_breadcrumb(sentry_breadcrumb);

    platch_respond_success_std(responsehandle, &STDNULL);
}

static void on_clear_breadcrumbs(
    struct sentry_plugin *plugin,
    const struct raw_std_value *arg,
    const FlutterPlatformMessageResponseHandle *responsehandle
) {
    if (!plugin->sentry_initialized) {
        platch_respond_error_std(responsehandle, "1", "Sentry is not initialized.", &STDNULL);
        return;
    }

    if (!raw_std_value_is_null(arg)) {
        platch_respond_error_std(responsehandle, "4", "Expected `arg` to be null.", &STDNULL);
        return;
    }

    LOG_SENTRY_DEBUG("clearBreadcrumbs()\n");

    /// TODO: Implement

    platch_respond_not_implemented(responsehandle);
}

static void
on_set_contexts(struct sentry_plugin *plugin, const struct raw_std_value *arg, const FlutterPlatformMessageResponseHandle *responsehandle) {
    (void) plugin;

    if (!plugin->sentry_initialized) {
        platch_respond_error_std(responsehandle, "1", "Sentry is not initialized.", &STDNULL);
        return;
    }

    if (!raw_std_value_is_map(arg)) {
        platch_respond_error_std(responsehandle, "4", "Expected `arg` to be a Map.", &STDNULL);
        return;
    }

    const struct raw_std_value *key = raw_std_map_find_str(arg, "key");
    if (key == NULL || !raw_std_value_is_string(key)) {
        platch_respond_error_std(responsehandle, "4", "Expected `arg['key']` to be a string.", &STDNULL);
        return;
    }

    const struct raw_std_value *value = raw_std_map_find_str(arg, "value");
    if (value == NULL) {
        platch_respond_error_std(responsehandle, "4", "Expected `arg['value']` to be a value.", &STDNULL);
        return;
    }

    sentry_value_t context = raw_std_value_as_sentry_value(value);

    char *context_json = sentry_value_to_json(context);
    LOG_SENTRY_DEBUG(
        "setContexts(), key: %.*s, value: %s\n",
        (int) raw_std_string_get_length(key),
        raw_std_string_get_nonzero_terminated(key),
        context_json
    );
    free(context_json);

    // clang-format off
    sentry_set_context_n(
        raw_std_string_get_nonzero_terminated(key),
        raw_std_string_get_length(key),
        context
    );

    // clang-format on

    platch_respond_success_std(responsehandle, &STDNULL);
}

static void on_remove_contexts(
    struct sentry_plugin *plugin,
    const struct raw_std_value *arg,
    const FlutterPlatformMessageResponseHandle *responsehandle
) {
    (void) plugin;

    if (!plugin->sentry_initialized) {
        platch_respond_error_std(responsehandle, "1", "Sentry is not initialized.", &STDNULL);
        return;
    }

    if (!raw_std_value_is_map(arg)) {
        platch_respond_error_std(responsehandle, "4", "Expected `arg` to be a Map.", &STDNULL);
        return;
    }

    const struct raw_std_value *key = raw_std_map_find_str(arg, "key");
    if (key == NULL || !raw_std_value_is_string(key)) {
        platch_respond_error_std(responsehandle, "4", "Expected `arg['key']` to be a string.", &STDNULL);
        return;
    }

    LOG_SENTRY_DEBUG("removeContexts(), key: %.*s\n", (int) raw_std_string_get_length(key), raw_std_string_get_nonzero_terminated(key));

    sentry_remove_context_n(raw_std_string_get_nonzero_terminated(key), raw_std_string_get_length(key));

    platch_respond_success_std(responsehandle, &STDNULL);
}

static void
on_set_extra(struct sentry_plugin *plugin, const struct raw_std_value *arg, const FlutterPlatformMessageResponseHandle *responsehandle) {
    (void) plugin;

    if (!plugin->sentry_initialized) {
        platch_respond_error_std(responsehandle, "1", "Sentry is not initialized.", &STDNULL);
        return;
    }

    if (!raw_std_value_is_map(arg)) {
        platch_respond_error_std(responsehandle, "4", "Expected `arg` to be a Map.", &STDNULL);
        return;
    }

    const struct raw_std_value *key = raw_std_map_find_str(arg, "key");
    if (key == NULL || !raw_std_value_is_string(key)) {
        platch_respond_error_std(responsehandle, "4", "Expected `arg['key']` to be a string.", &STDNULL);
        return;
    }

    const struct raw_std_value *value = raw_std_map_find_str(arg, "value");
    if (value == NULL) {
        platch_respond_error_std(responsehandle, "4", "Expected `arg['value']` to be a value.", &STDNULL);
        return;
    }

    sentry_value_t sentry_value = raw_std_value_as_sentry_value(value);

    char *value_json = sentry_value_to_json(sentry_value);
    LOG_SENTRY_DEBUG(
        "setExtra(), key: %.*s, value: %s\n",
        (int) raw_std_string_get_length(key),
        raw_std_string_get_nonzero_terminated(key),
        value_json
    );
    free(value_json);

    // clang-format off
    sentry_set_extra_n(
        raw_std_string_get_nonzero_terminated(key),
        raw_std_string_get_length(key),
        sentry_value
    );

    // clang-format on

    platch_respond_success_std(responsehandle, &STDNULL);
}

static void
on_remove_extra(struct sentry_plugin *plugin, const struct raw_std_value *arg, const FlutterPlatformMessageResponseHandle *responsehandle) {
    (void) plugin;

    if (!plugin->sentry_initialized) {
        platch_respond_error_std(responsehandle, "1", "Sentry is not initialized.", &STDNULL);
        return;
    }

    if (!raw_std_value_is_map(arg)) {
        platch_respond_error_std(responsehandle, "4", "Expected `arg` to be a Map.", &STDNULL);
        return;
    }

    const struct raw_std_value *key = raw_std_map_find_str(arg, "key");
    if (key == NULL || !raw_std_value_is_string(key)) {
        platch_respond_error_std(responsehandle, "4", "Expected `arg['key']` to be a string.", &STDNULL);
        return;
    }

    LOG_SENTRY_DEBUG("removeExtra(), key: %.*s\n", (int) raw_std_string_get_length(key), raw_std_string_get_nonzero_terminated(key));

    sentry_remove_extra_n(raw_std_string_get_nonzero_terminated(key), raw_std_string_get_length(key));

    platch_respond_success_std(responsehandle, &STDNULL);
}

static void
on_set_tag(struct sentry_plugin *plugin, const struct raw_std_value *arg, const FlutterPlatformMessageResponseHandle *responsehandle) {
    (void) plugin;

    if (!plugin->sentry_initialized) {
        platch_respond_error_std(responsehandle, "1", "Sentry is not initialized.", &STDNULL);
        return;
    }

    if (!raw_std_value_is_map(arg)) {
        platch_respond_error_std(responsehandle, "4", "Expected `arg` to be a Map.", &STDNULL);
        return;
    }

    const struct raw_std_value *key = raw_std_map_find_str(arg, "key");
    if (key == NULL || !raw_std_value_is_string(key)) {
        platch_respond_error_std(responsehandle, "4", "Expected `arg['key']` to be a string.", &STDNULL);
        return;
    }

    const struct raw_std_value *value = raw_std_map_find_str(arg, "value");
    if (value == NULL || !raw_std_value_is_string(value)) {
        platch_respond_error_std(responsehandle, "4", "Expected `arg['value']` to be a string.", &STDNULL);
        return;
    }

    LOG_SENTRY_DEBUG(
        "setTag(), key: %.*s, value: %.*s\n",
        (int) raw_std_string_get_length(key),
        raw_std_string_get_nonzero_terminated(key),
        (int) raw_std_string_get_length(value),
        raw_std_string_get_nonzero_terminated(value)
    );

    // clang-format off
    sentry_set_tag_n(
        raw_std_string_get_nonzero_terminated(key),
        raw_std_string_get_length(key),
        raw_std_string_get_nonzero_terminated(value),
        raw_std_string_get_length(value)
    );

    // clang-format on

    platch_respond_success_std(responsehandle, &STDNULL);
}

static void
on_remove_tag(struct sentry_plugin *plugin, const struct raw_std_value *arg, const FlutterPlatformMessageResponseHandle *responsehandle) {
    (void) plugin;

    if (!plugin->sentry_initialized) {
        platch_respond_error_std(responsehandle, "1", "Sentry is not initialized.", &STDNULL);
        return;
    }

    if (!raw_std_value_is_map(arg)) {
        platch_respond_error_std(responsehandle, "4", "Expected `arg` to be a Map.", &STDNULL);
        return;
    }

    const struct raw_std_value *key = raw_std_map_find_str(arg, "key");
    if (key == NULL || !raw_std_value_is_string(key)) {
        platch_respond_error_std(responsehandle, "4", "Expected `arg['key']` to be a string.", &STDNULL);
        return;
    }

    LOG_SENTRY_DEBUG("removeTag(), key: %.*s\n", (int) raw_std_string_get_length(key), raw_std_string_get_nonzero_terminated(key));

    sentry_remove_tag_n(raw_std_string_get_nonzero_terminated(key), raw_std_string_get_length(key));

    platch_respond_success_std(responsehandle, &STDNULL);
}

static void on_discard_profiler(
    struct sentry_plugin *plugin,
    const struct raw_std_value *arg,
    const FlutterPlatformMessageResponseHandle *responsehandle
) {
    (void) plugin;
    (void) arg;

    LOG_SENTRY_DEBUG("discardProfiler()\n");

    /// TODO: Implement
    platch_respond_not_implemented(responsehandle);
}

static void on_collect_profile(
    struct sentry_plugin *plugin,
    const struct raw_std_value *arg,
    const FlutterPlatformMessageResponseHandle *responsehandle
) {
    (void) plugin;

    if (!raw_std_value_is_map(arg)) {
        platch_respond_error_std(responsehandle, "4", "Expected `arg` to be a Map.", &STDNULL);
        return;
    }

    const struct raw_std_value *trace_id = raw_std_map_find_str(arg, "traceId");
    if (trace_id == NULL || !raw_std_value_is_string(trace_id)) {
        platch_respond_error_std(responsehandle, "4", "Expected `arg['traceId']` to be a string.", &STDNULL);
        return;
    }

    const struct raw_std_value *start_time = raw_std_map_find_str(arg, "startTime");
    if (start_time == NULL || !raw_std_value_is_int(start_time)) {
        platch_respond_error_std(responsehandle, "4", "Expected `arg['startTime']` to be an int.", &STDNULL);
        return;
    }

    const struct raw_std_value *end_time = raw_std_map_find_str(arg, "endTime");
    if (end_time == NULL || !raw_std_value_is_int(end_time)) {
        platch_respond_error_std(responsehandle, "4", "Expected `arg['endTime']` to be an int.", &STDNULL);
        return;
    }

    LOG_SENTRY_DEBUG(
        "collectProfile(), traceId: %.*s, startTime: %" PRId64 ", endTime: %" PRId64 "\n",
        (int) raw_std_string_get_length(trace_id),
        raw_std_string_get_nonzero_terminated(trace_id),
        raw_std_value_as_int(start_time),
        raw_std_value_as_int(end_time)
    );

    /// TODO: Implement

    platch_respond_not_implemented(responsehandle);
}

static void on_method_call(void *userdata, const FlutterPlatformMessage *message) {
    const FlutterPlatformMessageResponseHandle *responsehandle;
    const struct raw_std_value *envelope, *method, *arg;
    struct sentry_plugin *plugin;

    ASSERT_NOT_NULL(userdata);
    ASSERT_NOT_NULL(message);
    plugin = userdata;
    responsehandle = message->response_handle;
    envelope = (const struct raw_std_value *) (message->message);
    if (!raw_std_method_call_check(envelope, message->message_size)) {
        platch_respond_error_std(responsehandle, "malformed-message", "", &STDNULL);
        return;
    }

    method = raw_std_method_call_get_method(envelope);
    arg = raw_std_method_call_get_arg(envelope);

    if (raw_std_string_equals(method, "initNativeSdk")) {
        on_init_native_sdk(plugin, arg, responsehandle);
    } else if (raw_std_string_equals(method, "captureEnvelope")) {
        on_capture_envelope(plugin, arg, responsehandle);
    } else if (raw_std_string_equals(method, "loadImageList")) {
        on_load_image_list(plugin, arg, responsehandle);
    } else if (raw_std_string_equals(method, "closeNativeSdk")) {
        on_close_native_sdk(plugin, arg, responsehandle);
    } else if (raw_std_string_equals(method, "fetchNativeAppStart")) {
        on_fetch_native_app_start(plugin, arg, responsehandle);
    } else if (raw_std_string_equals(method, "beginNativeFrames")) {
        on_begin_native_frames(plugin, arg, responsehandle);
    } else if (raw_std_string_equals(method, "endNativeFrames")) {
        on_end_native_frames(plugin, arg, responsehandle);
    } else if (raw_std_string_equals(method, "setUser")) {
        on_set_user(plugin, arg, responsehandle);
    } else if (raw_std_string_equals(method, "addBreadcrumb")) {
        on_add_breadcrumb(plugin, arg, responsehandle);
    } else if (raw_std_string_equals(method, "clearBreadcrumbs")) {
        on_clear_breadcrumbs(plugin, arg, responsehandle);
    } else if (raw_std_string_equals(method, "setContexts")) {
        on_set_contexts(plugin, arg, responsehandle);
    } else if (raw_std_string_equals(method, "removeContexts")) {
        on_remove_contexts(plugin, arg, responsehandle);
    } else if (raw_std_string_equals(method, "setExtra")) {
        on_set_extra(plugin, arg, responsehandle);
    } else if (raw_std_string_equals(method, "removeExtra")) {
        on_remove_extra(plugin, arg, responsehandle);
    } else if (raw_std_string_equals(method, "setTag")) {
        on_set_tag(plugin, arg, responsehandle);
    } else if (raw_std_string_equals(method, "removeTag")) {
        on_remove_tag(plugin, arg, responsehandle);
    } else if (raw_std_string_equals(method, "discardProfiler")) {
        on_discard_profiler(plugin, arg, responsehandle);
    } else if (raw_std_string_equals(method, "collectProfile")) {
        on_collect_profile(plugin, arg, responsehandle);
    } else {
        platch_respond_error_std(responsehandle, "unknown-method", "", &STDNULL);
    }
}

enum plugin_init_result sentry_plugin_deinit(struct flutterpi *flutterpi, void **userdata_out) {
    struct sentry_plugin *plugin;
    int ok;

    plugin = malloc(sizeof *plugin);
    if (plugin == NULL) {
        return PLUGIN_INIT_RESULT_ERROR;
    }

    ok = plugin_registry_set_receiver_v2_locked(
        flutterpi_get_plugin_registry(flutterpi),
        SENTRY_PLUGIN_METHOD_CHANNEL,
        on_method_call,
        plugin
    );
    if (ok != 0) {
        free(plugin);
        return PLUGIN_INIT_RESULT_ERROR;
    }

    *userdata_out = plugin;

    return PLUGIN_INIT_RESULT_INITIALIZED;
}

void sentry_plugin_init(struct flutterpi *flutterpi, void *userdata) {
    struct sentry_plugin *plugin;

    ASSERT_NOT_NULL(userdata);
    plugin = userdata;

    if (plugin->sentry_initialized) {
        sentry_close();
    }

    plugin_registry_remove_receiver_v2_locked(flutterpi_get_plugin_registry(flutterpi), SENTRY_PLUGIN_METHOD_CHANNEL);
    free(plugin);
}

FLUTTERPI_PLUGIN("sentry", sentry_plugin_init, sentry_plugin_deinit, NULL);
