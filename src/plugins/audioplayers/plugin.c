#define _GNU_SOURCE

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include <gst/gst.h>
#include <glib.h>

#include "flutter_embedder.h"
#include "util/asserts.h"
#include "util/macros.h"

#include "flutter-pi.h"
#include "platformchannel.h"
#include "pluginregistry.h"
#include "notifier_listener.h"

#include "util/collection.h"
#include "util/list.h"
#include "util/logging.h"
#include "util/khash.h"
#include "plugins/gstplayer.h"

#define AUDIOPLAYERS_LOCAL_CHANNEL "xyz.luan/audioplayers"
#define AUDIOPLAYERS_GLOBAL_CHANNEL "xyz.luan/audioplayers.global"

#define STR_LINK_TROUBLESHOOTING \
  "https://github.com/bluefireteam/audioplayers/blob/main/troubleshooting.md"

KHASH_MAP_INIT_STR(audioplayers, struct gstplayer *)

struct audioplayer_meta {
    char *id;
    char *event_channel;
    bool subscribed;
    bool release_on_stop;

    struct listener *duration_listener;
    struct listener *eos_listener;
    struct listener *error_listener;
};

struct plugin {
    struct flutterpi *flutterpi;
    bool initialized;

    khash_t(audioplayers) players;
};

static const char *player_get_id(struct gstplayer *player) {
    struct audioplayer_meta *meta = gstplayer_get_userdata(player);

    return meta->id;
}

#define LOG_AUDIOPLAYER_DEBUG(player, fmtstring, ...) LOG_DEBUG("audio player \"%s\": " fmtstring, player_get_id(player), ##__VA_ARGS__)
#define LOG_AUDIOPLAYER_ERROR(player, fmtstring, ...) LOG_ERROR("audio player \"%s\": " fmtstring, player_get_id(player), ##__VA_ARGS__)

static void on_receive_event_ch(void *userdata, const FlutterPlatformMessage *message);

static void respond_plugin_error_ext(const FlutterPlatformMessageResponseHandle *response_handle, const char *message, struct std_value *details) {
    platch_respond_error_std(response_handle, "LinuxAudioError", (char*) message, details);
}

static void respond_plugin_error(const FlutterPlatformMessageResponseHandle *response_handle, const char *message) {
    respond_plugin_error_ext(response_handle, message, NULL);
}

static bool ensure_gstreamer_initialized(struct plugin *plugin, const FlutterPlatformMessageResponseHandle *responsehandle) {
    if (plugin->initialized) {
        return true;
    }

    GError *error;
    gboolean success = gst_init_check(NULL, NULL, &error);
    if (success) {
        plugin->initialized = true;
        return true;
    }

    char *details = NULL;
    int status = asprintf(&details, "%s (Domain: %s, Code: %d)", error->message, g_quark_to_string(error->domain), error->code);
    if (status == -1) {
        // ENOMEM;
        return false;
    }

    // clang-format off
    respond_plugin_error_ext(
        responsehandle,
        "Failed to initialize gstreamer.",
        &STDSTRING(details)
    );
    // clang-format on

    free(details);

    return false;
}

static struct gstplayer *get_player_by_id(struct plugin *plugin, const char *id) {
    khint_t index = kh_get_audioplayers(&plugin->players, id);
    if (index == kh_end(&plugin->players)) {
        return NULL;
    }

    return kh_value(&plugin->players, index);
}

static const struct raw_std_value *get_player_id_from_arg(const struct raw_std_value *arg, const FlutterPlatformMessageResponseHandle *responsehandle) {
    if (!raw_std_value_is_map(arg)) {
        platch_respond_illegal_arg_std(responsehandle, "Expected `arg` to be a map.");
        return NULL;
    }

    const struct raw_std_value *player_id = raw_std_map_find_str(arg, "playerId");
    if (player_id == NULL || !raw_std_value_is_string(player_id)) {
        platch_respond_illegal_arg_std(responsehandle, "Expected `arg['playerId']` to be a string.");
        return NULL;
    }

    return player_id;
}

static struct gstplayer *get_player_from_arg(struct plugin *plugin, const struct raw_std_value *arg, const FlutterPlatformMessageResponseHandle *responsehandle) {
    const struct raw_std_value *id = get_player_id_from_arg(arg, responsehandle);
    if (id == NULL) {
        return NULL;
    }

    char *id_duped = raw_std_string_dup(id);
    if (id_duped == NULL) {
        return NULL;
    }

    struct gstplayer *player = get_player_by_id(plugin, id_duped);

    free(id_duped);

    if (player == NULL) {
        platch_respond_illegal_arg_std(responsehandle, "Expected `arg['playerId']` to be a valid player id.");
        return NULL;
    }

