#define _GNU_SOURCE
#include <alloca.h>
#include <semaphore.h>
#include <unistd.h>
#include <string.h>
#include <memory.h>

#include <gbm.h>
#include <flutter-pi.h>
#include <texture_registry.h>
#include <plugins/android_auto/android_auto.h>
#include <aasdk/ControlMessageIdsEnum.pb-c.h>
#include <aasdk/DrivingStatusEnum.pb-c.h>
#include <aasdk/ChannelOpenRequestMessage.pb-c.h>
#include <aasdk/ChannelOpenResponseMessage.pb-c.h>
#include <aasdk/SensorStartRequestMessage.pb-c.h>
#include <aasdk/SensorStartResponseMessage.pb-c.h>
#include <aasdk/SensorEventIndicationMessage.pb-c.h>
#include <aasdk/AVChannelMessageIdsEnum.pb-c.h>
#include <aasdk/InputChannelMessageIdsEnum.pb-c.h>
#include <aasdk/SensorChannelMessageIdsEnum.pb-c.h>
#include <aasdk/AVChannelSetupRequestMessage.pb-c.h>
#include <aasdk/AVChannelSetupResponseMessage.pb-c.h>
#include <aasdk/AVChannelStartIndicationMessage.pb-c.h>
#include <aasdk/AVChannelStopIndicationMessage.pb-c.h>
#include <aasdk/AVMediaAckIndicationMessage.pb-c.h>
#include <aasdk/AudioFocusRequestMessage.pb-c.h>
#include <aasdk/AudioFocusResponseMessage.pb-c.h>
#include <aasdk/VideoFocusRequestMessage.pb-c.h>
#include <aasdk/AVInputOpenRequestMessage.pb-c.h>
#include <aasdk/AVInputOpenResponseMessage.pb-c.h>
#include <aasdk/BindingRequestMessage.pb-c.h>
#include <aasdk/BindingResponseMessage.pb-c.h>
#include <aasdk/VideoFocusIndicationMessage.pb-c.h>

struct egl_context_creation_data {
    EGLContext context;
    EGLint err;
    sem_t created;
};

static struct aa_channel *aa_channel_new(struct aa_device *device) {
    struct aa_channel *ch;

    ch = malloc(sizeof *ch);
    if (ch == NULL) {
        return NULL;
    }

    ch->device = device;
    ch->destroy_callback = NULL;
    ch->channel_open_request_callback = NULL;
    ch->message_callback = NULL;
    ch->userdata = NULL;
    ch->debug_channel_name = NULL;

    return ch;
}

void aa_channel_destroy(struct aa_channel *channel) {
    if (channel->destroy_callback != NULL) {
        channel->destroy_callback(channel);
    }
    free(channel);
}

static int aa_channel_on_channel_open_request_msg(struct aa_channel *channel, size_t payload_size, uint8_t *payload) {
    F1x__Aasdk__Proto__Messages__ChannelOpenRequest *open_request;
    size_t open_response_packed_size;
    int32_t channel_id, priority;
    int ok;

    /**
     * Read the request.
     */
    open_request = f1x__aasdk__proto__messages__channel_open_request__unpack(NULL, payload_size, payload);
    if (open_request == NULL) {
        fprintf(stderr, "[android-auto plugin] [_ channel] Could not unpack channel open request.\n");
        return EINVAL;
    }

    printf("[android-auto plugin] [%s channel] channel open request. priority: %d.\n", channel->debug_channel_name, open_request->priority);

    channel_id = open_request->channel_id;
    priority = open_request->priority;

    f1x__aasdk__proto__messages__channel_open_request__free_unpacked(open_request, NULL);


    /**
     * Call the channel open request callback.
     */
    ok = 0;
    if (channel->channel_open_request_callback != NULL) {
        ok = channel->channel_open_request_callback(channel, channel_id, priority);
    }

    /**
     * Send the response.
     */
    F1x__Aasdk__Proto__Messages__ChannelOpenResponse open_response = F1X__AASDK__PROTO__MESSAGES__CHANNEL_OPEN_RESPONSE__INIT;
    open_response.status = ok == 0 ? F1X__AASDK__PROTO__ENUMS__STATUS__ENUM__OK : F1X__AASDK__PROTO__ENUMS__STATUS__ENUM__FAIL;

    open_response_packed_size = f1x__aasdk__proto__messages__channel_open_response__get_packed_size(&open_response);

    define_and_setup_aa_msg_on_stack(open_response_msg, open_response_packed_size + 2, channel->id, AA_MSG_FLAG_ENCRYPTED | AA_MSG_FLAG_CONTROL);

    *(uint16_t *) open_response_msg.payload->pointer = cpu_to_be16(F1X__AASDK__PROTO__IDS__CONTROL_MESSAGE__ENUM__CHANNEL_OPEN_RESPONSE);
    f1x__aasdk__proto__messages__channel_open_response__pack(&open_response, open_response_msg.payload->pointer + 2);

    ok = aa_device_send(channel->device, &open_response_msg);
    if (ok != 0) {
        return ok;
    }

    return 0;
}

