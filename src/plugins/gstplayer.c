#define _GNU_SOURCE

#include <stdatomic.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#include <pthread.h>

#include <drm_fourcc.h>
#include <gst/allocators/gstdmabuf.h>
#include <gst/app/gstappsink.h>
#include <gst/gstmemory.h>
#include <gst/gstpad.h>
#include <gst/video/gstvideometa.h>
#include <gst/gstelementfactory.h>
#include <gst/gstclock.h>
#include <gst/gstelement.h>
#include <gst/gstobject.h>
#include <gst/gstformat.h>
#include <gst/gstmessage.h>
#include <gst/gstsegment.h>

#include "flutter-pi.h"
#include "notifier_listener.h"
#include "platformchannel.h"
#include "pluginregistry.h"
#include "plugins/gstplayer.h"
#include "texture_registry.h"
#include "tracer.h"
#include "util/logging.h"
#include "util/macros.h"
#include "util/collection.h"
#include "util/asserts.h"

#include "config.h"

#ifdef HAVE_GSTREAMER_VIDEO_PLAYER
    #include "gstreamer_video_player.h"
#endif

#define LOG_PLAYER_DEBUG(player, fmtstring, ...) LOG_DEBUG("gstplayer-%"PRIi64": " fmtstring, player->debug_id, ##__VA_ARGS__)
#ifdef DEBUG
    #define LOG_PLAYER_ERROR(player, fmtstring, ...) LOG_ERROR("gstplayer-%"PRIi64": " fmtstring, player->debug_id, ##__VA_ARGS__)
#else
    #define LOG_PLAYER_ERROR(player, fmtstring, ...) LOG_ERROR(fmtstring, ##__VA_ARGS__)
#endif

#define LOG_GST_SET_STATE_ERROR(player, _element)                                                                       \
    LOG_PLAYER_ERROR(                                                                                                   \
        player,                                                                                                         \
        "setting gstreamer playback state failed. gst_element_set_state(element name: %s): GST_STATE_CHANGE_FAILURE\n", \
        GST_ELEMENT_NAME(_element)                                                                                      \
    )

#define LOG_GST_GET_STATE_ERROR(player, _element)                                                                  \
    LOG_PLAYER_ERROR(                                                                                              \
        player,                                                                                                    \
        "last gstreamer state change failed. gst_element_get_state(element name: %s): GST_STATE_CHANGE_FAILURE\n", \
        GST_ELEMENT_NAME(_element)                                                                                 \
    )

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
    bool looping;

    /**
     * @brief True if the looping should use gapless looping using either the about-to-finish callback
     * from playbin or segments.
     *
     * Configured in gstplayer_set_looping
     */
    bool gapless_looping;

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
    struct notifier duration_notifier, seeking_info_notifier;
    struct notifier eos_notifier;

    bool has_sent_info;
    struct incomplete_video_info info;

    bool has_duration;
    int64_t duration;

    bool has_seeking_info;
    struct seeking_info seeking_info;

    /**
     * The flutter texture that this video player is pushing frames to.
     */
    struct texture *texture;

    sd_event_source *busfd_events;

    /**
     * The gstreamer playbin.
     * 
     * In most cases this is the same as the pipeline (since a playbin is a pipeline).
     * The only exception is when the gstplayer was initialized using a pipeline description,
     * in which case we don't have a playbin. In that case, playbin will be NULL and
     * pipeline will be valid.
     */
    GstElement *playbin;

    /**
     * The gstreamer pipeline.
     */
    GstElement *pipeline;

    /**
     * The gstreamer audiopanorama element, used as the "audio-filter"
     * if audio playback is enabled, and used to change the audio
     * left/right balance.
     */
    GstElement *audiopanorama;

    /**
     * True if we're playing back a live source,
     * e.g. a live stream
     */
    bool is_live;

    /**
     * Callbacks to be called on ASYNC_DONE gstreamer messages.
     *
     * ASYNC_DONE messages indicate completion of an async state
     * change or a flushing seek.
     */
    size_t n_async_completers;
    struct async_completer completers[8];

    /**
     * @brief Use the playbin "uri" property and "about-to-finish" signal
     * to achieve gapless looping, if looping is desired.
     *
     * It's a bit unclear whether this is worse or equally as good as
     * using segments; so segment looping is preferred for now.
     *
     * However, segments are not always super reliable (e.g. playbin3
     * segment looping is broken in gstreamer < 1.22.9), so the playbin
     * method is kept intact still as a backup.
     */
    bool playbin_gapless;

    /**
     * @brief Use segments to do gapless looping, if looping is desired.
     *
     * (Instead of e.g. seeking back to start on EOS, or setting the
     * playbin uri property in about-to-finish)
     */
    bool segment_gapless;

    /**
     * The source uri this gstplayer should play back.
     *
     * Mostly used to as the argument to `g_object_set(p->playbin, "uri", ...)`
     * in on_about_to_finish, as querying the current source uri from the playbin
     * is not always reliable.
     */
    char *uri;

    /**
     * True if we did already issue a flushing seek
     * with GST_SEEK_FLAG_SEGMENT.
     *
     * A flushing seek with GST_SEEK_FLAG_SEGMENT has to be
     * issued to start gapless looping.
     */
    bool did_configure_segment;

    struct tracer *tracer;
};

static struct async_completer pop_completer(struct gstplayer *player) {
    ASSERT(player->n_async_completers > 0);

    struct async_completer completer = player->completers[0];