    return player;
}

static void send_error_event(struct audioplayer_meta *meta, GError *error) {
    if (!meta->subscribed) {
        return;
    }

    gchar* message;
    if (error->domain == GST_STREAM_ERROR ||
        error->domain == GST_RESOURCE_ERROR) {
        message =
            "Failed to set source. For troubleshooting, "
            "see: " STR_LINK_TROUBLESHOOTING;
    } else {
        message = "Unknown GstGError. See details.";
    }

    char *details = NULL;
    int status = asprintf(&details, "%s (Domain: %s, Code: %d)", error->message, g_quark_to_string(error->domain), error->code);
    if (status == -1) {
        // ENOMEM;
        return;
    }

    // clang-format off
    platch_send_error_event_std(
        meta->event_channel,
        "LinuxAudioError",
        message,
        &STDSTRING(details)
    );
    // clang-format on

    free(details);
}

static void send_prepared_event(struct audioplayer_meta *meta, bool prepared) {
    if (!meta->subscribed) {
        return;
    }

    // clang-format off
    platch_send_success_event_std(
        meta->event_channel,
        &STDMAP2(
            STDSTRING("event"), STDSTRING("audio.onPrepared"),
            STDSTRING("value"), STDBOOL(prepared)
        )
    );
    // clang-format on
}

static void send_duration_update(struct audioplayer_meta *meta, bool has_duration, int64_t duration_ms) {
    if (!meta->subscribed) {
        return;
    }

    if (!has_duration) {
        // TODO: Check the behaviour in upstream audioplayers
        return;
    }

    // clang-format off
    platch_send_success_event_std(
        meta->event_channel,
        &STDMAP2(
            STDSTRING("event"), STDSTRING("audio.onDuration"),
            STDSTRING("value"), STDINT64(duration_ms)
        )
    );
    // clang-format on
}

static void send_seek_completed(struct audioplayer_meta *meta) {
    if (!meta->subscribed) {
        return;
    }

    // clang-format off
    platch_send_success_event_std(
        meta->event_channel,
        &STDMAP1(
            STDSTRING("event"), STDSTRING("audio.onSeekComplete")
        )
    );
    // clang-format on
}

static void send_playback_complete(struct audioplayer_meta *meta) {
    if (!meta->subscribed) {
        return;
    }

    // clang-format off
    platch_send_success_event_std(
        meta->event_channel,
        &STDMAP1(
            STDSTRING("event"), STDSTRING("audio.onComplete")
        )
    );
    // clang-format on
}

UNUSED static void send_player_log(struct audioplayer_meta *meta, const char *message) {
    if (!meta->subscribed) {
        return;
    }

    // clang-format off
    platch_send_success_event_std(
        meta->event_channel,
        &STDMAP2(
            STDSTRING("event"), STDSTRING("audio.onLog"),
            STDSTRING("value"), STDSTRING((char*) message)
        )
    );
    // clang-format on
}

static void on_create(struct plugin *p, const struct raw_std_value *arg, const FlutterPlatformMessageResponseHandle *responsehandle) {
    const struct raw_std_value *player_id = get_player_id_from_arg(arg, responsehandle);
    if (!player_id) {
        return;
    }

    if (!ensure_gstreamer_initialized(p, responsehandle)) {
        return;
    }

    struct audioplayer_meta *meta = calloc(1, sizeof(struct audioplayer_meta));
    if (meta == NULL) {
        platch_respond_native_error_std(responsehandle, ENOMEM);
        return;
    }

    meta->id = raw_std_string_dup(player_id);
    if (meta->id == NULL) {
        free(meta);
        platch_respond_native_error_std(responsehandle, ENOMEM);
        return;
    }

    LOG_DEBUG("create(id: \"%s\")\n", meta->id);

    int status = 0;
    khint_t index = kh_put(audioplayers, &p->players, meta->id, &status);
    if (status == -1) {
        free(meta->id);
        free(meta);
        platch_respond_native_error_std(responsehandle, ENOMEM);
        return;
    } else if (status == 0) {
        free(meta->id);
        free(meta);

        platch_respond_illegal_arg_std(responsehandle, "Player with given id already exists.");
        return;
    }

    status = asprintf(&meta->event_channel, "xyz.luan/audioplayers/events/%s", meta->id);
    if (status == -1) {
        kh_del(audioplayers, &p->players, index);
        free(meta->id);
        free(meta);

        platch_respond_native_error_std(responsehandle, ENOMEM);
        return;
    }

    struct gstplayer *player = gstplayer_new(
        p->flutterpi,
        NULL,
        meta,
        /* play_video */ false, /* play_audio */ true,
        NULL
    );
    if (player == NULL) {
        free(meta->event_channel);
        kh_del(audioplayers, &p->players, index);
        free(meta->id);
        free(meta);

        platch_respond_error_std(responsehandle, "not-initialized", "Could not initialize gstplayer.", NULL);
        return;
    }

    gstplayer_set_userdata(player, meta);

    plugin_registry_set_receiver_v2(
        flutterpi_get_plugin_registry(flutterpi),
        meta->event_channel,
        on_receive_event_ch,
        player
    );

    kh_value(&p->players, index) = player;

    platch_respond_success_std(responsehandle, NULL);
}

