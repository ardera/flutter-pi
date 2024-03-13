#define _GNU_SOURCE

#include <stdio.h>
#include <stdbool.h>

#include <gst/gst.h>
#include <gst/gstelementfactory.h>
#include <gst/gstmessage.h>
#include <gst/gstsegment.h>

#include "flutter-pi.h"
#include "platformchannel.h"
#include "plugins/audioplayers.h"
#include "util/asserts.h"
#include "util/logging.h"

struct audio_player {
    GstElement *source;
    GstElement *playbin;
    GstBus *bus;

    GstElement *panorama;
    GstElement *audiobin;
    GstElement *audiosink;
    GstPad *panoramaSinkPad;

    bool is_initialized;
    bool is_playing;
    bool is_looping;
    bool is_seek_completed;
    double playback_rate;

    char *url;
    char *player_id;
    char *event_channel_name;

    _Atomic bool event_subscribed;
};

// Private Class functions
static gboolean audio_player_on_bus_message(GstBus *bus, GstMessage *message, struct audio_player *data);
static gboolean audio_player_on_refresh(struct audio_player *data);
static void audio_player_set_playback(struct audio_player *self, int64_t seekTo, double rate);
static void audio_player_on_media_error(struct audio_player *self, GError *error, gchar *debug);
static void audio_player_on_media_state_change(struct audio_player *self, GstObject *src, GstState *old_state, GstState *new_state);
static void audio_player_on_prepared(struct audio_player *self, bool value);
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

static void audio_player_source_setup(GstElement *playbin, GstElement *source, GstElement **p_src) {
    (void)(playbin);
    (void)(p_src);

    if (g_object_class_find_property(G_OBJECT_GET_CLASS(source), "ssl-strict") != 0) {
        g_object_set(G_OBJECT(source), "ssl-strict", FALSE, NULL);
    }
}

struct audio_player *audio_player_new(char *player_id, char *channel) {
    GPollFD fd;
    sd_event_source *busfd_event_source;

    struct audio_player *self = malloc(sizeof(struct audio_player));
    if (self == NULL) {
        return NULL;
    }

    self->url = NULL;
    self->source = NULL;
    self->is_initialized = false;
    self->is_playing = false;
    self->is_looping = false;
    self->is_seek_completed = false;
    self->playback_rate = 1.0;
    self->event_subscribed = false;

    gst_init(NULL, NULL);
    self->playbin = gst_element_factory_make("playbin", NULL);
    if (!self->playbin) {
        LOG_ERROR("Could not create gstreamer playbin.\n");
        goto deinit_self;
    }

    // Setup stereo balance controller
    self->panorama = gst_element_factory_make("audiopanorama", NULL);
    if (self->panorama) {
        self->audiobin = gst_bin_new(NULL);
        self->audiosink = gst_element_factory_make("autoaudiosink", NULL);

        gst_bin_add_many(GST_BIN(self->audiobin), self->panorama, self->audiosink, NULL);
        gst_element_link(self->panorama, self->audiosink);

        GstPad *sinkpad = gst_element_get_static_pad(self->panorama, "sink");
        self->panoramaSinkPad = gst_ghost_pad_new("sink", sinkpad);
        gst_element_add_pad(self->audiobin, self->panoramaSinkPad);
        gst_object_unref(GST_OBJECT(sinkpad));

        g_object_set(G_OBJECT(self->playbin), "audio-sink", self->audiobin, NULL);
        g_object_set(G_OBJECT(self->panorama), "method", 1, NULL);
    } else {
        self->audiobin = NULL;
        self->audiosink = NULL;
        self->panoramaSinkPad = NULL;
    }

    g_signal_connect(self->playbin, "source-setup", G_CALLBACK(audio_player_source_setup), &self->source);

    self->bus = gst_element_get_bus(self->playbin);

    gst_bus_get_pollfd(self->bus, &fd);

    flutterpi_sd_event_add_io(&busfd_event_source, fd.fd, EPOLLIN, on_bus_fd_ready, self);

    // Refresh continuously to emit recurring events
    g_timeout_add(1000, (GSourceFunc) audio_player_on_refresh, self);

    self->player_id = strdup(player_id);
    if (self->player_id == NULL) {
        goto deinit_player;
    }

    // audioplayers player event channel clang:
    // <local>/events/<player_id>
    asprintf(&self->event_channel_name, "%s/events/%s", channel, player_id);
    ASSERT_MSG(self->event_channel_name != NULL, "event channel name OEM");

    if (self->event_channel_name == NULL) {
        goto deinit_player_id;
    }

    return self;

    //Deinit doesn't require to NULL, as we just delete player.
deinit_player_id:
    free(self->player_id);

deinit_player:
    gst_object_unref(self->bus);

