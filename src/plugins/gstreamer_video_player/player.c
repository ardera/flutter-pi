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

#include <drm_fourcc.h>
#include <gst/gst.h>
#include <gst/gstmemory.h>
#include <gst/gstpad.h>
#include <gst/allocators/gstdmabuf.h>
#include <gst/app/gstappsink.h>
#include <gst/video/gstvideometa.h>

#define LOG_ERROR(...) fprintf(stderr, "[gstreamer video player] " __VA_ARGS__)

struct gstplayer {
    pthread_mutex_t lock;
    
    void *userdata;
    const char *video_uri;
    GstStructure *headers;
    
    bool looping;

    bool has_info;
    GstVideoInfo info;

    gstplayer_info_callback_t info_cb;
    void *info_cb_userdata;

    struct texture *texture;
    int64_t texture_id;
    GstElement *pipeline, *sink;
    GstBus *bus;
    sd_event_source *busfd_events;
    uint32_t drm_format;
};

static inline void lock(struct gstplayer *player) {
    pthread_mutex_lock(&player->lock);
}

static inline void unlock(struct gstplayer *player) {
    pthread_mutex_unlock(&player->lock);
}

void gstplayer_lock(struct gstplayer *player) {
    return lock(player);
}

void gstplayer_unlock(struct gstplayer *player) {
    return unlock(player);
}


static struct gstplayer *gstplayer_new(struct flutterpi *flutterpi, const char *uri, void *userdata) {
    struct gstplayer *player;
    struct texture *texture;
    const char *uri_owned;
    GstStructure *gst_headers;
    int64_t texture_id;
    int ok;

    player = malloc(sizeof *player);
    if (player == NULL) return NULL;
    
    texture = flutterpi_create_texture(flutterpi, player);
    if (texture == NULL) goto fail_free_player;

    texture_id = texture_get_id(texture);

    uri_owned = strdup(uri);
    if (uri_owned == NULL) goto fail_destroy_texture;

    gst_headers = gst_structure_new_empty("http headers");

    ok = pthread_mutex_init(&player->lock, NULL);
    if (ok != 0) goto fail_free_gst_headers;

    player->userdata = userdata;
    player->video_uri = uri_owned;
    player->headers = gst_headers;
    player->looping = false;
    player->has_info = false;
    memset(&player->info, 0, sizeof(player->info));
    player->info_cb = (gstplayer_info_callback_t) NULL;
    player->info_cb_userdata = NULL;
    player->texture = texture;
    player->texture_id = texture_id;
    player->pipeline = NULL;
    player->sink = NULL;
    player->bus = NULL;
    player->busfd_events = NULL;
    player->drm_format = 0;
    return player;


    fail_free_gst_headers:
    gst_structure_free(gst_headers);

    fail_free_uri_owned:
    free(uri_owned);

    fail_destroy_texture:
    texture_destroy(texture);

    fail_free_player:
    free(player);

    return NULL;
}

struct gstplayer *gstplayer_new_from_asset(
    struct flutterpi *flutterpi,
    const char *asset_path,
    const char *package_name,
    void *userdata
) {
    struct gstplayer *player;
    char *uri;

    asprintf(&uri, "%s/%s", flutterpi_get_asset_bundle_path(flutterpi), asset_path);
    if (uri == NULL) {
        return NULL;
    }

    player = gstplayer_new(flutterpi, uri, NULL, userdata);

    free(uri);

    return player;
}

struct gstplayer *gstplayer_new_from_network(
    struct flutterpi *flutterpi,
    const char *uri,
    const char *format_hint,
    void *userdata
) {
    return gstplayer_new(flutterpi, uri, http_headers, userdata);
}

struct gstplayer *gstplayer_new_from_file(
    struct flutterpi *flutterpi,
    const char *uri,
    void *userdata
) {
    return gstplayer_new(flutterpi, uri, NULL, userdata);
}

struct gstplayer *gstplayer_new_from_content_uri(
    struct flutterpi *flutterpi,
    const char *uri,
    void *userdata
) {
    return gstplayer_new(flutterpi, uri, NULL, userdata);
}   

void gstplayer_destroy(struct gstplayer *player) {
    pthread_mutex_destroy(&player->lock);
    if (player->headers != NULL) free(player->headers);
    free(player->video_uri);
    free(player->event_channel_name);
    texture_destroy(player->texture);
    free(player);
}

int64_t gstplayer_get_texture_id(struct gstplayer *player) {
    return player->texture_id;
}

void gstplayer_set_info_callback(struct gstplayer *player, gstplayer_info_callback_t cb, void *userdata) {
    player->info_cb = cb;
    player->info_cb_userdata = userdata;
    return 0;
}

void gstplayer_put_http_header(struct gstplayer *player, const char *key, const char *value) {
    GValue gvalue = G_VALUE_INIT;
    g_value_set_string(&gvalue, value);
    gst_structure_take_value(player->headers, key, &gvalue);
}

