#define _GNU_SOURCE

#include "plugins/audioplayers.h"

#include <collection.h>
#include <flutter-pi.h>
#include <platformchannel.h>
#include <pluginregistry.h>

FILE_DESCR("audioplayers plugin")

#define AUDIOPLAYERS_LOCAL_CHANNEL "xyz.luan/audioplayers"
#define AUDIOPLAYERS_GLOBAL_CHANNEL "xyz.luan/audioplayers.global"

static struct audio_player *audioplayers_linux_plugin_get_player(char *player_id, char *mode);

static struct plugin {
    struct flutterpi *flutterpi;
    bool initialized;
    struct concurrent_pointer_set players;
} plugin;

static int on_local_method_call(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *responsehandle) {
    struct audio_player *player;
    struct std_value *args, *tmp;
    const char *method;
    char *player_id, *mode;
    int result = 1;
    (void) responsehandle;
    (void) channel;
    method = object->method;
    args = &object->std_arg;

    if (args == NULL || !STDVALUE_IS_MAP(*args)) {
        return platch_respond_illegal_arg_std(responsehandle, "Expected `arg` to be a map.");
    }

    tmp = stdmap_get_str(&object->std_arg, "player_id");
    if (tmp == NULL || !STDVALUE_IS_STRING(*tmp)) {
        LOG_ERROR("Call missing mandatory parameter player_id.\n");
        return platch_respond_illegal_arg_std(responsehandle, "Expected `arg['player_id'] to be a string.");
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

    if (strcmp(method, "pause") == 0) {
        audio_player_pause(player);
    } else if (strcmp(method, "resume") == 0) {
        audio_player_resume(player);
    } else if (strcmp(method, "stop") == 0) {
        audio_player_pause(player);
        audio_player_set_position(player, 0);
    } else if (strcmp(method, "release") == 0) {
        audio_player_pause(player);
        audio_player_set_position(player, 0);
    } else if (strcmp(method, "seek") == 0) {
        tmp = stdmap_get_str(args, "position");
        if (tmp == NULL || !STDVALUE_IS_INT(*tmp)) {
            return platch_respond_illegal_arg_std(responsehandle, "Expected `arg['position']` to be an int.");
        }

        int64_t position = STDVALUE_AS_INT(*tmp);
        audio_player_set_position(player, position);
    } else if (strcmp(method, "setSourceUrl") == 0) {
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
            asprintf(&local_url, "file://%s", url);
            if (local_url == NULL) {
                return platch_respond_native_error_std(responsehandle, ENOMEM);
            }
            url = local_url;
        }

        audio_player_set_source_url(player, url);
    } else if (strcmp(method, "getDuration") == 0) {
        result = audio_player_get_duration(player);
    } else if (strcmp(method, "setVolume") == 0) {
        tmp = stdmap_get_str(args, "volume");
        if (tmp != NULL && STDVALUE_IS_FLOAT(*tmp)) {
            audio_player_set_volume(player, STDVALUE_AS_FLOAT(*tmp));
        } else {
            return platch_respond_illegal_arg_std(responsehandle, "Expected `arg['volume']` to be a float.");
        }
    } else if (strcmp(method, "getCurrentPosition") == 0) {
        result = audio_player_get_position(player);
    } else if (strcmp(method, "setPlaybackRate") == 0) {
        tmp = stdmap_get_str(args, "playback_rate");
        if (tmp != NULL && STDVALUE_IS_FLOAT(*tmp)) {
            audio_player_set_playback_rate(player, STDVALUE_AS_FLOAT(*tmp));
        } else {
            return platch_respond_illegal_arg_std(responsehandle, "Expected `arg['playback_rate']` to be a float.");
        }
    } else if (strcmp(method, "setReleaseMode") == 0) {
        tmp = stdmap_get_str(args, "release_mode");
        if (tmp != NULL && STDVALUE_IS_STRING(*tmp)) {
            char *release_mode = STDVALUE_AS_STRING(*tmp);
            bool looping = strstr(release_mode, "loop") != NULL;
            audio_player_set_looping(player, looping);
        } else {
            return platch_respond_illegal_arg_std(responsehandle, "Expected `arg['release_mode']` to be a string.");
        }
    } else if (strcmp(method, "setPlayerMode") == 0) {
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
    (void) userdata_out;
    int ok;
    plugin.flutterpi = flutterpi;
    plugin.initialized = false;

    ok = cpset_init(&plugin.players, CPSET_DEFAULT_MAX_SIZE);
    if (ok != 0)
        return kError_PluginInitResult;

    ok = plugin_registry_set_receiver(AUDIOPLAYERS_GLOBAL_CHANNEL, kStandardMethodCall, on_global_method_call);
    if (ok != 0) {
        goto fail_deinit_cpset;
    }

    ok = plugin_registry_set_receiver(AUDIOPLAYERS_LOCAL_CHANNEL, kStandardMethodCall, on_local_method_call);
    if (ok != 0) {
        goto fail_remove_global_receiver;
    }

    return kInitialized_PluginInitResult;

fail_remove_global_receiver:
    plugin_registry_remove_receiver(AUDIOPLAYERS_GLOBAL_CHANNEL);

fail_deinit_cpset:
    cpset_deinit(&plugin.players);

    return kError_PluginInitResult;
}

void audioplayers_plugin_deinit(struct flutterpi *flutterpi, void *userdata) {
    (void) flutterpi;
    (void) userdata;
    plugin_registry_remove_receiver(AUDIOPLAYERS_GLOBAL_CHANNEL);
    plugin_registry_remove_receiver(AUDIOPLAYERS_LOCAL_CHANNEL);

    struct audio_player *ptr;
    for_each_pointer_in_cpset(&plugin.players, ptr) {
        audio_player_destroy(ptr);
    }

    cpset_deinit(&plugin.players);
}

static struct audio_player *audioplayers_linux_plugin_get_player(char *player_id, char *mode) {
    (void) mode;
    struct audio_player *player;
    for_each_pointer_in_cpset(&plugin.players, player) {
        if (audio_player_is_id(player, player_id)) {
            return player;
        }
    }

    player = audio_player_new(player_id, AUDIOPLAYERS_LOCAL_CHANNEL);
    cpset_put_locked(&plugin.players, player);
    return player;
}

FLUTTERPI_PLUGIN("audioplayers", audioplayers, audioplayers_plugin_init, audioplayers_plugin_deinit)
