#define _GNU_SOURCE

#include "flutter-pi.h"
#include "platformchannel.h"
#include "pluginregistry.h"
#include "plugins/audioplayers.h"
#include "util/collection.h"
#include "util/list.h"
#include "util/logging.h"

#define AUDIOPLAYERS_LOCAL_CHANNEL "xyz.luan/audioplayers"
#define AUDIOPLAYERS_GLOBAL_CHANNEL "xyz.luan/audioplayers.global"

static struct audio_player *audioplayers_linux_plugin_get_player(char *player_id, char *mode);
static void audioplayers_linux_plugin_dispose_player(struct audio_player *player);

struct audio_player_entry {
    struct list_head entry;
    struct audio_player *player;
};

static struct plugin {
    struct flutterpi *flutterpi;
    bool initialized;

    struct list_head players;
} plugin;

static int on_local_method_call(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *responsehandle) {
    struct audio_player *player;
    struct std_value *args, *tmp;
    const char *method;
    char *player_id, *mode;
    struct std_value result = STDNULL;
    int ok;

    (void) responsehandle;
    (void) channel;
    method = object->method;
    args = &object->std_arg;

    LOG_DEBUG("call(method=%s)\n", method);

    if (args == NULL || !STDVALUE_IS_MAP(*args)) {
        return platch_respond_illegal_arg_std(responsehandle, "Expected `arg` to be a map.");
    }

    tmp = stdmap_get_str(&object->std_arg, "playerId");
    if (tmp == NULL || !STDVALUE_IS_STRING(*tmp)) {
        LOG_ERROR("Call missing mandatory parameter player_id.\n");
        return platch_respond_illegal_arg_std(responsehandle, "Expected `arg['playerId'] to be a string.");
    }
    player_id = STDVALUE_AS_STRING(*tmp);
    tmp = stdmap_get_str(args, "mode");
    if (tmp == NULL) {
        mode = "";
    } else if (STDVALUE_IS_STRING(*tmp)) {
        mode = STDVALUE_AS_STRING(*tmp);
    } else {
        return platch_respond_illegal_arg_std(responsehandle, "Expected `arg['mode']` to be a string or null.");
    }

    player = audioplayers_linux_plugin_get_player(player_id, mode);
    if (player == NULL) {
        return platch_respond_native_error_std(responsehandle, ENOMEM);
    }

    if (streq(method, "create")) {
        //audioplayers_linux_plugin_get_player() creates player if it doesn't exist
    } else if (streq(method, "pause")) {
        audio_player_pause(player);
    } else if (streq(method, "resume")) {
        audio_player_resume(player);
    } else if (streq(method, "stop")) {
        audio_player_pause(player);
        audio_player_set_position(player, 0);
    } else if (streq(method, "release")) {
        audio_player_release(player);
    } else if (streq(method, "seek")) {
        tmp = stdmap_get_str(args, "position");
        if (tmp == NULL || !STDVALUE_IS_INT(*tmp)) {
            return platch_respond_illegal_arg_std(responsehandle, "Expected `arg['position']` to be an int.");
        }

        int64_t position = STDVALUE_AS_INT(*tmp);
        audio_player_set_position(player, position);
    } else if (streq(method, "setSourceUrl")) {
        tmp = stdmap_get_str(args, "url");
        if (tmp == NULL || !STDVALUE_IS_STRING(*tmp)) {
            return platch_respond_illegal_arg_std(responsehandle, "Expected `arg['url']` to be a string.");
        }
        char *url = STDVALUE_AS_STRING(*tmp);

        tmp = stdmap_get_str(args, "isLocal");
        if (tmp == NULL || !STDVALUE_IS_BOOL(*tmp)) {
            return platch_respond_illegal_arg_std(responsehandle, "Expected `arg['isLocal']` to be a bool.");
        }

        bool is_local = STDVALUE_AS_BOOL(*tmp);
        if (is_local) {
            char *local_url = NULL;
            ok = asprintf(&local_url, "file://%s", url);
            if (ok < 0) {
                return platch_respond_native_error_std(responsehandle, ENOMEM);
            }
            url = local_url;
        }

        audio_player_set_source_url(player, url);
    } else if (streq(method, "getDuration")) {
        result = STDINT64(audio_player_get_duration(player));
    } else if (streq(method, "setVolume")) {
        tmp = stdmap_get_str(args, "volume");
        if (tmp != NULL && STDVALUE_IS_FLOAT(*tmp)) {
            audio_player_set_volume(player, STDVALUE_AS_FLOAT(*tmp));
        } else {
            return platch_respond_illegal_arg_std(responsehandle, "Expected `arg['volume']` to be a float.");
        }
    } else if (streq(method, "getCurrentPosition")) {
        result = STDINT64(audio_player_get_position(player));
    } else if (streq(method, "setPlaybackRate")) {
        tmp = stdmap_get_str(args, "playbackRate");
        if (tmp != NULL && STDVALUE_IS_FLOAT(*tmp)) {
            audio_player_set_playback_rate(player, STDVALUE_AS_FLOAT(*tmp));
        } else {
            return platch_respond_illegal_arg_std(responsehandle, "Expected `arg['playbackRate']` to be a float.");
        }
    } else if (streq(method, "setReleaseMode")) {
        tmp = stdmap_get_str(args, "releaseMode");
        if (tmp != NULL && STDVALUE_IS_STRING(*tmp)) {
            char *release_mode = STDVALUE_AS_STRING(*tmp);
            bool looping = strstr(release_mode, "loop") != NULL;
            audio_player_set_looping(player, looping);
        } else {
            return platch_respond_illegal_arg_std(responsehandle, "Expected `arg['releaseMode']` to be a string.");
        }
    } else if (streq(method, "setPlayerMode")) {
        // TODO check support for low latency mode:
        // https://gstreamer.freedesktop.org/documentation/additional/design/latency.html?gi-language=c
    } else if (strcmp(method, "setBalance") == 0) {
        tmp = stdmap_get_str(args, "balance");
        if (tmp != NULL && STDVALUE_IS_FLOAT(*tmp)) {
            audio_player_set_balance(player, STDVALUE_AS_FLOAT(*tmp));
        } else {
            return platch_respond_illegal_arg_std(responsehandle, "Expected `arg['balance']` to be a float.");
        }
    } else if (strcmp(method, "emitLog") == 0) {
        tmp = stdmap_get_str(args, "message");
        char *message;

        if (tmp == NULL) {
            message = "";
        } else if (STDVALUE_IS_STRING(*tmp)) {
            message = STDVALUE_AS_STRING(*tmp);
        } else {
            return platch_respond_illegal_arg_std(responsehandle, "Expected `arg['message']` to be a string.");
        }

        LOG_DEBUG("%s\n", message);
        //TODO: https://github.com/bluefireteam/audioplayers/blob/main/packages/audioplayers_linux/linux/audio_player.cc#L247
    } else if (strcmp(method, "emitError") == 0) {
        tmp = stdmap_get_str(args, "code");
        char *code;

        if (tmp == NULL) {
            code = "";
        } else if (STDVALUE_IS_STRING(*tmp)) {
            code = STDVALUE_AS_STRING(*tmp);
        } else {
            return platch_respond_illegal_arg_std(responsehandle, "Expected `arg['code']` to be a string.");
        }

        tmp = stdmap_get_str(args, "message");
        char *message;

        if (tmp == NULL) {
            message = "";
        } else if (STDVALUE_IS_STRING(*tmp)) {
            message = STDVALUE_AS_STRING(*tmp);
        } else {
            return platch_respond_illegal_arg_std(responsehandle, "Expected `arg['message']` to be a string.");
        }

        LOG_ERROR("Error: %s; message=%s\n", code, message);
        //TODO: https://github.com/bluefireteam/audioplayers/blob/main/packages/audioplayers_linux/linux/audio_player.cc#L144
    } else if (strcmp(method, "dispose") == 0) {
        audioplayers_linux_plugin_dispose_player(player);
        player = NULL;
    } else {
        return platch_respond_not_implemented(responsehandle);
    }

    return platch_respond_success_std(responsehandle, &result);
}

