#include <android_auto.h>
#include <aasdk/ControlMessageIdsEnum.pb-c.h>
#include <aasdk/AVChannelMessageIdsEnum.pb-c.h>
#include <aasdk/InputChannelMessageIdsEnum.pb-c.h>
#include <aasdk/SensorChannelMessageIdsEnum.pb-c.h>

struct aa_channel *aa_channel_new(struct aa_device *device) {
    struct aa_channel *ch;

    ch = malloc(sizeof *ch);
    if (ch == NULL) {
        return NULL;
    }

    ch->device = device;
    ch->destroy_callback = NULL;
    ch->message_callback = NULL;
    ch->userdata = NULL;

    return ch;
}

void aa_channel_destroy(struct aa_channel *channel) {
    if (channel->destroy_callback != NULL) {
        channel->destroy_callback(channel);
    }
    free(channel);
}

int aa_channel_on_message(struct aa_channel *channel, const struct aa_msg *msg) {
    if (channel->message_callback) {
        return channel->message_callback(channel, msg);
    }
    return 0;
}

int aa_channel_fill_features(struct aa_channel *channel, F1x__Aasdk__Proto__Data__ChannelDescriptor *desc) {
    if (channel->fill_features_callback != NULL) {
        return channel->fill_features_callback(channel, desc);
    }
    return 0;
}

void aa_channel_after_fill_features(struct aa_channel *channel, F1x__Aasdk__Proto__Data__ChannelDescriptor *desc) {
    if (channel->fill_features_callback != NULL) {
        return channel->after_fill_features_callback(channel, desc);
    }
}


int aa_video_channel_on_message(struct aa_channel *channel, const struct aa_msg *msg) {
    uint16_t message_id;
    uint8_t *payload;
    int ok;

    message_id = be16_to_cpu(*(uint16_t*) msg->payload.pointer);
    payload = msg->payload.pointer + 2;

    switch (message_id) {
        case F1X__AASDK__PROTO__IDS__INPUT_CHANNEL_MESSAGE__ENUM__BINDING_REQUEST:
        break;
        case F1X__AASDK__PROTO__IDS__CONTROL_MESSAGE__ENUM__CHANNEL_OPEN_REQUEST:
        break;
        default:
        return EINVAL;
    }

    return 0;
}

int aa_input_channel_fill_features(struct aa_channel *channel, F1x__Aasdk__Proto__Data__ChannelDescriptor *desc) {
    desc->channel_id = channel->id;
    
    F1x__Aasdk__Proto__Data__InputChannel *input_channel = malloc(sizeof(F1x__Aasdk__Proto__Data__InputChannel));
    f1x__aasdk__proto__data__input_channel__init(input_channel);

    F1x__Aasdk__Proto__Data__TouchConfig *touch_config = malloc(sizeof(F1x__Aasdk__Proto__Data__TouchConfig));
    f1x__aasdk__proto__data__touch_config__init(touch_config); 

    touch_config->width = UINT32_MAX;
    touch_config->height = UINT32_MAX;

    input_channel->touch_screen_config = touch_config;

    desc->input_channel = input_channel;

    return 0;
}

void aa_input_channel_after_fill_features(struct aa_channel *channel, F1x__Aasdk__Proto__Data__ChannelDescriptor *desc) {
    free(desc->input_channel->touch_screen_config);
    free(desc->input_channel);
}

struct aa_channel *aa_input_channel_new(struct aa_device *device) {
    struct aa_channel *channel = aa_channel_new(device);
    if (channel == NULL) {
        return NULL;
    }

    channel->fill_features_callback = aa_input_channel_fill_features;
}