int aa_channel_on_message(struct aa_channel *channel, struct aa_msg *msg) {
    int ok;
    if (channel->message_callback) {
        ok = channel->message_callback(channel, aa_msg_ref(msg));
    }

    aa_msg_unrefp(&msg);

    return ok;
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


static int aa_input_channel_on_binding_request(struct aa_channel *channel, size_t payload_size, uint8_t *payload) {
    F1x__Aasdk__Proto__Messages__BindingResponse response = F1X__AASDK__PROTO__MESSAGES__BINDING_RESPONSE__INIT;
    F1x__Aasdk__Proto__Messages__BindingRequest *request;
    size_t response_packed_size;

    request = f1x__aasdk__proto__messages__binding_request__unpack(NULL, payload_size, payload);
    if (request == NULL) {
        fprintf(stderr, "[android-auto plugin] [input channel] Could not unpack binding request.\n");
        return EPROTO;
    }

    printf("[android-auto plugin] [input channel] input channel binding request. n_scan_codes: %u, scan_codes = {", request->n_scan_codes);
    for (unsigned int i = 0; i < request->n_scan_codes; i++) {
        if (i != 0) {
            printf(", ");
        }

        printf("%d", request->scan_codes[i]);
    }
    printf("}\n");

    response.status = F1X__AASDK__PROTO__ENUMS__STATUS__ENUM__OK;
    if (request->n_scan_codes > 0) {
        fprintf(stderr, "[android-auto plugin] [input channel] Some scan codes in the binding request are not supported.\n");
        response.status = F1X__AASDK__PROTO__ENUMS__STATUS__ENUM__FAIL;
    }

    f1x__aasdk__proto__messages__binding_request__free_unpacked(request, NULL);

    response_packed_size = f1x__aasdk__proto__messages__binding_response__get_packed_size(&response);
    define_and_setup_aa_msg_on_stack(response_msg, response_packed_size + 2, channel->id, AA_MSG_FLAG_ENCRYPTED)
    *(uint16_t*) response_msg.payload->pointer = cpu_to_be16(F1X__AASDK__PROTO__IDS__INPUT_CHANNEL_MESSAGE__ENUM__BINDING_RESPONSE);
    f1x__aasdk__proto__messages__binding_response__pack(&response, response_msg.payload->pointer + 2);
    
    return aa_device_send(channel->device, &response_msg);
}

static int aa_input_channel_on_message(struct aa_channel *channel, struct aa_msg *msg) {
    uint16_t message_id;
    uint8_t *payload;
    size_t payload_size;
    int ok;

    message_id = be16_to_cpu(*(uint16_t*) msg->payload->pointer);
    payload = msg->payload->pointer + 2;
    payload_size = msg->payload->size - 2;

    switch (message_id) {
        case F1X__AASDK__PROTO__IDS__INPUT_CHANNEL_MESSAGE__ENUM__BINDING_REQUEST:
            ok = aa_input_channel_on_binding_request(channel, payload_size, payload);

            break;
        case F1X__AASDK__PROTO__IDS__CONTROL_MESSAGE__ENUM__CHANNEL_OPEN_REQUEST:
            ok = aa_channel_on_channel_open_request_msg(channel, payload_size, payload);
            break;
        default:
            ok = EINVAL;
            break;
    }

    aa_msg_unrefp(&msg);
    return ok;
}

static int aa_input_channel_fill_features(struct aa_channel *channel, F1x__Aasdk__Proto__Data__ChannelDescriptor *desc) {
    desc->channel_id = channel->id;
    
    F1x__Aasdk__Proto__Data__InputChannel *input_channel = malloc(sizeof(F1x__Aasdk__Proto__Data__InputChannel));
    if (input_channel == NULL) {
        return ENOMEM;
    }

    F1x__Aasdk__Proto__Data__TouchConfig *touch_config = malloc(sizeof(F1x__Aasdk__Proto__Data__TouchConfig));
    if (touch_config == NULL) {
        free(input_channel);
        return ENOMEM;
    }
    
    f1x__aasdk__proto__data__input_channel__init(input_channel);
    f1x__aasdk__proto__data__touch_config__init(touch_config); 

    touch_config->width = 800; //UINT32_MAX;
    touch_config->height = 480; //UINT32_MAX;

    input_channel->touch_screen_config = touch_config;

    desc->channel_id = channel->id;
    desc->input_channel = input_channel;

    return 0;
}

static void aa_input_channel_after_fill_features(struct aa_channel *channel, F1x__Aasdk__Proto__Data__ChannelDescriptor *desc) {
    free(desc->input_channel->touch_screen_config);
    free(desc->input_channel);
}

struct aa_channel *aa_input_channel_new(struct aa_device *device) {
    struct aa_channel *channel = aa_channel_new(device);
    if (channel == NULL) {
        return NULL;
    }

    channel->id = kAndroidAutoChannelInput;
    channel->message_callback = aa_input_channel_on_message;
    channel->fill_features_callback = aa_input_channel_fill_features;
    channel->after_fill_features_callback = aa_input_channel_after_fill_features;
    channel->debug_channel_name = "input";
    
    return channel;
}


static int on_execute_create_vout_context(void *userdata) {
    struct egl_context_creation_data *creation_data;

    creation_data = userdata;

    flutterpi_create_egl_context(&creation_data->context, &creation_data->err);

    sem_post(&creation_data->created);

    return 0;
}


struct dmabuf_texture {
    /**
     * @brief The EGL display behind this image.
     */
    EGLDisplay display;

    /**
     * @brief the backend EGL Image
     */
    EGLImageKHR egl_image;

    /**
     * @brief the name of the OpenGL texture
     */
    GLuint gl_texture;

    /**
     * @brief the width & height of the texture / underlying buffer
     */
    int32_t width, height;

    /**
     * @brief number of planes for the EGL Image
     */
    size_t n_planes;

    /**
     * @brief DRM FOURCC code
     */
    uint32_t format;

    /**
     * @brief the @ref format converted to a GL texture format.
     */
    GLenum gl_format;

    /**
     * @brief The dmabuf file descriptors for each plane
     */
    int plane_fds[3];
    int plane_offsets[3];
    int plane_strides[3];
};

struct video_channel_flutter_texture_frame_destruction_data {
    struct dmabuf_texture *texture;
    struct aa_channel *channel;
};

static struct dmabuf_texture *dmabuf_texture_new(
    EGLDisplay display,
    EGLContext context,
    int32_t width,
    int32_t height,
    uint32_t format,
    size_t n_planes,
    const int plane_fds[3],
    const int plane_offsets[3],
    const int plane_strides[3]
) {
    struct dmabuf_texture *t;
    EGLImage image;
    GLuint texture_id;
    EGLint egl_error;
    GLenum gl_error;

    t = malloc(sizeof *t);
    if (t == NULL) {
        return NULL;
    }

    eglGetError();
    glGetError();

    EGLint attr[] = {
        EGL_WIDTH, width,
        EGL_HEIGHT, height,
        EGL_LINUX_DRM_FOURCC_EXT, format,
        /* EGL_YUV_COLOR_SPACE_HINT_EXT (one of EGL_ITU_REC601_EXT, EGL_ITU_REC709_EXT, EGL_ITU_REC2020_EXT) */
        /* EGL_SAMPLE_RANGE_HINT_EXT (one of EGL_YUV_FULL_RANGE_EXT, EGL_YUV_NARROW_RANGE_EXT) */
        /* EGL_YUV_CHROMA_HORIZONTAL_SITING_HINT_EXT (one of EGL_YUV_CHROMA_SITING_0_EXT, EGL_YUV_CHROMA_SITING_0_5_EXT) */
        /* EGL_YUV_CHROMA_VERTICAL_SITING_HINT_EXT (one of EGL_YUV_CHROMA_SITING_0_EXT, EGL_YUV_CHROMA_SITING_0_5_EXT) */
        EGL_DMA_BUF_PLANE0_FD_EXT, n_planes >= 1 ? plane_fds[0] : 0,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, n_planes >= 1 ? plane_offsets[0] : 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, n_planes >= 1 ? plane_strides[0] : 0,
        EGL_DMA_BUF_PLANE1_FD_EXT, n_planes >= 2 ? plane_fds[1] : 0,
        EGL_DMA_BUF_PLANE1_OFFSET_EXT, n_planes >= 2 ? plane_offsets[1] : 0,
        EGL_DMA_BUF_PLANE1_PITCH_EXT, n_planes >= 2 ? plane_strides[1] : 0,
        EGL_DMA_BUF_PLANE2_FD_EXT, n_planes >= 2 ? plane_fds[2] : 0,
        EGL_DMA_BUF_PLANE2_OFFSET_EXT, n_planes >= 2 ? plane_offsets[2] : 0,
        EGL_DMA_BUF_PLANE2_PITCH_EXT, n_planes >= 2 ? plane_strides[2] : 0,
        EGL_NONE
    };

    attr[6 + 6*n_planes] = EGL_NONE;

    image = eglCreateImage(
        display,
        EGL_NO_CONTEXT,
        EGL_LINUX_DMA_BUF_EXT,
        NULL,
        attr
    );
    if ((egl_error = eglGetError()) != EGL_SUCCESS) {
        fprintf(stderr, "[android-auto plugin] [video service] Could not create EGL Image for displaying video. eglCreateImage: %d\n", egl_error);
        goto fail_free_texture;
    }

    texture_id = 0;

    if (context != EGL_NO_CONTEXT) {
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context);
        if ((egl_error = eglGetError()) != EGL_SUCCESS) {
            goto fail_destroy_image;
        }
    }

    glGenTextures(1, &texture_id);
    if ((gl_error = glGetError()) != GL_NO_ERROR) {
        fprintf(stderr, "[android-auto plugin] [video service] Could not create OpenGL Texture for displaying video. glGenTextures: %d\n", gl_error);
        goto fail_clear_current;
    }

    glActiveTexture(GL_TEXTURE0);
    if ((gl_error = glGetError()) != GL_NO_ERROR) {
        fprintf(stderr, "[android-auto plugin] [video service] Could not bind EGLImage to OpenGL Texture for displaying video. EGLImageTargetTexture2DOES: %d\n", gl_error);
        goto fail_delete_texture;;
    }

    glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture_id);
    if ((gl_error = glGetError()) != GL_NO_ERROR) {
        fprintf(stderr, "[android-auto plugin] [video service] Could not bind OpenGL Texture for displaying video. glBindTexture: %d\n", gl_error);
        goto fail_delete_texture;
    }

    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    if ((gl_error = glGetError()) != GL_NO_ERROR) {
        fprintf(stderr, "[android-auto plugin] [video service] Could not bind setup video output texture parameters. glTexParameteri: %d\n", gl_error);
        goto fail_unbind_texture;
    }

    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    if ((gl_error = glGetError()) != GL_NO_ERROR) {
        fprintf(stderr, "[android-auto plugin] [video service] Could not bind setup video output texture parameters. glTexParameteri: %d\n", gl_error);
        goto fail_unbind_texture;
    }

    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    if ((gl_error = glGetError()) != GL_NO_ERROR) {
        fprintf(stderr, "[android-auto plugin] [video service] Could not bind setup video output texture parameters. glTexParameteri: %d\n", gl_error);
        goto fail_unbind_texture;
    }

    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if ((gl_error = glGetError()) != GL_NO_ERROR) {
        fprintf(stderr, "[android-auto plugin] [video service] Could not bind setup video output texture parameters. glTexParameteri: %d\n", gl_error);
        goto fail_unbind_texture;
    }

    flutterpi.gl.EGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, (GLeglImageOES) image);
    if ((gl_error = glGetError()) != GL_NO_ERROR) {
        fprintf(stderr, "[android-auto plugin] [video service] Could not bind EGLImage to OpenGL Texture for displaying video. EGLImageTargetTexture2DOES: %d\n", gl_error);
        goto fail_unbind_texture;
    }

    glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
    if ((gl_error = glGetError()) != GL_NO_ERROR) {
        fprintf(stderr, "[android-auto plugin] [video service] Could not unbind OpenGL Texture. glBindTexture: %d\n", gl_error);
        goto fail_delete_texture;
    }

    if (context != NULL) {
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }

    t->display = display;
    t->egl_image = image;
    t->gl_texture = texture_id;
    t->width = width;
    t->height = height;
    t->format = format;
    t->n_planes = n_planes;
    memcpy(t->plane_fds, plane_fds, sizeof(t->plane_fds));
    memcpy(t->plane_offsets, plane_offsets, sizeof(t->plane_offsets));
    memcpy(t->plane_strides, plane_strides, sizeof(t->plane_strides));

    return t;


    fail_unbind_texture:
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);

    fail_delete_texture:
    glDeleteTextures(1, &texture_id);

    fail_clear_current:
    if (context != NULL) {
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }

    fail_destroy_image:
    eglDestroyImage(display, image);

    fail_free_texture:
    free(t);

    fail_return_null:
    return NULL;
}

static bool dmabuf_texture_can_update_to(
    struct dmabuf_texture *texture,
    int32_t width,
    int32_t height,
    uint32_t format,
    size_t n_planes,
    int plane_fds[3],
    int plane_offsets[3],
    int plane_strides[3]
) {
    return false &&
        (width == texture->width) &&
        (height == texture->height) && 
        (format == texture->format) &&
        (n_planes == texture->n_planes) &&
        (memcmp(plane_fds, texture->plane_fds, sizeof(texture->plane_fds)) == 0) &&
        (memcmp(plane_offsets, texture->plane_offsets, sizeof(texture->plane_offsets)) == 0) &&
        (memcmp(plane_strides, texture->plane_strides, sizeof(texture->plane_strides)) == 0);
}

static int dmabuf_texture_update_to(
    struct dmabuf_texture *texture,
    int32_t width,
    int32_t height,
    uint32_t format,
    size_t n_planes,
    int plane_fds[3],
    int plane_offsets[3],
    int plane_strides[3]
) {
    return EINVAL;
}

