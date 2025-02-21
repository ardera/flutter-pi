#define _GNU_SOURCE

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#include <pthread.h>

#include <drm_fourcc.h>
#include <gst/allocators/gstdmabuf.h>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/gstmemory.h>
#include <gst/gstpad.h>
#include <gst/video/gstvideometa.h>

#include "flutter-pi.h"
#include "notifier_listener.h"
#include "platformchannel.h"
#include "pluginregistry.h"
#include "plugins/gstreamer_video_player.h"
#include "texture_registry.h"
#include "util/logging.h"

#define LOG_GST_SET_STATE_ERROR(_element)                                                                               \
    LOG_ERROR(                                                                                                          \
        "setting gstreamer playback state failed. gst_element_set_state(element name: %s): GST_STATE_CHANGE_FAILURE\n", \
        GST_ELEMENT_NAME(_element)                                                                                      \
    )
#define LOG_GST_GET_STATE_ERROR(_element)                                                                          \
    LOG_ERROR(                                                                                                     \
        "last gstreamer state change failed. gst_element_get_state(element name: %s): GST_STATE_CHANGE_FAILURE\n", \
        GST_ELEMENT_NAME(_element)                                                                                 \
    )

#define LOG_PLAYER_DEBUG(player, fmtstring, ...) LOG_DEBUG("gstplayer-%"PRIi64": " fmtstring, player->debug_id, ##__VA_ARGS__)
#ifdef DEBUG
    #define LOG_PLAYER_ERROR(player, fmtstring, ...) LOG_ERROR("gstplayer-%"PRIi64": " fmtstring, player->debug_id, ##__VA_ARGS__)
#else
    #define LOG_PLAYER_ERROR(player, fmtstring, ...) LOG_ERROR(fmtstring, ##__VA_ARGS__)
#endif

struct incomplete_video_info {
    bool has_resolution;
    bool has_fps;
    bool has_duration;
    bool has_seeking_info;
    struct video_info info;
};

enum playpause_state { kPaused, kPlaying, kStepping };

enum playback_direction { kForward, kBackward };

#define PLAYPAUSE_STATE_AS_STRING(playpause_state) \
    ((playpause_state) == kPaused   ? "paused" :   \
     (playpause_state) == kPlaying  ? "playing" :  \
     (playpause_state) == kStepping ? "stepping" : \
                                      "?")


#ifdef DEBUG
static int64_t allocate_id() {
    static atomic_int_fast64_t next_id = 1;

    return atomic_fetch_add_explicit(&next_id, 1, memory_order_relaxed);
}
#endif
struct gstplayer {
#ifdef DEBUG
    int64_t debug_id;
#endif

    struct flutterpi *flutterpi;
    struct tracer *tracer;

    void *userdata;

    /**
     * @brief The desired playback rate that should be used when @ref playpause_state is kPlayingForward. (should be > 0)
     *
     */
    double playback_rate_forward;

    /**
     * @brief The desired playback rate that should be used when @ref playpause_state is kPlayingBackward. (should be < 0)
     *
     */
    double playback_rate_backward;

    /**
     * @brief True if the video should seemlessly start from the beginning once the end is reached.
     *
     */
    atomic_bool looping;

    /**
     * @brief The desired playback state. Either paused, playing, or single-frame stepping.
     *
     */
    enum playpause_state playpause_state;

    /**
     * @brief The desired playback direction.
     *
     */
    enum playback_direction direction;

    /**
     * @brief The actual, currently used playback rate.
     *
     */
    double current_playback_rate;

    /**
     * @brief The position reported if gstreamer position queries fail (for example, because gstreamer is currently
     * seeking to a new position. In that case, fallback_position_ms will be the seeking target position, so we report the
     * new position while we're seeking to it)
     */
    int64_t fallback_position_ms;

    /**
     * @brief True if there's a position that apply_playback_state should seek to.
     *
     */
    bool has_desired_position;

    /**
     * @brief True if gstplayer should seek to the nearest keyframe instead, which is a bit faster.
     *
     */
    bool do_fast_seeking;

    /**
     * @brief The position, if any, that apply_playback_state should seek to.
     *
     */
    int64_t desired_position_ms;

    struct notifier video_info_notifier, buffering_state_notifier, error_notifier;

    bool has_sent_info;
    struct incomplete_video_info info;

    // bool has_gst_info;
    // GstVideoInfo gst_info;

    struct texture *texture;

    // GstElement *pipeline, *sink;
    // GstBus *bus;
    sd_event_source *busfd_events;

    GstElement *playbin;

    bool is_live;
};

