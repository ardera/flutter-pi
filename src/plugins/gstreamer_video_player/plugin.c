#define _GNU_SOURCE

#include <stdint.h>
#include <pthread.h>
#include <stdio.h>
#include <inttypes.h>
#include <flutter-pi.h>

#include <collection.h>
#include <pluginregistry.h>
#include <platformchannel.h>
#include <texture_registry.h>
#include <plugins/gstreamer_video_player.h>

#include <gst/gst.h>

#define LOG_ERROR(...) fprintf(stderr, "[gstreamer video player] " __VA_ARGS__)

enum data_source_type {
	kDataSourceTypeAsset,
	kDataSourceTypeNetwork,
	kDataSourceTypeFile,
    kDataSourceTypeContentUri
};

struct gstplayer_meta {
    char *event_channel_name;
};

static struct plugin {
    struct flutterpi *flutterpi;
    bool initialized;
    struct concurrent_pointer_set players;
} plugin;

/// Add a player instance to the player collection.
int add_player(struct gstplayer *player) {
    return cpset_put_(&plugin.players, player);
}

/// Get a player instance by its id.
struct gstplayer *get_player_by_texture_id(int64_t texture_id) {
    struct gstplayer *player;
    
    cpset_lock(&plugin.players);
    for_each_pointer_in_cpset(&plugin.players, player) {
        if (gstplayer_get_texture_id(player) == texture_id) {
            cpset_unlock(&plugin.players);
            return player;
        }
    }

    cpset_unlock(&plugin.players);
    return NULL;
}

/// Get a player instance by its event channel name.
struct gstplayer *get_player_by_evch(const char *const event_channel_name) {
    struct gstplayer_meta *meta;
    struct gstplayer *player;
    
    cpset_lock(&plugin.players);
    for_each_pointer_in_cpset(&plugin.players, player) {
        meta = gstplayer_get_userdata_locked(player);
        if (strcmp(meta->event_channel_name, event_channel_name) == 0) {
            cpset_unlock(&plugin.players);
            return player;
        }
    }

    cpset_unlock(&plugin.players);
    return NULL;
}

/// Remove a player instance from the player collection.
static int remove_player(struct gstplayer *player) {
    return cpset_remove_(&plugin.players, player);
}

/// Get the player id from the given arg, which is a kStdMap.
/// (*texture_id_out = arg['playerId'])
/// If an error ocurrs, this will respond with an illegal argument error to the given responsehandle.
static int get_texture_id_from_map_arg(
    struct std_value *arg,
    int64_t *texture_id_out,
    FlutterPlatformMessageResponseHandle *responsehandle
) {
    int ok;

    if (arg->type != kStdMap) {
        ok = platch_respond_illegal_arg_std(
            responsehandle,
            "Expected `arg` to be a Map"
        );
        if (ok != 0) return ok;

        return EINVAL;
    }

    struct std_value *id = stdmap_get_str(arg, "playerId");
    if (id == NULL || !STDVALUE_IS_INT(*id)) {
        ok = platch_respond_illegal_arg_std(
            responsehandle,
            "Expected `arg['playerId']` to be an integer"
        );
        if (ok != 0) return ok;

        return EINVAL;
    }

    *texture_id_out = STDVALUE_AS_INT(*id);

    return 0;
}

/// Get the player associated with the id in the given arg, which is a kStdMap.
/// (*player_out = get_player_by_texture_id(get_texture_id_from_map_arg(arg)))
/// If an error ocurrs, this will respond with an illegal argument error to the given responsehandle.
static int get_player_from_map_arg(
    struct std_value *arg,
    struct gstplayer **player_out,
    FlutterPlatformMessageResponseHandle *responsehandle
) {
    struct gstplayer *player;
    int64_t player_id;
    int ok;

    player_id = 0;
    ok = get_texture_id_from_map_arg(arg, &player_id, responsehandle);
    if (ok != 0) {
        return ok;
    }

    player = get_player_by_texture_id(player_id);
    if (player == NULL) {
        ok = platch_respond_illegal_arg_std(responsehandle, "Expected `arg['playerId']` to be a valid player id.");
        if (ok != 0) return ok;

        return EINVAL;
    }

    *player_out = player;
    
    return 0;
}