static void dmabuf_texture_destroy(struct dmabuf_texture *texture, EGLContext context, bool close_fds) {
    if (context != EGL_NO_CONTEXT) {
        eglMakeCurrent(texture->display, EGL_NO_SURFACE, EGL_NO_SURFACE, context);
    }

    glDeleteTextures(1, &texture->gl_texture);

    if (context != EGL_NO_CONTEXT) {
        eglMakeCurrent(texture->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }

    eglDestroyImage(texture->display, texture->egl_image);

    if (close_fds) {
        for (unsigned int i = 0; i < texture->n_planes; i++) {
            close(texture->plane_fds[i]);
        }
    }
    
    free(texture);
}

static void on_video_frame_destroy(void *userdata) {
    struct video_channel_flutter_texture_frame_destruction_data *data;

    data = userdata;

    //dmabuf_texture_destroy(data->texture, data->channel->context, true);

    /*
    cpset_lock(&data->channel->stale_textures);

    if (cpset_get_count_pointers_locked(&data->channel->stale_textures) > 0) {
        
    } else {
        cpset_put_locked(&data->channel->stale_textures, data->texture);
    }

    cpset_unlock(&data->channel->stale_textures);
    */

    free(data);
}

static int add_video_frame_to_flutter_texture(
    struct aa_channel *channel,
    int32_t width,
    int32_t height,
    uint32_t format,
    size_t n_planes,
    int plane_fds[3],
    int plane_offsets[3],
    int plane_strides[3]
) {
    struct video_channel_flutter_texture_frame_destruction_data *destruction_data;
    struct dmabuf_texture *texture;
    int ok;

    destruction_data = malloc(sizeof *destruction_data);
    if (destruction_data == NULL) {
        return ENOMEM;
    }

    cpset_lock(&channel->stale_textures);

    for_each_pointer_in_cpset(&channel->stale_textures, texture) {
        bool can_update = dmabuf_texture_can_update_to(
            texture,
            width, height,
            format,
            n_planes,
            plane_fds,
            plane_offsets,
            plane_strides
        );

        if (can_update) {
            cpset_remove_locked(&channel->stale_textures, texture);
            break;
        }
    }

    cpset_unlock(&channel->stale_textures);

    if (texture == NULL) {
        texture = dmabuf_texture_new(
            channel->display,
            channel->context,
            width,
            height,
            format,
            n_planes,
            plane_fds,
            plane_offsets,
            plane_strides
        );
        if (texture == NULL) {
            free(destruction_data);
            return EINVAL;
        }
    } else {
        ok = dmabuf_texture_update_to(
            texture,
            width,
            height,
            format,
            n_planes,
            plane_fds,
            plane_offsets,
            plane_strides
        );
        if (texture == NULL) {
            free(destruction_data);
            return EINVAL;
        }
    }

    destruction_data->channel = channel;
    destruction_data->texture = texture;

    ok = texreg_schedule_update(
        channel->device->texture_id,
        &(FlutterOpenGLTexture) {
            .target = GL_TEXTURE_EXTERNAL_OES,
            .name = texture->gl_texture,
            .format = GL_RGBA8_OES,
            .user_data = destruction_data,
            .destruction_callback = on_video_frame_destroy,
            .width = width,
            .height = height
        }
    );
    if (ok != 0) {
        dmabuf_texture_destroy(texture, channel->context, true);
    }

    return 0;
}

static GstPadProbeReturn on_video_channel_appsink_query(
    GstPad *pad,
    GstPadProbeInfo *info,
	gpointer userdata
) {
	GstQuery *query = info->data;

	if (GST_QUERY_TYPE(query) != GST_QUERY_ALLOCATION) {
	    return GST_PAD_PROBE_OK;
    }

	gst_query_add_allocation_meta(query, GST_VIDEO_META_API_TYPE, NULL);

	return GST_PAD_PROBE_HANDLED;
}

static GstPadProbeReturn on_video_channel_probe_sink_pad(
    GstPad *pad,
    GstPadProbeInfo *info,
    gpointer userdata
) {
    struct aa_channel *channel;
	GstVideoInfo video_info;
    GstEvent *event = GST_PAD_PROBE_INFO_EVENT(info);
	GstCaps *caps;
    gboolean ok2;
    int ok;

    channel = userdata;

	if (GST_EVENT_TYPE(event) != GST_EVENT_CAPS)
		return GST_PAD_PROBE_OK;

	gst_event_parse_caps(event, &caps);

	if (!caps) {
		fprintf(stderr, "[android-auto plugin] [video service] Received gstreamer caps event without caps.\n");
		return GST_PAD_PROBE_OK;
	}

    ok2 = gst_video_info_from_caps(&video_info, caps);
	if (ok2 == FALSE) {
		fprintf(stderr, "[android-auto plugin] [video service] Received gstreamer caps event with invalid video info.\n");
		return GST_PAD_PROBE_OK;
	}

	switch (GST_VIDEO_INFO_FORMAT(&video_info)) {
	case GST_VIDEO_FORMAT_I420:
		channel->drm_format = DRM_FORMAT_YUV420;
		break;
	case GST_VIDEO_FORMAT_NV12:
		channel->drm_format = DRM_FORMAT_NV12;
		break;
	case GST_VIDEO_FORMAT_YUY2:
		channel->drm_format = DRM_FORMAT_YUYV;
		break;
	default:
		fprintf(stderr, "[android-auto plugin] [video service] Unknown video format: %s\n", gst_video_format_to_string(GST_VIDEO_INFO_FORMAT(&video_info)));
		return GST_PAD_PROBE_OK;
	}

    channel->video_info = video_info;

	return GST_PAD_PROBE_OK;
}

static void on_video_channel_decodebin_element_added(
    GstBin *bin,
    GstElement *element, 
    gpointer user_data
) {
	GstElementFactory *elem_factory;
	gchar const *factory_name;

	elem_factory = gst_element_get_factory(element);
	factory_name = gst_plugin_feature_get_name(elem_factory);

	if (g_str_has_prefix(factory_name, "v4l2video") && g_str_has_suffix(factory_name, "dec")) {
		gst_util_set_object_arg(G_OBJECT(element), "capture-io-mode", "dmabuf");
	}
}

static gboolean on_video_channel_bus_msg(GstBus *bus, GstMessage *msg, gpointer user_data) {
	struct aa_channel *channel;
    
    channel = (struct aa_channel*) user_data;

	(void)bus;

	switch (GST_MESSAGE_TYPE(msg)) {
	case GST_MESSAGE_STATE_CHANGED: {
		gchar *dotfilename;
		GstState old_gst_state, cur_gst_state, pending_gst_state;

		/* Only consider state change messages coming from
		 * the toplevel element. */
		if (GST_MESSAGE_SRC(msg) != GST_OBJECT(channel->pipeline))
			break;

		gst_message_parse_state_changed(msg, &old_gst_state, &cur_gst_state, &pending_gst_state);

		printf(
			"GStreamer state change:  old: %s  current: %s  pending: %s\n",
			gst_element_state_get_name(old_gst_state),
			gst_element_state_get_name(cur_gst_state),
			gst_element_state_get_name(pending_gst_state)
		);

		dotfilename = g_strdup_printf(
			"statechange__old-%s__cur-%s__pending-%s",
			gst_element_state_get_name(old_gst_state),
			gst_element_state_get_name(cur_gst_state),
			gst_element_state_get_name(pending_gst_state)
		);
		GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS(GST_BIN(channel->pipeline), GST_DEBUG_GRAPH_SHOW_ALL, dotfilename);
		g_free(dotfilename);

		break;
	}
	case GST_MESSAGE_REQUEST_STATE: {
		GstState requested_state;
		gst_message_parse_request_state(msg, &requested_state);
		printf(
			"state change to %s was requested by %s\n",
			gst_element_state_get_name(requested_state),
			GST_MESSAGE_SRC_NAME(msg)
		);
		gst_element_set_state(GST_ELEMENT(channel->pipeline), requested_state);
		break;
	}
	case GST_MESSAGE_LATENCY: {
		printf("redistributing latency\n");
		gst_bin_recalculate_latency(GST_BIN(channel->pipeline));
		break;
	}
	case GST_MESSAGE_INFO:
	case GST_MESSAGE_WARNING:
	case GST_MESSAGE_ERROR: {
		GError *error = NULL;
		gchar *debug_info = NULL;
		gchar const *prefix;

		switch (GST_MESSAGE_TYPE(msg)) {
			case GST_MESSAGE_INFO:
				gst_message_parse_info(msg, &error, &debug_info);
				prefix = "INFO";
				break;
			case GST_MESSAGE_WARNING:
				gst_message_parse_warning(msg, &error, &debug_info);
				prefix = "WARNING";
				break;
			case GST_MESSAGE_ERROR:
				gst_message_parse_error(msg, &error, &debug_info);
				prefix = "ERROR";
				break;
			default:
				g_assert_not_reached();
		}
		printf("GStreamer %s: %s; debug info: %s", prefix, error->message, debug_info);

		g_clear_error(&error);
		g_free(debug_info);

		if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
			GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS(GST_BIN(channel->pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "error");
		}

		// TODO: stop mainloop in case of an error

		break;
	}
	default:
		break;
	}

	return TRUE;
}

static void *video_channel_g_main_loop_entry(void *args) {
    struct aa_channel *channel = args;

    g_main_loop_run(channel->g_main_loop);

    return NULL;
}

static void on_video_channel_eos(GstAppSink *appsink, gpointer user_data) {
    printf("[android-auto plugin] [video channel] end of stream.\n");
}

static int on_execute_video_channel_new_sample_or_preroll(void *userdata) {
    struct video_channel_flutter_texture_frame_destruction_data *destruction_data;
    struct dmabuf_texture *texture;
    struct aa_channel *channel;
    struct gbm_bo *bo;
    GstVideoMeta meta;
    GstMapInfo map_info;
    GstSample *sample;
    GstBuffer *buf;
    GstMemory *mem;
    uint32_t format, stride;
    size_t n_mems;
    void *map, *map_data;
    int ok, fd, fds[3], offsets[3], strides[3];

    channel = userdata;

    /**
     * Fetch the preroll or sample.
     */
    sample = gst_app_sink_try_pull_preroll(channel->sink, 0);
    if (sample != NULL) {
        printf("[android-auto plugin] [video channel] pulled new preroll.\n");
    } else {
        sample = gst_app_sink_try_pull_sample(channel->sink, 0);
        if (sample != NULL) {
            printf("[android-auto plugin] [video channel] pulled new sample.\n");
        } else {
            printf("[android-auto plugin] [video channel] Could neither pull preroll nor sample from gstreamer appsink.\n");
            return 0;
        }
    }

    /**
     * Get the buffer associated with that sample.
     */
    buf = gst_sample_get_buffer(sample);
    if (buf == NULL) {
        fprintf(stderr, "[android-auto plugin] [video channel] gstreamer didn't provide a buffer for the video sample.\n");
        ok = EINVAL;
        goto fail_unref_sample;
    }

    /**
     * Get the video metadata associated with that buffer.
     * (Copy it, since we may unref `sample` earlier than the last access to `meta`)
     */
    {
        GstVideoMeta *pmeta = gst_buffer_get_video_meta(buf);
        if (pmeta == NULL) {
            fprintf(stderr, "[android-auto plugin] [video channel] gstreamer didn't provide metadata for the video sample.\n");
            ok = EINVAL;
            goto fail_unref_sample;
        }
        meta = *pmeta;
    }

    n_mems = gst_buffer_n_memory(buf);

    /**
     * If we have dmabuf memory,
     */
    if ((gst_buffer_n_memory(buf) == 1) && gst_is_dmabuf_memory(gst_buffer_peek_memory(buf, 0))) {
        mem = gst_buffer_peek_memory(buf, 0);
        if (mem == NULL) {
            fprintf(stderr, "[android-auto plugin] [video channel] memory associated with video sample buffer is NULL.\n");
            ok = EINVAL;
            goto fail_unref_sample;
        }

        ok = gst_dmabuf_memory_get_fd(mem);
        if (ok < 0) {
            fprintf(stderr, "[android-auto plugin] [video channel] Could not obtain dmabuf fd for gstreamer video sample buffer.\n");
            ok = EIO;
            goto fail_unref_sample;
        }

        fd = dup(ok);

        gst_sample_unref(sample);
        sample = NULL;
    } else {
        /**
         * We don't have a dmabuf fd, or we have multiple dmabuf fds (which doesn't have a zerocopy path right now).
         */
        
        ok = gst_buffer_map(buf, &map_info, GST_MAP_READ);
        if (!ok) {
            fprintf(stderr, "[android-auto plugin] [video channel] Could not map gstreamer buffer. gst_buffer_map\n");
            ok = EIO;
            goto fail_unref_sample;
        }

        bo = gbm_bo_create(flutterpi.gbm.device, map_info.size, 1, GBM_FORMAT_R8, GBM_BO_USE_LINEAR);
        if (bo == NULL) {
            ok = errno;
            fprintf(stderr, "[android-auto plugin] [video channel] Could not create GBM BO. gbm_bo_create\n");
            goto fail_unmap_gst_buf;
        }
        
        map = gbm_bo_map(bo, 0, 0, map_info.size, 1, GBM_BO_TRANSFER_WRITE, &stride, &map_data);
        if (map == NULL) {
            ok = errno;
            perror("[android-auto plugin] [video channel] Could not map GBM BO. gbm_bo_map");
            goto fail_destroy_gbm_bo;
        }

        memcpy(map, map_info.data, map_info.size);

        gbm_bo_unmap(bo, map_data);

        fd = gbm_bo_get_fd(bo);
        if (fd == -1) {
            fprintf(stderr, "[android-auto plugin] [video channel] Could not get dmabuf fd for GBM BO. gbm_bo_get_fd\n");
            ok = EINVAL;
            goto fail_destroy_gbm_bo;
        }

        gbm_bo_destroy(bo);
        gst_buffer_unmap(buf, &map_info);
        gst_sample_unref(sample);
        sample = NULL;
    } 

    /**
     * At this point, `sample` doesn't exist anymore, only the memory it contained.
     */

    switch (meta.format) {
        case GST_VIDEO_FORMAT_I420:
            format = DRM_FORMAT_YUV420;
            break;
        case GST_VIDEO_FORMAT_NV12:
            format = DRM_FORMAT_NV12;
            break;
        case GST_VIDEO_FORMAT_YUY2:
            format = DRM_FORMAT_YUYV;
            break;
        default:
            fprintf(stderr, "[android-auto plugin] [video channel] gstreamer video format is not recognized: %s\n", gst_video_format_to_string(meta.format));
            ok = ENOTSUP;
            goto fail_close_fd;
    }

    for (unsigned int i = 0; i < meta.n_planes; i++) {
        fds[i] = dup(fd);
        offsets[i] = meta.offset[i];
        strides[i] = meta.stride[i];
    }

    close(fd);

    destruction_data = malloc(sizeof *destruction_data);
    if (destruction_data == NULL) {
        goto fail_close_plane_fds;
    }

    /**
     * Try to find an old dmabuf texture we can update.
     */
    cpset_lock(&channel->stale_textures);

    for_each_pointer_in_cpset(&channel->stale_textures, texture) {
        bool can_update = dmabuf_texture_can_update_to(
            texture,
            meta.width,
            meta.height,
            format,
            meta.n_planes,
            fds,
            offsets,
            strides
        );

        if (can_update) {
            cpset_remove_locked(&channel->stale_textures, texture);
            break;
        }
    }

    cpset_unlock(&channel->stale_textures);

    if (texture == NULL) {
        /**
         * We didn't find an old dmabuf texture to update.
         * Let's create a new one.
         */

        texture = dmabuf_texture_new(
            channel->display,
            channel->context,
            meta.width,
            meta.height,
            format,
            meta.n_planes,
            fds,
            offsets,
            strides
        );
        if (texture == NULL) {
            ok = EIO;
            goto fail_free_destruction_data;
        }
    } else {
        ok = dmabuf_texture_update_to(
            texture,
            meta.width,
            meta.height,
            format,
            meta.n_planes,
            fds,
            offsets,
            strides
        );
        if (texture == NULL) {
            ok = EIO;
            goto fail_free_destruction_data;
        }
    }

    destruction_data->channel = channel;
    destruction_data->texture = texture;

    ok = texreg_schedule_update(
        channel->device->texture_id,
        &(FlutterOpenGLTexture) {
            .target = GL_TEXTURE_EXTERNAL_OES,
            .name = texture->gl_texture,
            .format = GL_RGBA8_OES,
            .user_data = destruction_data,
            .destruction_callback = on_video_frame_destroy,
            .width = meta.width,
            .height = meta.height
        }
    );
    if (ok != 0) {
        ok = EIO;
        goto fail_destroy_dmabuf_texture;
    }

    return GST_FLOW_OK;


    fail_destroy_dmabuf_texture:
    dmabuf_texture_destroy(texture, channel->context, true);

    fail_free_destruction_data:
    free(destruction_data);

    fail_close_plane_fds:
    for (unsigned int i = 0; i < meta.n_planes; i++) {
        close(fds[i]);
    }
    goto fail_return_ok;

    fail_close_fd:
    close(fd);
    goto fail_return_ok;

    fail_destroy_gbm_bo:
    gbm_bo_destroy(bo);

    fail_unmap_gst_buf:
    gst_buffer_unmap(buf, &map_info);

    fail_unref_sample:
    if (sample != NULL) {
        gst_sample_unref(sample);
    }

    fail_return_ok:
    return GST_FLOW_ERROR;
}

static GstFlowReturn on_video_channel_new_sample_or_preroll(GstAppSink *appsink, void *userdata) {
    flutterpi_post_platform_task(
        on_execute_video_channel_new_sample_or_preroll,
        userdata
    );
    return GST_FLOW_OK;
}

static int setup_video_output(
    struct aa_channel *channel,
    F1x__Aasdk__Proto__Enums__VideoResolution__Enum resolution
) {
    EGLImageKHR egl_image;
    EGLContext context;
    int64_t texture_id;
    EGLint egl_error;
    int ok;

    // filesrc name=\"src\" location=\"/opt/vc/src/hello_pi/hello_video/test.h264\"

    static const char *pipeline_desc = "appsrc name=\"src\" ! decodebin name=\"decode\" ! video/x-raw ! appsink sync=false name=\"sink\"";

    {
        struct egl_context_creation_data creation_data; // = malloc(sizeof(struct egl_context_creation_data));

        creation_data.context = EGL_NO_CONTEXT;
        sem_init(&creation_data.created, 0, 0);

        ok = flutterpi_post_platform_task(
            on_execute_create_vout_context,
            &creation_data
        );
        if (ok != 0) {
            //free(creation_data);
            return ok;
        }

        sem_wait(&creation_data.created);

        context = creation_data.context;
        if (creation_data.err != EGL_SUCCESS) {
            fprintf(stderr, "[android-auto plugin] [video service] Could not create EGL context. %d\n", creation_data.err);
            return EINVAL;
        }
    }

    ok = texreg_add(&texture_id, NULL);
    if (ok != 0) {
        eglDestroyContext(flutterpi.egl.display, context);
        return ok;
    }

    channel->g_main_loop = g_main_loop_new(NULL, FALSE);
    
    channel->pipeline = GST_PIPELINE(gst_parse_launch(pipeline_desc, NULL));

    channel->src = GST_APP_SRC(gst_bin_get_by_name(GST_BIN(channel->pipeline), "src"));
    
    gst_app_src_set_stream_type(channel->src, GST_APP_STREAM_TYPE_STREAM);
    gst_app_src_set_latency(channel->src, -1, 100);
    gst_app_src_set_max_bytes(channel->src, 0);

    channel->sink = GST_APP_SINK(gst_bin_get_by_name(GST_BIN(channel->pipeline), "sink"));

    {
        GstPad *pad = gst_element_get_static_pad(GST_ELEMENT(channel->sink), "sink");
        gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM, on_video_channel_appsink_query, NULL, NULL);
        gst_object_unref(pad);
    }

    gst_base_sink_set_max_lateness(GST_BASE_SINK(channel->sink), 20 * GST_MSECOND);
    gst_base_sink_set_qos_enabled(GST_BASE_SINK(channel->sink), TRUE);
    gst_app_sink_set_max_buffers(GST_APP_SINK(channel->sink), 2);

    gst_app_sink_set_callbacks(
        channel->sink,
        &(GstAppSinkCallbacks) {
            .eos = on_video_channel_eos,
            .new_preroll = on_video_channel_new_sample_or_preroll,
            .new_sample = on_video_channel_new_sample_or_preroll,
            ._gst_reserved = {NULL}
        },
        channel,
        NULL
    );
    
    gst_pad_add_probe(
        gst_element_get_static_pad(GST_ELEMENT(channel->sink), "sink"),
        GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
        on_video_channel_probe_sink_pad,
        channel,
        NULL
    );
    
    channel->decodebin = GST_BIN(gst_bin_get_by_name(GST_BIN(channel->pipeline), "decode"));

    g_signal_connect(channel->decodebin, "element-added", G_CALLBACK(on_video_channel_decodebin_element_added), channel);

    {
        GstBus *bus = gst_pipeline_get_bus(channel->pipeline);
        gst_bus_add_watch(bus, on_video_channel_bus_msg, channel);
        gst_object_unref(GST_OBJECT(bus));
    }

    channel->display = flutterpi.egl.display;
    channel->context = context;

    channel->device->has_texture_id = true;
    channel->device->texture_id = texture_id;

    gst_element_set_state(GST_ELEMENT(channel->pipeline), GST_STATE_PLAYING);

    pthread_create(&channel->g_main_loop_thread, NULL, video_channel_g_main_loop_entry, channel);

    return 0;

    fail_return_ok:
    return ok;
}