    if (self->panorama != NULL) {
        gst_element_set_state(self->audiobin, GST_STATE_NULL);

        gst_element_remove_pad(self->audiobin, self->panoramaSinkPad);
        gst_bin_remove(GST_BIN(self->audiobin), self->audiosink);
        gst_bin_remove(GST_BIN(self->audiobin), self->panorama);

        self->panorama = NULL;
        self->audiosink = NULL;
        self->panoramaSinkPad = NULL;
        self->audiobin = NULL;
    }

    gst_element_set_state(self->playbin, GST_STATE_NULL);
    gst_object_unref(self->playbin);

deinit_self:
    free(self);
    return NULL;
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
            audio_player_on_playback_ended(data);
            break;
        case GST_MESSAGE_DURATION_CHANGED:
            audio_player_on_duration_update(data);
            break;
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

gboolean audio_player_on_refresh(struct audio_player *self) {
    if (self == NULL) {
        return FALSE;
    }

    GstState playbinState;
    gst_element_get_state(self->playbin, &playbinState, NULL, GST_CLOCK_TIME_NONE);
    if (playbinState == GST_STATE_PLAYING) {
        audio_player_on_position_update(self);
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
    self->playback_rate = rate;
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
    if (!self->event_subscribed) {
        return;
    }

    char error_code[16] = {0};
    snprintf(error_code, sizeof(error_code), "%d", error->code);
    // clang-format off
    platch_send_error_event_std(
        self->event_channel_name,
        error_code,
        error->message,
        debug ? &STDSTRING(debug) : NULL
    );
    // clang-format on
}

void audio_player_on_media_state_change(struct audio_player *self, GstObject *src, GstState *old_state, GstState *new_state) {
    (void) old_state;
    if (src == GST_OBJECT(self->playbin)) {
        LOG_DEBUG("%s: on_media_state_change(old_state=%d, new_state=%d)\n", self->player_id, *old_state, *new_state);
        if (*new_state == GST_STATE_READY) {
            // Need to set to pause state, in order to make player functional
            GstStateChangeReturn ret = gst_element_set_state(self->playbin, GST_STATE_PAUSED);
            if (ret == GST_STATE_CHANGE_FAILURE) {
                LOG_ERROR("Unable to set the pipeline to the paused state.\n");
            }

            self->is_initialized = false;
        } else if (*old_state == GST_STATE_PAUSED && *new_state == GST_STATE_PLAYING) {
            audio_player_on_position_update(self);
            audio_player_on_duration_update(self);
        } else if (*new_state >= GST_STATE_PAUSED) {
            if (!self->is_initialized) {
                self->is_initialized = true;
                audio_player_on_prepared(self, true);
                if (self->is_playing) {
                    audio_player_resume(self);
                }
            }
        } else if (self->is_initialized) {
            self->is_initialized = false;
        }
    }
}

void audio_player_on_prepared(struct audio_player *self, bool value) {
    if (!self->event_subscribed) {
        return;
    }

    // clang-format off
    platch_send_success_event_std(
        self->event_channel_name,
        &STDMAP2(
            STDSTRING("event"), STDSTRING("audio.onPrepared"),
            STDSTRING("value"), STDBOOL(value)
        )
    );
    // clang-format on
}

void audio_player_on_position_update(struct audio_player *self) {
    if (!self->event_subscribed) {
        return;
    }

    // clang-format off
    platch_send_success_event_std(
        self->event_channel_name,
        &STDMAP2(
            STDSTRING("event"), STDSTRING("audio.onCurrentPosition"),
            STDSTRING("value"), STDINT64(audio_player_get_position(self))
        )
    );
    // clang-format on
}

void audio_player_on_duration_update(struct audio_player *self) {
    if (!self->event_subscribed) {
        return;
    }
    // clang-format off
    platch_send_success_event_std(
        self->event_channel_name,
        &STDMAP2(
            STDSTRING("event"), STDSTRING("audio.onDuration"),
            STDSTRING("value"), STDINT64(audio_player_get_duration(self))
        )
    );
    // clang-format on
}
void audio_player_on_seek_completed(struct audio_player *self) {
    audio_player_on_position_update(self);

    if (self->event_subscribed) {
        // clang-format off
        platch_send_success_event_std(
            self->event_channel_name,
            &STDMAP2(
                STDSTRING("event"), STDSTRING("audio.onSeekComplete"),
                STDSTRING("value"), STDBOOL(true)
            )
        );
        // clang-format on
    }
    self->is_seek_completed = true;
}
void audio_player_on_playback_ended(struct audio_player *self) {
    if (self->event_subscribed) {
        // clang-format off
        platch_send_success_event_std(
            self->event_channel_name,
            &STDMAP2(
                STDSTRING("event"), STDSTRING("audio.onComplete"),
                STDSTRING("value"), STDBOOL(true)
            )
        );
        // clang-format on
    }

    if (audio_player_get_looping(self)) {
        audio_player_play(self);
    } else {
        audio_player_pause(self);
        audio_player_set_position(self, 0);
    }
}

void audio_player_set_looping(struct audio_player *self, bool is_looping) {
    self->is_looping = is_looping;
}

bool audio_player_get_looping(struct audio_player *self) {
    return self->is_looping;
}

void audio_player_play(struct audio_player *self) {
    audio_player_set_position(self, 0);
    audio_player_resume(self);
}

void audio_player_pause(struct audio_player *self) {
    self->is_playing = false;

    if (!self->is_initialized) {
        return;
    }

    GstStateChangeReturn ret = gst_element_set_state(self->playbin, GST_STATE_PAUSED);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        LOG_ERROR("Unable to set the pipeline to the paused state.\n");
        return;
    }
    audio_player_on_position_update(self);  // Update to exact position when pausing
}

void audio_player_resume(struct audio_player *self) {
    self->is_playing = true;
    if (!self->is_initialized) {
        return;
    }

    GstStateChangeReturn ret = gst_element_set_state(self->playbin, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        LOG_ERROR("Unable to set the pipeline to the playing state.\n");
        return;
    }
    audio_player_on_position_update(self);
    audio_player_on_duration_update(self);
}

void audio_player_destroy(struct audio_player *self) {
    if (self->is_initialized) {
        audio_player_pause(self);
    }

    if (self->source) {
        gst_object_unref(GST_OBJECT(self->source));
        self->source = NULL;
    }

    gst_object_unref(self->bus);
    self->bus = NULL;

    if (self->panorama != NULL) {
        gst_element_set_state(self->audiobin, GST_STATE_NULL);

        gst_element_remove_pad(self->audiobin, self->panoramaSinkPad);
        gst_bin_remove(GST_BIN(self->audiobin), self->audiosink);
        gst_bin_remove(GST_BIN(self->audiobin), self->panorama);

        self->panorama = NULL;
        self->audiosink = NULL;
        self->panoramaSinkPad = NULL;
        self->audiobin = NULL;
    }

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

    if (self->event_channel_name != NULL) {
        free(self->event_channel_name);
        self->event_channel_name = NULL;;
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

void audio_player_set_balance(struct audio_player *self, double balance) {
    if (!self->panorama) {
        return;
    }

    if (balance > 1.0l) {
        balance = 1.0l;
    } else if (balance < -1.0l) {
        balance = -1.0l;
    }
    g_object_set(G_OBJECT(self->panorama), "panorama", balance, NULL);
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
    ASSERT_NOT_NULL(url);
    if (self->url == NULL || !streq(self->url, url)) {
        LOG_DEBUG("%s: set source=%s\n", self->player_id, url);
        if (self->url != NULL) {
            free(self->url);
            self->url = NULL;
        }
        self->url = strdup(url);
        gst_element_set_state(self->playbin, GST_STATE_NULL);
        self->is_initialized = false;
        self->is_playing = false;

        if (strlen(self->url) != 0) {
            g_object_set(self->playbin, "uri", self->url, NULL);
            if (self->playbin->current_state != GST_STATE_READY) {
                if (gst_element_set_state(self->playbin, GST_STATE_READY) == GST_STATE_CHANGE_FAILURE) {
                    //This should not happen generally
                    LOG_ERROR("Could not set player into ready state.\n");
                }
            }
        }
    } else {
        audio_player_on_prepared(self, true);
    }
}

bool audio_player_is_id(struct audio_player *self, char *player_id) {
    return streq(self->player_id, player_id);
}

const char* audio_player_subscribe_channel_name(const struct audio_player *self) {
    return self->event_channel_name;
}

bool audio_player_set_subscription_status(struct audio_player *self, const char *channel, bool value) {
    if (strcmp(self->event_channel_name, channel) == 0) {
        self->event_subscribed = value;
        return true;
    } else {
        return false;
    }
}

void audio_player_release(struct audio_player *self) {
    self->is_initialized = false;
    self->is_playing = false;
    if (self->url != NULL) {
        free(self->url);
        self->url = NULL;
    }

    GstState playbinState;
    gst_element_get_state(self->playbin, &playbinState, NULL, GST_CLOCK_TIME_NONE);

    if (playbinState > GST_STATE_NULL) {
        gst_element_set_state(self->playbin, GST_STATE_NULL);
    }
}