static int on_global_method_call(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *responsehandle) {
    (void) responsehandle;
    (void) channel;
    (void) object;

    return platch_respond_success_std(responsehandle, &STDBOOL(true));
}

static int on_receive_event_ch(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *responsehandle) {
    if (strcmp(object->method, "listen") == 0) {
        LOG_DEBUG("%s: listen()\n", channel);

        list_for_each_entry_safe(struct audio_player_entry, entry, &plugin.players, entry) {
            if (audio_player_set_subscription_status(entry->player, channel, true)) {
                return platch_respond_success_std(responsehandle, NULL);
            }
        }

        LOG_ERROR("%s: player not found\n", channel);
        return platch_respond_not_implemented(responsehandle);
    } else if (strcmp(object->method, "cancel") == 0) {
        LOG_DEBUG("%s: cancel()\n", channel);

        list_for_each_entry_safe(struct audio_player_entry, entry, &plugin.players, entry) {
            if (audio_player_set_subscription_status(entry->player, channel, false)) {
                return platch_respond_success_std(responsehandle, NULL);
            }
        }

        LOG_ERROR("%s: player not found\n", channel);
        return platch_respond_not_implemented(responsehandle);
    } else {
        return platch_respond_not_implemented(responsehandle);
    }

    return 0;
}

