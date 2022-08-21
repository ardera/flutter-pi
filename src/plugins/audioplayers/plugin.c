#include <collection.h>
#include <flutter-pi.h>
#include <platformchannel.h>
#include <pluginregistry.h>

#include "plugins/audioplayers.h"

FILE_DESCR("Audioplayers")

#define AUDIOPLAYERS_LOCAL_CHANNEL "xyz.luan/audioplayers"
#define AUDIOPLAYERS_GLOBAL_CHANNEL "xyz.luan/audioplayers.global"

static AudioPlayer* audioplayers_linux_plugin_get_player(char* playerId, char* mode);

static struct plugin
{
    struct flutterpi* flutterpi;
    bool initialized;
    struct concurrent_pointer_set players;
} plugin;

static int on_local_method_call(char* channel, struct platch_obj* object, FlutterPlatformMessageResponseHandle* responsehandle)
{
    const char* method;
    int result = 1;

    (void)responsehandle;
    (void)channel;

    method = object->method;
    struct std_value* flPlayerId = stdmap_get_str(&object->std_arg, "playerId");
    if (flPlayerId == NULL) {
        LOG_ERROR("Call missing mandatory parameter playerId.");
        result = 0;
    }
    char* playerId = STDVALUE_AS_STRING(*flPlayerId);

    struct std_value* args = &object->std_arg;
    struct std_value* flMode = stdmap_get_str(args, "mode");

    char* mode = flMode == NULL ? "" : STDVALUE_AS_STRING(*flMode);

    // CONTINE
    AudioPlayer* player = audioplayers_linux_plugin_get_player(playerId, mode);

    if (strcmp(method, "pause") == 0) {
        AudioPlayer_Pause(player);
        result = 1;
    } else if (strcmp(method, "resume") == 0) {
        AudioPlayer_Resume(player);
        result = 1;
    } else if (strcmp(method, "stop") == 0) {
        AudioPlayer_Pause(player);
        AudioPlayer_SetPosition(player, 0);
        result = 1;
    } else if (strcmp(method, "release") == 0) {
        AudioPlayer_Pause(player);
        AudioPlayer_SetPosition(player, 0);
        result = 1;
    } else if (strcmp(method, "seek") == 0) {
        struct std_value* flPosition = stdmap_get_str(args, "position");
        int position = flPosition == NULL ? (int)(AudioPlayer_GetPosition(player)) : STDVALUE_AS_INT(*flPosition);
        AudioPlayer_SetPosition(player, position);
        result = 1;
    } else if (strcmp(method, "setSourceUrl") == 0) {
        struct std_value* flUrl = stdmap_get_str(args, "url");
        if (flUrl == NULL) {
            LOG_ERROR("Null URL received on setSourceUrl");
            result = 0;
            return platch_respond_illegal_arg_std(responsehandle, "URL is NULL");
        }
        char* url = STDVALUE_AS_STRING(*flUrl);

        struct std_value* flIsLocal = stdmap_get_str(args, "isLocal");
        bool isLocal = (flIsLocal == NULL) ? false : STDVALUE_AS_BOOL(*flIsLocal);
        if (isLocal) {
            size_t size = strlen(url) + 7 + 1;
            char* tmp = malloc(size);
            snprintf(tmp, size, "file://%s", url);
            url = tmp;
        }
        AudioPlayer_SetSourceUrl(player, url);
        result = 1;
    } else if (strcmp(method, "getDuration") == 0) {
        result = AudioPlayer_GetDuration(player);
    } else if (strcmp(method, "setVolume") == 0) {
        struct std_value* flVolume = stdmap_get_str(args, "volume");
        if (flVolume == NULL) {
            LOG_ERROR("setVolume called with NULL arg, setting vol to 1.0");
            AudioPlayer_SetVolume(player, 1.0);
        } else if (!STDVALUE_IS_FLOAT(*flVolume)) {
            LOG_ERROR("setVolume called with non float arg, setting vol to 1.0");
            AudioPlayer_SetVolume(player, 1.0);
        } else {
            double volume = STDVALUE_AS_FLOAT(*flVolume);
            AudioPlayer_SetVolume(player, volume);
        }
        result = 1;
    } else if (strcmp(method, "getCurrentPosition") == 0) {
        result = AudioPlayer_GetPosition(player);
    } else if (strcmp(method, "setPlaybackRate") == 0) {
        struct std_value* flPlaybackRate = stdmap_get_str(args, "playbackRate");
        double playbackRate = flPlaybackRate == NULL ? 1.0 : STDVALUE_AS_FLOAT(*flPlaybackRate);
        AudioPlayer_SetPlaybackRate(player, playbackRate);
        result = 1;
    } else if (strcmp(method, "setReleaseMode") == 0) {
        struct std_value* flReleaseMode = stdmap_get_str(args, "releaseMode");
        char* releaseMode = flReleaseMode == NULL ? "" : STDVALUE_AS_STRING(*flReleaseMode);
        if (releaseMode == NULL) {
            LOG_ERROR("Error calling setReleaseMode, releaseMode cannot be null");
            result = 0;
            return platch_respond_illegal_arg_std(responsehandle, "releaseMode cannot be null");
        }
        bool looping = strstr(releaseMode, "loop") != NULL;
        AudioPlayer_SetLooping(player, looping);
        result = 1;
    } else if (strcmp(method, "setPlayerMode") == 0) {
        // TODO check support for low latency mode:
        // https://gstreamer.freedesktop.org/documentation/additional/design/latency.html?gi-language=c
        result = 1;
    } else {
        return platch_respond_not_implemented(responsehandle);
    }

    return platch_respond_success_std(responsehandle, &STDINT64(result));
}

static int on_global_method_call(char* channel, struct platch_obj* object, FlutterPlatformMessageResponseHandle* responsehandle)
{
    const char* method;

    (void)responsehandle;
    (void)channel;

    method = object->method;

    return platch_respond(
      responsehandle,
      &(struct platch_obj){ .codec = kStandardMethodCallResponse, .success = true, .std_result = { .type = kStdTrue } });
}

enum plugin_init_result audioplayers_plugin_init(struct flutterpi* flutterpi, void** userdata_out)
{
    (void)userdata_out;
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

void audioplayers_plugin_deinit(struct flutterpi* flutterpi, void* userdata)
{
    (void)flutterpi;
    (void)userdata;
    plugin_registry_remove_receiver(AUDIOPLAYERS_GLOBAL_CHANNEL);
    plugin_registry_remove_receiver(AUDIOPLAYERS_LOCAL_CHANNEL);

    AudioPlayer* ptr;
    for_each_pointer_in_cpset(&plugin.players, ptr)
    {
        AudioPlayer_Dispose(ptr);
    }

    cpset_deinit(&plugin.players);
}

static AudioPlayer* audioplayers_linux_plugin_get_player(char* playerId, char* mode)
{
    (void)mode;
    AudioPlayer* player = NULL;
    AudioPlayer* p;
    for_each_pointer_in_cpset(&plugin.players, p)
    {
        if (!strcmp(p->playerId, playerId)) {
            player = p;
            break;
        }
    }
    if (player != NULL) {
        return player;
    } else {
        AudioPlayer* player = AudioPlayer_new(playerId, AUDIOPLAYERS_LOCAL_CHANNEL);
        cpset_put_locked(&plugin.players, player);
        return player;
    }
}

FLUTTERPI_PLUGIN("audioplayers_flutter_pi", audioplayers, audioplayers_plugin_init, audioplayers_plugin_deinit)