static int ensure_initialized() {
    GError *gst_error;
    gboolean success;

    if (plugin.initialized) {
        return 0;
    }

    success = gst_init_check(NULL, NULL, &gst_error);
    if (!success) {
        LOG_ERROR("Could not initialize gstreamer: %s\n", gst_error->message);
        return gst_error->code;
    }

    plugin.initialized = true;
    return 0;
}

/*******************************************************
 * CHANNEL HANDLERS                                    *
 * handle method calls on the method and event channel *
 *******************************************************/
static int on_receive_evch(
    char *channel,
    struct platch_obj *object,
    FlutterPlatformMessageResponseHandle *responsehandle
) {
    struct gstplayer *player;
    const char *method;
    int ok;

    method = object->method;

    player = get_player_by_evch(channel);
    if (player == NULL) {
        return platch_respond_not_implemented(responsehandle);
    }

    if STREQ("listen", method) {
        /// TODO: Implement
    } else if STREQ("cancel", method) {
        /// TODO: Implement
    } else {
        return platch_respond_not_implemented(responsehandle);
    }
}

static int on_initialize(
	const char *channel,
	struct platch_obj *object, 
	FlutterPlatformMessageResponseHandle *responsehandle
) {
    int ok;

    ok = ensure_initialized();
    if (ok != 0) {
        return respond_init_failed(responsehandle);
    }

    // listen to event channel here

    return platch_respond_success_std(responsehandle, NULL);
}

static int check_headers(
    const struct std_value *headers,
    FlutterPlatformMessageResponseHandle *responsehandle
) {
    const struct std_value *key, *value;

    if (headers == NULL || STDVALUE_IS_NULL(*headers)) {
        return 0;
    } else if (headers->type != kStdMap) {
        platch_respond_illegal_arg_std(
            responsehandle,
            "Expected `arg['httpHeaders']` to be a map of strings or null."
        );
        return EINVAL;
    }

    for (int i = 0; i < headers->size; i++) {
        key = headers->keys + i;
        value = headers->values + i;

        if (STDVALUE_IS_NULL(*key) || STDVALUE_IS_NULL(*value)) {
            // ignore this value
            continue;
        } else if (STDVALUE_IS_STRING(*key) && STDVALUE_IS_STRING(*value)) {
            // valid too
            continue;
        } else {
            platch_respond_illegal_arg_std(
                responsehandle,
                "Expected `arg['httpHeaders']` to be a map of strings or null."
            );
            return EINVAL;
        }
    }

    return 0;
}

static int add_headers_to_player(
    const struct std_value *headers,
    struct gst_player *player
) {
    const struct std_value *key, *value;

    if (headers == NULL || STDVALUE_IS_NULL(*headers)) {
        return 0;
    } else if (headers->type != kStdMap) {
        DEBUG_ASSERT(false);
    }

    for (int i = 0; i < headers->size; i++) {
        key = headers->keys + i;
        value = headers->values + i;

        if (STDVALUE_IS_NULL(*key) || STDVALUE_IS_NULL(*value)) {
            // ignore this value
            continue;
        } else if (STDVALUE_IS_STRING(*key) && STDVALUE_IS_STRING(*value)) {
            gstplayer_put_http_header(player, STDVALUE_AS_STRING(*key), STDVALUE_AS_STRING(*value));
        } else {
            DEBUG_ASSERT(false);
        }
    }

    return 0;
}

/// Allocates and initializes a gstplayer_meta struct, which we
/// use to store additional information in a gstplayer instance.
/// (The event channel name for that player)
static struct gstplayer_meta *create_meta(int64_t texture_id) {
    struct gstplayer_meta *meta;
    char *event_channel_name;

    meta = malloc(sizeof *meta);
    if (meta == NULL) {
        return NULL;
    }

    asprintf(
        &event_channel_name,
        "flutter.io/videoPlayer/videoEvents%" PRId64,
        texture_id
    );

    if (event_channel_name == NULL) {
        free(meta);
        return NULL;
    }

    meta->event_channel_name = event_channel_name;
    return meta;
}

static void destroy_meta(struct gstplayer_meta *meta) {
    free(meta->event_channel_name);
    free(meta);
}