static int maybe_send_info(struct gstplayer *player) {
    struct video_info *duped;

    if (player->info.has_resolution && player->info.has_fps && player->info.has_duration && player->info.has_seeking_info) {
        // we didn't send the info yet but we have complete video info now.
        // send it!
        duped = memdup(&(player->info.info), sizeof(player->info.info));
        if (duped == NULL) {
            return ENOMEM;
        }

        notifier_notify(&player->video_info_notifier, duped);
    }
    return 0;
}

static void fetch_duration(struct gstplayer *player) {
    gboolean ok;
    int64_t duration;

    ok = gst_element_query_duration(player->playbin, GST_FORMAT_TIME, &duration);
    if (ok == FALSE) {
        if (player->is_live) {
            player->info.info.duration_ms = INT64_MAX;
            player->info.has_duration = true;
            return;
        } else {
            LOG_PLAYER_ERROR(player, "Could not fetch duration. (gst_element_query_duration)\n");
            return;
        }
    }

    player->info.info.duration_ms = GST_TIME_AS_MSECONDS(duration);
    player->info.has_duration = true;
}

static void fetch_seeking(struct gstplayer *player) {
    GstQuery *seeking_query;
    gboolean ok, seekable;
    int64_t seek_begin, seek_end;

    seeking_query = gst_query_new_seeking(GST_FORMAT_TIME);
    ok = gst_element_query(player->playbin, seeking_query);
    if (ok == FALSE) {
        if (player->is_live) {
            player->info.info.can_seek = false;
            player->info.info.seek_begin_ms = 0;
            player->info.info.seek_end_ms = 0;
            player->info.has_seeking_info = true;
            return;
        } else {
            LOG_PLAYER_DEBUG(player, "Could not query seeking info. (gst_element_query)\n");
            return;
        }
    }

    gst_query_parse_seeking(seeking_query, NULL, &seekable, &seek_begin, &seek_end);

    gst_query_unref(seeking_query);

    player->info.info.can_seek = seekable;
    player->info.info.seek_begin_ms = GST_TIME_AS_MSECONDS(seek_begin);
    player->info.info.seek_end_ms = GST_TIME_AS_MSECONDS(seek_end);
    player->info.has_seeking_info = true;
}

static void update_buffering_state(struct gstplayer *player, GstObject *element) {
    struct buffering_state *state;
    GstBufferingMode mode;
    GstQuery *query;
    gboolean ok, busy;
    int64_t start, stop, buffering_left;
    int n_ranges, percent, avg_in, avg_out;

    query = gst_query_new_buffering(GST_FORMAT_TIME);
    ok = gst_element_query(GST_ELEMENT(element), query);
    if (ok == FALSE) {
        LOG_PLAYER_DEBUG(player, "Could not query precise buffering state.\n");
        goto fail_unref_query;
    }

    gst_query_parse_buffering_percent(query, &busy, &percent);
    gst_query_parse_buffering_stats(query, &mode, &avg_in, &avg_out, &buffering_left);

    n_ranges = (int) gst_query_get_n_buffering_ranges(query);

    state = malloc(sizeof(*state) + n_ranges * sizeof(struct buffering_range));
    if (state == NULL) {
        goto fail_unref_query;
    }

    for (int i = 0; i < n_ranges; i++) {
        ok = gst_query_parse_nth_buffering_range(query, (unsigned int) i, &start, &stop);
        if (ok == FALSE) {
            LOG_ERROR("Could not parse %dth buffering range from buffering state. (gst_query_parse_nth_buffering_range)\n", i);
            goto fail_free_state;
        }

        state->ranges[i].start_ms = GST_TIME_AS_MSECONDS(start);
        state->ranges[i].stop_ms = GST_TIME_AS_MSECONDS(stop);
    }

    gst_query_unref(query);

    state->percent = percent;
    state->mode =
        (mode == GST_BUFFERING_STREAM    ? BUFFERING_MODE_STREAM :
         mode == GST_BUFFERING_DOWNLOAD  ? BUFFERING_MODE_DOWNLOAD :
         mode == GST_BUFFERING_TIMESHIFT ? BUFFERING_MODE_TIMESHIFT :
         mode == GST_BUFFERING_LIVE      ? BUFFERING_MODE_LIVE :
                                           (assert(0), BUFFERING_MODE_STREAM));
    state->avg_in = avg_in;
    state->avg_out = avg_out;
    state->time_left_ms = buffering_left;
    state->n_ranges = n_ranges;

    notifier_notify(&player->buffering_state_notifier, state);
    return;

fail_free_state:
    free(state);

fail_unref_query:
    gst_query_unref(query);
}

