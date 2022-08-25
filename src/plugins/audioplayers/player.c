#include <stdio.h>

#include "gst/gst.h"
#include "gst/gstelementfactory.h"
#include "gst/gstmessage.h"
#include "gst/gstsegment.h"
#include "platformchannel.h"

#include <flutter-pi.h>
#include <plugins/audioplayers.h>

FILE_DESCR("audioplayers player")

struct audio_player {
    GstElement *playbin;
    GstBus *bus;

    bool is_initialized;
    bool is_looping;
    bool is_seek_completed;
    double playback_rate;

    char *url;
    char *player_id;
    char *channel;
};

// Private Class functions
static gboolean audio_player_on_bus_message(GstBus *bus, GstMessage *message, struct audio_player *data);
static gboolean audio_player_on_refresh(struct audio_player *data);
static void audio_player_set_playback(struct audio_player *self, int64_t seekTo, double rate);
static void audio_player_on_media_error(struct audio_player *self, GError *error, gchar *debug);
static void audio_player_on_media_state_change(struct audio_player *self, GstObject *src, GstState *old_state, GstState *new_state);
static void audio_player_on_position_update(struct audio_player *self);
static void audio_player_on_duration_update(struct audio_player *self);
static void audio_player_on_seek_completed(struct audio_player *self);
static void audio_player_on_playback_ended(struct audio_player *self);

static int on_bus_fd_ready(sd_event_source *src, int fd, uint32_t revents, void *userdata) {
    struct audio_player *player = userdata;
    GstMessage *msg;

    (void) src;
    (void) fd;
    (void) revents;

    /* DEBUG_TRACE_BEGIN(player, "on_bus_fd_ready"); */

    msg = gst_bus_pop(player->bus);
    if (msg != NULL) {
        audio_player_on_bus_message(player->bus, msg, player);
        gst_message_unref(msg);
    }

    /* DEBUG_TRACE_END(player, "on_bus_fd_ready"); */

    return 0;
}

struct audio_player *audio_player_new(char *player_id, char *channel) {
    GPollFD fd;
    sd_event_source *busfd_event_source;

    struct audio_player *self = malloc(sizeof(struct audio_player));
    if (self == NULL) {
        return NULL;
    }

    self->url = NULL;
    self->is_initialized = false;
    self->is_looping = false;
    self->is_seek_completed = false;
    self->playback_rate = 1.0;

    gst_init(NULL, NULL);
    self->playbin = gst_element_factory_make("playbin", "playbin");
    if (!self->playbin) {
        LOG_ERROR("Could not create gstreamer playbin.\n");
        goto deinit_self;
    }

    self->bus = gst_element_get_bus(self->playbin);

    gst_bus_get_pollfd(self->bus, &fd);

    flutterpi_sd_event_add_io(&busfd_event_source, fd.fd, EPOLLIN, on_bus_fd_ready, self);

    // Refresh continuously to emit recurring events
    g_timeout_add(1000, (GSourceFunc) audio_player_on_refresh, self);

    self->player_id = strdup(player_id);
    if (self->player_id == NULL)
        goto deinit_player;

    self->channel = strdup(channel);
    if (self->channel == NULL)
        goto deinit_player_id;

    return self;

    //Deinit doesn't require to NULL, as we just delete player.
deinit_player_id:
    free(self->player_id);

deinit_player:
    free(self->channel);

    gst_object_unref(self->bus);
    gst_element_set_state(self->playbin, GST_STATE_NULL);
    gst_object_unref(self->playbin);

deinit_self:
    free(self);
    return NULL;
}

void audio_player_source_setup(GstElement *playbin, GstElement *source, GstElement **p_src) {
    (void) playbin;
    (void) source;
    (void) p_src;
    /**
     * Consider if we want to add option to enable strict SSL check.
    if (g_object_class_find_property(G_OBJECT_GET_CLASS(source), "ssl-strict") != 0) {
        g_object_set(G_OBJECT(source), "ssl-strict", FALSE, NULL);
    }
    */
}

