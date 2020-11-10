#ifndef ANDROID_AUTO_H_
#define ANDROID_AUTO_H_

#include <stdbool.h>
#include <libusb.h>

#include <openssl/ssl.h>
#include <openssl/engine.h>
#include <openssl/err.h>
#include <openssl/conf.h>
#include <gst/gst.h>
#include <gst/gstmemory.h>
#include <gst/gstpad.h>
#include <gst/allocators/gstdmabuf.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/video/gstvideometa.h>
#include <drm_fourcc.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <collection.h>
#include <aasdk/ChannelDescriptorData.pb-c.h>

#define GOOGLE_VENDOR_ID 0x18D1
#define AOAP_PRODUCT_ID 0x2D00
#define AOAP_WITH_ADB_PRODUCT_ID 0x2D01

#define TRANSFER_TIMEOUT_MS 60000

#define ANDROID_AUTO_METHOD_CHANNEL "flutterpi/android_auto"
#define ANDROID_AUTO_EVENT_CHANNEL "flutterpi/android_auto/events"

enum aoa_request {
    ACCESSORY_GET_PROTOCOL = 51,
    ACCESSORY_SEND_STRING = 52,
    ACCESSORY_START = 53
};

enum accessory_string {
    ACCESSORY_STRING_MANUFACTURER = 0,
    ACCESSORY_STRING_MODEL = 1,
    ACCESSORY_STRING_DESCRIPTION = 2,
    ACCESSORY_STRING_VERSION = 3,
    ACCESSORY_STRING_URI = 4,
    ACCESSORY_STRING_SERIAL = 5
};

enum aoa_query_type {
    kGetProtocolVersion,
    kSendManufacturer,
    kSendModel,
    kSendDescription,
    kSendVersion,
    kSendUri,
    kSendSerial,
    kStart
};

enum aa_xfer_buffer_type {
    kXferBufferHeap,
    kXferBufferLibusbDevMem,
    kXferBufferUserManaged
};

enum aa_transfer_direction {
    kTransferDirectionIn,
    kTransferDirectionOut
};

enum aa_msg_frame_type {
    kFrameTypeMiddle = 0,
    kFrameTypeFirst = 1,
    kFrameTypeLast = 2,
    kFrameTypeBulk = 3
};

enum aa_msg_frame_size_type {
    kFrameSizeShort,
    kFrameSizeExtended
};

enum aa_channel_id {
    kAndroidAutoChannelControl,
    kAndroidAutoChannelInput,
    kAndroidAutoChannelSensor,
    kAndroidAutoChannelVideo,
    kAndroidAutoChannelMediaAudio,
    kAndroidAutoChannelSpeechAudio,
    kAndroidAutoChannelSystemAudio,
    kAndroidAutoChannelAVInput,
    kAndroidAutoChannelBluetooth,
    kAndroidAutoChannelNone = 255
};

enum aa_device_connection {
    kUSB,
    kWifi,
    kBluetooth
};

struct aoa_switcher_args {
    libusb_context *context;
    libusb_device *device;
};

struct aoa_device {
    struct aaplugin *aaplugin;
    libusb_device *device;
};

struct aaplugin {
    libusb_context *libusb_context;
    libusb_hotplug_callback_handle hotplug_cb_handle;

    SSL_CTX *ssl_context;

    bool usb_enabled;
    bool bluetooth_enabled;
    bool wifi_enabled;

    struct {
        char *headunit_name;
        char *car_model;
        char *car_year;
        char *car_serial;
        bool left_hand_drive_vehicle;
        char *headunit_manufacturer;
        char *headunit_model;
        char *sw_build;
        char *sw_version;
        bool can_play_native_media_during_vr;
        bool hide_clock;
    } hu_info;

    struct aa_device *aa_device;

    bool event_channel_has_listener;
};

struct aa_xfer_buffer {
    enum aa_xfer_buffer_type type;

    uint8_t *pointer;
    size_t size, allocated_size;

    struct libusb_device_handle *libusb_device_handle;

    bool is_allocated;
    size_t n_refs;
};

struct aa_msg_assembly_data {
    bool is_constructing;
    struct aa_msg *msg;
    size_t offset;
};

struct aa_msg {
    struct aa_xfer_buffer *payload;
    enum aa_channel_id channel;
    uint8_t flags;

    bool is_allocated;
    size_t n_refs;
};