static int apply_playback_state(struct gstplayer *player) {
    GstStateChangeReturn ok;
    GstState desired_state, current_state, pending_state;
    double desired_rate;
    int64_t position;

    desired_state = player->playpause_state == kPlaying ? GST_STATE_PLAYING : GST_STATE_PAUSED; /* use GST_STATE_PAUSED if we're stepping */

    /// Use 1.0 if we're stepping, otherwise use the stored playback rate for the current direction.
    if (player->playpause_state == kStepping) {
        desired_rate = player->direction == kForward ? 1.0 : -1.0;
    } else {
        desired_rate = player->direction == kForward ? player->playback_rate_forward : player->playback_rate_backward;
    }

    if (player->current_playback_rate != desired_rate || player->has_desired_position) {
        if (player->has_desired_position) {
            position = player->desired_position_ms * GST_MSECOND;
        } else {
            ok = gst_element_query_position(GST_ELEMENT(player->playbin), GST_FORMAT_TIME, &position);
            if (ok == FALSE) {
                LOG_PLAYER_ERROR(player, "Could not get the current playback position to apply the playback speed.\n");
                return EIO;
            }
        }

        if (player->direction == kForward) {
            LOG_PLAYER_DEBUG(
                player,
                "gst_element_seek(..., rate: %f, start: %" GST_TIME_FORMAT ", end: %" GST_TIME_FORMAT ", ...)\n",
                desired_rate,
                GST_TIME_ARGS(position),
                GST_TIME_ARGS(GST_CLOCK_TIME_NONE)
            );
            ok = gst_element_seek(
                GST_ELEMENT(player->playbin),
                desired_rate,
                GST_FORMAT_TIME,
                GST_SEEK_FLAG_FLUSH |
                    (player->do_fast_seeking ? GST_SEEK_FLAG_KEY_UNIT | GST_SEEK_FLAG_SNAP_NEAREST : GST_SEEK_FLAG_ACCURATE),
                GST_SEEK_TYPE_SET,
                position,
                GST_SEEK_TYPE_SET,
                GST_CLOCK_TIME_NONE
            );
            if (ok == FALSE) {
                LOG_PLAYER_ERROR(
                    player,
                    "Could not set the new playback speed / playback position (speed: %f, pos: %" GST_TIME_FORMAT ").\n",
                    desired_rate,
                    GST_TIME_ARGS(position)
                );
                return EIO;
            }
        } else {
            LOG_PLAYER_DEBUG(
                player, 
                "gst_element_seek(..., rate: %f, start: %" GST_TIME_FORMAT ", end: %" GST_TIME_FORMAT ", ...)\n",
                desired_rate,
                GST_TIME_ARGS(0),
                GST_TIME_ARGS(position)
            );
            ok = gst_element_seek(
                GST_ELEMENT(player->playbin),
                desired_rate,
                GST_FORMAT_TIME,
                GST_SEEK_FLAG_FLUSH |
                    (player->do_fast_seeking ? GST_SEEK_FLAG_KEY_UNIT | GST_SEEK_FLAG_SNAP_NEAREST : GST_SEEK_FLAG_ACCURATE),
                GST_SEEK_TYPE_SET,
                0,
                GST_SEEK_TYPE_SET,
                position
            );

            if (ok == FALSE) {
                LOG_PLAYER_ERROR(
                    player,
                    "Could not set the new playback speed / playback position (speed: %f, pos: %" GST_TIME_FORMAT ").\n",
                    desired_rate,
                    GST_TIME_ARGS(position)
                );
                return EIO;
            }
        }

        player->current_playback_rate = desired_rate;
        player->fallback_position_ms = GST_TIME_AS_MSECONDS(position);
        player->has_desired_position = false;
    }

    ok = gst_element_get_state(player->playbin, &current_state, &pending_state, 0);
    if (ok == GST_STATE_CHANGE_FAILURE) {
        LOG_PLAYER_DEBUG(
            player, 
            "last gstreamer pipeline state change failed. gst_element_get_state(element name: %s): GST_STATE_CHANGE_FAILURE\n",
            GST_ELEMENT_NAME(player->playbin)
        );
        return EIO;
    }

    if (pending_state == GST_STATE_VOID_PENDING) {
        if (current_state == desired_state) {
            // we're already in the desired state, and we're also not changing it
            // no need to do anything.
            LOG_PLAYER_DEBUG(
                player, 
                "apply_playback_state(playing: %s): already in desired state and none pending\n",
                PLAYPAUSE_STATE_AS_STRING(player->playpause_state)
            );
            return 0;
        }

        LOG_PLAYER_DEBUG(
            player, 
            "apply_playback_state(playing: %s): setting state to %s\n",
            PLAYPAUSE_STATE_AS_STRING(player->playpause_state),
            gst_element_state_get_name(desired_state)
        );

        ok = gst_element_set_state(player->playbin, desired_state);

        if (ok == GST_STATE_CHANGE_FAILURE) {
            LOG_GST_SET_STATE_ERROR(player->playbin);
            return EIO;
        }
    } else if (pending_state != desired_state) {
        // queue to be executed when pending async state change completes
        /// TODO: Implement properly

        LOG_PLAYER_DEBUG(
            player, 
            "apply_playback_state(playing: %s): async state change in progress, setting state to %s\n",
            PLAYPAUSE_STATE_AS_STRING(player->playpause_state),
            gst_element_state_get_name(desired_state)
        );

        ok = gst_element_set_state(player->playbin, desired_state);
        if (ok == GST_STATE_CHANGE_FAILURE) {
            LOG_GST_SET_STATE_ERROR(player->playbin);
            return EIO;
        }
    }
    return 0;
}