/// Creates a new video player.
/// Should respond to the platform message when the player has established its viewport.
static int on_create(
	const char *channel,
	struct platch_obj *object, 
	FlutterPlatformMessageResponseHandle *responsehandle
) {
    struct gstplayer_meta *meta;
    struct gstplayer *player;
    enum   data_source_type source_type;
    struct std_value *arg, *temp;
    char *asset, *uri, *package_name, *format_hint;
    int ok;

    arg = &(object->std_arg);

    ok = ensure_initialized();
    if (ok != 0) {
        return respond_init_failed(responsehandle);
    }

    temp = stdmap_get_str(arg, "sourceType");
    if (temp != NULL && STDVALUE_IS_STRING(*temp)) {
        char *source_type_str = temp->string_value;

        if STREQ("DataSourceType.asset", source_type_str) {
            source_type = kDataSourceTypeAsset;
        } else if STREQ("DataSourceType.network", source_type_str) {
            source_type = kDataSourceTypeNetwork;
        } else if STREQ("DataSourceType.file", source_type_str) {
            source_type = kDataSourceTypeFile;
        } else if STREQ("DataSourceType.contentUri", source_type_str) {
            source_type = kDataSourceTypeContentUri;
        } else {
            goto invalid_source_type;
        }
    } else {
        invalid_source_type:

        return platch_respond_illegal_arg_std(
            responsehandle,
            "Expected `arg['sourceType']` to be a stringification of the [DataSourceType] enum."
        );
    }

    temp = stdmap_get_str(arg, "asset");
    if (temp == NULL || temp->type == kStdNull) {
        asset = NULL;
    } else if (temp != NULL && temp->type == kStdString) {
        asset = temp->string_value;
    } else {
        return platch_respond_illegal_arg_std(
            responsehandle,
            "Expected `arg['asset']` to be a String or null."
        );
    }

    temp = stdmap_get_str(arg, "uri");
    if (temp == NULL || temp->type == kStdNull) {
        uri = NULL;
    } else if (temp != NULL && temp->type == kStdString) {
        uri = temp->string_value;
    } else {
        return platch_respond_illegal_arg_std(
            responsehandle,
            "Expected `arg['uri']` to be a String or null."
        );
    }

    temp = stdmap_get_str(arg, "packageName");
    if (temp == NULL || temp->type == kStdNull) {
        package_name = NULL;
    } else if (temp != NULL && temp->type == kStdString) {
        package_name = temp->string_value;
    } else {
        return platch_respond_illegal_arg_std(
            responsehandle,
            "Expected `arg['packageName']` to be a String or null."
        );
    }

    temp = stdmap_get_str(arg, "formatHint");
    if (temp == NULL || temp->type == kStdNull) {
        format_hint = NULL;
    } else if (temp != NULL && temp->type == kStdString) {
        format_hint = temp->string_value;
    } else {
        return platch_respond_illegal_arg_std(
            responsehandle,
            "Expected `arg['formatHint']` to be a String or null."
        );
    }

    temp = stdmap_get_str(arg, "httpHeaders");

    // check our headers are valid, so we don't create our player for nothing
    ok = check_headers(temp, responsehandle);
    if (ok != 0) {
        return 0;
    }

    // create our actual player (this doesn't initialize it)
    if (asset != NULL) {
        player = gstplayer_new_from_asset(&flutterpi, asset, package_name, NULL);
    } else {
        player = gstplayer_new_from_network(&flutterpi, uri, format_hint, NULL);
    }
    if (player == NULL) {
        LOG_ERROR("Couldn't create gstreamer video player.\n");
        ok = EIO;
        goto fail_free_duped_headers;
    }

    // create a meta object so we can store the event channel name
    // of a player with it
    meta = create_meta(gstplayer_get_texture_id(player));
    if (meta == NULL) {
        ok = ENOMEM;
        goto fail_destroy_player;
    }
    
    gstplayer_set_userdata_locked(player, meta);

    // Add all our HTTP headers to gstplayer using gstplayer_put_http_header
    add_headers_to_player(temp, player);

    // add it to our player collection
    ok = add_player(player);
    if (ok != 0) {
        goto fail_destroy_meta;
    }

    // set a receiver on the videoEvents event channel
    ok = plugin_registry_set_receiver(
        meta->event_channel_name,
        kStandardMethodCall,
        on_receive_evch
    );
    if (ok != 0) {
        goto fail_remove_player;
    }

    // should we wait for it to be initialized here?
    // return platch_respond_success_std(responsehandle, NULL);
    return 0;


    fail_remove_evch_listener:
    plugin_registry_remove_receiver(meta->event_channel_name);

    fail_remove_player:
    remove_player(player);

    fail_destroy_meta:
    destroy_meta(meta);

    fail_destroy_player:
    gstplayer_destroy(player);
    goto fail_respond_native_error;

    fail_free_duped_headers:
    duped_headers_free(headers, size_headers);

    fail_respond_native_error:
    return platch_respond_native_error_std(responsehandle, ok);
}