static int aa_video_channel_on_avchannel_setup_request(struct aa_channel *channel, size_t payload_size, uint8_t *payload) {
    F1x__Aasdk__Proto__Messages__AVChannelSetupResponse setup_response = F1X__AASDK__PROTO__MESSAGES__AVCHANNEL_SETUP_RESPONSE__INIT;
    F1x__Aasdk__Proto__Messages__AVChannelSetupRequest *setup_request;
    uint32_t config_value;
    int64_t texture_id;
    size_t packed_size;
    bool setup_ok;
    int ok;

    setup_request = f1x__aasdk__proto__messages__avchannel_setup_request__unpack(NULL, payload_size, payload);

    printf("[android auto plugin] [video service] setup request, config index: %u\n", setup_request->config_index);

    f1x__aasdk__proto__messages__avchannel_setup_request__free_unpacked(setup_request, NULL);

    ok = setup_video_output(channel, F1X__AASDK__PROTO__ENUMS__VIDEO_RESOLUTION__ENUM___480p);
    if (ok != 0) {
        fprintf(stderr, "[android auto plugin] [video service] failed to setup video output. setup_video_output: %s\n", strerror(ok));
    }

    config_value = 0; 

    setup_response.n_configs = 1;
    setup_response.configs = &config_value;
    setup_response.max_unacked = 1;
    setup_response.media_status = ok == 0? F1X__AASDK__PROTO__ENUMS__AVCHANNEL_SETUP_STATUS__ENUM__OK : F1X__AASDK__PROTO__ENUMS__AVCHANNEL_SETUP_STATUS__ENUM__FAIL;

    packed_size = f1x__aasdk__proto__messages__avchannel_setup_response__get_packed_size(&setup_response);

    define_and_setup_aa_msg_on_stack(response_msg, packed_size + 2, channel->id, AA_MSG_FLAG_ENCRYPTED);

    *(uint16_t *) response_msg.payload->pointer = cpu_to_be16(F1X__AASDK__PROTO__IDS__AVCHANNEL_MESSAGE__ENUM__SETUP_RESPONSE);
    f1x__aasdk__proto__messages__avchannel_setup_response__pack(&setup_response, response_msg.payload->pointer + 2);

    ok = aa_device_send(channel->device, &response_msg);
    if (ok != 0) {
        fprintf(stderr, "[android-auto] [video service] Could not send av channel setup response. aa_device_send: %s\n", strerror(ok));
        return ok;
    }

    sync_android_auto_state(channel->device->aaplugin);

    return 0;
}