static void on_pause(struct plugin *p, const struct raw_std_value *arg, const FlutterPlatformMessageResponseHandle *responsehandle) {
    struct gstplayer *player = get_player_from_arg(p, arg, responsehandle);
    if (player == NULL) {
        return;
    }

    LOG_AUDIOPLAYER_DEBUG(player, "pause()\n");

    gstplayer_pause(player);

    platch_respond_success_std(responsehandle, NULL);
}

static void on_resume(struct plugin *p, const struct raw_std_value *arg, const FlutterPlatformMessageResponseHandle *responsehandle) {
    struct gstplayer *player = get_player_from_arg(p, arg, responsehandle);
    if (player == NULL) {
        return;
    }

    LOG_AUDIOPLAYER_DEBUG(player, "resume()\n");

    /// TODO: Should resume behave different to play?
    gstplayer_play(player);

    platch_respond_success_std(responsehandle, NULL);
}

static void on_stop(struct plugin *p, const struct raw_std_value *arg, const FlutterPlatformMessageResponseHandle *responsehandle) {
    struct gstplayer *player = get_player_from_arg(p, arg, responsehandle);
    if (player == NULL) {
        return;
    }

    LOG_AUDIOPLAYER_DEBUG(player, "stop()\n");

    /// TODO: Maybe provide gstplayer_stop
    int err = gstplayer_pause(player);
    if (err != 0) {
        platch_respond_success_std(responsehandle, NULL);
        return;
    }

    err = gstplayer_seek_to(player, 0, /* nearest_keyframe */ false);
    if (err != 0) {
        platch_respond_success_std(responsehandle, NULL);
        return;
    }

    platch_respond_success_std(responsehandle, NULL);
}

static void on_release(struct plugin *p, const struct raw_std_value *arg, const FlutterPlatformMessageResponseHandle *responsehandle) {
    struct gstplayer *player = get_player_from_arg(p, arg, responsehandle);
    if (player == NULL) {
        return;
    }

    LOG_AUDIOPLAYER_DEBUG(player, "release()\n");

    gstplayer_set_source(player, NULL);

    platch_respond_success_std(responsehandle, NULL);
}

static void on_seek(struct plugin *p, const struct raw_std_value *arg, const FlutterPlatformMessageResponseHandle *responsehandle) {
    struct gstplayer *player = get_player_from_arg(p, arg, responsehandle);
    if (player == NULL) {
        return;
    }

    const struct raw_std_value *position = raw_std_map_find_str(arg, "position");
    if (position == NULL || !raw_std_value_is_int(position)) {
        platch_respond_illegal_arg_std(responsehandle, "Expected `arg['position'] to be an int.");
        return;
    }

    int64_t position_int = raw_std_value_as_int(position);

    LOG_AUDIOPLAYER_DEBUG(player, "seek(position_ms: %"PRIi64")\n", position_int);

    gstplayer_seek_with_completer(
        player,
        position_int,
        /* nearest_keyframe */ false,
        (struct async_completer) {
            .on_done = (void_callback_t) send_seek_completed,
            .on_error = NULL,
            .userdata = gstplayer_get_userdata(player)
        }
    );

    platch_respond_success_std(responsehandle, NULL);
}

static void on_set_source_url_complete(void *userdata) {
    struct audioplayer_meta *meta = userdata;

    send_prepared_event(meta, true);
}