static int on_dispose(
	const char *channel,
	struct platch_obj *object, 
	FlutterPlatformMessageResponseHandle *responsehandle
) {
    struct gstplayer *player;
    struct std_value *arg;
    int ok;

    arg = &object->std_arg;

    ok = get_player_from_map_arg(arg, &player, responsehandle);
    if (ok != 0) {
        return ok;
    }

    remove_player(player);
    gstplayer_destroy(player);
    return platch_respond_success_std(responsehandle, NULL);
}

static int on_set_looping(
	const char *channel,
	struct platch_obj *object, 
	FlutterPlatformMessageResponseHandle *responsehandle
) {
    struct gstplayer *player;
    struct std_value *arg, *temp;
    bool loop;
    int ok;

    arg = &object->std_arg;

    ok = get_player_from_map_arg(arg, &player, responsehandle);
    if (ok != 0) return ok;

    temp = stdmap_get_str(arg, "looping");
    if (STDVALUE_IS_BOOL(*temp)) {
        loop = STDVALUE_AS_BOOL(*temp);
    } else {
        return platch_respond_illegal_arg_std(
            responsehandle,
            "Expected `arg['looping']` to be a boolean."
        );
    }
    
    gstplayer_set_looping(player, loop);
    return platch_respond_success_std(responsehandle, NULL);
}

static int on_set_volume(
	const char *channel,
	struct platch_obj *object, 
	FlutterPlatformMessageResponseHandle *responsehandle
) {
    struct gstplayer *player;
    struct std_value *arg, *temp;
    double volume;
    int ok;

    arg = &object->std_arg;

    ok = get_player_from_map_arg(arg, &player, responsehandle);
    if (ok != 0) return ok;

    temp = stdmap_get_str(arg, "volume");
    if (STDVALUE_IS_FLOAT(*temp)) {
        volume = STDVALUE_AS_FLOAT(*temp);
    } else {
        return platch_respond_illegal_arg_std(
            responsehandle,
            "Expected `arg['volume']` to be a float/double."
        );
    }

    gstplayer_set_volume(player, volume);
    return platch_respond_success_std(responsehandle, NULL);
}

static int on_set_playback_speed(
	const char *channel,
	struct platch_obj *object, 
	FlutterPlatformMessageResponseHandle *responsehandle
) {
    struct gstplayer *player;
    struct std_value *arg, *temp;
    double speed;
    int ok;

    arg = &object->std_arg;

    ok = get_player_from_map_arg(arg, &player, responsehandle);
    if (ok != 0) return ok;

    temp = stdmap_get_str(arg, "speed");
    if (STDVALUE_IS_FLOAT(*temp)) {
        speed = STDVALUE_AS_FLOAT(*temp);
    } else {
        return platch_respond_illegal_arg_std(
            responsehandle,
            "Expected `arg['speed']` to be a float/double."
        );
    }

    gstplayer_set_playback_speed(player, speed);
    return platch_respond_success_std(responsehandle, NULL);
}

static int on_play(
	const char *channel,
	struct platch_obj *object, 
	FlutterPlatformMessageResponseHandle *responsehandle
) {
    struct gstplayer *player;
    struct std_value *arg;
    int ok;

    arg = &object->std_arg;

    ok = get_player_from_map_arg(arg, &player, responsehandle);
    if (ok != 0) return ok;

    gstplayer_play(player);
    return platch_respond_success_std(responsehandle, NULL);
}

static int on_get_position(
	const char *channel,
	struct platch_obj *object, 
	FlutterPlatformMessageResponseHandle *responsehandle
) {
    struct gstplayer *player;
    struct std_value *arg;
    int64_t position;
    int ok;

    arg = &object->std_arg;

    ok = get_player_from_map_arg(arg, &player, responsehandle);
    if (ok != 0) return ok;

    position = gstplayer_get_position(player);

    /// TODO: Implement
    return platch_respond_success_std(responsehandle, NULL);
}