    player->n_async_completers--;
    if (player->n_async_completers > 0) {
        memmove(player->completers + 0, player->completers + 1, player->n_async_completers * sizeof(struct async_completer));
    }

    return completer;
}

static void on_async_done_message(struct gstplayer *player) {
    if (player->n_async_completers > 0) {
        struct async_completer completer = pop_completer(player);

        if (completer.on_done) {
            completer.on_done(completer.userdata);
        }
    }
}

static void on_async_error(struct gstplayer *player, GError *error) {
    if (player->n_async_completers > 0) {
        struct async_completer completer = pop_completer(player);

        if (completer.on_error) {
            completer.on_error(completer.userdata, error);
        }
    }
}

static int maybe_send_video_info(struct gstplayer *player) {
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

    ok = gst_element_query_duration(player->pipeline, GST_FORMAT_TIME, &duration);
    if (ok == FALSE) {
        if (player->is_live) {
            player->info.info.duration_ms = INT64_MAX;
            player->info.has_duration = true;

            player->has_duration = true;
            player->duration = INT64_MAX;
            return;
        } else {
            LOG_PLAYER_ERROR(player, "Could not fetch duration. (gst_element_query_duration)\n");
            return;
        }
    }

    player->info.info.duration_ms = GST_TIME_AS_MSECONDS(duration);
    player->info.has_duration = true;

    player->duration = GST_TIME_AS_MSECONDS(duration);
    player->has_duration = true;
}