gboolean audio_player_on_bus_message(GstBus *bus, GstMessage *message, struct audio_player *data) {
    (void) bus;
    switch (GST_MESSAGE_TYPE(message)) {
        case GST_MESSAGE_ERROR: {
            GError *err;
            gchar *debug;

            gst_message_parse_error(message, &err, &debug);
            audio_player_on_media_error(data, err, debug);
            g_error_free(err);
            g_free(debug);
            break;
        }
        case GST_MESSAGE_STATE_CHANGED: {
            GstState old_state, new_state;

            gst_message_parse_state_changed(message, &old_state, &new_state, NULL);
            audio_player_on_media_state_change(data, message->src, &old_state, &new_state);
            break;
        }
        case GST_MESSAGE_EOS:
            gst_element_set_state(data->playbin, GST_STATE_READY);
            audio_player_on_playback_ended(data);
            break;
        case GST_MESSAGE_DURATION_CHANGED: audio_player_on_duration_update(data); break;
        case GST_MESSAGE_ASYNC_DONE:
            if (!data->is_seek_completed) {
                audio_player_on_seek_completed(data);
                data->is_seek_completed = true;
            }
            break;
        default:
            // For more GstMessage types see:
            // https://gstreamer.freedesktop.org/documentation/gstreamer/gstmessage.html?gi-language=c#enumerations
            break;
    }

    // Continue watching for messages
    return TRUE;
}

gboolean audio_player_on_refresh(struct audio_player *data) {
    if (data->playbin->current_state == GST_STATE_PLAYING) {
        audio_player_on_position_update(data);
    }
    return TRUE;
}

void audio_player_set_playback(struct audio_player *self, int64_t seekTo, double rate) {
    const GstSeekFlags seek_flags = GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE;

    if (!self->is_initialized) {
        return;
    }
    // See:
    // https://gstreamer.freedesktop.org/documentation/tutorials/basic/playback-speed.html?gi-language=c
    if (!self->is_seek_completed) {
        return;
    }
    if (rate == 0) {
        // Do not set rate if it's 0, rather pause.
        audio_player_pause(self);
        return;
    }

    if (self->playback_rate != rate) {
        self->playback_rate = rate;
    }
    self->is_seek_completed = false;

    GstEvent *seek_event;
    if (rate > 0) {
        seek_event = gst_event_new_seek(rate, GST_FORMAT_TIME, seek_flags, GST_SEEK_TYPE_SET, seekTo * GST_MSECOND, GST_SEEK_TYPE_NONE, -1);
    } else {
        seek_event = gst_event_new_seek(rate, GST_FORMAT_TIME, seek_flags, GST_SEEK_TYPE_SET, 0, GST_SEEK_TYPE_SET, seekTo * GST_MSECOND);
    }
    if (!gst_element_send_event(self->playbin, seek_event)) {
        // Not clear how to treat this error?
        const int64_t seekMs = seekTo * GST_MSECOND;
        LOG_ERROR("Could not set playback to position " GST_STIME_FORMAT " and rate %f.\n", GST_TIME_ARGS(seekMs), rate);
        self->is_seek_completed = true;
    }
}
void audio_player_on_media_error(struct audio_player *self, GError *error, gchar *debug) {
    (void) debug;
    char error_message[256] = {0};
    snprintf(error_message, sizeof(error_message), "Error: %d; message=%s", error->code, error->message);
    if (self->channel) {
        // clang-format off
        platch_call_std(
            self->channel,
            "audio.onError",
            &STDMAP2(
                STDSTRING("player_id"),
                STDSTRING(self->player_id),
                STDSTRING("value"),
                STDSTRING(error_message)
            ),
            NULL,
            NULL
        );
        // clang-format on
    }
}

void audio_player_on_media_state_change(struct audio_player *self, GstObject *src, GstState *old_state, GstState *new_state) {
    (void) old_state;
    if (strcmp(GST_OBJECT_NAME(src), "playbin") == 0) {
        if (*new_state >= GST_STATE_READY) {
            if (!self->is_initialized) {
                self->is_initialized = true;
                audio_player_pause(self);  // Need to set to pause state, in order to get duration
            }
        } else if (self->is_initialized) {
            self->is_initialized = false;
        }
    }
}
void audio_player_on_position_update(struct audio_player *self) {
    if (self->channel) {
        // clang-format off
        platch_call_std(
            self->channel,
            "audio.onCurrentPosition",
            &STDMAP2(
                STDSTRING("player_id"),
                STDSTRING(self->player_id),
                STDSTRING("value"),
                STDINT64(audio_player_get_position(self))
            ),
            NULL,
            NULL
         );
        // clang-format on
    }
}
void audio_player_on_duration_update(struct audio_player *self) {
    if (self->channel) {
        // clang-format off
        platch_call_std(
            self->channel,
            "audio.onDuration",
            &STDMAP2(
                STDSTRING("player_id"),
                STDSTRING(self->player_id),
                STDSTRING("value"),
                STDINT64(audio_player_get_duration(self))
            ),
            NULL,
            NULL
        );
        // clang-format on
    }
}
void audio_player_on_seek_completed(struct audio_player *self) {
    if (self->channel) {
        audio_player_on_position_update(self);
        // clang-format off
        platch_call_std(
            self->channel,
            "audio.onSeekComplete",
            &STDMAP2(
                STDSTRING("player_id"),
                STDSTRING(self->player_id),
                STDSTRING("value"),
                STDBOOL(true)
            ),
            NULL,
            NULL
        );
        // clang-format on
    }
}
void audio_player_on_playback_ended(struct audio_player *self) {
    audio_player_set_position(self, 0);
    if (audio_player_get_looping(self)) {
        audio_player_play(self);
    }
    if (self->channel) {
        // clang-format off
        platch_call_std(
            self->channel,
            "audio.onComplete",
            &STDMAP2(
                STDSTRING("player_id"),
                STDSTRING(self->player_id),
                STDSTRING("value"),
                STDBOOL(true)
            ),
            NULL,
            NULL
        );
        // clang-format on
    }
}