static void on_gstreamer_error_message(struct gstplayer *player, GstMessage *msg) {
    (void) player;
    
    GError *error;
    gchar *debug_info;

    gst_message_parse_error(msg, &error, &debug_info);

    LOG_PLAYER_ERROR(
        player,
        "gstreamer error: code: %d, domain: %s, msg: %s (debug info: %s)\n",
        error->code,
        g_quark_to_string(error->domain),
        error->message,
        debug_info
    );
    g_clear_error(&error);
    g_free(debug_info);
}

static void on_gstreamer_warning_message(struct gstplayer *player, GstMessage *msg) {
    (void) player;

    GError *error;
    gchar *debug_info;

    gst_message_parse_warning(msg, &error, &debug_info);

    LOG_PLAYER_ERROR(
        player,
        "gstreamer warning: code: %d, domain: %s, msg: %s (debug info: %s)\n",
        error->code,
        g_quark_to_string(error->domain),
        error->message,
        debug_info
    );
    g_clear_error(&error);
    g_free(debug_info);
}

static void on_gstreamer_info_message(struct gstplayer *player, GstMessage *msg) {
    GError *error;
    gchar *debug_info;

    gst_message_parse_info(msg, &error, &debug_info);

    LOG_PLAYER_DEBUG(player, "gstreamer info: %s (debug info: %s)\n", error->message, debug_info);
    g_clear_error(&error);
    g_free(debug_info);
}

static void on_buffering_message(struct gstplayer *player, GstMessage *msg) {
    GstBufferingMode mode;
    int64_t buffering_left;
    int percent, avg_in, avg_out;

    gst_message_parse_buffering(msg, &percent);
    gst_message_parse_buffering_stats(msg, &mode, &avg_in, &avg_out, &buffering_left);

    LOG_PLAYER_DEBUG(
        player,
        "buffering, src: %s, percent: %d, mode: %s, avg in: %d B/s, avg out: %d B/s, %" GST_TIME_FORMAT "\n",
        GST_MESSAGE_SRC_NAME(msg),
        percent,
        mode == GST_BUFFERING_STREAM    ? "stream" :
        mode == GST_BUFFERING_DOWNLOAD  ? "download" :
        mode == GST_BUFFERING_TIMESHIFT ? "timeshift" :
        mode == GST_BUFFERING_LIVE      ? "live" :
                                            "?",
        avg_in,
        avg_out,
        GST_TIME_ARGS(buffering_left * GST_MSECOND)
    );

    /// TODO: GST_MESSAGE_BUFFERING is only emitted when we actually need to wait on some buffering till we can resume the playback.
    /// However, the info we send to the callback also contains information on the buffered video ranges.
    /// That information is constantly changing, but we only notify the player about it when we actively wait for the buffer to be filled.
    update_buffering_state(player, GST_MESSAGE_SRC(msg));
}

static void on_state_change_message(struct gstplayer *player, GstMessage *msg) {
    GstState old, current, pending;

    gst_message_parse_state_changed(msg, &old, &current, &pending);
            
    if (GST_MESSAGE_SRC(msg) == GST_OBJECT(player->playbin)) {
        LOG_PLAYER_DEBUG(
            player,
            "playbin state changed: src: %s, old: %s, current: %s, pending: %s\n",
            GST_MESSAGE_SRC_NAME(msg),
            gst_element_state_get_name(old),
            gst_element_state_get_name(current),
            gst_element_state_get_name(pending)
        );

        if (!player->info.has_duration && (current == GST_STATE_PAUSED || current == GST_STATE_PLAYING)) {
            // it's our pipeline that changed to either playing / paused, and we don't have info about our video duration yet.
            // get that info now.
            // technically we can already fetch the duration when the decodebin changed to PAUSED state.
            fetch_duration(player);
            fetch_seeking(player);
            maybe_send_info(player);
        }
    }
}