static int aa_video_channel_on_avchannel_start_indication(struct aa_channel *channel, size_t payload_size, uint8_t *payload) {
    F1x__Aasdk__Proto__Messages__AVChannelStartIndication *start_indication;

    start_indication = f1x__aasdk__proto__messages__avchannel_start_indication__unpack(NULL, payload_size, payload);

    printf("[android-auto plugin] [video service] av channel start indication. config = %u, session = %d\n", start_indication->config, start_indication->session);

    channel->has_session = true;
    channel->session = start_indication->session;    

    f1x__aasdk__proto__messages__avchannel_start_indication__free_unpacked(start_indication, NULL);

    return 0;
}

static int aa_video_channel_on_avchannel_stop_indication(struct aa_channel *channel, size_t payload_size, uint8_t *payload) {
    printf("[android-auto plugin] [video service] av channel stop indication.\n");
    
    return 0;
}

static int aa_video_channel_on_avchannel_av_media_with_timestamp_indication(struct aa_channel *channel, struct aa_msg *msg) {
    F1x__Aasdk__Proto__Messages__AVMediaAckIndication ack_indication = F1X__AASDK__PROTO__MESSAGES__AVMEDIA_ACK_INDICATION__INIT;
    uint64_t timestamp;
    uint8_t *payload, *media;
    size_t payload_size, media_size, response_data_size;
    int ok;

    payload_size = msg->payload->size - 2;
    payload = msg->payload->pointer + 2;

    /**
     * Unpack the message.
     */
    timestamp = be64_to_cpu(*(uint64_t *) payload);

    media_size = payload_size - 8;
    media = payload + 8;

    printf("[android-auto plugin] [video service] AV media with timestamp indication. timestamp: %llu, media_size: %u\n", timestamp, media_size);

    GstBuffer *gst_buf = gst_buffer_new_wrapped_full(
        0,
        media,
        media_size,
        0,
        media_size,
        msg,
        (GDestroyNotify) aa_msg_unref
    );
    if (gst_buf == NULL) {
        fprintf(stderr, "[android-auto plugin] [video channel] Could not create gstreamer buffer from android auto message.\n");
        aa_msg_unrefp(&msg);
        return EINVAL;
    }

    gst_app_src_push_buffer(channel->src, gst_buf);

    //gst_buffer_unref(gst_buf);

    /**
     * Send the response. (AV Media ACK Indication)
     */
    ack_indication.session = channel->session;
    ack_indication.value = 1;

    response_data_size = f1x__aasdk__proto__messages__avmedia_ack_indication__get_packed_size(&ack_indication);

    define_and_setup_aa_msg_on_stack(response_msg, response_data_size + 2, channel->id, AA_MSG_FLAG_ENCRYPTED);

    *(uint16_t *) response_msg.payload->pointer = cpu_to_be16(F1X__AASDK__PROTO__IDS__AVCHANNEL_MESSAGE__ENUM__AV_MEDIA_ACK_INDICATION);
    f1x__aasdk__proto__messages__avmedia_ack_indication__pack(&ack_indication, response_msg.payload->pointer + 2);

    ok = aa_device_send(channel->device, &response_msg);
    if (ok != 0) {
        fprintf(stderr, "[android-auto] [video service] Could not send av channel av media ack indication. aa_device_send: %s\n", strerror(ok));
        return ok;
    }

    return 0;
}

static int aa_video_channel_on_avchannel_av_media_indication(struct aa_channel *channel, struct aa_msg *msg) {
    F1x__Aasdk__Proto__Messages__AVMediaAckIndication ack_indication = F1X__AASDK__PROTO__MESSAGES__AVMEDIA_ACK_INDICATION__INIT;
    uint8_t *payload, *media;
    size_t payload_size, media_size, response_data_size;
    int ok;

    payload_size = msg->payload->size - 2;
    payload = msg->payload->pointer + 2;
    
    /**
     * Unpack the message.
     */
    media_size = payload_size;
    media = payload;

    printf("[android-auto plugin] [video service] AV media indication. media_size: %u\n", media_size);

    GstBuffer *gst_buf = gst_buffer_new_wrapped_full(
        0,
        media,
        media_size,
        0,
        media_size,
        msg,
        (GDestroyNotify) aa_msg_unref
    );
    if (gst_buf == NULL) {
        fprintf(stderr, "[android-auto plugin] [video channel] Could not create gstreamer buffer from android auto message.\n");
        aa_msg_unrefp(&msg);
        return EINVAL;
    }

    gst_app_src_push_buffer(channel->src, gst_buf);

    //gst_buffer_unref(gst_buf);

    /**
     * Send the response. (AV Media ACK Indication)
     */
    ack_indication.session = channel->session;
    ack_indication.value = 1;

    response_data_size = f1x__aasdk__proto__messages__avmedia_ack_indication__get_packed_size(&ack_indication);

    define_and_setup_aa_msg_on_stack(response_msg, response_data_size + 2, channel->id, AA_MSG_FLAG_ENCRYPTED);

    *(uint16_t *) response_msg.payload->pointer = cpu_to_be16(F1X__AASDK__PROTO__IDS__AVCHANNEL_MESSAGE__ENUM__AV_MEDIA_ACK_INDICATION);
    f1x__aasdk__proto__messages__avmedia_ack_indication__pack(&ack_indication, response_msg.payload->pointer + 2);

    ok = aa_device_send(channel->device, &response_msg);
    if (ok != 0) {
        fprintf(stderr, "[android-auto] [video service] Could not send av channel av media ack indication. aa_device_send: %s\n", strerror(ok));
        return ok;
    }

    return 0;
}

static int aa_video_channel_on_avchannel_video_focus_request(struct aa_channel *channel, size_t payload_size, uint8_t *payload) {
    F1x__Aasdk__Proto__Messages__VideoFocusIndication focus_indication = F1X__AASDK__PROTO__MESSAGES__VIDEO_FOCUS_INDICATION__INIT;
    F1x__Aasdk__Proto__Messages__VideoFocusRequest *focus_request;
    size_t focus_indication_packed_size;

    focus_request = f1x__aasdk__proto__messages__video_focus_request__unpack(NULL, payload_size, payload);
    
    printf("[android-auto plugin] [video service] video focus request. display_index: ");
    if (focus_request->has_disp_index) {
        printf("%d", focus_request->disp_index);
    } else {
        printf("(none)");
    }

    printf(
        ", focus_mode: %s, focus_reason: %s\n",
        protobuf_c_enum_descriptor_get_value(&f1x__aasdk__proto__enums__video_focus_mode__enum__descriptor, focus_request->focus_mode)->name,
        protobuf_c_enum_descriptor_get_value(&f1x__aasdk__proto__enums__video_focus_reason__enum__descriptor, focus_request->focus_reason)->name
    );

    channel->device->is_focused = focus_request->focus_mode == F1X__AASDK__PROTO__ENUMS__VIDEO_FOCUS_MODE__ENUM__FOCUSED;

    sync_android_auto_state(channel->device->aaplugin);

    f1x__aasdk__proto__messages__video_focus_request__free_unpacked(focus_request, NULL);

    focus_indication.focus_mode = channel->device->is_focused ?
        F1X__AASDK__PROTO__ENUMS__VIDEO_FOCUS_MODE__ENUM__FOCUSED :
        F1X__AASDK__PROTO__ENUMS__VIDEO_FOCUS_MODE__ENUM__UNFOCUSED;
    focus_indication.unrequested = false;

    focus_indication_packed_size = f1x__aasdk__proto__messages__video_focus_indication__get_packed_size(&focus_indication);
    define_and_setup_aa_msg_on_stack(focus_indication_msg, focus_indication_packed_size + 2, channel->id, AA_MSG_FLAG_ENCRYPTED)
    *(uint16_t*) focus_indication_msg.payload->pointer = cpu_to_be16(F1X__AASDK__PROTO__IDS__AVCHANNEL_MESSAGE__ENUM__VIDEO_FOCUS_INDICATION);
    f1x__aasdk__proto__messages__video_focus_indication__pack(&focus_indication, focus_indication_msg.payload->pointer + 2);

    return aa_device_send(channel->device, &focus_indication_msg);
}