static void on_set_source_url(struct plugin *p, const struct raw_std_value *arg, const FlutterPlatformMessageResponseHandle *responsehandle) {
    struct gstplayer *player = get_player_from_arg(p, arg, responsehandle);
    if (player == NULL) {
        return;
    }

    const struct raw_std_value *src_url = raw_std_map_find_str(arg, "url");
    if (src_url == NULL || !raw_std_value_is_string(src_url)) {
        platch_respond_illegal_arg_std(responsehandle, "Expected `arg['url']` to be a string.");
        return;
    }

    const struct raw_std_value *is_local = raw_std_map_find_str(arg, "isLocal");
    if (src_url != NULL && !raw_std_value_is_null(is_local) && !raw_std_value_is_bool(is_local)) {
        platch_respond_illegal_arg_std(responsehandle, "Expected `arg['isLocal']` to be a bool or null.");
        return;
    }

    const struct raw_std_value *mime_type = raw_std_map_find_str(arg, "mimeType");
    if (mime_type != NULL && !raw_std_value_is_null(mime_type) && !raw_std_value_is_string(mime_type)) {
        platch_respond_illegal_arg_std(responsehandle, "Expected `arg['mimeType']` to be a bool or null.");
        return;
    }

    char *src_url_duped = raw_std_string_dup(src_url);
    if (!src_url_duped) return;

    LOG_AUDIOPLAYER_DEBUG(player, "set_source_url(url: \"%s\")\n", src_url_duped);

    // audioplayers attempts to use file paths (e.g. /tmp/abcd) as source URIs.
    // detect that and constrcut a proper url from it.
    if (src_url_duped[0] == '/') {
        free(src_url_duped);

        int result = asprintf(
            &src_url_duped,
            "file://%.*s",
            (int) raw_std_string_get_length(src_url),
            raw_std_string_get_nonzero_terminated(src_url)
        );
        if (result < 0) {
            return;
        }
    }

    bool ok = gstplayer_set_source_with_completer(
        player,
        src_url_duped,
        (struct async_completer) {
            .on_done = on_set_source_url_complete,
            .userdata = gstplayer_get_userdata(player)
        }
    );

    free(src_url_duped);

    if (!ok) {
        respond_plugin_error(responsehandle, "Could not preroll pipeline.");
        return;
    }

    platch_respond_success_std(responsehandle, NULL);
}

static void on_get_duration(struct plugin *p, const struct raw_std_value *arg, const FlutterPlatformMessageResponseHandle *responsehandle) {
    struct gstplayer *player = get_player_from_arg(p, arg, responsehandle);
    if (player == NULL) {
        return;
    }

    LOG_AUDIOPLAYER_DEBUG(player, "get_duration()\n");

    int64_t duration_ms = gstplayer_get_duration(player);
    if (duration_ms == -1) {
        platch_respond_success_std(responsehandle, NULL);
        return;
    }

    platch_respond_success_std(responsehandle, &STDINT64(duration_ms));
}

static void on_set_volume(struct plugin *p, const struct raw_std_value *arg, const FlutterPlatformMessageResponseHandle *responsehandle) {
    struct gstplayer *player = get_player_from_arg(p, arg, responsehandle);
    if (player == NULL) {
        return;
    }

    const struct raw_std_value *volume = raw_std_map_find_str(arg, "volume");
    if (volume == NULL || !raw_std_value_is_float64(volume)) {
        platch_respond_illegal_arg_std(responsehandle, "Expected `arg['volume'] to be a double.");
        return;
    }

    double volume_float = raw_std_value_as_float64(volume);

    LOG_AUDIOPLAYER_DEBUG(player, "set_volume(volume: %f)\n", volume_float);

    gstplayer_set_volume(player, volume_float);

    platch_respond_success_std(responsehandle, NULL);
}

static void on_get_position(struct plugin *p, const struct raw_std_value *arg, const FlutterPlatformMessageResponseHandle *responsehandle) {
    struct gstplayer *player = get_player_from_arg(p, arg, responsehandle);
    if (player == NULL) {
        return;
    }

    int64_t position = gstplayer_get_position(player);
    if (position < 0) {
        platch_respond_success_std(responsehandle, &STDNULL);
        return;
    }

    platch_respond_success_std(responsehandle, &STDINT64(position));
}

static void on_set_playback_rate(struct plugin *p, const struct raw_std_value *arg, const FlutterPlatformMessageResponseHandle *responsehandle) {
    struct gstplayer *player = get_player_from_arg(p, arg, responsehandle);
    if (player == NULL) {
        return;
    }

    const struct raw_std_value *rate = raw_std_map_find_str(arg, "playbackRate");
    if (rate == NULL || !raw_std_value_is_float64(rate)) {
        platch_respond_illegal_arg_std(responsehandle, "Expected `arg['playbackRate'] to be a double.");
        return;
    }

    double rate_float = raw_std_value_as_float64(rate);

    LOG_AUDIOPLAYER_DEBUG(player, "set_playback_rate(rate: %f)\n", rate_float);

    if (rate_float < 0.0) {
        respond_plugin_error(responsehandle, "Backward playback is not supported.\n");
        return;
    } else if (rate_float == 0.0) {
        gstplayer_pause(player);
    } else {
        gstplayer_set_playback_speed(player, rate_float);
    }

    platch_respond_success_std(responsehandle, NULL);
}