static void on_application_message(struct gstplayer *player, GstMessage *msg) {
    if (gst_message_has_name(msg, "appsink-eos")) {
        if (player->looping) {
            // we have an appsink end of stream event
            // and we should be looping, so seek back to start
            LOG_PLAYER_DEBUG(player, "appsink eos, seeking back to segment start (flushing)\n");
            gst_element_seek(
                GST_ELEMENT(player->playbin),
                player->current_playback_rate,
                GST_FORMAT_TIME,
                GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE,
                GST_SEEK_TYPE_SET,
                0,
                GST_SEEK_TYPE_SET,
                GST_CLOCK_TIME_NONE
            );

            apply_playback_state(player);
        }
    } else if (gst_message_has_name(msg, "video-info")) {
        const GstStructure *structure = gst_message_get_structure(msg);
        
        const GValue *value = gst_structure_get_value(structure, "info");
        assert(G_VALUE_HOLDS_POINTER(value));

        GstVideoInfo *info = g_value_get_pointer(value);

        player->info.info.width = GST_VIDEO_INFO_WIDTH(info);
        player->info.info.height = GST_VIDEO_INFO_HEIGHT(info);
        player->info.info.fps = (double) GST_VIDEO_INFO_FPS_N(info) / GST_VIDEO_INFO_FPS_D(info);
        player->info.has_resolution = true;
        player->info.has_fps = true;

        gst_video_info_free(info);

        LOG_PLAYER_DEBUG(player, "Determined resolution: %d x %d and framerate: %f\n", player->info.info.width, player->info.info.height, player->info.info.fps);
    } else if (gst_message_has_name(msg, "about-to-finish")) {
        LOG_PLAYER_DEBUG(player, "Got about-to-finish signal\n");
    }
}

static void on_bus_message(struct gstplayer *player, GstMessage *msg) {
    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR:
            on_gstreamer_error_message(player, msg);
            break;

        case GST_MESSAGE_WARNING:
            on_gstreamer_warning_message(player, msg);
            break;

        case GST_MESSAGE_INFO:
            on_gstreamer_info_message(player, msg);
            break;

        case GST_MESSAGE_BUFFERING:
            on_buffering_message(player, msg);
            break;

        case GST_MESSAGE_STATE_CHANGED:
            on_state_change_message(player, msg);
            break;

        case GST_MESSAGE_ASYNC_DONE: break;

        case GST_MESSAGE_LATENCY:
            LOG_PLAYER_DEBUG(player, "gstreamer: redistributing latency\n");
            gst_bin_recalculate_latency(GST_BIN(player->playbin));
            break;

        case GST_MESSAGE_EOS:
            LOG_PLAYER_DEBUG(player, "end of stream, src: %s\n", GST_MESSAGE_SRC_NAME(msg));
            break;

        case GST_MESSAGE_REQUEST_STATE: {
            GstState requested;

            gst_message_parse_request_state(msg, &requested);
            LOG_PLAYER_DEBUG(
                player,
                "gstreamer state change to %s was requested by %s\n",
                gst_element_state_get_name(requested),
                GST_MESSAGE_SRC_NAME(msg)
            );
            gst_element_set_state(GST_ELEMENT(player->playbin), requested);
            break;
        }

        case GST_MESSAGE_APPLICATION:
            on_application_message(player, msg);
            break;

        default:
            LOG_PLAYER_DEBUG(player, "gstreamer message: %s, src: %s\n", GST_MESSAGE_TYPE_NAME(msg), GST_MESSAGE_SRC_NAME(msg));
            break;
    }
    return;
}

static int on_bus_fd_ready(sd_event_source *s, int fd, uint32_t revents, void *userdata) {
    (void) s;
    (void) fd;
    (void) revents;

    struct gstplayer *player = userdata;

    GstMessage *msg = gst_bus_pop(gst_element_get_bus(player->playbin));
    if (msg != NULL) {
        on_bus_message(player, msg);
        gst_message_unref(msg);
    }

    return 0;
}

void on_source_setup(GstElement *playbin, GstElement *source, gpointer userdata) {
    (void) playbin;

    ASSERT_NOT_NULL(userdata);

    if (g_object_class_find_property(G_OBJECT_GET_CLASS(source), "extra-headers") != NULL) {
        g_object_set(source, "extra-headers", (GstStructure *) userdata, NULL);
    } else {
        LOG_ERROR("Failed to set custom HTTP headers because gstreamer source element has no 'extra-headers' property.\n");
    }
}

typedef enum {
    GST_PLAY_FLAG_VIDEO = (1 << 0),
    GST_PLAY_FLAG_AUDIO = (1 << 1),
    GST_PLAY_FLAG_TEXT = (1 << 2)
} GstPlayFlags;

static void on_element_setup(GstElement *playbin, GstElement *element, gpointer userdata) {
    (void) playbin;
    (void) userdata;

    GstElementFactory *factory = gst_element_get_factory(element);
    if (factory == NULL) {
        return;
    }

    const char *factory_name = gst_plugin_feature_get_name(factory);

    if (g_str_has_prefix(factory_name, "v4l2video") && g_str_has_suffix(factory_name, "dec")) {
        gst_util_set_object_arg(G_OBJECT(element), "capture-io-mode", "dmabuf");
        LOG_DEBUG("Applied capture-io-mode = dmabuf\n");
    }
}