static int aa_video_channel_on_message(struct aa_channel *channel, struct aa_msg *msg) {
    uint16_t message_id;
    uint8_t *payload;
    size_t payload_size;
    int ok;

    message_id = be16_to_cpu(*(uint16_t*) msg->payload->pointer);
    payload = msg->payload->pointer + 2;
    payload_size = msg->payload->size - 2;

    switch (message_id) {
        case F1X__AASDK__PROTO__IDS__AVCHANNEL_MESSAGE__ENUM__SETUP_REQUEST:
            ok = aa_video_channel_on_avchannel_setup_request(channel, payload_size, payload);
            break;

        case F1X__AASDK__PROTO__IDS__AVCHANNEL_MESSAGE__ENUM__START_INDICATION:
            ok = aa_video_channel_on_avchannel_start_indication(channel, payload_size, payload);
            break;

        case F1X__AASDK__PROTO__IDS__AVCHANNEL_MESSAGE__ENUM__STOP_INDICATION:
            ok = aa_video_channel_on_avchannel_stop_indication(channel, payload_size, payload);
            break;

        case F1X__AASDK__PROTO__IDS__AVCHANNEL_MESSAGE__ENUM__AV_MEDIA_WITH_TIMESTAMP_INDICATION:
            ok = aa_video_channel_on_avchannel_av_media_with_timestamp_indication(channel, aa_msg_ref(msg));
            break;

        case F1X__AASDK__PROTO__IDS__AVCHANNEL_MESSAGE__ENUM__AV_MEDIA_INDICATION:
            ok = aa_video_channel_on_avchannel_av_media_indication(channel, aa_msg_ref(msg));
            break; 

        case F1X__AASDK__PROTO__IDS__AVCHANNEL_MESSAGE__ENUM__VIDEO_FOCUS_REQUEST:
            ok = aa_video_channel_on_avchannel_video_focus_request(channel, payload_size, payload);
            break;

        case F1X__AASDK__PROTO__IDS__CONTROL_MESSAGE__ENUM__CHANNEL_OPEN_REQUEST:
            ok = aa_channel_on_channel_open_request_msg(channel, payload_size, payload);
            break;

        default:
            ok = EINVAL;
            break;
    }

    aa_msg_unrefp(&msg);

    return ok;
}

static int aa_video_channel_fill_features(struct aa_channel *channel, F1x__Aasdk__Proto__Data__ChannelDescriptor *desc) {
    int ok;

    F1x__Aasdk__Proto__Data__AVChannel *avchannel = malloc(sizeof(F1x__Aasdk__Proto__Data__AVChannel));
    if (avchannel == NULL) {
        return ENOMEM;
    }
    
    F1x__Aasdk__Proto__Data__VideoConfig *video_config = malloc(sizeof(F1x__Aasdk__Proto__Data__VideoConfig));
    if (video_config == NULL) {
        free(avchannel);
        return ENOMEM;
    }
    
    F1x__Aasdk__Proto__Data__VideoConfig **pvideo_configs = malloc(sizeof(F1x__Aasdk__Proto__Data__VideoConfig *));
    if (pvideo_configs == NULL) {
        free(avchannel);
        free(video_config);
        return ENOMEM;
    }

    f1x__aasdk__proto__data__avchannel__init(avchannel);
    f1x__aasdk__proto__data__video_config__init(video_config);

    *pvideo_configs = video_config;

    video_config->video_resolution = F1X__AASDK__PROTO__ENUMS__VIDEO_RESOLUTION__ENUM___480p;
    video_config->video_fps = F1X__AASDK__PROTO__ENUMS__VIDEO_FPS__ENUM___60;

    video_config->margin_width = 0;
    video_config->margin_height = 0;
    video_config->dpi = flutterpi.display.pixel_ratio * 38 * 25.4 / 10.0;
    video_config->has_additional_depth = false;

    avchannel->stream_type = F1X__AASDK__PROTO__ENUMS__AVSTREAM_TYPE__ENUM__VIDEO;
    avchannel->n_video_configs = 1;
    avchannel->video_configs = pvideo_configs;
    avchannel->has_available_while_in_call = true;
    avchannel->available_while_in_call = true;

    desc->channel_id = kAndroidAutoChannelVideo;
    desc->av_channel = avchannel;

    return 0;
}

static void aa_video_channel_after_fill_features(struct aa_channel *channel, F1x__Aasdk__Proto__Data__ChannelDescriptor *desc) {
    free(desc->av_channel->video_configs[0]);
    free(desc->av_channel->video_configs);
    free(desc->av_channel);
}

static void aa_video_channel_destroy(struct aa_channel *channel) {

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
    channel->debug_channel_name = "video";

    channel->has_session = false;
    channel->session = -1;
    channel->display = EGL_NO_DISPLAY;
    channel->context = EGL_NO_CONTEXT;
    channel->stale_textures = CPSET_INITIALIZER(CPSET_DEFAULT_MAX_SIZE);

    return channel;
}


static int aa_sensor_channel_send_event(
    struct aa_channel *channel,
    bool has_driving_status,
    F1x__Aasdk__Proto__Enums__DrivingStatus__Enum driving_status,
    bool has_is_night,
    bool is_night
) {
    F1x__Aasdk__Proto__Data__DrivingStatus *driving_status_data;
    F1x__Aasdk__Proto__Data__NightMode *night_mode_data;
    F1x__Aasdk__Proto__Messages__SensorEventIndication ind
        = F1X__AASDK__PROTO__MESSAGES__SENSOR_EVENT_INDICATION__INIT;
    size_t packed_size;

    if (has_driving_status) {
        driving_status_data = alloca(sizeof(F1x__Aasdk__Proto__Data__DrivingStatus));
        f1x__aasdk__proto__data__driving_status__init(driving_status_data);

        driving_status_data->status = driving_status;

        ind.n_driving_status = 1;
        ind.driving_status = &driving_status_data;
    }

    if (has_is_night) {
        night_mode_data = alloca(sizeof(F1x__Aasdk__Proto__Data__NightMode));
        f1x__aasdk__proto__data__night_mode__init(night_mode_data);

        night_mode_data->is_night = is_night;

        ind.n_night_mode = 1;
        ind.night_mode = &night_mode_data;
    }

    packed_size = f1x__aasdk__proto__messages__sensor_event_indication__get_packed_size(&ind);

    define_and_setup_aa_msg_on_stack(ind_msg, packed_size + 2, kAndroidAutoChannelSensor, AA_MSG_FLAG_ENCRYPTED);

    *(uint16_t *) ind_msg.payload->pointer = cpu_to_be16(F1X__AASDK__PROTO__IDS__SENSOR_CHANNEL_MESSAGE__ENUM__SENSOR_EVENT_INDICATION);
    f1x__aasdk__proto__messages__sensor_event_indication__pack(&ind, ind_msg.payload->pointer + 2);

    return aa_device_send(channel->device, &ind_msg);
}

static int aa_sensor_channel_send_driving_status(struct aa_channel *channel, F1x__Aasdk__Proto__Enums__DrivingStatus__Enum driving_status) {
    return aa_sensor_channel_send_event(
        channel,
        true,
        driving_status,
        false,
        false
    );
}

static int aa_sensor_channel_send_night_data(struct aa_channel *channel, bool is_night) {
    return aa_sensor_channel_send_event(
        channel,
        false,
        0,
        true,
        is_night
    );
}

static int aa_sensor_channel_on_sensor_start_request(struct aa_channel *channel, size_t payload_size, uint8_t *payload) {
    F1x__Aasdk__Proto__Messages__SensorStartResponseMessage start_response
        = F1X__AASDK__PROTO__MESSAGES__SENSOR_START_RESPONSE_MESSAGE__INIT;
    F1x__Aasdk__Proto__Messages__SensorStartRequestMessage *start_request;
    F1x__Aasdk__Proto__Enums__SensorType__Enum sensor_type;
    int64_t refresh_interval;
    size_t packed_size;
    int ok;

    start_request = f1x__aasdk__proto__messages__sensor_start_request_message__unpack(NULL, payload_size, payload);
    if (start_request == NULL) {
        fprintf(stderr, "[android-auto plugin] [sensor channel] Could not unpack sensor start request.\n");
        return EPROTO;
    }

    printf(
        "[android-auto plugin] [sensor channel] sensor start request. sensor_type: %s, refresh_interval: %lld\n",
        protobuf_c_enum_descriptor_get_value(&f1x__aasdk__proto__enums__sensor_type__enum__descriptor, start_request->sensor_type)->name,
        start_request->refresh_interval
    );

    sensor_type = start_request->sensor_type;
    refresh_interval = start_request->refresh_interval;

    f1x__aasdk__proto__messages__sensor_start_request_message__free_unpacked(start_request, NULL);

    start_response.status = F1X__AASDK__PROTO__ENUMS__STATUS__ENUM__OK;

    packed_size = f1x__aasdk__proto__messages__sensor_start_response_message__get_packed_size(&start_response);

    define_and_setup_aa_msg_on_stack(start_response_msg, packed_size + 2, kAndroidAutoChannelSensor, AA_MSG_FLAG_ENCRYPTED);

    *(uint16_t *) start_response_msg.payload->pointer = cpu_to_be16(F1X__AASDK__PROTO__IDS__SENSOR_CHANNEL_MESSAGE__ENUM__SENSOR_START_RESPONSE);
    f1x__aasdk__proto__messages__sensor_start_response_message__pack(&start_response, start_response_msg.payload->pointer + 2);

    ok = aa_device_send(channel->device, &start_response_msg);
    if (ok != 0) {
        return ok;
    }

    if (refresh_interval != -1) {
        if (sensor_type == F1X__AASDK__PROTO__ENUMS__SENSOR_TYPE__ENUM__DRIVING_STATUS) {
            return aa_sensor_channel_send_driving_status(channel, F1X__AASDK__PROTO__ENUMS__DRIVING_STATUS__ENUM__UNRESTRICTED);
        } else if (sensor_type == F1X__AASDK__PROTO__ENUMS__SENSOR_TYPE__ENUM__NIGHT_DATA) {
            return aa_sensor_channel_send_night_data(channel, true);
        }
    }

    return 0;
}

int aa_sensor_channel_on_message(struct aa_channel *channel, struct aa_msg *msg) {
    uint16_t message_id;
    uint8_t *payload;
    size_t payload_size;
    int ok;
    
    message_id = be16_to_cpu(*(uint16_t*) msg->payload->pointer);
    payload = msg->payload->pointer + 2;
    payload_size = msg->payload->size - 2;
    
    switch (message_id) {
        case F1X__AASDK__PROTO__IDS__SENSOR_CHANNEL_MESSAGE__ENUM__SENSOR_START_REQUEST:
            ok = aa_sensor_channel_on_sensor_start_request(channel, payload_size, payload);
            break;
        case F1X__AASDK__PROTO__IDS__CONTROL_MESSAGE__ENUM__CHANNEL_OPEN_REQUEST:
            ok = aa_channel_on_channel_open_request_msg(channel, payload_size, payload);
            break;
        default:
            ok = EINVAL;
            break;
    }
    
    aa_msg_unrefp(&msg);
    
    return ok;
}