static void on_set_release_mode(struct plugin *p, const struct raw_std_value *arg, const FlutterPlatformMessageResponseHandle *responsehandle) {
    struct gstplayer *player = get_player_from_arg(p, arg, responsehandle);
    if (player == NULL) {
        return;
    }

    const struct raw_std_value *mode = raw_std_map_find_str(arg, "releaseMode");
    if (mode == NULL || !raw_std_value_is_string(mode)) {
        platch_respond_illegal_arg_std(responsehandle, "Expected `arg['releaseMode'] to be a string.");
        return;
    }

    LOG_AUDIOPLAYER_DEBUG(player, "set_release_mode(mode: %.*s)\n",
        (int) raw_std_string_get_length(mode),
        raw_std_string_get_nonzero_terminated(mode)
    );

    bool is_release = false;
    bool is_loop = false;
    bool is_stop = false;

    if (raw_std_string_equals(mode, "ReleaseMode.release")) {
        is_release = true;
    } else if (raw_std_string_equals(mode, "ReleaseMode.loop")) {
        is_loop = true;
    } else if (raw_std_string_equals(mode, "ReleaseMode.stop")) {
        is_stop = true;
    } else {
        platch_respond_illegal_arg_std(responsehandle, "Expected `arg['releaseMode']` to be a string-ification of a ReleaseMode enum value.");
        return;
    }

    // TODO: Handle ReleaseMode.release & ReleaseMode.stop
    (void) is_release;
    (void) is_stop;

    int err = gstplayer_set_looping(player, is_loop, false);
    if (err != 0) {
        platch_respond_success_std(responsehandle, NULL);
        return;
    }

    platch_respond_success_std(responsehandle, NULL);
}

static void on_set_player_mode(struct plugin *p, const struct raw_std_value *arg, const FlutterPlatformMessageResponseHandle *responsehandle) {
    struct gstplayer *player = get_player_from_arg(p, arg, responsehandle);
    if (player == NULL) {
        return;
    }

    const struct raw_std_value *mode = raw_std_map_find_str(arg, "playerMode");
    if (mode == NULL || !raw_std_value_is_string(mode)) {
        platch_respond_illegal_arg_std(responsehandle, "Expected `arg['playerMode'] to be a string.");
        return;
    }

    LOG_AUDIOPLAYER_DEBUG(player, "set_player_mode(mode: %.*s)\n",
        (int) raw_std_string_get_length(mode),
        raw_std_string_get_nonzero_terminated(mode)
    );

    bool is_media_player = false;
    bool is_low_latency = false;

    if (raw_std_string_equals(mode, "PlayerMode.mediaPlayer")) {
        is_media_player = true;
    } else if (raw_std_string_equals(mode, "PlayerMode.lowLatency")) {
        is_low_latency = true;
    } else {
        platch_respond_illegal_arg_std(responsehandle, "Expected `arg['playerMode']` to be a string-ification of a PlayerMode enum value.");
        return;
    }

    // TODO: Handle player mode
    // TODO check support for low latency mode:
    // https://gstreamer.freedesktop.org/documentation/additional/design/latency.html?gi-language=c
    (void) is_media_player;
    (void) is_low_latency;

    platch_respond_success_std(responsehandle, NULL);
}

static void on_set_balance(struct plugin *p, const struct raw_std_value *arg, const FlutterPlatformMessageResponseHandle *responsehandle) {
    struct gstplayer *player = get_player_from_arg(p, arg, responsehandle);
    if (player == NULL) {
        return;
    }

    const struct raw_std_value *balance = raw_std_map_find_str(arg, "balance");
    if (balance == NULL || !raw_std_value_is_float64(balance)) {
        platch_respond_illegal_arg_std(responsehandle, "Expected `arg['balance'] to be a double.");
        return;
    }

    double balance_float = raw_std_value_as_float64(balance);

    LOG_AUDIOPLAYER_DEBUG(player, "set_balance(balance: %f)\n", balance_float);

    if (balance_float < -1.0) {
        balance_float = -1.0;
    } else if (balance_float > 1.0) {
        balance_float = 1.0;
    }

    gstplayer_set_audio_balance(player, balance_float);

    platch_respond_success_std(responsehandle, NULL);
}