enum plugin_init_result audioplayers_plugin_init(struct flutterpi *flutterpi, void **userdata_out) {
    int ok;

    (void) userdata_out;

    plugin.flutterpi = flutterpi;
    plugin.initialized = false;
    list_inithead(&plugin.players);

    ok = plugin_registry_set_receiver_locked(AUDIOPLAYERS_GLOBAL_CHANNEL, kStandardMethodCall, on_global_method_call);
    if (ok != 0) {
        return PLUGIN_INIT_RESULT_ERROR;
    }

    ok = plugin_registry_set_receiver_locked(AUDIOPLAYERS_LOCAL_CHANNEL, kStandardMethodCall, on_local_method_call);
    if (ok != 0) {
        goto fail_remove_global_receiver;
    }

    return PLUGIN_INIT_RESULT_INITIALIZED;

fail_remove_global_receiver:
    plugin_registry_remove_receiver_locked(AUDIOPLAYERS_GLOBAL_CHANNEL);

    return PLUGIN_INIT_RESULT_ERROR;
}

void audioplayers_plugin_deinit(struct flutterpi *flutterpi, void *userdata) {
    (void) flutterpi;
    (void) userdata;

    plugin_registry_remove_receiver_locked(AUDIOPLAYERS_GLOBAL_CHANNEL);
    plugin_registry_remove_receiver_locked(AUDIOPLAYERS_LOCAL_CHANNEL);

    list_for_each_entry_safe(struct audio_player_entry, entry, &plugin.players, entry) {
        audio_player_destroy(entry->player);
        list_del(&entry->entry);
        free(entry);
    }
}

static struct audio_player *audioplayers_linux_plugin_get_player(char *player_id, char *mode) {
    struct audio_player_entry *entry;
    struct audio_player *player;

    (void) mode;

    list_for_each_entry_safe(struct audio_player_entry, entry, &plugin.players, entry) {
        if (audio_player_is_id(entry->player, player_id)) {
            return entry->player;
        }
    }

    entry = malloc(sizeof *entry);
    ASSUME(entry != NULL);

    LOG_DEBUG("Create player(id=%s)\n", player_id);
    player = audio_player_new(player_id, AUDIOPLAYERS_LOCAL_CHANNEL);

    if (player == NULL) {
        LOG_ERROR("player(id=%s) cannot be created", player_id);
        free(entry);
        return NULL;
    }

    const char* event_channel = audio_player_subscribe_channel_name(player);
    // set a receiver on the videoEvents event channel
    int ok = plugin_registry_set_receiver(
        event_channel,
        kStandardMethodCall,
        on_receive_event_ch
    );
    if (ok != 0) {
        LOG_ERROR("Cannot set player receiver for event channel: %s\n", event_channel);
        audio_player_destroy(player);
        free(entry);
        return NULL;
    }

    entry->entry = (struct list_head){ NULL, NULL };
    entry->player = player;

    list_add(&entry->entry, &plugin.players);
    return player;
}

static void audioplayers_linux_plugin_dispose_player(struct audio_player *player) {
    list_for_each_entry_safe(struct audio_player_entry, entry, &plugin.players, entry) {
        if (entry->player == player) {
            list_del(&entry->entry);
            plugin_registry_remove_receiver(audio_player_subscribe_channel_name(player));
            audio_player_destroy(player);
        }
    }
}

FLUTTERPI_PLUGIN("audioplayers", audioplayers, audioplayers_plugin_init, audioplayers_plugin_deinit)