struct aa_channel {
    struct aa_device *device;
    int (*channel_open_request_callback)(struct aa_channel *channel, int32_t channel_id, int32_t priority);
    int (*message_callback)(struct aa_channel *channel, struct aa_msg *msg);
    void (*destroy_callback)(struct aa_channel *channel);
    int (*fill_features_callback)(struct aa_channel *channel, F1x__Aasdk__Proto__Data__ChannelDescriptor *channel_descriptor);
    void (*after_fill_features_callback)(struct aa_channel *channel, F1x__Aasdk__Proto__Data__ChannelDescriptor *channel_descriptor);
    void *userdata;
    enum aa_channel_id id;
    char *debug_channel_name;

    union {
        struct {
            bool has_session;
            int32_t session;
            EGLDisplay display;
            EGLSurface surface;
            EGLContext context;
            struct concurrent_pointer_set stale_textures;
            GstPipeline *pipeline;
            GstAppSrc *src;
            GstAppSink *sink;
            GstBin *decodebin;
            GstVideoInfo video_info;
            uint32_t drm_format;
            GMainLoop *g_main_loop;
            pthread_t g_main_loop_thread;
        };
        struct {
            F1x__Aasdk__Proto__Enums__AudioType__Enum audio_type;
            unsigned int sample_rate;
            unsigned int bit_depth;
            unsigned int channel_count;
        };
    };
};

struct aa_device {
    struct aaplugin *aaplugin;

    enum aa_device_connection connection;
    union {
        struct {
            libusb_device *usb_device;
            libusb_device_handle *usb_handle;
            struct libusb_endpoint_descriptor in_endpoint, out_endpoint;
        };
    };
    SSL *ssl;

    struct aa_msg_assembly_data msg_assembly_buffers[256];

    struct aa_xfer_buffer receive_buffers[2];
    struct {
        size_t start;
        size_t length;
    } receive_buffer_info[2];
    uint8_t receive_buffer_index;

    char *device_name;
    char *device_brand;

    struct pointer_set channels;

    bool is_focused;

    bool has_texture_id;
    int64_t texture_id;
};


#define AA_MSG_FRAME_TYPE_MASK (0b11)
#define AA_MSG_FLAG_CONTROL (1 << 2)
#define AA_MSG_FLAG_ENCRYPTED (1 << 3)

#define AA_MSG_HEADER_SIZE 2
#define AA_RECEIVE_TRANSFER_LENGTH 16384
#define AA_RECEIVE_TRANSFER_LENGTH_MASK 0x3FFF

/**
 * common
 */
static inline uint16_t cpu_to_be16(uint16_t x) {
    union {
		uint8_t  b8[2];
		uint16_t b16;
	} _tmp;
	_tmp.b8[0] = (uint8_t) (x >> 8);
	_tmp.b8[1] = (uint8_t) (x & 0xff);
	return _tmp.b16;
}

#define be16_to_cpu(x) cpu_to_be16(x)

static inline uint32_t cpu_to_be32(uint32_t x) {
    union {
		uint8_t  b8[4];
		uint32_t b32;
	} _tmp;
	_tmp.b8[0] = (uint8_t) (x >> 24);
	_tmp.b8[1] = (uint8_t) (x >> 16) & 0xFF;
    _tmp.b8[2] = (uint8_t) (x >> 8) & 0xFF;
    _tmp.b8[3] = (uint8_t) x & 0xFF;
	return _tmp.b32;
}

#define be32_to_cpu(x) cpu_to_be32(x)

static inline uint64_t cpu_to_be64(uint64_t x) {
    union {
		uint8_t  b8[8];
		uint64_t b64;
	} _tmp;
    _tmp.b8[0] = (uint8_t) (x >> 56) & 0xFF;
	_tmp.b8[1] = (uint8_t) (x >> 48) & 0xFF;
    _tmp.b8[2] = (uint8_t) (x >> 40) & 0xFF;
    _tmp.b8[3] = (uint8_t) (x >> 32) & 0xFF;
    _tmp.b8[4] = (uint8_t) (x >> 24) & 0xFF;
    _tmp.b8[5] = (uint8_t) (x >> 16) & 0xFF;
    _tmp.b8[6] = (uint8_t) (x >> 8) & 0xFF;
    _tmp.b8[7] = (uint8_t) x & 0xFF;
	return _tmp.b64;
}

#define be64_to_cpu(x) cpu_to_be64(x)


int get_errno_for_libusb_error(int libusb_error);

const char *get_str_for_libusb_error(int libusb_error);

/**
 * transfer buffers
 */
int aa_xfer_buffer_initialize_on_stack_for_device(struct aa_xfer_buffer *out, struct aa_device *dev, size_t size);

int aa_xfer_buffer_initialize_on_stack_from_pointer(struct aa_xfer_buffer *out, void *pointer, size_t size);

