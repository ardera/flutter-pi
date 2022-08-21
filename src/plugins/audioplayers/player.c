#include "gst/gst.h"
#include "gst/gstelementfactory.h"
#include "gst/gstmessage.h"
#include "gst/gstsegment.h"
#include "platformchannel.h"
#include <flutter-pi.h>
#include <plugins/audioplayers.h>
#include <stdio.h>

FILE_DESCR("AudioPlayer::player")

// Private Class functions
void AudioPlayer_SourceSetup(GstElement* playbin, GstElement* source, GstElement** p_src);
gboolean AudioPlayer_OnBusMessage(GstBus* bus, GstMessage* message, AudioPlayer* data);
gboolean AudioPlayer_OnRefresh(AudioPlayer* data);

// Private methods
void AudioPlayer_SetPlayback(AudioPlayer* self, int64_t seekTo, double rate);
void AudioPlayer_OnMediaError(AudioPlayer* self, GError* error, gchar* debug);
void AudioPlayer_OnMediaStateChange(AudioPlayer* self, GstObject* src, GstState* old_state, GstState* new_state);
void AudioPlayer_OnPositionUpdate(AudioPlayer* self);
void AudioPlayer_OnDurationUpdate(AudioPlayer* self);
void AudioPlayer_OnSeekCompleted(AudioPlayer* self);
void AudioPlayer_OnPlaybackEnded(AudioPlayer* self);

static int on_bus_fd_ready(sd_event_source* s, int fd, uint32_t revents, void* userdata)
{
    struct AudioPlayer* player = userdata;
    GstMessage* msg;

    (void)s;
    (void)fd;
    (void)revents;

    /* DEBUG_TRACE_BEGIN(player, "on_bus_fd_ready"); */

    msg = gst_bus_pop(player->bus);
    if (msg != NULL) {
        AudioPlayer_OnBusMessage(player->bus, msg, player);
        gst_message_unref(msg);
    }

    /* DEBUG_TRACE_END(player, "on_bus_fd_ready"); */

    return 0;
}

AudioPlayer* AudioPlayer_new(char* playerId, char* channel)
{
    AudioPlayer* self = malloc(sizeof(AudioPlayer));
    if (self == NULL) {
        return NULL;
    }

    self->url = NULL;
    self->_isInitialized = false;
    self->_isLooping = false;
    self->_isSeekCompleted = false;
    self->_playbackRate = 1.0;

    gst_init(NULL, NULL);
    self->playbin = gst_element_factory_make("playbin", "playbin");
    if (!self->playbin) {
        LOG_ERROR("Not all elements could be created");
    }
    g_signal_connect(self->playbin, "source-setup", G_CALLBACK(AudioPlayer_SourceSetup), &self->source);

    self->bus = gst_element_get_bus(self->playbin);

    // Watch bus messages for one time events
    // gst_bus_add_watch(self->bus, (GstBusFunc)AudioPlayer_OnBusMessage, self);

    GPollFD fd;
    sd_event_source* busfd_event_source;
    gst_bus_get_pollfd(self->bus, &fd);

    flutterpi_sd_event_add_io(&busfd_event_source, fd.fd, EPOLLIN, on_bus_fd_ready, self);

    // Refresh continuously to emit reoccuring events
    g_timeout_add(1000, (GSourceFunc)AudioPlayer_OnRefresh, self);

    self->playerId = malloc(strlen(playerId) + 1);
    if (self->playerId == NULL)
        goto deinit_ptr;
    strcpy(self->playerId, playerId);

    self->_channel = malloc(strlen(channel) + 1);
    if (self->_channel == NULL)
        goto deinit_player_id;
    strcpy(self->_channel, channel);

    return self;

    // NOTE: deinit other stuff?

deinit_player_id:
    free(self->playerId);

deinit_ptr:
    free(self);
    return NULL;
}

void AudioPlayer_SourceSetup(GstElement* playbin, GstElement* source, GstElement** p_src)
{
    (void)playbin;
    (void)p_src;
    if (g_object_class_find_property(G_OBJECT_GET_CLASS(source), "ssl-strict") != 0) {
        g_object_set(G_OBJECT(source), "ssl-strict", FALSE, NULL);
    }
}