static int on_seek_to(
	const char *channel,
	struct platch_obj *object, 
	FlutterPlatformMessageResponseHandle *responsehandle
) {
    struct gstplayer *player;
    struct std_value *arg, *temp;
    int64_t position;
    int ok;

    arg = &(object->std_arg);

    ok = get_player_from_map_arg(arg, &player, responsehandle);
    if (ok != 0) return 0;

    temp = stdmap_get_str(arg, "position");
    if (STDVALUE_IS_INT(*temp)) {
        position = STDVALUE_AS_INT(*temp);
    } else {
        return platch_respond_illegal_arg_std(
            responsehandle,
            "Expected `arg['position']` to be an integer."
        );
    }

    gstplayer_seek_to(player, position);
    return platch_respond_success_std(responsehandle, NULL);
}

static int on_pause(
	const char *channel,
	struct platch_obj *object, 
	FlutterPlatformMessageResponseHandle *responsehandle
) {
    struct gstplayer *player;
    struct std_value *arg;
    int ok;
    
    arg = &object->std_arg;

    ok = get_player_from_map_arg(arg, &player, responsehandle);
    if (ok != 0) return ok;

    gstplayer_pause(player);
    return platch_respond_success_std(responsehandle, NULL);
}

static int on_set_mix_with_others(
    const char *channel,
    struct platch_obj *object,
    FlutterPlatformMessageResponseHandle *responsehandle
) {
    struct gstplayer *player;
    struct std_value *arg;
    int ok;
    
    arg = &object->std_arg;

    ok = get_player_from_map_arg(arg, &player, responsehandle);
    if (ok != 0) return ok;

    /// TODO: Implement
    UNIMPLEMENTED();
}


int8_t gstplayer_is_present(void) {
    return plugin_registry_is_plugin_present("gstreamer_video_player");
}

int gstplayer_init() {
    int ok;

    ok = plugin_registry_set_receiver("dev.flutter.pigeon.VideoPlayerApi.initialize", kStandardMessageCodec, on_initialize);
    if (ok != 0) return ok;

    ok = plugin_registry_set_receiver("dev.flutter.pigeon.VideoPlayerApi.create", kStandardMessageCodec, on_create);
    if (ok != 0) return ok;

    ok = plugin_registry_set_receiver("dev.flutter.pigeon.VideoPlayerApi.dispose", kStandardMessageCodec, on_dispose);
    if (ok != 0) return ok;

    ok = plugin_registry_set_receiver("dev.flutter.pigeon.VideoPlayerApi.setLooping", kStandardMessageCodec, on_set_looping);
    if (ok != 0) return ok;

    ok = plugin_registry_set_receiver("dev.flutter.pigeon.VideoPlayerApi.setVolume", kStandardMessageCodec, on_set_volume);
    if (ok != 0) return ok;

    ok = plugin_registry_set_receiver("dev.flutter.pigeon.VideoPlayerApi.setPlaybackSpeed", kStandardMessageCodec, on_set_playback_speed);
    if (ok != 0) return ok;

    ok = plugin_registry_set_receiver("dev.flutter.pigeon.VideoPlayerApi.play", kStandardMessageCodec, on_play);
    if (ok != 0) return ok;

    ok = plugin_registry_set_receiver("dev.flutter.pigeon.VideoPlayerApi.position", kStandardMessageCodec, on_get_position);
    if (ok != 0) return ok;

    ok = plugin_registry_set_receiver("dev.flutter.pigeon.VideoPlayerApi.seekTo", kStandardMessageCodec, on_seek_to);
    if (ok != 0) return ok;

    ok = plugin_registry_set_receiver("dev.flutter.pigeon.VideoPlayerApi.pause", kStandardMessageCodec, on_pause);
    if (ok != 0) return ok;

    ok = plugin_registry_set_receiver("dev.flutter.pigeon.VideoPlayerApi.setMixWithOthers", kStandardMessageCodec, on_set_mix_with_others);
    if (ok != 0) return ok;

    return 0;
}

int gstplayer_deinit() {
    return 0;
}