int aa_sensor_channel_fill_features(struct aa_channel *channel, F1x__Aasdk__Proto__Data__ChannelDescriptor *desc) {
    int ok;

    F1x__Aasdk__Proto__Data__SensorChannel *sensor_channel = malloc(sizeof(F1x__Aasdk__Proto__Data__SensorChannel));
    if (sensor_channel == NULL) {
        return ENOMEM;
    }

    F1x__Aasdk__Proto__Data__Sensor *sensors = malloc(3 * sizeof(F1x__Aasdk__Proto__Data__Sensor));
    if (sensors == NULL) {
        free(sensor_channel);
        return ENOMEM;
    }

    F1x__Aasdk__Proto__Data__Sensor **psensors = malloc(3 * sizeof(F1x__Aasdk__Proto__Data__Sensor*));
    if (psensors == NULL) {
        free(sensors);
        free(sensor_channel);
        return ENOMEM;
    }

    f1x__aasdk__proto__data__sensor_channel__init(sensor_channel);

    for (int i = 0; i < 3; i++) {
        f1x__aasdk__proto__data__sensor__init(sensors + i);
        psensors[i] = sensors + i;
    }

    sensors[0].type = F1X__AASDK__PROTO__ENUMS__SENSOR_TYPE__ENUM__DRIVING_STATUS;
    sensors[1].type = F1X__AASDK__PROTO__ENUMS__SENSOR_TYPE__ENUM__NIGHT_DATA;
    sensors[2].type = F1X__AASDK__PROTO__ENUMS__SENSOR_TYPE__ENUM__LOCATION;

    sensor_channel->n_sensors = 2;
    sensor_channel->sensors = psensors;

    desc->channel_id = kAndroidAutoChannelSensor;
    desc->sensor_channel = sensor_channel;

    return 0;
}

void aa_sensor_channel_after_fill_features(struct aa_channel *channel, F1x__Aasdk__Proto__Data__ChannelDescriptor *desc) {
    free(desc->sensor_channel->sensors[0]);
    free(desc->sensor_channel->sensors);
    free(desc->sensor_channel);
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
    channel->debug_channel_name = "sensor";

    return channel;
}



static int aa_audio_channel_on_avchannel_setup_request(struct aa_channel *channel, size_t payload_size, uint8_t *payload) {
    F1x__Aasdk__Proto__Messages__AVChannelSetupResponse setup_response = F1X__AASDK__PROTO__MESSAGES__AVCHANNEL_SETUP_RESPONSE__INIT;
    F1x__Aasdk__Proto__Messages__AVChannelSetupRequest *setup_request;
    uint32_t config_value;
    int64_t texture_id;
    size_t packed_size;
    bool setup_ok;
    int ok;

    setup_request = f1x__aasdk__proto__messages__avchannel_setup_request__unpack(NULL, payload_size, payload);

    printf("[android-auto plugin] [audio channel] setup request, config index: %u\n", setup_request->config_index);

    f1x__aasdk__proto__messages__avchannel_setup_request__free_unpacked(setup_request, NULL);

    ok = 0;
    if (ok != 0) {
        fprintf(stderr, "[android auto plugin] [audio channel] failed to setup audio output. setup_audio_output: %s\n", strerror(ok));
    }

    config_value = 0; 

    setup_response.n_configs = 1;
    setup_response.configs = &config_value;
    setup_response.max_unacked = 1;
    setup_response.media_status = ok == 0? F1X__AASDK__PROTO__ENUMS__AVCHANNEL_SETUP_STATUS__ENUM__OK : F1X__AASDK__PROTO__ENUMS__AVCHANNEL_SETUP_STATUS__ENUM__FAIL;

    packed_size = f1x__aasdk__proto__messages__avchannel_setup_response__get_packed_size(&setup_response);

    define_and_setup_aa_msg_on_stack(response_msg, packed_size + 2, channel->id, AA_MSG_FLAG_ENCRYPTED);

    *(uint16_t *) response_msg.payload->pointer = cpu_to_be16(F1X__AASDK__PROTO__IDS__AVCHANNEL_MESSAGE__ENUM__SETUP_RESPONSE);
    f1x__aasdk__proto__messages__avchannel_setup_response__pack(&setup_response, response_msg.payload->pointer + 2);

    ok = aa_device_send(channel->device, &response_msg);
    if (ok != 0) {
        fprintf(stderr, "[android-auto plugin] [audio channel] Could not send AV channel setup response. aa_device_send: %s\n", strerror(ok));
        return ok;
    }

    sync_android_auto_state(channel->device->aaplugin);

    return 0;
}

static int aa_audio_channel_on_avchannel_start_indication(struct aa_channel *channel, size_t payload_size, uint8_t *payload) {
    F1x__Aasdk__Proto__Messages__AVChannelStartIndication *start_indication;

    start_indication = f1x__aasdk__proto__messages__avchannel_start_indication__unpack(NULL, payload_size, payload);

    printf("[android-auto plugin] [audio channel] av channel start indication. config = %u, session = %d\n", start_indication->config, start_indication->session);

    channel->has_session = true;
    channel->session = start_indication->session;    

    f1x__aasdk__proto__messages__avchannel_start_indication__free_unpacked(start_indication, NULL);

    return 0;
}

static int aa_audio_channel_on_avchannel_stop_indication(struct aa_channel *channel, size_t payload_size, uint8_t *payload) {
    printf("[android-auto plugin] [audio channel] av channel stop indication.\n");
    
    return 0;
}

static int aa_audio_channel_on_avchannel_av_media_with_timestamp_indication(struct aa_channel *channel, struct aa_msg *msg) {
    F1x__Aasdk__Proto__Messages__AVMediaAckIndication ack_indication = F1X__AASDK__PROTO__MESSAGES__AVMEDIA_ACK_INDICATION__INIT;
    uint64_t timestamp;
    uint8_t *payload, *media;
    size_t payload_size, media_size, response_data_size;
    int ok;

    payload_size = msg->payload->size - 2;
    payload = msg->payload->pointer + 2;

    /**
     * Unpack the message.
     */
    timestamp = be64_to_cpu(*(uint64_t *) payload);

    media_size = payload_size - 8;
    media = payload + 8;

    printf("[android-auto plugin] [audio channel] av media with timestamp indication. timestamp: %llu, media_size: %u\n", timestamp, media_size);

    aa_msg_unrefp(&msg);

    /**
     * Send the response. (AV Media ACK Indication)
     */
    ack_indication.session = channel->session;
    ack_indication.value = 1;

    response_data_size = f1x__aasdk__proto__messages__avmedia_ack_indication__get_packed_size(&ack_indication);

    define_and_setup_aa_msg_on_stack(response_msg, response_data_size + 2, channel->id, AA_MSG_FLAG_ENCRYPTED);

    *(uint16_t *) response_msg.payload->pointer = cpu_to_be16(F1X__AASDK__PROTO__IDS__AVCHANNEL_MESSAGE__ENUM__AV_MEDIA_ACK_INDICATION);
    f1x__aasdk__proto__messages__avmedia_ack_indication__pack(&ack_indication, response_msg.payload->pointer + 2);

    ok = aa_device_send(channel->device, &response_msg);
    if (ok != 0) {
        fprintf(stderr, "[android-auto plugin] [audio channel] Could not send av channel av media ack indication. aa_device_send: %s\n", strerror(ok));
        return ok;
    }

    return 0;
}

static int aa_audio_channel_on_avchannel_av_media_indication(struct aa_channel *channel, struct aa_msg *msg) {
    F1x__Aasdk__Proto__Messages__AVMediaAckIndication ack_indication = F1X__AASDK__PROTO__MESSAGES__AVMEDIA_ACK_INDICATION__INIT;
    uint8_t *payload, *media;
    size_t payload_size, media_size, response_data_size;
    int ok;

    payload_size = msg->payload->size - 2;
    payload = msg->payload->pointer + 2;
    
    /**
     * Unpack the message.
     */
    media_size = payload_size;
    media = payload;

    printf("[android-auto plugin] [audio channel] AV media indication. media_size: %u\n", media_size);

    aa_msg_unrefp(&msg);

    /**
     * Send the response. (AV Media ACK Indication)
     */
    ack_indication.session = channel->session;
    ack_indication.value = 1;

    response_data_size = f1x__aasdk__proto__messages__avmedia_ack_indication__get_packed_size(&ack_indication);

    define_and_setup_aa_msg_on_stack(response_msg, response_data_size + 2, channel->id, AA_MSG_FLAG_ENCRYPTED);

    *(uint16_t *) response_msg.payload->pointer = cpu_to_be16(F1X__AASDK__PROTO__IDS__AVCHANNEL_MESSAGE__ENUM__AV_MEDIA_ACK_INDICATION);
    f1x__aasdk__proto__messages__avmedia_ack_indication__pack(&ack_indication, response_msg.payload->pointer + 2);

    ok = aa_device_send(channel->device, &response_msg);
    if (ok != 0) {
        fprintf(stderr, "[android-auto] [video service] Could not send av channel av media ack indication. aa_device_send: %s\n", strerror(ok));
        return ok;
    }

    return 0;
}

static int aa_audio_channel_on_message(struct aa_channel *channel, struct aa_msg *msg) {
    uint16_t message_id;
    uint8_t *payload;
    size_t payload_size;
    int ok;

    message_id = be16_to_cpu(*(uint16_t*) msg->payload->pointer);
    payload = msg->payload->pointer + 2;
    payload_size = msg->payload->size - 2;

    if (!(msg->flags & AA_MSG_FLAG_ENCRYPTED)) {
        printf("test\n");
    }

    switch (message_id) {
        case F1X__AASDK__PROTO__IDS__AVCHANNEL_MESSAGE__ENUM__SETUP_REQUEST:
            ok = aa_audio_channel_on_avchannel_setup_request(channel, payload_size, payload);
            break;
        case F1X__AASDK__PROTO__IDS__AVCHANNEL_MESSAGE__ENUM__START_INDICATION:
            printf("[android-auto plugin] [audio channel] start indication.\n");
            ok = aa_audio_channel_on_avchannel_start_indication(channel, payload_size, payload);
            break;
        case F1X__AASDK__PROTO__IDS__AVCHANNEL_MESSAGE__ENUM__STOP_INDICATION:
            printf("[android-auto plugin] [audio channel] stop indication.\n");
            ok = aa_audio_channel_on_avchannel_stop_indication(channel, payload_size, payload);
            break;
        case F1X__AASDK__PROTO__IDS__AVCHANNEL_MESSAGE__ENUM__AV_MEDIA_WITH_TIMESTAMP_INDICATION:
            printf("[android-auto plugin] [audio channel] av media with timestamp indication.\n");
            ok = aa_audio_channel_on_avchannel_av_media_with_timestamp_indication(channel, aa_msg_ref(msg));
            break;
        case F1X__AASDK__PROTO__IDS__AVCHANNEL_MESSAGE__ENUM__AV_MEDIA_INDICATION:
            printf("[android-auto plugin] [audio channel] av media indication.\n");
            ok = aa_audio_channel_on_avchannel_av_media_indication(channel, aa_msg_ref(msg));
            break;
        case F1X__AASDK__PROTO__IDS__CONTROL_MESSAGE__ENUM__CHANNEL_OPEN_REQUEST:
            ok = aa_channel_on_channel_open_request_msg(channel, payload_size, payload);
            break;
        default:
            fprintf(stderr, "[android-auto plugin] [audio channel] Unhandled message. message_id: %hu\n", message_id);
            aa_msg_unrefp(&msg);
            return EINVAL;
    }

    aa_msg_unrefp(&msg);

    return 0;
}