gboolean AudioPlayer_OnBusMessage(GstBus* bus, GstMessage* message, AudioPlayer* data)
{
    (void)bus;
    switch (GST_MESSAGE_TYPE(message)) {
        case GST_MESSAGE_ERROR: {
            GError* err;
            gchar* debug;

            gst_message_parse_error(message, &err, &debug);
            AudioPlayer_OnMediaError(data, err, debug);
            g_error_free(err);
            g_free(debug);
            break;
        }
        case GST_MESSAGE_STATE_CHANGED: {
            GstState old_state, new_state;

            gst_message_parse_state_changed(message, &old_state, &new_state, NULL);
            AudioPlayer_OnMediaStateChange(data, message->src, &old_state, &new_state);
            break;
        }
        case GST_MESSAGE_EOS:
            gst_element_set_state(data->playbin, GST_STATE_READY);
            AudioPlayer_OnPlaybackEnded(data);
            break;
        case GST_MESSAGE_DURATION_CHANGED:
            AudioPlayer_OnDurationUpdate(data);
            break;
        case GST_MESSAGE_ASYNC_DONE:
            if (!data->_isSeekCompleted) {
                AudioPlayer_OnSeekCompleted(data);
                data->_isSeekCompleted = true;
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

gboolean AudioPlayer_OnRefresh(AudioPlayer* data)
{
    if (data->playbin->current_state == GST_STATE_PLAYING) {
        AudioPlayer_OnPositionUpdate(data);
    }
    return TRUE;
}

void AudioPlayer_SetPlayback(AudioPlayer* self, int64_t seekTo, double rate)
{
    if (!self->_isInitialized) {
        return;
    }
    // See:
    // https://gstreamer.freedesktop.org/documentation/tutorials/basic/playback-speed.html?gi-language=c
    if (!self->_isSeekCompleted) {
        return;
    }
    if (rate == 0) {
        // Do not set rate if it's 0, rather pause.
        AudioPlayer_Pause(self);
        return;
    }

    if (self->_playbackRate != rate) {
        self->_playbackRate = rate;
    }
    self->_isSeekCompleted = false;

    GstSeekFlags seek_flags = GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE;
    GstEvent* seek_event;
    if (rate > 0) {
        seek_event =
          gst_event_new_seek(rate, GST_FORMAT_TIME, seek_flags, GST_SEEK_TYPE_SET, seekTo * GST_MSECOND, GST_SEEK_TYPE_NONE, -1);
    } else {
        seek_event =
          gst_event_new_seek(rate, GST_FORMAT_TIME, seek_flags, GST_SEEK_TYPE_SET, 0, GST_SEEK_TYPE_SET, seekTo * GST_MSECOND);
    }
    if (!gst_element_send_event(self->playbin, seek_event)) {
        // FIXME
        LOG_ERROR("Could not set playback to position %ld and rate %f.", seekTo, rate);
        self->_isSeekCompleted = true;
    }
}
void AudioPlayer_OnMediaError(AudioPlayer* self, GError* error, gchar* debug)
{
    (void)debug;
    char* error_message = malloc(256 * sizeof(char));
    snprintf(error_message, 256, "Error: %d; message=%s", error->code, error->message);
    LOG_ERROR("%s", error_message);
    if (self->_channel) {
        platch_call_std(
          self->_channel,
          "audio.onError",
          &STDMAP2(STDSTRING("playerId"), STDSTRING(self->playerId), STDSTRING("value"), STDSTRING(error_message)),
          NULL,
          NULL);
    }
}

void AudioPlayer_OnMediaStateChange(AudioPlayer* self, GstObject* src, GstState* old_state, GstState* new_state)
{
    (void)old_state;
    if (strcmp(GST_OBJECT_NAME(src), "playbin") == 0) {
        if (*new_state >= GST_STATE_READY) {
            if (!self->_isInitialized) {
                self->_isInitialized = true;
                AudioPlayer_Pause(self); // Need to set to pause state, in order to get duration
            }
        } else if (self->_isInitialized) {
            self->_isInitialized = false;
        }
    }
}
void AudioPlayer_OnPositionUpdate(AudioPlayer* self)
{
    if (self->_channel) {
        platch_call_std(
          self->_channel,
          "audio.onCurrentPosition",
          &STDMAP2(STDSTRING("playerId"), STDSTRING(self->playerId), STDSTRING("value"), STDINT64(AudioPlayer_GetPosition(self))),
          NULL,
          NULL);
    }
}
void AudioPlayer_OnDurationUpdate(AudioPlayer* self)
{
    if (self->_channel) {
        platch_call_std(
          self->_channel,
          "audio.onDuration",
          &STDMAP2(STDSTRING("playerId"), STDSTRING(self->playerId), STDSTRING("value"), STDINT64(AudioPlayer_GetDuration(self))),
          NULL,
          NULL);
    }
}
void AudioPlayer_OnSeekCompleted(AudioPlayer* self)
{
    if (self->_channel) {
        AudioPlayer_OnPositionUpdate(self);
        platch_call_std(
          self->_channel,
          "audio.onSeekComplete",
          &STDMAP2(STDSTRING("playerId"), STDSTRING(self->playerId), STDSTRING("value"), STDBOOL(true)),
          NULL,
          NULL);
    }
}
void AudioPlayer_OnPlaybackEnded(AudioPlayer* self)
{
    AudioPlayer_SetPosition(self, 0);
    if (AudioPlayer_GetLooping(self)) {
        AudioPlayer_Play(self);
    }
    if (self->_channel) {
        platch_call_std(
          self->_channel,
          "audio.onComplete",
          &STDMAP2(STDSTRING("playerId"), STDSTRING(self->playerId), STDSTRING("value"), STDBOOL(true)),
          NULL,
          NULL);
    }
}

void AudioPlayer_SetLooping(AudioPlayer* self, bool isLooping)
{
    self->_isLooping = isLooping;
}

bool AudioPlayer_GetLooping(AudioPlayer* self)
{
    return self->_isLooping;
}

void AudioPlayer_Play(AudioPlayer* self)
{
    if (!self->_isInitialized) {
        return;
    }
    AudioPlayer_SetPosition(self, 0);
    AudioPlayer_Resume(self);
}

void AudioPlayer_Pause(AudioPlayer* self)
{
    GstStateChangeReturn ret = gst_element_set_state(self->playbin, GST_STATE_PAUSED);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        LOG_ERROR("Unable to set the pipeline to the paused state.");
        return;
    }
    AudioPlayer_OnPositionUpdate(self); // Update to exact position when pausing
}

void AudioPlayer_Resume(AudioPlayer* self)
{
    if (!self->_isInitialized) {
        return;
    }
    GstStateChangeReturn ret = gst_element_set_state(self->playbin, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        LOG_ERROR("Unable to set the pipeline to the playing state.");
        return;
    }
    AudioPlayer_OnDurationUpdate(self); // Update duration when start playing, as no event is emitted elsewhere
}

void AudioPlayer_Dispose(AudioPlayer* self)
{
    if (self->_isInitialized) {
        AudioPlayer_Pause(self);
    }
    gst_object_unref(self->bus);
    gst_object_unref(self->source);

    gst_element_set_state(self->playbin, GST_STATE_NULL);
    gst_object_unref(self->playbin);

    self->_isInitialized = false;

    if (self->url != NULL) {
        free(self->url);
        self->url = NULL;
    }

    if (self->playerId != NULL) {
        free(self->playerId);
        self->playerId = NULL;
    }

    if (self->_channel != NULL) {
        free(self->_channel);
        self->_channel = NULL;
    }
}

int64_t AudioPlayer_GetPosition(AudioPlayer* self)
{
    gint64 current = 0;
    if (!gst_element_query_position(self->playbin, GST_FORMAT_TIME, &current)) {
        LOG_ERROR("Could not query current position.");
        return 0;
    }
    return current / 1000000;
}

int64_t AudioPlayer_GetDuration(AudioPlayer* self)
{
    gint64 duration = 0;
    if (!gst_element_query_duration(self->playbin, GST_FORMAT_TIME, &duration)) {
        LOG_ERROR("Could not query current duration.");
        return 0;
    }
    return duration / 1000000;
}

void AudioPlayer_SetVolume(AudioPlayer* self, double volume)
{
    if (volume > 1) {
        volume = 1;
    } else if (volume < 0) {
        volume = 0;
    }
    g_object_set(G_OBJECT(self->playbin), "volume", volume, NULL);
}

void AudioPlayer_SetPlaybackRate(AudioPlayer* self, double rate)
{
    AudioPlayer_SetPlayback(self, AudioPlayer_GetPosition(self), rate);
}

void AudioPlayer_SetPosition(AudioPlayer* self, int64_t position)
{
    if (!self->_isInitialized) {
        return;
    }
    AudioPlayer_SetPlayback(self, position, self->_playbackRate);
}

void AudioPlayer_SetSourceUrl(AudioPlayer* self, char* url)
{
    if (self->url == NULL || strcmp(self->url, url)) {
        if (self->url != NULL) {
            free(self->url);
            self->url = NULL;
        }
        self->url = malloc(strlen(url) + 1);
        strcpy(self->url, url);
        gst_element_set_state(self->playbin, GST_STATE_NULL);
        if (strlen(self->url) != 0) {
            g_object_set(self->playbin, "uri", self->url, NULL);
            if (self->playbin->current_state != GST_STATE_READY) {
                gst_element_set_state(self->playbin, GST_STATE_READY);
            }
        }
        self->_isInitialized = false;
    }
}