static void on_bus_message(struct gstplayer *player, GstMessage *msg) {
    const gchar *prefix;
    struct gstplayer *player;
    GstState old, current, pending, requested;
    GError *error;
    gchar *debug_info;

    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_STATE_CHANGED:
            gst_message_parse_state_changed(msg, &old, &current, &pending);
            printf(
                "[gstreamer video player] gstreamer state change: old: %s, current: %s, pending: %s\n",
                gst_element_state_get_name(old),
                gst_element_state_get_name(current),
                gst_element_state_get_name(pending)
            );
            break;
        
        case GST_MESSAGE_REQUEST_STATE:
            gst_message_parse_request_state(msg, &requested);
            printf(
                "[gstreamer video player] gstreamer state change to %s was requested by %s\n",
                gst_element_state_get_name(requested),
                GST_MESSAGE_SRC_NAME(msg)
            );
            gst_element_set_state(GST_ELEMENT(dec->pipeline), requested);
            break;

        case GST_MESSAGE_LATENCY:
            printf("[gstreamer video player] gstreamer: redistributing latency\n");
            gst_bin_recalculate_latency(GST_BIN(player->pipeline));
            break;

        case GST_MESSAGE_INFO:
            gst_message_parse_info(msg, &error, &debug_info);
            fprintf(stderr, "[gstreamer video player] gstreamer info: %s (debug info: %s)\n", error->message, debug_info);
            g_clear_error(&error);
            g_free(debug_info);
            break;

        case GST_MESSAGE_WARNING:
            gst_message_parse_warning(msg, &error, &debug_info);
            fprintf(stderr, "[gstreamer video player] gstreamer warning: %s (debug info: %s)\n", error->message, debug_info);
            g_clear_error(&error);
            g_free(debug_info);
            break;

        case GST_MESSAGE_ERROR:
            gst_message_parse_error(msg, &error, &debug_info);
            fprintf(stderr, "[gstreamer video player] gstreamer error: %s (debug info: %s)\n", error->message, debug_info);
            g_clear_error(&error);
            g_free(debug_info);
            break;

        default:
            break;
    }

    return TRUE;
}

static int on_bus_fd_ready(sd_event_source *s, int fd, uint32_t revents, void *userdata) {
    struct gstplayer *player;
    GstMessage *msg;

    player = userdata;
    
    msg = gst_bus_pop(player->bus);
    if (msg != NULL) {
        on_bus_message(player, msg);
        gst_message_unref(msg);
    }
}

static GstPadProbeReturn on_query_appsink(GstPad *pad, GstPadProbeInfo *info, void *userdata) {
    GstQuery *query;

    (void) pad;
    (void) userdata;

    query = info->data;

    if (GST_QUERY_TYPE(query) != GST_QUERY_ALLOCATION) {
        return GST_PAD_PROBE_OK;
    }

    gst_query_add_allocation_meta(query, GST_VIDEO_META_API_TYPE, NULL);

    return GST_PAD_PROBE_HANDLED;
}

static void on_element_added(GstBin *bin, GstElement *element, void *userdata) {
    GstElementFactory *factory;
    const char *factory_name;

    (void) userdata;
    (void) bin;

    factory = gst_element_get_factory(element);
    factory_name = gst_plugin_feature_get_name(factory);

    if (g_str_has_prefix(factory_name, "v4l2video") && g_str_has_suffix(factory_name, "dec")) {
        gst_util_set_object_arg(G_OBJECT(element), "capture-io-mode", "dmabuf");
        fprintf(stderr, "[gstreamer video player] found gstreamer V4L2 video decoder element with name \"%s\"\n", GST_OBJECT_NAME(element));
    }
}

static GstPadProbeReturn on_probe_pad(GstPad *pad, GstPadProbeInfo *info, void *userdata) {
    struct gstplayer *player;
    GstVideoInfo vinfo;
    GstEvent *event;
    GstCaps *caps;
    gboolean ok;

    (void) pad;

    player = userdata;
    event = GST_PAD_PROBE_INFO_EVENT(info);
    
    if (GST_EVENT_TYPE(event) != GST_EVENT_CAPS) {
        return GST_PAD_PROBE_OK;
    }

    gst_event_parse_caps(event, &caps);
    if (caps == NULL) {
        LOG_ERROR("gstreamer: caps event without caps\n");
        return GST_PAD_PROBE_OK;
    }

    ok = gst_video_info_from_caps(&vinfo, caps);
    if (!ok) {
        LOG_ERROR("gstreamer: caps event with invalid video caps\n");
        return GST_PAD_PROBE_OK;
    }

    switch (GST_VIDEO_INFO_FORMAT(&vinfo)) {
        case GST_VIDEO_FORMAT_Y42B:
            player->drm_format = DRM_FORMAT_YUV422;
            break;
        case GST_VIDEO_FORMAT_YV12:
            player->drm_format = DRM_FORMAT_YVU420;
            break;
        case GST_VIDEO_FORMAT_I420:
            player->drm_format = DRM_FORMAT_YUV420;
            break;
        case GST_VIDEO_FORMAT_NV12:
            player->drm_format = DRM_FORMAT_NV12;
            break;
        case GST_VIDEO_FORMAT_NV21:
            player->drm_format = DRM_FORMAT_NV21;
            break;
        case GST_VIDEO_FORMAT_YUY2:
            player->drm_format = DRM_FORMAT_YUYV;
            break;
        default:
            LOG_ERROR("unknown video format: %s\n", GST_VIDEO_INFO_NAME(&info));
            break;
    }

    const GstVideoColorimetry *color = &GST_VIDEO_INFO_COLORIMETRY(&vinfo);
    
    if (gst_video_colorimetry_matches(color, GST_VIDEO_COLORIMETRY_BT601)) {

    } else if (gst_video_colorimetry_matches(color, GST_VIDEO_COLORIMETRY_BT709)) {

    } else if (gst_video_colorimetry_matches(color, GST_VIDEO_COLORIMETRY_BT2020)) {

    } else {

    }

    return GST_PAD_PROBE_OK;
}