static int aa_audio_channel_fill_features(struct aa_channel *channel, F1x__Aasdk__Proto__Data__ChannelDescriptor *desc) {
    int ok;

    F1x__Aasdk__Proto__Data__AVChannel *av_channel = malloc(sizeof(F1x__Aasdk__Proto__Data__AVChannel));
    if (av_channel == NULL) {
        return ENOMEM;
    }
    
    F1x__Aasdk__Proto__Data__AudioConfig *audio_config = malloc(sizeof(F1x__Aasdk__Proto__Data__AudioConfig));
    if (audio_config == NULL) {
        free(av_channel);
        return ENOMEM;
    }
    
    F1x__Aasdk__Proto__Data__AudioConfig **paudio_configs = malloc(sizeof(F1x__Aasdk__Proto__Data__AudioConfig*));
    if (paudio_configs == NULL) {
        free(audio_config);
        free(av_channel);
        return ENOMEM;
    }

    f1x__aasdk__proto__data__avchannel__init(av_channel);
    f1x__aasdk__proto__data__audio_config__init(audio_config);
    
    paudio_configs[0] = audio_config;

    audio_config->sample_rate = channel->sample_rate;
    audio_config->bit_depth = channel->bit_depth;
    audio_config->channel_count = channel->channel_count;

    av_channel->stream_type = F1X__AASDK__PROTO__ENUMS__AVSTREAM_TYPE__ENUM__AUDIO;
    av_channel->has_audio_type = true;
    av_channel->audio_type = F1X__AASDK__PROTO__ENUMS__AUDIO_TYPE__ENUM__SYSTEM;
    av_channel->n_audio_configs = 1;
    av_channel->audio_configs = paudio_configs;
    av_channel->has_available_while_in_call = true;
    av_channel->available_while_in_call = true;    

    desc->channel_id = channel->id;
    desc->av_channel = av_channel;

    return 0;
}

static void aa_audio_channel_after_fill_features(struct aa_channel *channel, F1x__Aasdk__Proto__Data__ChannelDescriptor *desc) {
    free(desc->av_channel->audio_configs[0]);
    free(desc->av_channel->audio_configs);
    free(desc->av_channel);
}

static void aa_audio_channel_destroy(struct aa_channel *channel) {

}

struct aa_channel *aa_audio_channel_new(
    struct aa_device *device,
    enum aa_channel_id channel_id,
    F1x__Aasdk__Proto__Enums__AudioType__Enum audio_type,
    unsigned int sample_rate,
    unsigned int bit_depth,
    unsigned int channel_count
) {
    struct aa_channel *channel = aa_channel_new(device);
    if (channel == NULL) {
        return NULL;
    }

    channel->id = channel_id;
    channel->message_callback = aa_audio_channel_on_message;
    channel->fill_features_callback = aa_audio_channel_fill_features;
    channel->after_fill_features_callback = aa_audio_channel_after_fill_features;
    channel->destroy_callback = aa_audio_channel_destroy;
    channel->debug_channel_name = "audio";

    channel->audio_type = audio_type;
    channel->sample_rate = sample_rate;
    channel->bit_depth = bit_depth;
    channel->channel_count = channel_count;

    return channel;
}


static int aa_audio_input_channel_on_av_input_open_request(struct aa_channel *channel, size_t payload_size, uint8_t *payload) {
    F1x__Aasdk__Proto__Messages__AVInputOpenResponse open_response
        = F1X__AASDK__PROTO__MESSAGES__AVINPUT_OPEN_RESPONSE__INIT;
    F1x__Aasdk__Proto__Messages__AVInputOpenRequest *open_request;

    open_request = f1x__aasdk__proto__messages__avinput_open_request__unpack(NULL, payload_size, payload);

    printf(
        "[android-auto plugin] [audio input service] AV input open request. open: %s, anc: %s, ec: %s, max_unacked: ",
        open_request->open? "true" : "false",
        open_request->has_anc? (open_request->anc? "true" : "false") : "null",
        open_request->has_ec? (open_request->ec? "true" : "false") : "null"
    );

    if (open_request->has_max_unacked) {
        printf("%d\n", open_request->max_unacked);
    } else {
        printf("null\n");
    }

    f1x__aasdk__proto__messages__avinput_open_request__free_unpacked(open_request, NULL);

    return 0;
}

int aa_audio_input_channel_on_message(struct aa_channel *channel, struct aa_msg *msg) {
    uint16_t message_id;
    uint8_t *payload;
    size_t payload_size;
    int ok;
    
    message_id = be16_to_cpu(*(uint16_t*) msg->payload->pointer);
    payload = msg->payload->pointer + 2;
    payload_size = msg->payload->size - 2;
    
    switch (message_id) {
        case F1X__AASDK__PROTO__IDS__AVCHANNEL_MESSAGE__ENUM__AV_INPUT_OPEN_REQUEST:
            ok = aa_audio_input_channel_on_av_input_open_request(channel, payload_size, payload);
            break;
        case F1X__AASDK__PROTO__IDS__CONTROL_MESSAGE__ENUM__CHANNEL_OPEN_REQUEST:
            ok = aa_channel_on_channel_open_request_msg(channel, payload_size, payload);
            break;
        default:
            ok = EINVAL;
            break;
    }
    
    aa_msg_unrefp(&msg);
    
    return 0;
}

int aa_audio_input_channel_fill_features(struct aa_channel *channel, F1x__Aasdk__Proto__Data__ChannelDescriptor *desc) {
    int ok;

    F1x__Aasdk__Proto__Data__AVInputChannel *av_input_channel = malloc(sizeof(F1x__Aasdk__Proto__Data__AVInputChannel));
    if (av_input_channel == NULL) {
        return ENOMEM;
    }
    
    F1x__Aasdk__Proto__Data__AudioConfig *audio_config = malloc(sizeof(F1x__Aasdk__Proto__Data__AudioConfig));
    if (audio_config == NULL) {
        free(av_input_channel);
        return ENOMEM;
    }

    f1x__aasdk__proto__data__avinput_channel__init(av_input_channel);
    f1x__aasdk__proto__data__audio_config__init(audio_config);
    
    audio_config->sample_rate = 16000;
    audio_config->bit_depth = 16;
    audio_config->channel_count = 1;

    av_input_channel->stream_type = F1X__AASDK__PROTO__ENUMS__AVSTREAM_TYPE__ENUM__AUDIO;
    av_input_channel->audio_config = audio_config;
    av_input_channel->has_available_while_in_call = true;
    av_input_channel->available_while_in_call = true;    

    desc->channel_id = channel->id;
    desc->av_input_channel = av_input_channel;

    return 0;
}

void aa_audio_input_channel_after_fill_features(struct aa_channel *channel, F1x__Aasdk__Proto__Data__ChannelDescriptor *desc) {
    free(desc->av_input_channel->audio_config);
    free(desc->av_input_channel);
}

void aa_audio_input_channel_destroy(struct aa_channel *channel) {

}

struct aa_channel *aa_audio_input_channel_new(struct aa_device *device) {
    struct aa_channel *channel = aa_channel_new(device);
    if (channel == NULL) {
        return NULL;
    }

    channel->id = kAndroidAutoChannelAVInput;
    channel->message_callback = aa_audio_input_channel_on_message;
    channel->fill_features_callback = aa_audio_input_channel_fill_features;
    channel->after_fill_features_callback = aa_audio_input_channel_after_fill_features;
    channel->destroy_callback = aa_audio_input_channel_destroy;
    channel->debug_channel_name = "audio input";

    return channel;
}


int aa_wifi_channel_on_message(struct aa_channel *channel, struct aa_msg *msg) {
    uint16_t message_id;
    uint8_t *payload;
    size_t payload_size;
    int ok;
    
    message_id = be16_to_cpu(*(uint16_t*) msg->payload->pointer);
    payload = msg->payload->pointer + 2;
    payload_size = msg->payload->size - 2;
    
    switch (message_id) {
        case F1X__AASDK__PROTO__IDS__CONTROL_MESSAGE__ENUM__CHANNEL_OPEN_REQUEST:
            ok = aa_channel_on_channel_open_request_msg(channel, payload_size, payload);
            break;
        default:
            ok = EINVAL;
            break;
    }
    
    aa_msg_unrefp(&msg);
    
    return 0;
}

int aa_wifi_channel_fill_features(struct aa_channel *channel, F1x__Aasdk__Proto__Data__ChannelDescriptor *desc) {
    F1x__Aasdk__Proto__Data__WifiChannel *wifi_channel;
    char *ssid;
    int ok;

    wifi_channel = malloc(sizeof(F1x__Aasdk__Proto__Data__WifiChannel));
    if (wifi_channel == NULL) {
        return ENOMEM;
    }

    ssid = strdup("");
    if (ssid == NULL) {
        free(wifi_channel);
        return ENOMEM;
    }

    f1x__aasdk__proto__data__wifi_channel__init(wifi_channel);

    wifi_channel->ssid = ssid;

    desc->wifi_channel = wifi_channel;

    return 0;
}

void aa_wifi_channel_after_fill_features(struct aa_channel *channel, F1x__Aasdk__Proto__Data__ChannelDescriptor *desc) {
    free(desc->wifi_channel->ssid);
    free(desc->wifi_channel);
}

void aa_wifi_channel_destroy(struct aa_channel *channel) {

}

struct aa_channel *aa_wifi_channel_new(struct aa_device *device) {
    struct aa_channel *channel = aa_channel_new(device);
    if (channel == NULL) {
        return NULL;
    }

    channel->id = 14;
    channel->message_callback = aa_wifi_channel_on_message;
    channel->fill_features_callback = aa_wifi_channel_fill_features;
    channel->after_fill_features_callback = aa_wifi_channel_after_fill_features;
    channel->destroy_callback = aa_wifi_channel_destroy;
    channel->debug_channel_name = "wifi";

    return channel;
}