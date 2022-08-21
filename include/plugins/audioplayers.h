#ifndef AUDIOPLAYERS_H_
#define AUDIOPLAYERS_H_

#include <gst/gst.h>
#include <stdbool.h>

typedef struct AudioPlayer
{
    GstElement* playbin;
    GstElement* source;
    GstBus* bus;

    bool _isInitialized;
    bool _isLooping;
    bool _isSeekCompleted;
    double _playbackRate;

    char* url;
    char* playerId;
    /* FlMethodChannel* _channel; */
    char* _channel;

} AudioPlayer;

AudioPlayer* AudioPlayer_new(char* playerId, char* channel);

// Instance function

int64_t AudioPlayer_GetPosition(AudioPlayer* self);

int64_t AudioPlayer_GetDuration(AudioPlayer* self);

bool AudioPlayer_GetLooping(AudioPlayer* self);

void AudioPlayer_Play(AudioPlayer* self);

void AudioPlayer_Pause(AudioPlayer* self);

void AudioPlayer_Resume(AudioPlayer* self);

void AudioPlayer_Dispose(AudioPlayer* self);

void AudioPlayer_SetLooping(AudioPlayer* self, bool isLooping);

void AudioPlayer_SetVolume(AudioPlayer* self, double volume);

void AudioPlayer_SetPlaybackRate(AudioPlayer* self, double rate);

void AudioPlayer_SetPosition(AudioPlayer* self, int64_t position);

void AudioPlayer_SetSourceUrl(AudioPlayer* self, char* url);

#endif // AUDIOPLAYERS_H_