static void on_player_emit_log(struct plugin *p, const struct raw_std_value *arg, const FlutterPlatformMessageResponseHandle *responsehandle) {
    struct gstplayer *player = get_player_from_arg(p, arg, responsehandle);
    if (player == NULL) {
        return;
    }

    const struct raw_std_value *message = raw_std_map_find_str(arg, "message");
    if (message == NULL || !raw_std_value_is_string(message)) {
        platch_respond_illegal_arg_std(responsehandle, "Expected `arg['message'] to be a string.");
        return;
    }

    LOG_DEBUG("%.*s", (int) raw_std_string_get_length(message), raw_std_string_get_nonzero_terminated(message));

    platch_respond_success_std(responsehandle, NULL);
}

static void on_player_emit_error(struct plugin *p, const struct raw_std_value *arg, const FlutterPlatformMessageResponseHandle *responsehandle) {
    struct gstplayer *player = get_player_from_arg(p, arg, responsehandle);
    if (player == NULL) {
        return;
    }

    const struct raw_std_value *code = raw_std_map_find_str(arg, "code");
    if (code == NULL || !raw_std_value_is_string(code)) {
        platch_respond_illegal_arg_std(responsehandle, "Expected `arg['code'] to be a string.");
        return;
    }

    const struct raw_std_value *message = raw_std_map_find_str(arg, "message");
    if (message == NULL || !raw_std_value_is_string(message)) {
        platch_respond_illegal_arg_std(responsehandle, "Expected `arg['message'] to be a string.");
        return;
    }

    LOG_ERROR(
        "%.*s, %.*s",
        (int) raw_std_string_get_length(code), raw_std_string_get_nonzero_terminated(code),
        (int) raw_std_string_get_length(message), raw_std_string_get_nonzero_terminated(message)
    );

    platch_respond_success_std(responsehandle, NULL);
}

static void on_dispose(struct plugin *p, const struct raw_std_value *arg, const FlutterPlatformMessageResponseHandle *responsehandle) {
    const struct raw_std_value *id = get_player_id_from_arg(arg, responsehandle);
    if (id == NULL) {
        return;
    }

    struct gstplayer *player = get_player_from_arg(p, arg, responsehandle);
    if (player == NULL) {
        return;
    }

    LOG_AUDIOPLAYER_DEBUG(player, "dispose()\n");

    char *id_duped = raw_std_string_dup(id);

    khint_t index = kh_get(audioplayers, &p->players, id_duped);

    // Should be valid since we already know the player exists from above
    assert(index <= kh_end(&p->players));

    free(id_duped);

    // Remove the entry from the hashmap
    kh_del(audioplayers, &p->players, index);

    struct audioplayer_meta *meta = gstplayer_get_userdata(player);

    plugin_registry_remove_receiver_v2(flutterpi_get_plugin_registry(p->flutterpi), meta->event_channel);
    free(meta->event_channel);
    free(meta->id);
    free(meta);

    // Destroy the player
    gstplayer_destroy(player);

    platch_respond_success_std(responsehandle, NULL);
}

static void on_player_method_call(void *userdata, const FlutterPlatformMessage *message) {
    struct plugin *plugin = userdata;

    const struct raw_std_value *envelope = raw_std_method_call_from_buffer(message->message, message->message_size);
    if (!envelope) {
        platch_respond_malformed_message_std(message);
        return;
    }

    const struct raw_std_value *arg = raw_std_method_call_get_arg(envelope);
    ASSERT_NOT_NULL(arg);

    if (raw_std_method_call_is_method(envelope, "create")) {
        on_create(plugin, arg, message->response_handle);
    } else if (raw_std_method_call_is_method(envelope, "pause")) {
        on_pause(plugin, arg, message->response_handle);
    } else if (raw_std_method_call_is_method(envelope, "resume")) {
        on_resume(plugin, arg, message->response_handle);
    } else if (raw_std_method_call_is_method(envelope, "stop")) {
        on_stop(plugin, arg, message->response_handle);
    } else if (raw_std_method_call_is_method(envelope, "release")) {
        on_release(plugin, arg, message->response_handle);
    } else if (raw_std_method_call_is_method(envelope, "seek")) {
        on_seek(plugin, arg, message->response_handle);
    } else if (raw_std_method_call_is_method(envelope, "setSourceUrl")) {
        on_set_source_url(plugin, arg, message->response_handle);
    } else if (raw_std_method_call_is_method(envelope, "getDuration")) {
        on_get_duration(plugin, arg, message->response_handle);
    } else if (raw_std_method_call_is_method(envelope, "setVolume")) {
        on_set_volume(plugin, arg, message->response_handle);
    } else if (raw_std_method_call_is_method(envelope, "getCurrentPosition")) {
        on_get_position(plugin, arg, message->response_handle);
    } else if (raw_std_method_call_is_method(envelope, "setPlaybackRate")) {
        on_set_playback_rate(plugin, arg, message->response_handle);
    } else if (raw_std_method_call_is_method(envelope, "setReleaseMode")) {
        on_set_release_mode(plugin, arg, message->response_handle);
    } else if (raw_std_method_call_is_method(envelope, "setPlayerMode")) {
        on_set_player_mode(plugin, arg, message->response_handle);
    } else if (raw_std_method_call_is_method(envelope, "setBalance") == 0) {
        on_set_balance(plugin, arg, message->response_handle);
    } else if (raw_std_method_call_is_method(envelope, "emitLog") == 0) {
        on_player_emit_log(plugin, arg, message->response_handle);
    } else if (raw_std_method_call_is_method(envelope, "emitError") == 0) {
        on_player_emit_error(plugin, arg, message->response_handle);
    } else if (raw_std_method_call_is_method(envelope, "dispose") == 0) {
        on_dispose(plugin, arg, message->response_handle);
    } else {
        platch_respond_not_implemented(message->response_handle);
    }
}