int aa_video_channel_on_message(struct aa_channel *channel, const struct aa_msg *msg) {
    uint16_t message_id;
    uint8_t *payload;
    int ok;

    message_id = be16_to_cpu(*(uint16_t*) msg->payload.pointer);
    payload = msg->payload.pointer + 2;

    switch (message_id) {
        case F1X__AASDK__PROTO__IDS__AVCHANNEL_MESSAGE__ENUM__SETUP_REQUEST:
        break;
        case F1X__AASDK__PROTO__IDS__AVCHANNEL_MESSAGE__ENUM__START_INDICATION:
        break;
        case F1X__AASDK__PROTO__IDS__AVCHANNEL_MESSAGE__ENUM__STOP_INDICATION:
        break;
        case F1X__AASDK__PROTO__IDS__AVCHANNEL_MESSAGE__ENUM__AV_MEDIA_WITH_TIMESTAMP_INDICATION:
        break;
        case F1X__AASDK__PROTO__IDS__AVCHANNEL_MESSAGE__ENUM__AV_MEDIA_INDICATION:
        break;
        case F1X__AASDK__PROTO__IDS__AVCHANNEL_MESSAGE__ENUM__VIDEO_FOCUS_REQUEST:
        break;
        case F1X__AASDK__PROTO__IDS__CONTROL_MESSAGE__ENUM__CHANNEL_OPEN_REQUEST:
        break;
        default:
        return EINVAL;
    }

    return 0;
}

int aa_video_channel_fill_features(struct aa_channel *channel, F1x__Aasdk__Proto__Data__ChannelDescriptor *desc) {
    int ok;

    F1x__Aasdk__Proto__Data__AVChannel *avchannel = malloc(sizeof(F1x__Aasdk__Proto__Data__AVChannel));
    if (avchannel == NULL) {
        return ENOMEM;
    }
    f1x__aasdk__proto__data__avchannel__init(avchannel);

    F1x__Aasdk__Proto__Data__VideoConfig *video_config = malloc(sizeof(F1x__Aasdk__Proto__Data__VideoConfig));
    if (video_config == NULL) {
        free(avchannel);
        return ENOMEM;
    }
    f1x__aasdk__proto__data__video_config__init(video_config);

    F1x__Aasdk__Proto__Data__AudioConfig **pvideo_configs = malloc(sizeof(F1x__Aasdk__Proto__Data__VideoConfig *));
    if (pvideo_configs == NULL) {
        free(avchannel);
        free(video_config);
        return ENOMEM;
    }
    pvideo_configs[0] = video_config;

    avchannel->stream_type = F1X__AASDK__PROTO__ENUMS__AVSTREAM_TYPE__ENUM__VIDEO;
    avchannel->has_available_while_in_call = true;
    avchannel->available_while_in_call = true;

    video_config->video_resolution = F1X__AASDK__PROTO__ENUMS__VIDEO_RESOLUTION__ENUM___480p;
    video_config->video_fps = F1X__AASDK__PROTO__ENUMS__VIDEO_FPS__ENUM___60;

    avchannel->n_video_configs = 1;
    avchannel->video_configs = &pvideo_configs;

    desc->av_channel = avchannel;

    return 0;
}

void aa_video_channel_after_fill_features(struct aa_channel *channel, F1x__Aasdk__Proto__Data__ChannelDescriptor *desc) {
    free(desc->av_channel->video_configs[0]);
    free(desc->av_channel->video_configs);
    free(desc->av_channel);
}

void aa_video_channel_destroy(struct aa_channel *channel) {

}

struct aa_channel *aa_video_channel_new(struct aa_device *device) {
    struct aa_channel *channel = aa_channel_new(device);
    if (channel == NULL) {
        return NULL;
    }

    channel->id = kAndroidAutoChannelVideo;
    channel->message_callback = aa_video_channel_on_message;
    channel->fill_features_callback = aa_video_channel_fill_features;
    channel->after_fill_features_callback = aa_video_channel_after_fill_features;
    channel->destroy_callback = aa_video_channel_destroy;

    return channel;
}


int aa_sensor_channel_on_message(struct aa_channel *channel, const struct aa_msg *msg) {
    uint16_t message_id;
    uint8_t *payload;
    int ok;

    message_id = be16_to_cpu(*(uint16_t*) msg->payload.pointer);
    payload = msg->payload.pointer + 2;

    switch (message_id) {
        case F1X__AASDK__PROTO__IDS__SENSOR_CHANNEL_MESSAGE__ENUM__SENSOR_START_REQUEST:
        break;
        case F1X__AASDK__PROTO__IDS__CONTROL_MESSAGE__ENUM__CHANNEL_OPEN_REQUEST:
        break;
        default:
        return EINVAL;
    }

    return 0;
}

