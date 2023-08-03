#define _GNU_SOURCE

#include "flutter-pi.h"
#include "platformchannel.h"
#include "pluginregistry.h"
#include "plugins/audioplayers.h"

#include "util/collection.h"
#include "util/list.h"

#define AUDIOPLAYERS_LOCAL_CHANNEL "xyz.luan/audioplayers"
#define AUDIOPLAYERS_GLOBAL_CHANNEL "xyz.luan/audioplayers.global"

static struct audio_player *audioplayers_linux_plugin_get_player(char *player_id, char *mode);

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
    int result = 1;
    int ok;

    (void) responsehandle;
    (void) channel;
    method = object->method;
    args = &object->std_arg;

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

    if (streq(method, "pause")) {
        audio_player_pause(player);
    } else if (streq(method, "resume")) {
        audio_player_resume(player);
    } else if (streq(method, "stop")) {
        audio_player_pause(player);
        audio_player_set_position(player, 0);
    } else if (streq(method, "release")) {
        audio_player_pause(player);
        audio_player_set_position(player, 0);
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

        tmp = stdmap_get_str(args, "is_local");
        if (tmp == NULL || !STDVALUE_IS_BOOL(*tmp)) {
            return platch_respond_illegal_arg_std(responsehandle, "Expected `arg['is_local']` to be a bool.");
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
        result = audio_player_get_duration(player);
    } else if (streq(method, "setVolume")) {
        tmp = stdmap_get_str(args, "volume");
        if (tmp != NULL && STDVALUE_IS_FLOAT(*tmp)) {
            audio_player_set_volume(player, STDVALUE_AS_FLOAT(*tmp));
        } else {
            return platch_respond_illegal_arg_std(responsehandle, "Expected `arg['volume']` to be a float.");
        }
    } else if (streq(method, "getCurrentPosition")) {
        result = audio_player_get_position(player);
    } else if (streq(method, "setPlaybackRate")) {
        tmp = stdmap_get_str(args, "playback_rate");
        if (tmp != NULL && STDVALUE_IS_FLOAT(*tmp)) {
            audio_player_set_playback_rate(player, STDVALUE_AS_FLOAT(*tmp));
        } else {
            return platch_respond_illegal_arg_std(responsehandle, "Expected `arg['playback_rate']` to be a float.");
        }
    } else if (streq(method, "setReleaseMode")) {
        tmp = stdmap_get_str(args, "release_mode");
        if (tmp != NULL && STDVALUE_IS_STRING(*tmp)) {
            char *release_mode = STDVALUE_AS_STRING(*tmp);
            bool looping = strstr(release_mode, "loop") != NULL;
            audio_player_set_looping(player, looping);
        } else {
            return platch_respond_illegal_arg_std(responsehandle, "Expected `arg['release_mode']` to be a string.");
        }
    } else if (streq(method, "setPlayerMode")) {
        // TODO check support for low latency mode:
        // https://gstreamer.freedesktop.org/documentation/additional/design/latency.html?gi-language=c
    } else {
        return platch_respond_not_implemented(responsehandle);
    }

    return platch_respond_success_std(responsehandle, &STDINT64(result));
}

static int on_global_method_call(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *responsehandle) {
    (void) responsehandle;
    (void) channel;
    (void) object;

    return platch_respond_success_std(responsehandle, &STDBOOL(true));
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

    player = audio_player_new(player_id, AUDIOPLAYERS_LOCAL_CHANNEL);
    if (player == NULL) {
        free(entry);
        return NULL;
    }

    entry->entry = (struct list_head) {NULL, NULL};
    entry->player = player;

    list_add(&entry->entry, &plugin.players);
    return player;
}

FLUTTERPI_PLUGIN("audioplayers", audioplayers, audioplayers_plugin_init, audioplayers_plugin_deinit)