static void on_init(struct plugin *p, const struct raw_std_value *arg, const FlutterPlatformMessageResponseHandle *responsehandle) {
    (void) p;
    (void) arg;
    platch_respond_success_std(responsehandle, NULL);
}

static void on_set_audio_context(struct plugin *p, const struct raw_std_value *arg, const FlutterPlatformMessageResponseHandle *responsehandle) {
    (void) p;
    (void) arg;
    platch_respond_success_std(responsehandle, NULL);
}

static void on_emit_log(
    struct plugin *p,
    const struct raw_std_value *arg,
    const FlutterPlatformMessageResponseHandle *responsehandle
) {
    (void) p;

    const struct raw_std_value *message = raw_std_map_find_str(arg, "message");
    if (message == NULL || !raw_std_value_is_string(message)) {
        platch_respond_illegal_arg_std(responsehandle, "Expected `arg['message'] to be a string.");
        return;
    }

    LOG_DEBUG("%.*s", (int) raw_std_string_get_length(message), raw_std_string_get_nonzero_terminated(message));

    platch_respond_success_std(responsehandle, NULL);
}

static void on_emit_error(
    struct plugin *p,
    const struct raw_std_value *arg,
    const FlutterPlatformMessageResponseHandle *responsehandle
) {
    (void) p;

    const struct raw_std_value *code = raw_std_map_find_str(arg, "code");
    if (code == NULL || !raw_std_value_is_string(code)) {
        platch_respond_illegal_arg_std(responsehandle, "Expected `arg['code'] to be a string.");
        return;
    }

    const struct raw_std_value *message = raw_std_map_find_str(arg, "message");
    if (message == NULL || !raw_std_value_is_string(message)) {
        platch_respond_illegal_arg_std(responsehandle, "Expected `arg['message'] to be a string.");
        return;
    }

    LOG_ERROR(
        "%.*s, %.*s",
        (int) raw_std_string_get_length(code), raw_std_string_get_nonzero_terminated(code),
        (int) raw_std_string_get_length(message), raw_std_string_get_nonzero_terminated(message)
    );

    platch_respond_success_std(responsehandle, NULL);
}

static void on_global_method_call(void *userdata, const FlutterPlatformMessage *message) {
    struct plugin *plugin = userdata;

    const struct raw_std_value *envelope = raw_std_method_call_from_buffer(message->message, message->message_size);
    if (!envelope) {
        platch_respond_malformed_message_std(message);
        return;
    }

    const struct raw_std_value *arg = raw_std_method_call_get_arg(envelope);
    ASSERT_NOT_NULL(arg);

    if (raw_std_method_call_is_method(envelope, "init")) {
        on_init(plugin, arg, message->response_handle);
    } else if (raw_std_method_call_is_method(envelope, "setAudioContext")) {
        on_set_audio_context(plugin, arg, message->response_handle);
    } else if (raw_std_method_call_is_method(envelope, "emitLog")) {
        on_emit_log(plugin, arg, message->response_handle);
    } else if (raw_std_method_call_is_method(envelope, "emitError")) {
        on_emit_error(plugin, arg, message->response_handle);
    } else {
        platch_respond_not_implemented(message->response_handle);
    }
}

static enum listener_return on_duration_notify(void *arg, void *userdata) {
    ASSERT_NOT_NULL(userdata);
    struct gstplayer *player = userdata;

    struct audioplayer_meta *meta = gstplayer_get_userdata(player);
    ASSERT_NOT_NULL(meta);

    if (arg != NULL) {
        int64_t *duration_ms = arg;
        send_duration_update(meta, true, *duration_ms);
    } else {
        send_duration_update(meta, false, -1);
    }

    return kNoAction;
}

