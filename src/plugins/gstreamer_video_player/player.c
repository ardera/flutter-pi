#define _GNU_SOURCE

#include <stdint.h>
#include <pthread.h>
#include <stdio.h>
#include <inttypes.h>
#include <sys/eventfd.h>

#include <flutter-pi.h>
#include <collection.h>
#include <pluginregistry.h>
#include <platformchannel.h>
#include <texture_registry.h>
#include <event_loop.h>
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
    
    struct flutterpi *flutterpi;
    void *userdata;
    char *video_uri;
    GstStructure *headers;
    
    bool looping;

    bool has_info;
    GstVideoInfo info;

    sd_event_source_generic *info_evsrc;
    gstplayer_info_callback_t info_cb;
    void *info_cb_userdata;

    struct texture *texture;
    int64_t texture_id;

    struct frame_interface frame_interface;

    GstElement *pipeline, *sink;
    GstBus *bus;
    sd_event_source *busfd_events;
    uint32_t drm_format;
    bool has_drm_modifier;
    uint64_t drm_modifier;
    EGLint egl_color_space;
};

#define MAX_N_PLANES 4
#define MAX_N_EGL_DMABUF_IMAGE_ATTRIBUTES 6 + 6*MAX_N_PLANES + 1

static inline void lock(struct gstplayer *player) {
    pthread_mutex_lock(&player->lock);
}

static inline void unlock(struct gstplayer *player) {
    pthread_mutex_unlock(&player->lock);
}

DEFINE_LOCK_OPS(gstplayer, lock)

static struct gstplayer *gstplayer_new(struct flutterpi *flutterpi, const char *uri, void *userdata) {
    struct gstplayer *player;
    struct texture *texture;
    GstStructure *gst_headers;
    EGLDisplay display;
    EGLContext create_context, destroy_context;
    int64_t texture_id;
    char *uri_owned;
    int ok;

    player = malloc(sizeof *player);
    if (player == NULL) return NULL;
    
    texture = flutterpi_create_texture(flutterpi);
    if (texture == NULL) goto fail_free_player;

    display = flutterpi_get_egl_display(flutterpi);
    if (display == EGL_NO_DISPLAY) {
        goto fail_destroy_texture;
    }

    create_context = flutterpi_create_egl_context(flutterpi);
    if (create_context == EGL_NO_CONTEXT) {
        goto fail_destroy_texture;
    }

    destroy_context = flutterpi_create_egl_context(flutterpi);
    if (destroy_context == EGL_NO_CONTEXT) {
        goto fail_destroy_create_context;
    }

    texture_id = texture_get_id(texture);

    uri_owned = strdup(uri);
    if (uri_owned == NULL) goto fail_destroy_destroy_context;

    gst_headers = gst_structure_new_empty("http-headers");

    ok = pthread_mutex_init(&player->lock, NULL);
    if (ok != 0) goto fail_free_gst_headers;

    player->flutterpi = flutterpi;
    player->userdata = userdata;
    player->video_uri = uri_owned;
    player->headers = gst_headers;
    player->looping = false;
    player->has_info = false;
    memset(&player->info, 0, sizeof(player->info));
    player->info_evsrc = NULL;
    player->info_cb = (gstplayer_info_callback_t) NULL;
    player->info_cb_userdata = NULL;
    player->texture = texture;
    player->texture_id = texture_id;
    player->frame_interface = (struct frame_interface) {
        .display = display,
        .create_context = create_context,
        .destroy_context = destroy_context,
        .eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC) eglGetProcAddress("eglCreateImageKHR"),
        .eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC) eglGetProcAddress("eglDestroyImageKHR"),
        .glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC) eglGetProcAddress("glEGLImageTargetTexture2DOES"),
        .supports_extended_imports = false,
        .eglQueryDmaBufFormatsEXT = (PFNEGLQUERYDMABUFFORMATSEXTPROC) eglGetProcAddress("eglQueryDmaBufFormatsEXT"),
        .eglQueryDmaBufModifiersEXT = (PFNEGLQUERYDMABUFMODIFIERSEXTPROC) eglGetProcAddress("eglQueryDmaBufModifiersEXT")
    };
    player->pipeline = NULL;
    player->sink = NULL;
    player->bus = NULL;
    player->busfd_events = NULL;
    player->drm_format = 0;
    return player;

    fail_free_gst_headers:
    gst_structure_free(gst_headers);
    free(uri_owned);

    fail_destroy_destroy_context:
    eglDestroyContext(display, destroy_context);

    fail_destroy_create_context:
    eglDestroyContext(display, create_context);

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

    (void) package_name;

    asprintf(&uri, "%s/%s", flutterpi_get_asset_bundle_path(flutterpi), asset_path);
    if (uri == NULL) {
        return NULL;
    }

    player = gstplayer_new(flutterpi, uri, userdata);

    free(uri);

    return player;
}

