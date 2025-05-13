#include <gst/gstelement.h>
#include <gst/app/app.h>
#include <gst/video/video-format.h>
#include <gst/video/gstvideometa.h>
#include <gst/allocators/gstdmabuf.h>

#include "../gstreamer_video_player.h"
#include "texture_registry.h"
#include "util/logging.h"

struct texture_sink {
    struct texture *fl_texture;
    struct frame_interface *interface;
};

static void on_destroy_texture_frame(const struct texture_frame *texture_frame, void *userdata) {
    struct video_frame *frame;

    (void) texture_frame;

    ASSERT_NOT_NULL(texture_frame);
    ASSERT_NOT_NULL(userdata);

    frame = userdata;

    frame_destroy(frame);
}

static void on_appsink_eos(GstAppSink *appsink, void *userdata) {
    gboolean ok;

    ASSERT_NOT_NULL(appsink);
    ASSERT_NOT_NULL(userdata);

    (void) userdata;

    LOG_DEBUG("on_appsink_eos()\n");

    // this method is called from the streaming thread.
    // we shouldn't access the player directly here, it could change while we use it.
    // post a message to the gstreamer bus instead, will be handled by
    // @ref on_bus_message.
    ok = gst_element_post_message(
        GST_ELEMENT(appsink),
        gst_message_new_application(GST_OBJECT(appsink), gst_structure_new_empty("appsink-eos"))
    );
    if (ok == FALSE) {
        LOG_ERROR("Could not post appsink end-of-stream event to the message bus.\n");
    }
}

static GstFlowReturn on_appsink_new_preroll(GstAppSink *appsink, void *userdata) {
    struct video_frame *frame;
    GstSample *sample;

    ASSERT_NOT_NULL(appsink);
    ASSERT_NOT_NULL(userdata);

    struct texture_sink *meta = userdata;

    sample = gst_app_sink_try_pull_preroll(appsink, 0);
    if (sample == NULL) {
        LOG_ERROR("gstreamer returned a NULL sample.\n");
        return GST_FLOW_ERROR;
    }

    // supply video info here
    frame = frame_new(meta->interface, sample, NULL);

    // the frame has a reference on the sample internally.
    gst_sample_unref(sample);

    if (frame != NULL) {
        texture_push_frame(
            meta->fl_texture,
            &(struct texture_frame){
                .gl = *frame_get_gl_frame(frame),
                .destroy = on_destroy_texture_frame,
                .userdata = frame,
            }
        );
    }

    return GST_FLOW_OK;
}

static GstFlowReturn on_appsink_new_sample(GstAppSink *appsink, void *userdata) {
    struct video_frame *frame;
    GstSample *sample;

    ASSERT_NOT_NULL(appsink);
    ASSERT_NOT_NULL(userdata);

    struct texture_sink *meta = userdata;

    sample = gst_app_sink_try_pull_sample(appsink, 0);
    if (sample == NULL) {
        LOG_ERROR("gstreamer returned a NULL sample.\n");
        return GST_FLOW_ERROR;
    }

    // supply video info here
    frame = frame_new(meta->interface, sample, NULL);

    // the frame has a reference on the sample internally.
    gst_sample_unref(sample);

    if (frame != NULL) {
        texture_push_frame(
            meta->fl_texture,
            &(struct texture_frame){
                .gl = *frame_get_gl_frame(frame),
                .destroy = on_destroy_texture_frame,
                .userdata = frame,
            }
        );
    }

    return GST_FLOW_OK;
}

static void on_appsink_cbs_destroy(void *userdata) {
    struct gstplayer *player;

    LOG_DEBUG("on_appsink_cbs_destroy()\n");
    ASSERT_NOT_NULL(userdata);

    player = userdata;

    (void) player;
}

static GstCaps *caps_for_frame_interface(struct frame_interface *interface) {
    GstCaps *caps = gst_caps_new_empty();
    if (caps == NULL) {
        return NULL;
    }

    /// TODO: Add dmabuf caps here
    for_each_format_in_frame_interface(i, format, interface) {
        GstVideoFormat gst_format = gst_video_format_from_drm_format(format->format);
        if (gst_format == GST_VIDEO_FORMAT_UNKNOWN) {
            continue;
        }

        gst_caps_append(caps, gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, gst_video_format_to_string(gst_format), NULL));
    }

    return caps;
}