void audio_player_set_looping(struct audio_player *self, bool is_looping) {
    self->is_looping = is_looping;
}

bool audio_player_get_looping(struct audio_player *self) {
    return self->is_looping;
}

void audio_player_play(struct audio_player *self) {
    if (!self->is_initialized) {
        return;
    }
    audio_player_set_position(self, 0);
    audio_player_resume(self);
}

void audio_player_pause(struct audio_player *self) {
    GstStateChangeReturn ret = gst_element_set_state(self->playbin, GST_STATE_PAUSED);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        LOG_ERROR("Unable to set the pipeline to the paused state.\n");
        return;
    }
    audio_player_on_position_update(self);  // Update to exact position when pausing
}

void audio_player_resume(struct audio_player *self) {
    if (!self->is_initialized) {
        return;
    }
    GstStateChangeReturn ret = gst_element_set_state(self->playbin, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        LOG_ERROR("Unable to set the pipeline to the playing state.\n");
        return;
    }
    // Update duration when start playing, as no event is emitted elsewhere
    audio_player_on_duration_update(self);
}

void audio_player_destroy(struct audio_player *self) {
    if (self->is_initialized) {
        audio_player_pause(self);
    }
    gst_object_unref(self->bus);
    self->bus = NULL;

    gst_element_set_state(self->playbin, GST_STATE_NULL);
    gst_object_unref(self->playbin);
    self->playbin = NULL;

    self->is_initialized = false;

    if (self->url != NULL) {
        free(self->url);
        self->url = NULL;
    }

    if (self->player_id != NULL) {
        free(self->player_id);
        self->player_id = NULL;
    }

    if (self->channel != NULL) {
        free(self->channel);
        self->channel = NULL;
    }

    free(self);
}

int64_t audio_player_get_position(struct audio_player *self) {
    gint64 current = 0;
    if (!gst_element_query_position(self->playbin, GST_FORMAT_TIME, &current)) {
        LOG_ERROR("Could not query current position.\n");
        return 0;
    }
    return current / 1000000;
}

int64_t audio_player_get_duration(struct audio_player *self) {
    gint64 duration = 0;
    if (!gst_element_query_duration(self->playbin, GST_FORMAT_TIME, &duration)) {
        LOG_ERROR("Could not query current duration.\n");
        return 0;
    }
    return duration / 1000000;
}

void audio_player_set_volume(struct audio_player *self, double volume) {
    if (volume > 1) {
        volume = 1;
    } else if (volume < 0) {
        volume = 0;
    }
    g_object_set(G_OBJECT(self->playbin), "volume", volume, NULL);
}

void audio_player_set_playback_rate(struct audio_player *self, double rate) {
    audio_player_set_playback(self, audio_player_get_position(self), rate);
}

void audio_player_set_position(struct audio_player *self, int64_t position) {
    if (!self->is_initialized) {
        return;
    }
    audio_player_set_playback(self, position, self->playback_rate);
}

void audio_player_set_source_url(struct audio_player *self, char *url) {
    DEBUG_ASSERT_NOT_NULL(url);
    if (self->url == NULL || strcmp(self->url, url)) {
        if (self->url != NULL) {
            free(self->url);
            self->url = NULL;
        }
        self->url = strdup(url);
        gst_element_set_state(self->playbin, GST_STATE_NULL);
        if (strlen(self->url) != 0) {
            g_object_set(self->playbin, "uri", self->url, NULL);
            if (self->playbin->current_state != GST_STATE_READY) {
                gst_element_set_state(self->playbin, GST_STATE_READY);
            }
        }
        self->is_initialized = false;
    }
}

bool audio_player_is_id(struct audio_player *self, char *player_id) {
    return strcmp(self->player_id, player_id) == 0;
}