struct gstplayer *gstplayer_new_from_network(
    struct flutterpi *flutterpi,
    const char *uri,
    enum format_hint format_hint,
    void *userdata
) {
    (void) format_hint;
    return gstplayer_new(flutterpi, uri, userdata);
}

struct gstplayer *gstplayer_new_from_file(
    struct flutterpi *flutterpi,
    const char *uri,
    void *userdata
) {
    return gstplayer_new(flutterpi, uri, userdata);
}

struct gstplayer *gstplayer_new_from_content_uri(
    struct flutterpi *flutterpi,
    const char *uri,
    void *userdata
) {
    return gstplayer_new(flutterpi, uri, userdata);
}   

void gstplayer_destroy(struct gstplayer *player) {
    sd_event_source_unref(player->busfd_events);
    pthread_mutex_destroy(&player->lock);
    if (player->headers != NULL) free(player->headers);
    free(player->video_uri);
    texture_destroy(player->texture);
    free(player);
}

int64_t gstplayer_get_texture_id(struct gstplayer *player) {
    return player->texture_id;
}

void gstplayer_set_info_callback(struct gstplayer *player, gstplayer_info_callback_t cb, void *userdata) {
    player->info_cb = cb;
    player->info_cb_userdata = userdata;
    return;
}

void gstplayer_put_http_header(struct gstplayer *player, const char *key, const char *value) {
    GValue gvalue = G_VALUE_INIT;
    g_value_set_string(&gvalue, value);
    gst_structure_take_value(player->headers, key, &gvalue);
}

static void on_bus_message(struct gstplayer *player, GstMessage *msg) {
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
            gst_element_set_state(GST_ELEMENT(player->pipeline), requested);
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

    return;
}

static int on_bus_fd_ready(sd_event_source *s, int fd, uint32_t revents, void *userdata) {
    struct gstplayer *player;
    GstMessage *msg;

    (void) s;
    (void) fd;
    (void) revents;

    player = userdata;
    
    msg = gst_bus_pop(player->bus);
    if (msg != NULL) {
        on_bus_message(player, msg);
        gst_message_unref(msg);
    }

    return 0;
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
            LOG_ERROR("unsupported video format: %s\n", GST_VIDEO_INFO_NAME(&vinfo));
            player->drm_format = 0;
            break;
    }

    const GstVideoColorimetry *color = &GST_VIDEO_INFO_COLORIMETRY(&vinfo);
    
    if (gst_video_colorimetry_matches(color, GST_VIDEO_COLORIMETRY_BT601)) {
        player->egl_color_space = EGL_ITU_REC601_EXT;
    } else if (gst_video_colorimetry_matches(color, GST_VIDEO_COLORIMETRY_BT709)) {
        player->egl_color_space = EGL_ITU_REC709_EXT;
    } else if (gst_video_colorimetry_matches(color, GST_VIDEO_COLORIMETRY_BT2020)) {
        player->egl_color_space = EGL_ITU_REC2020_EXT;
    } else {
        LOG_ERROR("unsupported video colorimetry: %s\n", gst_video_colorimetry_to_string(color));
        player->egl_color_space = EGL_NONE;
    }

    memcpy(&player->info, &vinfo, sizeof vinfo);
    player->has_info = true;

    if (player->info_evsrc != NULL) {
        sd_event_source_generic_signal(player->info_evsrc, &player->info);
    }

    return GST_PAD_PROBE_OK;
}

void gstplayer_set_userdata_locked(struct gstplayer *player, void *userdata) {
    player->userdata = userdata;
}

void *gstplayer_get_userdata_locked(struct gstplayer *player) {
    return player->userdata;
}

static void on_destroy_texture_frame(const struct texture_frame *texture_frame, void *userdata) {
    struct video_frame *frame;

    (void) texture_frame;

    DEBUG_ASSERT_NOT_NULL(texture_frame);
    DEBUG_ASSERT_NOT_NULL(userdata);

    frame = userdata;

    frame_destroy(frame);
}

static void on_appsink_eos(GstAppSink *appsink, void *userdata) {
    struct gstplayer *player;

    DEBUG_ASSERT_NOT_NULL(appsink);
    DEBUG_ASSERT_NOT_NULL(userdata);

    player = userdata;

    (void) player;

    /// TODO: Implement
}