int aa_sensor_channel_fill_features(struct aa_channel *channel, F1x__Aasdk__Proto__Data__ChannelDescriptor *desc) {
    int ok;
    return 0;
}

void aa_sensor_channel_after_fill_features(struct aa_channel *channel, F1x__Aasdk__Proto__Data__ChannelDescriptor *desc) {
    
}

void aa_sensor_channel_destroy(struct aa_channel *channel) {

}

struct aa_channel *aa_sensor_channel_new(struct aa_device *device) {
    struct aa_channel *channel = aa_channel_new(device);
    if (channel == NULL) {
        return NULL;
    }

    channel->id = kAndroidAutoChannelSensor;
    channel->message_callback = aa_sensor_channel_on_message;
    channel->fill_features_callback = aa_sensor_channel_fill_features;
    channel->after_fill_features_callback = aa_sensor_channel_after_fill_features;
    channel->destroy_callback = aa_sensor_channel_destroy;

    return channel;
}


int aa_audio_channel_on_message(struct aa_channel *channel, const struct aa_msg *msg) {
    uint16_t message_id;
    uint8_t *payload;
    int ok;

    message_id = be16_to_cpu(*(uint16_t*) msg->payload.pointer);
    payload = msg->payload.pointer + 2;

    switch (message_id) {
        case F1X__AASDK__PROTO__IDS__AVCHANNEL_MESSAGE__ENUM__SETUP_REQUEST:
        break;
        case F1X__AASDK__PROTO__IDS__AVCHANNEL_MESSAGE__ENUM__START_INDICATION:
        break;
        case F1X__AASDK__PROTO__IDS__AVCHANNEL_MESSAGE__ENUM__STOP_INDICATION:
        break;
        case F1X__AASDK__PROTO__IDS__AVCHANNEL_MESSAGE__ENUM__AV_MEDIA_WITH_TIMESTAMP_INDICATION:
        break;
        case F1X__AASDK__PROTO__IDS__AVCHANNEL_MESSAGE__ENUM__AV_MEDIA_INDICATION:
        break;
        case F1X__AASDK__PROTO__IDS__CONTROL_MESSAGE__ENUM__CHANNEL_OPEN_REQUEST:
        break;
        default:
        return EINVAL;
    }

    return 0;

    return 0;
}

int aa_audio_channel_fill_features(struct aa_channel *channel, F1x__Aasdk__Proto__Data__ChannelDescriptor *desc) {
    int ok;

    F1x__Aasdk__Proto__Data__AVChannel *av_channel = malloc(sizeof(F1x__Aasdk__Proto__Data__AVChannel));
    if (av_channel == NULL) {
        return ENOMEM;
    }
    f1x__aasdk__proto__data__avchannel__init(av_channel);

    F1x__Aasdk__Proto__Data__AudioConfig *audio_config = malloc(sizeof(F1x__Aasdk__Proto__Data__AudioConfig));
    if (audio_config == NULL) {
        free(av_channel);
        return ENOMEM;
    }
    f1x__aasdk__proto__data__audio_config__init(audio_config);

    audio_config->sample_rate = 0;
    audio_config->bit_depth = 0;
    audio_config->channel_count = 0;

    av_channel->stream_type = F1X__AASDK__PROTO__ENUMS__AVSTREAM_TYPE__ENUM__AUDIO;
    av_channel->has_available_while_in_call = true;
    av_channel->available_while_in_call = true;    

    desc->av_channel = av_channel;

    return 0;
}

void aa_audio_channel_after_fill_features(struct aa_channel *channel, F1x__Aasdk__Proto__Data__ChannelDescriptor *desc) {
    
}

void aa_audio_channel_destroy(struct aa_channel *channel) {

}

struct aa_channel *aa_audio_channel_new(struct aa_device *device) {
    struct aa_channel *channel = aa_channel_new(device);
    if (channel == NULL) {
        return NULL;
    }

    channel->message_callback = aa_audio_channel_on_message;
    channel->fill_features_callback = aa_audio_channel_fill_features;
    channel->after_fill_features_callback = aa_audio_channel_after_fill_features;
    channel->destroy_callback = aa_audio_channel_destroy;

    return channel;
}