struct aa_xfer_buffer *aa_xfer_buffer_new_for_device(struct aa_device *dev, size_t size);

struct aa_xfer_buffer *aa_xfer_buffer_new_from_pointer(struct aa_device *dev, void *pointer, size_t size);

int aa_xfer_buffer_resize(struct aa_xfer_buffer *buffer, size_t new_size, bool allow_unused_memory);

struct aa_xfer_buffer *aa_xfer_buffer_ref(struct aa_xfer_buffer *buffer);

void aa_xfer_buffer_free(struct aa_xfer_buffer *buffer);

void aa_xfer_buffer_unref(struct aa_xfer_buffer *buffer);

void aa_xfer_buffer_unrefp(struct aa_xfer_buffer **buffer);


struct aa_msg *aa_msg_new(enum aa_channel_id channel_id, uint8_t flags, struct aa_xfer_buffer *payload);

struct aa_msg *aa_msg_new_with_new_buffer_for_device(enum aa_channel_id channel_id, uint8_t flags, struct aa_device *dev, size_t size);

struct aa_msg *aa_msg_new_with_new_buffer_from_pointer(enum aa_channel_id channel_id, uint8_t flags, struct aa_device *dev, void *pointer, size_t size);

struct aa_msg *aa_msg_ref(struct aa_msg *msg);

void aa_msg_unref(struct aa_msg *msg);

void aa_msg_unrefp(struct aa_msg **msg);


#define define_xfer_buffer_on_stack(var_name, size) \
    uint16_t var_name##__buffer[(size) >> 1]; \
    struct aa_xfer_buffer var_name; \
    aa_xfer_buffer_initialize_on_stack_from_pointer(&(var_name), var_name##__buffer, size);

#define define_and_setup_aa_msg_on_stack(var_name, __payload_size, __channel, __flags) \
    uint16_t var_name##__buffer[((__payload_size) +1) >> 1]; \
    struct aa_xfer_buffer var_name##__xfer_buffer; \
    aa_xfer_buffer_initialize_on_stack_from_pointer(&(var_name##__xfer_buffer), var_name##__buffer, __payload_size); \
    struct aa_msg var_name = { \
        .channel = (__channel), \
        .flags = (__flags), \
        .payload = &var_name##__xfer_buffer, \
        .is_allocated = false, \
        .n_refs = 0 \
    }; \

/**
 * android auto devices
 */
int aa_device_transfer(
    struct aa_device *dev,
    enum aa_transfer_direction direction,
    struct aa_xfer_buffer *buffer,
    size_t offset,
    size_t length,
    size_t *actually_transferred
);

int aa_device_send(
    struct aa_device *dev,
    const struct aa_msg *msg
);

int aa_device_receive_msg(struct aa_device *device, struct aa_msg **msg_out);

int aa_device_receive_msg_from_channel(struct aa_device *device, enum aa_channel_id channel, struct aa_msg **msg_out);

int aa_dev_manage(struct aa_device *device);

void *aa_dev_mgr_entry(void *arg);

/**
 * android auto channels
 */

void aa_channel_destroy(struct aa_channel *channel);

int aa_channel_on_message(struct aa_channel *channel, struct aa_msg *msg);

int aa_channel_fill_features(struct aa_channel *channel, F1x__Aasdk__Proto__Data__ChannelDescriptor *desc);

void aa_channel_after_fill_features(struct aa_channel *channel, F1x__Aasdk__Proto__Data__ChannelDescriptor *desc);

struct aa_channel *aa_audio_input_channel_new(struct aa_device *device);

struct aa_channel *aa_audio_channel_new(
    struct aa_device *device,
    enum aa_channel_id channel_id,
    F1x__Aasdk__Proto__Enums__AudioType__Enum audio_type,
    unsigned int sample_rate,
    unsigned int bit_depth,
    unsigned int channel_count
);

struct aa_channel *aa_sensor_channel_new(struct aa_device *device);

struct aa_channel *aa_video_channel_new(struct aa_device *device);

struct aa_channel *aa_input_channel_new(struct aa_device *device);

struct aa_channel *aa_wifi_channel_new(struct aa_device *device);

/**
 * plugin stuff
 */
int aaplugin_init(void);

int aaplugin_deinit(void);

int send_android_auto_state(
    struct aaplugin *plugin,
    bool connected,
    bool has_interface,
    enum aa_device_connection interface,
    char *device_name,
    char *device_brand,
    bool has_texture_id,
    int64_t texture_id,
    bool has_is_focused,
    bool is_focused
);

int sync_android_auto_state(
    struct aaplugin *plugin
);

#endif