static gboolean on_appsink_new_event(GstAppSink *appsink, gpointer userdata) {
    (void) userdata;

    GstMiniObject *obj;
    
    do {
        obj = gst_app_sink_try_pull_object(appsink, 0);
        if (obj == NULL) {
            return FALSE;
        }

        if (!GST_IS_EVENT(obj)) {
            LOG_DEBUG("Got non-event from gst_app_sink_try_pull_object.\n");
        }
    } while (obj && !GST_IS_EVENT(obj));

    // GstEvent *event = GST_EVENT_CAST(obj);

    // char *str = gst_structure_to_string(gst_event_get_structure(event));
    // LOG_DEBUG("Got event: %s\n", str);
    // g_free(str);

    gst_mini_object_unref(obj);

    return FALSE;
}

#if THIS_GSTREAMER_VER >= GSTREAMER_VER(1, 24, 0)
static gboolean on_appsink_propose_allocation(GstAppSink *appsink, GstQuery *query, gpointer userdata) {
    (void) appsink;
    (void) userdata;

    gst_query_add_allocation_meta(query, GST_VIDEO_META_API_TYPE, NULL);

    return FALSE;
}
#else
static GstPadProbeReturn on_query_appsink_pad(GstPad *pad, GstPadProbeInfo *info, void *userdata) {
    GstQuery *query;

    (void) pad;
    (void) userdata;

    query = gst_pad_probe_info_get_query(info);
    if (query == NULL) {
        LOG_DEBUG("Couldn't get query from pad probe info.\n");
        return GST_PAD_PROBE_OK;
    }

    if (GST_QUERY_TYPE(query) != GST_QUERY_ALLOCATION) {
        return GST_PAD_PROBE_OK;
    }

    gst_query_add_allocation_meta(query, GST_VIDEO_META_API_TYPE, NULL);

    return GST_PAD_PROBE_HANDLED;
}
#endif

GstElement *flutter_gl_texture_sink_new(struct texture *texture, struct gl_renderer *renderer) {
    ASSERT_NOT_NULL(texture);
    ASSERT_NOT_NULL(renderer);

    struct texture_sink *meta = calloc(1, sizeof(struct texture_sink));
    if (meta == NULL) {
        return NULL;
    }

    meta->fl_texture = texture;

    GstElement *element = gst_element_factory_make("appsink", "appsink");
    if (element == NULL) {
        free(meta);
        return NULL;
    }

    meta->interface = frame_interface_new(renderer);
    if (meta->interface == NULL) {
        gst_object_unref(element);
        free(meta);
        return NULL;
    }

    GstCaps *caps = caps_for_frame_interface(meta->interface);
    if (caps == NULL) {
        frame_interface_unref(meta->interface);
        gst_object_unref(element);
        free(meta);
        return NULL;
    }

    GstBaseSink *basesink = GST_BASE_SINK_CAST(element);
    GstAppSink *appsink = GST_APP_SINK_CAST(element);

    gst_base_sink_set_max_lateness(basesink, 20 * GST_MSECOND);
    gst_base_sink_set_qos_enabled(basesink, TRUE);
    gst_base_sink_set_sync(basesink, TRUE);
    gst_app_sink_set_max_buffers(appsink, 2);
    gst_app_sink_set_emit_signals(appsink, TRUE);
    gst_app_sink_set_drop(appsink, FALSE);
    gst_app_sink_set_caps(appsink, caps);
    gst_caps_unref(caps);

    GstAppSinkCallbacks cbs;
    memset(&cbs, 0, sizeof(cbs));

    cbs.new_preroll = on_appsink_new_preroll;
    cbs.new_sample = on_appsink_new_sample;
    cbs.eos = on_appsink_eos;
#if THIS_GSTREAMER_VER >= GSTREAMER_VER(1, 20, 0)
    cbs.new_event = on_appsink_new_event;
#endif

#if THIS_GSTREAMER_VER >= GSTREAMER_VER(1, 24, 0)
    cbs.propose_allocation = on_appsink_propose_allocation;
#else
    GstPad *pad = gst_element_get_static_pad(sink, "sink");
    if (pad == NULL) {
        LOG_ERROR("Couldn't get static pad `sink` from appsink.\n");
        frame_interface_unref(meta->interface);
        gst_object_unref(element);
        free(meta);
        return NULL;
    }

    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM, on_query_appsink_pad, NULL);
#endif

    gst_app_sink_set_callbacks(
        GST_APP_SINK(appsink),
        &cbs,
        meta,
        on_appsink_cbs_destroy
    );

    return element;
}