static void on_about_to_finish(GstElement *playbin, gpointer userdata) {
    (void) userdata;

    GstBus *bus = gst_element_get_bus(playbin);
    if (bus == NULL) {
        LOG_ERROR("Could not acquire bus to post about-to-finish message.\n");
        return;
    }

    GstStructure *s = gst_structure_new_empty("about-to-finish");
    if (s == NULL) {
        LOG_ERROR("Could not create about-to-finish gst structure.\n");
        gst_object_unref(bus);
        return;
    }

    GstMessage *msg = gst_message_new_application(GST_OBJECT(playbin), s);
    if (msg == NULL) {
        LOG_ERROR("Could not create about-to-finish gst message.\n");
        gst_structure_free(s);
        gst_object_unref(bus);
        return;
    }

    gboolean ok = gst_bus_post(bus, msg);
    if (ok != TRUE) {
        LOG_ERROR("Could not notify player about about-to-finish signal.\n");
    }

    gst_object_unref(bus);
}

static GstPadProbeReturn on_video_sink_event(GstPad *pad, GstPadProbeInfo *info, gpointer userdata) {
    GstBus *bus = userdata;
    
    (void) pad;

    GstEvent *event = gst_pad_probe_info_get_event(info);
    if (event == NULL) {
        return GST_PAD_PROBE_OK;
    }

    if (GST_EVENT_TYPE(event) != GST_EVENT_CAPS) {
        return GST_PAD_PROBE_OK;
    }

    GstCaps *caps = NULL;
    gst_event_parse_caps(event, &caps);

    if (!caps) {
        LOG_ERROR("Could not parse caps event.\n");
        return GST_PAD_PROBE_OK;
    }

    GstVideoInfo *videoinfo = gst_video_info_new_from_caps(caps);
    if (!videoinfo) {
        LOG_ERROR("Could not determine video properties of caps event.\n");
        return GST_PAD_PROBE_OK;
    }

    GValue v = G_VALUE_INIT;
    g_value_init(&v, G_TYPE_POINTER);
    g_value_set_pointer(&v, videoinfo);

    GstStructure *msg_structure = gst_structure_new_empty("video-info");
    gst_structure_set_value(msg_structure, "info", &v);

    gst_bus_post(bus, gst_message_new_application(GST_OBJECT(pad), msg_structure));

    // We're just interested in the caps event.
    // Once we have that, we can unlisten.
    return GST_PAD_PROBE_REMOVE;
}

static struct gstplayer *gstplayer_new_v2(struct flutterpi *flutterpi, const char *uri, void *userdata, bool play_video, bool play_audio, bool subtitles, GstStructure *headers) {
    struct gstplayer *p = calloc(1, sizeof(struct gstplayer));
    if (p == NULL) {
        return NULL;
    }
    
#ifdef DEBUG
    p->debug_id = allocate_id();
#endif
    p->userdata = userdata;

    value_notifier_init(&p->video_info_notifier, NULL, free);
    value_notifier_init(&p->buffering_state_notifier, NULL, free);
    change_notifier_init(&p->error_notifier);

    /// TODO: Use playbin or playbin3?
    p->playbin = gst_element_factory_make("playbin3", "playbin");
    if (p->playbin == NULL) {
        LOG_PLAYER_ERROR(p, "Couldn't create playbin instance.\n");
        goto fail_free_p;
    }

    g_object_set(p->playbin, "uri", uri, NULL);

    gint flags = 0;

    g_object_get(p->playbin, "flags", &flags, NULL);

    if (play_video) {
        flags |= GST_PLAY_FLAG_VIDEO;
    } else {
        flags &= ~GST_PLAY_FLAG_VIDEO;
    }

    if (play_audio) {
        flags |= GST_PLAY_FLAG_AUDIO;
    } else {
        flags &= ~GST_PLAY_FLAG_AUDIO;
    }

    if (subtitles) {
        flags |= GST_PLAY_FLAG_TEXT;
    } else {
        flags &= ~GST_PLAY_FLAG_TEXT;
    }

    g_object_set(p->playbin, "flags", flags, NULL);