static GstFlowReturn on_appsink_new_preroll(GstAppSink *appsink, void *userdata) {
    struct gstplayer *player;
    GstSample *sample;

    DEBUG_ASSERT_NOT_NULL(appsink);
    DEBUG_ASSERT_NOT_NULL(userdata);

    player = userdata;

    LOG_ERROR("on_appsink_new_preroll\n");

    sample = gst_app_sink_try_pull_preroll(appsink, 0);

    /// TODO: Implement
    (void) player;
    (void) sample;
    
    gst_sample_unref(sample);

    return GST_FLOW_OK;
}

static GstFlowReturn on_appsink_new_sample(GstAppSink *appsink, void *userdata) {
    struct gstplayer *player;
    struct video_frame *frame;
    GstSample *sample;

    DEBUG_ASSERT_NOT_NULL(appsink);
    DEBUG_ASSERT_NOT_NULL(userdata);

    player = userdata;

    LOG_ERROR("on_appsink_new_sample\n");

    sample = gst_app_sink_try_pull_sample(appsink, 0);
    if (sample == NULL) {
        LOG_ERROR("gstreamer returned a NULL sample.\n");
        return GST_FLOW_ERROR;
    }

    frame = frame_new(
        &player->frame_interface,
        &(struct frame_info) {
            .drm_format = player->drm_format,
            .egl_color_space = player->egl_color_space,
            .gst_info = &player->info
        },
        gst_buffer_ref(gst_sample_get_buffer(sample))
    );

    if (frame != NULL) {
        texture_push_frame(player->texture, &(struct texture_frame) {
            .gl = *frame_get_gl_frame(frame),
            .destroy = on_destroy_texture_frame,
            .userdata = frame,
        });
    }

    gst_sample_unref(sample);

    return GST_FLOW_OK;
}

static void on_appsink_cbs_destroy(void *userdata) {
    struct gstplayer *player;

    DEBUG_ASSERT_NOT_NULL(userdata);

    player = userdata;

    (void) player;
}

int gstplayer_initialize(struct gstplayer *player) {
    sd_event_source *busfd_event_source;
    GstElement *pipeline, *sink, *src, *decodebin;
    GstBus *bus;
    GstPad *pad;
    GPollFD fd;
    GError *error = NULL;
    int ok;
    
    static const char *pipeline_descr = "uridecodebin name=\"src\" ! decodebin name=\"decode\" ! video/x-raw ! appsink sync=false name=\"sink\"";

    pipeline = gst_parse_launch(pipeline_descr, &error);
    if (pipeline == NULL) {
        LOG_ERROR("Could create GStreamer pipeline from description: %s (pipeline: `%s`)\n", error->message, pipeline_descr);
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
    gst_base_sink_set_sync(GST_BASE_SINK(sink), TRUE);
    gst_app_sink_set_max_buffers(GST_APP_SINK(sink), 2);
    gst_app_sink_set_emit_signals(GST_APP_SINK(sink), TRUE);
    gst_app_sink_set_drop(GST_APP_SINK(sink), FALSE);

    gst_app_sink_set_callbacks(
        GST_APP_SINK(sink),
        &(GstAppSinkCallbacks) {
            .eos = on_appsink_eos,
            .new_preroll = on_appsink_new_preroll,
            .new_sample = on_appsink_new_sample,
            ._gst_reserved = {0}
        },
        player,
        on_appsink_cbs_destroy
    );

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
        goto fail_unref_src;
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

sd_event_source_generic *gstplayer_probe_video_info(struct gstplayer *player, gstplayer_info_callback_t callback, void *userdata) {
    sd_event_source_generic *s;

    s = flutterpi_sd_event_add_generic(player->flutterpi, callback, userdata);
    if (s == NULL) {
        return NULL;
    }

    if (player->has_info) {
        sd_event_source_generic_signal(s, &player->info);
    } else {
        player->info_evsrc = s;
    }

    return s;
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
    (void) player;
    (void) volume;
    /// TODO: Implement
    return 0;
}

int64_t gstplayer_get_position(struct gstplayer *player) {
    (void) player;
    /// TODO: Implement
    return 0;
}

int gstplayer_seek_to(struct gstplayer *player, int64_t position) {
    (void) player;
    (void) position;
    /// TODO: Implement
    return 0;
}

int gstplayer_set_playback_speed(struct gstplayer *player, double playback_speed) {
    (void) player;
    (void) playback_speed;
    /// TODO: Implement
    return 0;
}