void gstplayer_set_userdata_locked(struct gstplayer *player, void *userdata) {
    player->userdata = userdata;
}

void *gstplayer_get_userdata_locked(struct gstplayer *player) {
    return player->userdata;
}

int gstplayer_initialize(struct gstplayer *player) {
    sd_event_source *busfd_event_source;
    GstElement *pipeline, *sink, *src, *decodebin;
    GstBus *bus;
    GstPad *pad;
    GPollFD fd;
    GError *error;
    int ok;
    
    static const char *pipeline = "uridecodebin name=\"src\" ! decodebin name=\"decode\" ! video/x-raw ! appsink sync=false name=\"sink\"";

    pipeline = gst_parse_launch(pipeline, NULL);
    if (pipeline == NULL) {
        LOG_ERROR("Could create GStreamer pipeline from description: %s (pipeline: `%s`)\n", error->message, pipeline);
        return error->code;
    }

    sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
    if (sink == NULL) {
        LOG_ERROR("Couldn't find appsink in pipeline bin.\n");
        ok = EINVAL;
        goto fail_unref_pipeline;
    }

    pad = gst_element_get_static_pad(sink, "sink");
    if (pad == NULL) {
        LOG_ERROR("Couldn't get static pad \"sink\" from video sink.\n");
        ok = EINVAL;
        goto fail_unref_sink;
    }
    
    gst_pad_add_probe(
        pad,
        GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM,
        on_query_appsink,
        player,
        NULL
    );

    src = gst_bin_get_by_name(GST_BIN(pipeline), "src");
    if (src == NULL) {
        LOG_ERROR("Couldn't find filesrc in pipeline bin.\n");
        ok = EINVAL;
        goto fail_unref_sink;
    }

    g_object_set(G_OBJECT(src), "uri", player->video_uri, NULL);

    gst_base_sink_set_max_lateness(GST_BASE_SINK(sink), 20 * GST_MSECOND);
    gst_base_sink_set_qos_enabled(GST_BASE_SINK(sink), TRUE);
    gst_app_sink_set_max_buffers(GST_APP_SINK(sink), 2);

    gst_pad_add_probe(
        pad,
        GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
        on_probe_pad,
        player,
        NULL
    );

    decodebin = gst_bin_get_by_name(GST_BIN(pipeline), "decode");
    if (decodebin == NULL) {
        LOG_ERROR("Couldn't find decodebin in pipeline bin.\n");
        ok = EINVAL;
        goto fail_unref_sink;
    }

    g_signal_connect(decodebin, "element-added", G_CALLBACK(on_element_added), player);

    bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));

    gst_bus_get_pollfd(bus, &fd);

    flutterpi_sd_event_add_io(
        &busfd_event_source,
        fd.fd,
        EPOLLIN,
        on_bus_fd_ready,
        player
    );

    gst_object_unref(GST_OBJECT(bus));

    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    player->sink = sink;
    player->pipeline = pipeline;
    player->bus = bus;
    player->busfd_events = busfd_event_source;

    gst_object_unref(src);
    gst_object_unref(decodebin);
    gst_object_unref(pad);
    return 0;


    fail_unref_src:
    gst_object_unref(src);

    fail_unref_sink:
    gst_object_unref(sink);

    fail_unref_pipeline:
    gst_object_unref(pipeline);

    return ok;
}

int gstplayer_play(struct gstplayer *player) {
    return gst_element_set_state(player->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE ? EIO : 0;
}

int gstplayer_pause(struct gstplayer *player) {
    return gst_element_set_state(player->pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE ? EIO : 0;
}

int gstplayer_set_looping(struct gstplayer *player, bool looping) {
    player->looping = looping;
    return 0;
}

int gstplayer_set_volume(struct gstplayer *player, double volume) {
    /// TODO: Implement
    return 0;
}

int gstplayer_seek_to(struct gstplayer *player, double position) {
    /// TODO: Implement
    return 0;
}

int gstplayer_set_playback_speed(struct gstplayer *player, double playback_speed) {
    /// TODO: Implement
    return 0;
}
