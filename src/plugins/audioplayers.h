#ifndef AUDIOPLAYERS_H_
#define AUDIOPLAYERS_H_

#include <stdbool.h>
#include <stdint.h>

struct audio_player;

struct audio_player *audio_player_new(char *playerId, char *channel);

// Instance function

int64_t audio_player_get_position(struct audio_player *self);

int64_t audio_player_get_duration(struct audio_player *self);

bool audio_player_get_looping(struct audio_player *self);

void audio_player_play(struct audio_player *self);

void audio_player_pause(struct audio_player *self);

void audio_player_resume(struct audio_player *self);

void audio_player_destroy(struct audio_player *self);

void audio_player_set_looping(struct audio_player *self, bool isLooping);

void audio_player_set_volume(struct audio_player *self, double volume);

void audio_player_set_playback_rate(struct audio_player *self, double rate);

void audio_player_set_balance(struct audio_player *self, double balance);

void audio_player_set_position(struct audio_player *self, int64_t position);

void audio_player_set_source_url(struct audio_player *self, char *url);

bool audio_player_is_id(struct audio_player *self, char *id);

const char* audio_player_subscribe_channel_name(const struct audio_player *self);

///Asks to subscribe to channel events
///
///`value` - Indicates whether to subscribe or unsubscribe
///
///Returns `true` if player uses `channel`, otherwise returns `false
bool audio_player_set_subscription_status(struct audio_player *self, const char *channel, bool value);

void audio_player_release(struct audio_player *self);

#endif  // AUDIOPLAYERS_H_