static enum listener_return on_eos_notify(void *arg, void *userdata) {
    (void) arg;

    ASSERT_NOT_NULL(userdata);
    struct gstplayer *player = userdata;

    struct audioplayer_meta *meta = gstplayer_get_userdata(player);
    ASSERT_NOT_NULL(meta);

    send_playback_complete(meta);

    return kNoAction;
}

static enum listener_return on_error_notify(void *arg, void *userdata) {
    ASSERT_NOT_NULL(arg);
    GError *error = arg;

    ASSERT_NOT_NULL(userdata);
    struct gstplayer *player = userdata;

    struct audioplayer_meta *meta = gstplayer_get_userdata(player);
    ASSERT_NOT_NULL(meta);

    send_error_event(meta, error);

    return kNoAction;
}

static void on_receive_event_ch(void *userdata, const FlutterPlatformMessage *message) {
    ASSERT_NOT_NULL(userdata);

    struct gstplayer *player = userdata;

    struct audioplayer_meta *meta = gstplayer_get_userdata(player);
    ASSERT_NOT_NULL(meta);

    const struct raw_std_value *envelope = raw_std_method_call_from_buffer(message->message, message->message_size);
    if (envelope == NULL) {
        platch_respond_malformed_message_std(message);
        return;
    }

    /// TODO: Implement
    if (raw_std_method_call_is_method(envelope, "listen")) {
        platch_respond_success_std(message->response_handle, NULL);

        if (!meta->subscribed) {
            meta->subscribed = true;

            meta->duration_listener = notifier_listen(gstplayer_get_duration_notifier(player), on_duration_notify, NULL, player);
            meta->eos_listener = notifier_listen(gstplayer_get_eos_notifier(player), on_eos_notify, NULL, player);
            meta->error_listener = notifier_listen(gstplayer_get_error_notifier(player), on_error_notify, NULL, player);
        }
    } else if (raw_std_method_call_is_method(envelope, "cancel")) {
        platch_respond_success_std(message->response_handle, NULL);

        if (meta->subscribed) {
            meta->subscribed = false;

            notifier_unlisten(gstplayer_get_eos_notifier(player), meta->error_listener);
            notifier_unlisten(gstplayer_get_eos_notifier(player), meta->eos_listener);
            notifier_unlisten(gstplayer_get_duration_notifier(player), meta->duration_listener);
        }
    } else {
        platch_respond_not_implemented(message->response_handle);
    }
}

enum plugin_init_result audioplayers_plugin_init(struct flutterpi *flutterpi, void **userdata_out) {
    int ok;

    (void) userdata_out;

    struct plugin *plugin = calloc(1, sizeof(struct plugin));
    if (plugin == NULL) {
        return PLUGIN_INIT_RESULT_ERROR;
    }

    plugin->flutterpi = flutterpi;
    plugin->initialized = false;

    ok = plugin_registry_set_receiver_v2_locked(
        flutterpi_get_plugin_registry(flutterpi),
        AUDIOPLAYERS_GLOBAL_CHANNEL,
        on_global_method_call,
        plugin
    );
    if (ok != 0) {
        return PLUGIN_INIT_RESULT_ERROR;
    }

    ok = plugin_registry_set_receiver_v2_locked(
        flutterpi_get_plugin_registry(flutterpi),
        AUDIOPLAYERS_LOCAL_CHANNEL,
        on_player_method_call,
        plugin
    );
    if (ok != 0) {
        goto fail_remove_global_receiver;
    }

    return PLUGIN_INIT_RESULT_INITIALIZED;

fail_remove_global_receiver:
    plugin_registry_remove_receiver_v2_locked(
        flutterpi_get_plugin_registry(flutterpi),
        AUDIOPLAYERS_GLOBAL_CHANNEL
    );

    return PLUGIN_INIT_RESULT_ERROR;
}

void audioplayers_plugin_deinit(struct flutterpi *flutterpi, void *userdata) {
    (void) flutterpi;

    ASSERT_NOT_NULL(userdata);
    struct plugin *plugin = userdata;

    plugin_registry_remove_receiver_v2_locked(flutterpi_get_plugin_registry(flutterpi), AUDIOPLAYERS_GLOBAL_CHANNEL);
    plugin_registry_remove_receiver_v2_locked(flutterpi_get_plugin_registry(flutterpi), AUDIOPLAYERS_LOCAL_CHANNEL);

    const char *id;
    struct gstplayer *player;
    kh_foreach(&plugin->players, id, player, {
        gstplayer_destroy(player);
        free((char*) id);
    })
}

FLUTTERPI_PLUGIN("audioplayers", audioplayers, audioplayers_plugin_init, audioplayers_plugin_deinit)