    if (play_video) {
        p->texture = flutterpi_create_texture(flutterpi);
        if (p->texture == NULL) {
            goto fail_unref_playbin;
        }
        
        struct gl_renderer *gl_renderer = flutterpi_get_gl_renderer(flutterpi);

        GstElement *sink = flutter_gl_texture_sink_new(p->texture, gl_renderer);
        if (sink == NULL) {
            goto fail_destroy_texture;
        }

        /// TODO: What's the ownership transfer here?
        g_object_set(p->playbin, "video-sink", sink, NULL);
    
        // Apply capture-io-mode: dmabuf to any v4l2 decoders.
        /// TODO: This might be unnecessary / deprecated nowadays.
        g_signal_connect(p->playbin, "element-setup", G_CALLBACK(on_element_setup), NULL);

        GstPad *video_sink_pad = gst_element_get_static_pad(sink, "sink");
        if (video_sink_pad == NULL) {
            LOG_PLAYER_ERROR(p, "Could not acquire sink pad of video sink to wait for video configuration.\n");
            goto fail_destroy_texture;
        }

        // This will send a `video-info` application message to the bus when it sees a caps event.
        gst_pad_add_probe(video_sink_pad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, on_video_sink_event, gst_pipeline_get_bus(GST_PIPELINE(p->playbin)), NULL);
    }

    // Only try to configure headers if we actually have some.
    if (headers != NULL && gst_structure_n_fields(headers) > 0) {
        g_signal_connect(p->playbin, "source-setup", G_CALLBACK(on_source_setup), headers);
    }

    g_signal_connect(p->playbin, "about-to-finish", G_CALLBACK(on_about_to_finish), NULL);

    // Listen to the bus
    GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(p->playbin));
    ASSERT_NOT_NULL(bus);

    GPollFD fd;
    gst_bus_get_pollfd(bus, &fd);

    flutterpi_sd_event_add_io(&p->busfd_events, fd.fd, EPOLLIN, on_bus_fd_ready, p);

    gst_object_unref(bus);

    GstStateChangeReturn status = gst_element_set_state(p->playbin, GST_STATE_PAUSED);
    if (status == GST_STATE_CHANGE_NO_PREROLL) {
        LOG_PLAYER_DEBUG(p, "Is live!\n");
        p->is_live = true;
    } else if (status == GST_STATE_CHANGE_FAILURE) {
        LOG_PLAYER_ERROR(p, "Could not set pipeline to paused state.\n");
        goto fail_rm_event_source;
    } else {
        LOG_PLAYER_DEBUG(p, "Not live!\n");
        p->is_live = false;
    }

    return p;

fail_rm_event_source:
    sd_event_source_disable_unref(p->busfd_events);

fail_destroy_texture:
    gst_object_unref(p->playbin);
    
    // The flutter upload sink uses the texture internally,
    // so the playbin (which contains the upload sink) must be destroyed first,
    // before the texture can be destroyed.
    if (play_video) {
        texture_destroy(p->texture);
    }
    return NULL;

fail_unref_playbin:
    gst_object_unref(p->playbin);

fail_free_p:
    free(p);
    return NULL;
}

struct gstplayer *gstplayer_new_from_asset(struct flutterpi *flutterpi, const char *asset_path, const char *package_name, void *userdata) {
    struct gstplayer *player;
    char *uri;
    int ok;

    (void) package_name;

    ok = asprintf(&uri, "file://%s/%s", flutterpi_get_asset_bundle_path(flutterpi), asset_path);
    if (ok < 0) {
        return NULL;
    }

    player = gstplayer_new_v2(flutterpi, uri, userdata, true, true, false, NULL);

    free(uri);

    return player;
}

struct gstplayer *gstplayer_new_from_network(struct flutterpi *flutterpi, const char *uri, enum format_hint format_hint, void *userdata, GstStructure *headers) {
    (void) format_hint;
    return gstplayer_new_v2(flutterpi, uri, userdata, true, true, false, headers);
}

struct gstplayer *gstplayer_new_from_file(struct flutterpi *flutterpi, const char *uri, void *userdata) {
    return gstplayer_new_v2(flutterpi, uri, userdata, true, true, false, NULL);
}

struct gstplayer *gstplayer_new_from_content_uri(struct flutterpi *flutterpi, const char *uri, void *userdata, GstStructure *headers) {
    return gstplayer_new_v2(flutterpi, uri, userdata, true, true, false, headers);
}

struct gstplayer *gstplayer_new_from_pipeline(struct flutterpi *flutterpi, const char *pipeline, void *userdata) {
    /// TODO: Implement
    (void) flutterpi;
    (void) pipeline;
    (void) userdata;
    return NULL;
}

void gstplayer_destroy(struct gstplayer *player) {
    LOG_PLAYER_DEBUG(player, "destroy()\n");
    notifier_deinit(&player->video_info_notifier);
    notifier_deinit(&player->buffering_state_notifier);
    notifier_deinit(&player->error_notifier);
    gst_element_set_state(GST_ELEMENT(player->playbin), GST_STATE_READY);
    gst_element_set_state(GST_ELEMENT(player->playbin), GST_STATE_NULL);
    gst_object_unref(player->playbin);
    if (player->texture) {
        texture_destroy(player->texture);
    }
    free(player);
}