static void fetch_seeking(struct gstplayer *player) {
    GstQuery *seeking_query;
    gboolean ok, seekable;
    int64_t seek_begin, seek_end;

    seeking_query = gst_query_new_seeking(GST_FORMAT_TIME);
    ok = gst_element_query(player->pipeline, seeking_query);
    if (ok == FALSE) {
        if (player->is_live) {
            player->info.info.can_seek = false;
            player->info.info.seek_begin_ms = 0;
            player->info.info.seek_end_ms = 0;
            player->info.has_seeking_info = true;

            player->seeking_info.can_seek = false;
            player->seeking_info.seek_begin_ms = 0;
            player->seeking_info.seek_end_ms = 0;
            player->has_seeking_info = true;
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

    player->seeking_info.can_seek = seekable;
    player->seeking_info.seek_begin_ms = GST_TIME_AS_MSECONDS(seek_begin);
    player->seeking_info.seek_end_ms = GST_TIME_AS_MSECONDS(seek_end);
    player->has_seeking_info = true;
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

    TRACER_BEGIN(player->tracer, "apply_playback_state()");

    TRACER_BEGIN(player->tracer, "gst_element_get_state()");
    ok = gst_element_get_state(player->pipeline, &current_state, &pending_state, 0);
    TRACER_END(player->tracer, "gst_element_get_state()");

    if (ok == GST_STATE_CHANGE_FAILURE) {
        LOG_PLAYER_DEBUG(
            player,
            "last gstreamer pipeline state change failed. gst_element_get_state(element name: %s): GST_STATE_CHANGE_FAILURE\n",
            GST_ELEMENT_NAME(player->pipeline)
        );
        goto fail_stop_trace;
    }

    if (current_state == GST_STATE_NULL) {
        // We don't have a playback source right now.
        // Don't do anything.
        TRACER_END(player->tracer, "apply_playback_state()");
        return 0;
    }

    desired_state = player->playpause_state == kPlaying ? GST_STATE_PLAYING : GST_STATE_PAUSED; /* use GST_STATE_PAUSED if we're stepping */

    /// Use 1.0 if we're stepping, otherwise use the stored playback rate for the current direction.
    if (player->playpause_state == kStepping) {
        desired_rate = player->direction == kForward ? 1.0 : -1.0;
    } else {
        desired_rate = player->direction == kForward ? player->playback_rate_forward : player->playback_rate_backward;
    }

    bool is_segment_looping = player->looping && player->gapless_looping && player->segment_gapless;
    if (player->current_playback_rate != desired_rate || player->has_desired_position || (player->did_configure_segment != is_segment_looping)) {
        if (player->has_desired_position) {
            position = player->desired_position_ms * GST_MSECOND;
        } else {
            TRACER_BEGIN(player->tracer, "gst_element_query_position()");
            ok = gst_element_query_position(GST_ELEMENT(player->pipeline), GST_FORMAT_TIME, &position);
            TRACER_END(player->tracer, "gst_element_query_position()");
            
            if (ok == FALSE) {
                LOG_PLAYER_ERROR(player, "Could not get the current playback position to apply the playback speed.\n");
                goto fail_stop_trace;
            }
        }

        GstSeekFlags seek_flags = GST_SEEK_FLAG_FLUSH;

        // Only configure segment looping if we actually
        // are segment looping, because it will
        // swallow the end-of-stream events apparently.
        if (is_segment_looping) {
            seek_flags |= GST_SEEK_FLAG_SEGMENT;
        }

        if (player->do_fast_seeking) {
            seek_flags |= GST_SEEK_FLAG_KEY_UNIT | GST_SEEK_FLAG_SNAP_NEAREST;
        } else {
            seek_flags |= GST_SEEK_FLAG_ACCURATE;
        }

        if (player->direction == kForward) {
            LOG_PLAYER_DEBUG(
                player,
                "gst_element_seek(..., rate: %f, start: %" GST_TIME_FORMAT ", end: %" GST_TIME_FORMAT ", ...)\n",
                desired_rate,
                GST_TIME_ARGS(position),
                GST_TIME_ARGS(GST_CLOCK_TIME_NONE)
            );

            TRACER_BEGIN(player->tracer, "gst_element_seek()");
            ok = gst_element_seek(
                GST_ELEMENT(player->pipeline),
                desired_rate,
                GST_FORMAT_TIME,
                seek_flags,
                GST_SEEK_TYPE_SET, position,
                GST_SEEK_TYPE_SET, GST_CLOCK_TIME_NONE
            );
            TRACER_END(player->tracer, "gst_element_seek()");

            if (ok == FALSE) {
                LOG_PLAYER_ERROR(
                    player,
                    "Could not set the new playback speed / playback position (speed: %f, pos: %" GST_TIME_FORMAT ").\n",
                    desired_rate,
                    GST_TIME_ARGS(position)
                );
                goto fail_stop_trace;
            }
        } else {
            LOG_PLAYER_DEBUG(
                player,
                "gst_element_seek(..., rate: %f, start: %" GST_TIME_FORMAT ", end: %" GST_TIME_FORMAT ", ...)\n",
                desired_rate,
                GST_TIME_ARGS(0),
                GST_TIME_ARGS(position)
            );

            TRACER_BEGIN(player->tracer, "gst_element_seek()");
            ok = gst_element_seek(
                GST_ELEMENT(player->pipeline),
                desired_rate,
                GST_FORMAT_TIME,
                seek_flags,
                GST_SEEK_TYPE_SET, 0,
                GST_SEEK_TYPE_SET, position
            );
            TRACER_END(player->tracer, "gst_element_seek()");

            if (ok == FALSE) {
                LOG_PLAYER_ERROR(
                    player,
                    "Could not set the new playback speed / playback position (speed: %f, pos: %" GST_TIME_FORMAT ").\n",
                    desired_rate,
                    GST_TIME_ARGS(position)
                );
                goto fail_stop_trace;
            }
        }

        player->current_playback_rate = desired_rate;
        player->fallback_position_ms = GST_TIME_AS_MSECONDS(position);
        player->has_desired_position = false;
        player->did_configure_segment = is_segment_looping;
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
            TRACER_END(player->tracer, "apply_playback_state()");
            return 0;
        }

        LOG_PLAYER_DEBUG(
            player,
            "apply_playback_state(playing: %s): setting state to %s\n",
            PLAYPAUSE_STATE_AS_STRING(player->playpause_state),
            gst_element_state_get_name(desired_state)
        );

        TRACER_BEGIN(player->tracer, "gst_element_set_state()");
        ok = gst_element_set_state(player->pipeline, desired_state);
        TRACER_END(player->tracer, "gst_element_set_state()");

        if (ok == GST_STATE_CHANGE_FAILURE) {
            LOG_GST_SET_STATE_ERROR(player, player->pipeline);
            goto fail_stop_trace;
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

        TRACER_BEGIN(player->tracer, "gst_element_set_state()");
        ok = gst_element_set_state(player->pipeline, desired_state);
        TRACER_END(player->tracer, "gst_element_set_state()");

        if (ok == GST_STATE_CHANGE_FAILURE) {
            LOG_GST_SET_STATE_ERROR(player, player->pipeline);
            goto fail_stop_trace;
        }
    }

    TRACER_END(player->tracer, "apply_playback_state()");
    return 0;

fail_stop_trace:
    TRACER_END(player->tracer, "apply_playback_state()");
    return EIO;
}

static void on_eos_message(struct gstplayer *player, GstMessage *msg) {
    if (GST_MESSAGE_SRC(msg) == GST_OBJECT(player->pipeline)) {
        if (player->looping) {
            LOG_PLAYER_DEBUG(player, "pipeline end of stream, seeking back to start (flushing)\n");
            player->desired_position_ms = 0;
            player->has_desired_position = true;
            apply_playback_state(player);
        } else {
            LOG_PLAYER_DEBUG(player, "pipeline end of stream\n");
            notifier_notify(&player->eos_notifier, NULL);
        }
    } else {
        LOG_PLAYER_DEBUG(player, "end of stream for element: %s\n", GST_MESSAGE_SRC_NAME(msg));
    }
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

    on_async_error(player, error);

    notifier_notify(&player->error_notifier, error);

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

    if (percent == 0 || percent == 100) {
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
    }

    /// TODO: GST_MESSAGE_BUFFERING is only emitted when we actually need to wait on some buffering till we can resume the playback.
    /// However, the info we send to the callback also contains information on the buffered video ranges.
    /// That information is constantly changing, but we only notify the player about it when we actively wait for the buffer to be filled.
    update_buffering_state(player, GST_MESSAGE_SRC(msg));
}

static void on_state_changed_message(struct gstplayer *player, GstMessage *msg) {
    GstState old, current, pending;

    gst_message_parse_state_changed(msg, &old, &current, &pending);

    if (GST_MESSAGE_SRC(msg) == GST_OBJECT(player->pipeline)) {
        LOG_PLAYER_DEBUG(
            player,
            "pipeline state changed: old: %s, current: %s, pending: %s\n",
            gst_element_state_get_name(old),
            gst_element_state_get_name(current),
            gst_element_state_get_name(pending)
        );

        if (current == GST_STATE_READY || current == GST_STATE_NULL) {
            if (player->has_duration) {
                player->has_duration = false;
                notifier_notify(&player->duration_notifier, NULL);
            }

            player->info.has_duration = false;

            player->has_seeking_info = false;
            player->info.has_seeking_info = false;

            player->did_configure_segment = false;
        } else if ((current == GST_STATE_PAUSED || current == GST_STATE_PLAYING) && (old == GST_STATE_READY || old == GST_STATE_NULL)) {
            // it's our pipeline that changed to either playing / paused, and we don't have info about our video duration yet.
            // get that info now.
            // technically we can already fetch the duration when the decodebin changed to PAUSED state.

            if (!player->has_duration) {
                fetch_duration(player);

                if (player->has_duration) {
                    int64_t *duped = memdup(&player->duration, sizeof(int64_t));

                    notifier_notify(&player->duration_notifier, duped);
                }
            }

            if (!player->has_seeking_info) {
                fetch_seeking(player);

                if (player->has_seeking_info) {
                    struct seeking_info *duped = memdup(&player->seeking_info, sizeof(struct seeking_info));

                    notifier_notify(&player->seeking_info_notifier, duped);
                }
            }

            maybe_send_video_info(player);
        }
    }
}

static void on_segment_start_message(struct gstplayer *player, GstMessage *msg) {
    GstFormat format;
    gint64 position;
    gst_message_parse_segment_start(msg, &format, &position);

    if (format == GST_FORMAT_TIME) {
        LOG_PLAYER_DEBUG(
            player,
            "segment start. src: %s, position: %" GST_TIME_FORMAT "\n",
            GST_MESSAGE_SRC_NAME(msg),
            GST_TIME_ARGS(position)
        );
    } else {
        LOG_PLAYER_DEBUG(
            player,
            "segment start. src: %s, position: %" PRId64 " (%s)\n",
            GST_MESSAGE_SRC_NAME(msg),
            position,
            gst_format_get_name(format)
        );
    }
}

static void on_segment_done_message(struct gstplayer *player, GstMessage *msg) {
    (void) msg;

    if (player->looping && player->gapless_looping && player->segment_gapless) {
        LOG_PLAYER_DEBUG(player, "Segment done. Seeking back to segment start (segment, non-flushing)\n");
        gboolean ok = gst_element_seek(
            player->pipeline,
            player->current_playback_rate,
            GST_FORMAT_TIME,
            GST_SEEK_FLAG_SEGMENT,
            GST_SEEK_TYPE_SET, 0,
            GST_SEEK_TYPE_SET, GST_CLOCK_TIME_NONE
        );
        if (!ok) {
            LOG_PLAYER_DEBUG(player, "Could not seek back to segment start.\n");
        }
    }
}

static void on_duration_changed_message(struct gstplayer *player, GstMessage *msg) {
    (void) msg;

    if (!player->has_duration) {
        fetch_duration(player);

        if (player->has_duration) {
            int64_t *duped = memdup(&player->duration, sizeof(int64_t));

            notifier_notify(&player->duration_notifier, duped);
        }
    }

    if (!player->has_seeking_info) {
        fetch_seeking(player);

        if (player->has_seeking_info) {
            struct seeking_info *duped = memdup(&player->seeking_info, sizeof(struct seeking_info));

            notifier_notify(&player->seeking_info_notifier, duped);
        }
    }

    maybe_send_video_info(player);
}

static void on_about_to_finish_message(struct gstplayer *player) {
    ASSERT_NOT_NULL(player->playbin);
    
    if (player->looping && player->uri && player->playbin_gapless) {
        LOG_PLAYER_DEBUG(player, "Got about-to-finish signal, configuring next playback item\n");
        g_object_set(player->playbin, "uri", player->uri, NULL);
    } else {
        LOG_PLAYER_DEBUG(player, "Got about-to-finish signal\n");
    }
}

static void on_application_message(struct gstplayer *player, GstMessage *msg) {
    if (gst_message_has_name(msg, "appsink-eos")) {
        // unhandled
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
        on_about_to_finish_message(player);
    }
}

static void start_async(struct gstplayer *player, struct async_completer completer) {
    ASSERT(player->n_async_completers < ARRAY_SIZE(player->completers));

    player->completers[player->n_async_completers++] = completer;
}

static void on_bus_message(struct gstplayer *player, GstMessage *msg) {
    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_EOS:
            on_eos_message(player, msg);
            break;

        case GST_MESSAGE_ERROR:
            on_gstreamer_error_message(player, msg);
            break;

        case GST_MESSAGE_WARNING:
            on_gstreamer_warning_message(player, msg);
            break;

        case GST_MESSAGE_INFO:
            on_gstreamer_info_message(player, msg);
            break;

        case GST_MESSAGE_TAG: {
            if (0) {
                GstTagList *tags;
                gst_message_parse_tag(msg, &tags);

                char *str = gst_tag_list_to_string(tags);

                LOG_PLAYER_DEBUG(player, "%s found tags: %s\n", GST_MESSAGE_SRC_NAME(msg), str);

                free(str);
            }
            break;
        }

        case GST_MESSAGE_BUFFERING:
            on_buffering_message(player, msg);
            break;

        case GST_MESSAGE_STATE_CHANGED:
            on_state_changed_message(player, msg);
            break;

        case GST_MESSAGE_APPLICATION:
            on_application_message(player, msg);
            break;

        case GST_MESSAGE_SEGMENT_START:
            on_segment_start_message(player, msg);
            break;

        case GST_MESSAGE_SEGMENT_DONE:
            on_segment_done_message(player, msg);
            break;

        case GST_MESSAGE_DURATION_CHANGED:
            on_duration_changed_message(player, msg);
            break;

        case GST_MESSAGE_LATENCY:
            LOG_PLAYER_DEBUG(player, "redistributing latency\n");
            gst_bin_recalculate_latency(GST_BIN(player->pipeline));
            break;

        case GST_MESSAGE_ASYNC_DONE:
            on_async_done_message(player);
            break;

        case GST_MESSAGE_REQUEST_STATE: {
            GstState requested;

            gst_message_parse_request_state(msg, &requested);
            gst_element_set_state(GST_ELEMENT(player->pipeline), requested);
            break;
        }

        case GST_MESSAGE_QOS: {
            if (0) {
                gboolean live = false;
                uint64_t running_time = 0;
                uint64_t stream_time = 0;
                uint64_t timestamp = 0;
                uint64_t duration = 0;

                GstFormat format = GST_FORMAT_DEFAULT;
                uint64_t processed = 0;
                uint64_t dropped = 0;

                int64_t jitter = 0;
                double proportion = 1.0;
                int quality = 0;

                gst_message_parse_qos(msg, &live, &running_time, &stream_time, &timestamp, &duration);
                gst_message_parse_qos_stats(msg, &format, &processed, &dropped);
                gst_message_parse_qos_values(msg, &jitter, &proportion, &quality);

                LOG_PLAYER_DEBUG(
                    player,
                    "Quality of Service: %s\n"
                    "  live: %s\n"
                    "  running time: %" GST_TIME_FORMAT "\n"
                    "  stream time: %" GST_TIME_FORMAT "\n"
                    "  timestamp: %" GST_TIME_FORMAT "\n"
                    "  duration: %" GST_TIME_FORMAT "\n"
                    "  processed: %" PRIu64 " (%s)\n"
                    "  dropped: %" PRIu64 " (%s)\n"
                    "  jitter: %" PRId64 "\n"
                    "  proportion: %f\n"
                    "  quality: %d\n",
                    GST_MESSAGE_SRC_NAME(msg),
                    live ? "yes" : "no",
                    GST_TIME_ARGS(running_time),
                    GST_TIME_ARGS(stream_time),
                    GST_TIME_ARGS(timestamp),
                    GST_TIME_ARGS(duration),
                    processed, gst_format_get_name(format),
                    dropped, gst_format_get_name(format),
                    jitter,
                    proportion,
                    quality
                );
            }
            break;
        }

        default:
            if (0) {
                LOG_PLAYER_DEBUG(player, "gstreamer message: %s, src: %s\n", GST_MESSAGE_TYPE_NAME(msg), GST_MESSAGE_SRC_NAME(msg));
            }

            break;
    }
    return;
}

static int on_bus_fd_ready(sd_event_source *s, int fd, uint32_t revents, void *userdata) {
    (void) s;
    (void) fd;
    (void) revents;

    struct gstplayer *player = userdata;

    GstMessage *msg = gst_bus_pop(gst_element_get_bus(player->pipeline));
    if (msg != NULL) {
        TRACER_BEGIN(player->tracer, "on_bus_message()");
        on_bus_message(player, msg);
        TRACER_END(player->tracer, "on_bus_message()");

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

/**
 * See: https://gitlab.freedesktop.org/gstreamer/gstreamer/-/blob/main/subprojects/gst-plugins-base/gst/playback/gstplay-enum.h
 */
typedef enum {
    GST_PLAY_FLAG_VIDEO = (1 << 0),
    GST_PLAY_FLAG_AUDIO = (1 << 1),
    GST_PLAY_FLAG_TEXT = (1 << 2)
} GstPlayFlags;

UNUSED static void on_element_setup(GstElement *playbin, GstElement *element, gpointer userdata) {
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

UNUSED static GstPadProbeReturn on_video_sink_event(GstPad *pad, GstPadProbeInfo *info, gpointer userdata) {
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

    GstVideoInfo *videoinfo = gst_video_info_new();
    ASSUME(videoinfo != NULL);

    if (!gst_video_info_from_caps(videoinfo, caps)) {
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

struct gstplayer *gstplayer_new(struct flutterpi *flutterpi, const char *uri, void *userdata, bool play_video, bool play_audio, GstStructure *headers) {
    ASSERT_NOT_NULL(flutterpi);

    struct gstplayer *p = calloc(1, sizeof(struct gstplayer));
    if (p == NULL) {
        return NULL;
    }

#ifdef DEBUG
    p->debug_id = allocate_id();
#endif
    p->userdata = userdata;
    p->current_playback_rate = 1.0;
    p->playback_rate_forward = 1.0;
    p->playback_rate_backward = 1.0;

    // Gapless looping is configured in the gstplayer_set_looping call.
    //
    // Without gapless looping, we'll just seek back to start on EOS,
    // which always works.
    p->gapless_looping = false;

    // Gapless looping using playbin "about-to-finish" is unreliable
    // in audio playback.
    //
    // E.g., using the audioplayers example and looping the first ("coin")
    // sound, switching to the second sound will first play the second sound,
    // then play part of the first sound at higher pitch, and then loop the
    // second sound.
    //
    // Also, it seems like the playbin recreates all the elements & decoders,
    // so it's not super resource-saving either.
    p->playbin_gapless = false;

    // Segment gapless looping works mostly fine, but is also
    // not completely reliable.
    //
    // E.g., looping the second ("laser") sound of the audioplayers
    // example will play back 1-2 seconds of noise after
    // the laser sound, then play the laser sound, then noise, etc.
    //
    // Segment looping does not work with playbin3 in gstreamer
    // < 1.22.9 because of a bug in multiqueue.
    p->segment_gapless = true;

    p->tracer = flutterpi_get_tracer(flutterpi);

    TRACER_BEGIN(p->tracer, "gstplayer_new()");

    value_notifier_init(&p->video_info_notifier, NULL, free);
    value_notifier_init(&p->duration_notifier, NULL, free);
    value_notifier_init(&p->seeking_info_notifier, NULL, free);
    value_notifier_init(&p->buffering_state_notifier, NULL, free);
    change_notifier_init(&p->error_notifier);
    change_notifier_init(&p->eos_notifier);

    // playbin is more reliable for now than playbin3 (see above)
    p->playbin = gst_element_factory_make("playbin", "playbin");
    if (p->playbin == NULL) {
        LOG_PLAYER_ERROR(p, "Couldn't create playbin instance.\n");
        goto fail_free_p;
    }

    p->pipeline = p->playbin;

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

    flags &= ~GST_PLAY_FLAG_TEXT;

    g_object_set(p->playbin, "flags", flags, NULL);

    if (play_video) {
#ifdef HAVE_GSTREAMER_VIDEO_PLAYER
        p->texture = flutterpi_create_texture(flutterpi);
        if (p->texture == NULL) {
            goto fail_unref_playbin;
        }

        struct gl_renderer *gl_renderer = flutterpi_get_gl_renderer(flutterpi);

        GstElement *sink = flutter_gl_texture_sink_new(p->texture, gl_renderer, p->tracer);
        if (sink == NULL) {
            goto fail_destroy_texture;
        }

        GstPad *video_sink_pad = gst_element_get_static_pad(sink, "sink");
        if (video_sink_pad == NULL) {
            LOG_PLAYER_ERROR(p, "Could not acquire sink pad of video sink to wait for video configuration.\n");
            goto fail_destroy_texture;
        }

        // This will send a `video-info` application message to the bus when it sees a caps event.
        gst_pad_add_probe(video_sink_pad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, on_video_sink_event, gst_pipeline_get_bus(GST_PIPELINE(p->playbin)), NULL);

        gst_object_unref(video_sink_pad);
        video_sink_pad = NULL;

        // playbin (playsink) takes a (sinking) reference
        // on the video sink
        g_object_set(p->playbin, "video-sink", sink, NULL);

        // Apply capture-io-mode: dmabuf to any v4l2 decoders.
        /// TODO: This might be unnecessary / deprecated nowadays.
        g_signal_connect(p->playbin, "element-setup", G_CALLBACK(on_element_setup), NULL);
#else
        (void) flutterpi;

        ASSERT_MSG(0, "Video playback with gstplayer is only supported when building with the gstreamer video player plugin.");
        goto fail_unref_playbin;
#endif
    }


    if (play_audio) {
        p->audiopanorama = gst_element_factory_make("audiopanorama", NULL);
        if (p->audiopanorama != NULL) {
            g_object_set(p->playbin, "audio-filter", p->audiopanorama, NULL);
        }
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

    // If we have a URI, preroll it.
    if (uri != NULL) {
        g_object_set(p->playbin, "uri", uri, NULL);

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

        p->uri = strdup(uri);
    }

    TRACER_END(p->tracer, "gstplayer_new()");

    LOG_PLAYER_DEBUG(p, "gstplayer_new(\"%s\", %s): %s\n", uri ?: "", play_audio ? "with audio" : "without audio", p->is_live ? "live" : "not live");

    return p;

fail_rm_event_source:
    sd_event_source_set_enabled(p->busfd_events, false);
    sd_event_source_unref(p->busfd_events);

fail_destroy_texture: UNUSED
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
    TRACER_END(p->tracer, "gstplayer_new()");
    free(p);
    return NULL;
}

struct gstplayer *gstplayer_new_from_asset(struct flutterpi *flutterpi, const char *asset_path, const char *package_name, bool play_video, bool play_audio, void *userdata) {
    struct gstplayer *player;
    char *uri;
    int ok;

    (void) package_name;

    ok = asprintf(&uri, "file://%s/%s", flutterpi_get_asset_bundle_path(flutterpi), asset_path);
    if (ok < 0) {
        return NULL;
    }

    player = gstplayer_new(flutterpi, uri, userdata, /* play_video */ play_video, /* play_audio */ play_audio, NULL);

    free(uri);

    return player;
}

struct gstplayer *gstplayer_new_from_network(struct flutterpi *flutterpi, const char *uri, enum format_hint format_hint, bool play_video, bool play_audio, void *userdata, GstStructure *headers) {
    (void) format_hint;
    return gstplayer_new(flutterpi, uri, userdata, /* play_video */ play_video, /* play_audio */ play_audio, headers);
}

struct gstplayer *gstplayer_new_from_file(struct flutterpi *flutterpi, const char *uri, bool play_video, bool play_audio, void *userdata) {
    return gstplayer_new(flutterpi, uri, userdata, /* play_video */ play_video, /* play_audio */ play_audio, NULL);
}

struct gstplayer *gstplayer_new_from_content_uri(struct flutterpi *flutterpi, const char *uri, bool play_video, bool play_audio, void *userdata, GstStructure *headers) {
    return gstplayer_new(flutterpi, uri, userdata, /* play_video */ play_video, /* play_audio */ play_audio, headers);
}

struct gstplayer *gstplayer_new_from_pipeline(struct flutterpi *flutterpi, const char *pipeline_descr, void *userdata) {
    ASSERT_NOT_NULL(flutterpi);

    struct gstplayer *p = calloc(1, sizeof(struct gstplayer));
    if (p == NULL) {
        return NULL;
    }

#ifdef DEBUG
    p->debug_id = allocate_id();
#endif
    p->userdata = userdata;
    p->current_playback_rate = 1.0;
    p->playback_rate_forward = 1.0;
    p->playback_rate_backward = 1.0;

    p->gapless_looping = false;
    p->playbin_gapless = false;
    p->segment_gapless = false;

    p->tracer = flutterpi_get_tracer(flutterpi);

    value_notifier_init(&p->video_info_notifier, NULL, free);
    value_notifier_init(&p->duration_notifier, NULL, free);
    value_notifier_init(&p->seeking_info_notifier, NULL, free);
    value_notifier_init(&p->buffering_state_notifier, NULL, free);
    change_notifier_init(&p->error_notifier);
    change_notifier_init(&p->eos_notifier);

    GError *error = NULL;
    p->pipeline = gst_parse_launch(pipeline_descr, &error);
    if (p->pipeline == NULL) {
        LOG_ERROR("Could create GStreamer pipeline from description: %s (pipeline: `%s`)\n", error->message, pipeline_descr);
        return NULL;
    }

    // Remove the sink from the parsed pipeline description, and add our own sink.
    GstElement *sink = gst_bin_get_by_name(GST_BIN(p->pipeline), "sink");
    if (sink == NULL) {
        LOG_ERROR("Couldn't find appsink in pipeline bin.\n");
        goto fail_unref_pipeline;
    }

    p->texture = flutterpi_create_texture(flutterpi);
    if (p->texture == NULL) {
        goto fail_unref_pipeline;
    }

    struct gl_renderer *gl_renderer = flutterpi_get_gl_renderer(flutterpi);

    if (!flutter_gl_texture_sink_patch(sink, p->texture, gl_renderer, p->tracer)) {
        LOG_ERROR("Could not setup appsink.\n");
        goto fail_unref_pipeline;
    }

    // Listen to the bus
    GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(p->pipeline));
    ASSERT_NOT_NULL(bus);

    GPollFD fd;
    gst_bus_get_pollfd(bus, &fd);

    flutterpi_sd_event_add_io(&p->busfd_events, fd.fd, EPOLLIN, on_bus_fd_ready, p);

    gst_object_unref(bus);

    GstStateChangeReturn status = gst_element_set_state(p->pipeline, GST_STATE_PAUSED);
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
    sd_event_source_set_enabled(p->busfd_events, false);
    sd_event_source_unref(p->busfd_events);

fail_destroy_texture: UNUSED
    gst_object_unref(p->pipeline);

    // The flutter upload sink uses the texture internally,
    // so the appsink (which contains the upload sink) must be destroyed first,
    // before the texture can be destroyed.
    texture_destroy(p->texture);
    return NULL;

fail_unref_pipeline:
    gst_object_unref(p->pipeline);
    return NULL;
}

void gstplayer_destroy(struct gstplayer *player) {
    LOG_PLAYER_DEBUG(player, "destroy()\n");
    notifier_deinit(&player->video_info_notifier);
    notifier_deinit(&player->duration_notifier);
    notifier_deinit(&player->seeking_info_notifier);
    notifier_deinit(&player->buffering_state_notifier);
    notifier_deinit(&player->error_notifier);
    notifier_deinit(&player->eos_notifier);

    gst_element_set_state(GST_ELEMENT(player->pipeline), GST_STATE_READY);
    gst_element_set_state(GST_ELEMENT(player->pipeline), GST_STATE_NULL);

    player->playbin = NULL;
    gst_object_unref(player->pipeline);

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

int gstplayer_set_looping(struct gstplayer *player, bool looping, bool gapless) {
    LOG_PLAYER_DEBUG(player, "set_looping(%s, gapless: %s)\n", looping ? "true" : "false", gapless ? "true" : "false");

    if (player->playbin_gapless && gapless) {
        ASSERT_NOT_NULL(player->playbin);

        // If we're enabling (gapless) looping,
        // already configure the next playback URI,
        // since we don't know if the about-to-finish callback
        // has already arrived or not.
        if (!player->looping && looping && player->uri) {
            g_object_set(player->playbin, "uri", player->uri, NULL);
        }
    }

    player->looping = looping;
    player->gapless_looping = gapless;

    apply_playback_state(player);

    return 0;
}

int gstplayer_set_volume(struct gstplayer *player, double volume) {
    if (player->playbin) {
        LOG_PLAYER_DEBUG(player, "set_volume(%f)\n", volume);
        g_object_set(player->playbin, "volume", (gdouble) volume, NULL);
    } else {
        LOG_PLAYER_DEBUG(player, "set_volume(%f): can't set volume on pipeline video player\n", volume);
    }
    
    return 0;
}

int64_t gstplayer_get_position(struct gstplayer *player) {
    GstState current, pending;
    gboolean ok;
    int64_t position;

    GstStateChangeReturn statechange = gst_element_get_state(GST_ELEMENT(player->pipeline), &current, &pending, 0);
    if (statechange == GST_STATE_CHANGE_FAILURE) {
        LOG_GST_GET_STATE_ERROR(player, player->pipeline);
        return -1;
    }

    if (statechange == GST_STATE_CHANGE_ASYNC) {
        // we don't have position data yet.
        // report the latest known (or the desired) position.
        return player->fallback_position_ms;
    }

    ok = gst_element_query_position(player->pipeline, GST_FORMAT_TIME, &position);
    if (ok == FALSE) {
        LOG_PLAYER_ERROR(player, "Could not query gstreamer position. (gst_element_query_position)\n");
        return 0;
    }

    return GST_TIME_AS_MSECONDS(position);
}

int64_t gstplayer_get_duration(struct gstplayer *player) {
    if (!player->has_duration) {
        return -1;
    } else {
        return player->duration;
    }
}

int gstplayer_seek_to(struct gstplayer *player, int64_t position, bool nearest_keyframe) {
    LOG_PLAYER_DEBUG(player, "seek_to(%" PRId64 ")\n", position);
    player->has_desired_position = true;
    player->desired_position_ms = position;
    player->do_fast_seeking = nearest_keyframe;
    return apply_playback_state(player);
}

int gstplayer_seek_with_completer(struct gstplayer *player, int64_t position, bool nearest_keyframe, struct async_completer completer) {
    LOG_PLAYER_DEBUG(player, "seek_to(%" PRId64 ")\n", position);
    player->has_desired_position = true;
    player->desired_position_ms = position;
    player->do_fast_seeking = nearest_keyframe;

    if (completer.on_done || completer.on_error) {
        start_async(player, completer);
    }

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

    gst_ok = gst_element_send_event(player->pipeline, gst_event_new_step(GST_FORMAT_BUFFERS, 1, 1, TRUE, FALSE));
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

    gst_ok = gst_element_send_event(player->pipeline, gst_event_new_step(GST_FORMAT_BUFFERS, 1, 1, TRUE, FALSE));
    if (gst_ok == FALSE) {
        LOG_PLAYER_ERROR(player, "Could not send frame-step event to pipeline. (gst_element_send_event)\n");
        return EIO;
    }

    return 0;
}

void gstplayer_set_audio_balance(struct gstplayer *player, float balance) {
    if (player->audiopanorama) {
        g_object_set(player->audiopanorama, "panorama", (gfloat) balance, NULL);
    }
}

float gstplayer_get_audio_balance(struct gstplayer *player) {
    if (player->audiopanorama) {
        gfloat balance = 0.0;
        g_object_get(player->audiopanorama, "panorama", &balance, NULL);
        return balance;
    } else {
        return 0.0;
    }
}

bool gstplayer_set_source_with_completer(struct gstplayer *p, const char *uri, struct async_completer completer) {
    GstStateChangeReturn result;
    const char *current_uri = NULL;

    if (!p->playbin) {
        LOG_PLAYER_ERROR(p, "Can't set source for a pipeline video player.\n");
        return false;
    }

    g_object_get(p->playbin, "current-uri", &current_uri, NULL);

    // If we're already playing back the desired uri, don't change it.
    if ((current_uri == uri) || (uri && current_uri && streq(current_uri, uri))) {
        if (completer.on_done) {
            completer.on_done(completer.userdata);
        }

        return true;
    }

    p->uri = strdup(uri);

    // If the playbin supports instant-uri, use it.
    // if (g_object_class_find_property(G_OBJECT_GET_CLASS(p->playbin), "instant-uri")) {
    //     g_object_set(p->playbin, "instant-uri", TRUE, "uri", uri, NULL);
    // } else {

    result = gst_element_set_state(p->playbin, GST_STATE_NULL);
    if (result != GST_STATE_CHANGE_SUCCESS) {
        LOG_PLAYER_ERROR(p, "Could not set pipeline to NULL state to change uri.\n");
        return false;
    }

    g_object_set(p->playbin, "uri", uri, NULL);

    result = gst_element_set_state(p->playbin, GST_STATE_PAUSED);
    if (result == GST_STATE_CHANGE_FAILURE) {
        LOG_PLAYER_ERROR(p, "Could not set pipeline to PAUSED state to play new uri.\n");
        return false;
    } else if (result == GST_STATE_CHANGE_NO_PREROLL) {
        p->is_live = true;

        if (completer.on_done != NULL) {
            completer.on_done(completer.userdata);
        }
    } else if (result == GST_STATE_CHANGE_SUCCESS) {
        p->is_live = false;

        if (completer.on_done) {
            completer.on_done(completer.userdata);
        }
    } else if (result == GST_STATE_CHANGE_ASYNC) {
        /// TODO: What is is_live here?
        p->is_live = false;

        if (completer.on_done || completer.on_error) {
            start_async(p, completer);
        }
    }

    gstplayer_seek_to(p, 0, false);

    return true;
}

bool gstplayer_set_source(struct gstplayer *p, const char *uri) {
    return gstplayer_set_source_with_completer(p, uri, (struct async_completer) {
        .on_done = NULL,
        .on_error = NULL,
        .userdata = NULL
    });
}

struct notifier *gstplayer_get_video_info_notifier(struct gstplayer *player) {
    return &player->video_info_notifier;
}

struct notifier *gstplayer_get_duration_notifier(struct gstplayer *player) {
    return &player->duration_notifier;
}

struct notifier *gstplayer_get_seeking_info_notifier(struct gstplayer *player) {
    return &player->seeking_info_notifier;
}

struct notifier *gstplayer_get_buffering_state_notifier(struct gstplayer *player) {
    return &player->buffering_state_notifier;
}

struct notifier *gstplayer_get_error_notifier(struct gstplayer *player) {
    return &player->error_notifier;
}

struct notifier *gstplayer_get_eos_notifier(struct gstplayer *player) {
    return &player->eos_notifier;
}
