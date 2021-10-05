#ifndef _FLUTTERPI_INCLUDE_PLUGINS_OMXPLAYER_VIDEO_PLUGIN_H
#define _FLUTTERPI_INCLUDE_PLUGINS_OMXPLAYER_VIDEO_PLUGIN_H

struct video_info {
    uint32_t drm_format;
    
    size_t width, height;
    size_t fps;
};

typedef void (*gstplayer_info_callback_t)(const struct video_info *info, void *userdata);

struct gstplayer;
struct flutterpi;

struct gstplayer *gstplayer_new_from_asset(
    struct flutterpi *flutterpi,
    const char *asset_path,
    const char *package_name,
    void *userdata
);

struct gstplayer *gstplayer_new_from_network(
    struct flutterpi *flutterpi,
    const char *uri,
    const char *format_hint,
    void *userdata
);

struct gstplayer *gstplayer_new_from_file(
    struct flutterpi *flutterpi,
    const char *uri,
    void *userdata
);

struct gstplayer *gstplayer_new_from_content_uri(
    struct flutterpi *flutterpi,
    const char *uri,
    void *userdata
);

void gstplayer_destroy(struct gstplayer *player);

void gstplayer_lock(struct gstplayer *player);

void gstplayer_unlock(struct gstplayer *player);

void gstplayer_set_userdata_locked(struct gstplayer *player, void *userdata);

void *gstplayer_get_userdata_locked(struct gstplayer *player);

int64_t gstplayer_get_texture_id(struct gstplayer *player);

void gstplayer_set_info_callback(struct gstplayer *player, gstplayer_info_callback_t cb, void *userdata);

void gstplayer_put_http_header(struct gstplayer *player, const char *key, const char *value);

int gstplayer_initialize(struct gstplayer *player);

int gstplayer_play(struct gstplayer *player);

int gstplayer_pause(struct gstplayer *player);

int64_t gstplayer_get_position(struct gstplayer *player);

int gstplayer_set_looping(struct gstplayer *player, bool looping);

int gstplayer_set_volume(struct gstplayer *player, double volume);

int gstplayer_seek_to(struct gstplayer *player, double position);

int gstplayer_set_playback_speed(struct gstplayer *player, double playback_speed);

int gstplayer_init();
int gstplayer_deinit();

#endif