int64_t gstplayer_get_texture_id(struct gstplayer *player) {
    // If the player was started with play_video == false, player->texture is NULL.
    return player->texture ? texture_get_id(player->texture) : -1;
}

void gstplayer_set_userdata(struct gstplayer *player, void *userdata) {
    player->userdata = userdata;
}

void *gstplayer_get_userdata(struct gstplayer *player) {
    return player->userdata;
}

int gstplayer_play(struct gstplayer *player) {
    LOG_PLAYER_DEBUG(player, "play()\n");
    player->playpause_state = kPlaying;
    player->direction = kForward;
    return apply_playback_state(player);
}

int gstplayer_pause(struct gstplayer *player) {
    LOG_PLAYER_DEBUG(player, "pause()\n");
    player->playpause_state = kPaused;
    player->direction = kForward;
    return apply_playback_state(player);
}

int gstplayer_set_looping(struct gstplayer *player, bool looping) {
    LOG_PLAYER_DEBUG(player, "set_looping(%s)\n", looping ? "true" : "false");
    player->looping = looping;
    return 0;
}

int gstplayer_set_volume(struct gstplayer *player, double volume) {
    LOG_PLAYER_DEBUG(player, "set_volume(%f)\n", volume);
    g_object_set(player->playbin, "volume", (gdouble) volume, NULL);
    return 0;
}

int64_t gstplayer_get_position(struct gstplayer *player) {
    GstState current, pending;
    gboolean ok;
    int64_t position;

    GstStateChangeReturn statechange = gst_element_get_state(GST_ELEMENT(player->playbin), &current, &pending, 0);
    if (statechange == GST_STATE_CHANGE_FAILURE) {
        LOG_GST_GET_STATE_ERROR(player->playbin);
        return -1;
    }

    if (statechange == GST_STATE_CHANGE_ASYNC) {
        // we don't have position data yet.
        // report the latest known (or the desired) position.
        return player->fallback_position_ms;
    }

    ok = gst_element_query_position(player->playbin, GST_FORMAT_TIME, &position);
    if (ok == FALSE) {
        LOG_PLAYER_ERROR(player, "Could not query gstreamer position. (gst_element_query_position)\n");
        return 0;
    }

    return GST_TIME_AS_MSECONDS(position);
}

int gstplayer_seek_to(struct gstplayer *player, int64_t position, bool nearest_keyframe) {
    LOG_PLAYER_DEBUG(player, "seek_to(%" PRId64 ")\n", position);
    player->has_desired_position = true;
    player->desired_position_ms = position;
    player->do_fast_seeking = nearest_keyframe;
    return apply_playback_state(player);
}

int gstplayer_set_playback_speed(struct gstplayer *player, double playback_speed) {
    LOG_PLAYER_DEBUG(player, "set_playback_speed(%f)\n", playback_speed);
    ASSERT_MSG(playback_speed > 0, "playback speed must be > 0.");
    player->playback_rate_forward = playback_speed;
    return apply_playback_state(player);
}

int gstplayer_step_forward(struct gstplayer *player) {
    gboolean gst_ok;
    int ok;

    ASSERT_NOT_NULL(player);

    player->playpause_state = kStepping;
    player->direction = kForward;
    ok = apply_playback_state(player);
    if (ok != 0) {
        return ok;
    }

    gst_ok = gst_element_send_event(player->playbin, gst_event_new_step(GST_FORMAT_BUFFERS, 1, 1, TRUE, FALSE));
    if (gst_ok == FALSE) {
        LOG_PLAYER_ERROR(player, "Could not send frame-step event to pipeline. (gst_element_send_event)\n");
        return EIO;
    }
    return 0;
}

int gstplayer_step_backward(struct gstplayer *player) {
    gboolean gst_ok;
    int ok;

    ASSERT_NOT_NULL(player);

    player->playpause_state = kStepping;
    player->direction = kBackward;
    ok = apply_playback_state(player);
    if (ok != 0) {
        return ok;
    }

    gst_ok = gst_element_send_event(player->playbin, gst_event_new_step(GST_FORMAT_BUFFERS, 1, 1, TRUE, FALSE));
    if (gst_ok == FALSE) {
        LOG_PLAYER_ERROR(player, "Could not send frame-step event to pipeline. (gst_element_send_event)\n");
        return EIO;
    }

    return 0;
}

struct notifier *gstplayer_get_video_info_notifier(struct gstplayer *player) {
    return &player->video_info_notifier;
}

struct notifier *gstplayer_get_buffering_state_notifier(struct gstplayer *player) {
    return &player->buffering_state_notifier;
}

struct notifier *gstplayer_get_error_notifier(struct gstplayer *player) {
    return &player->error_notifier;
}
