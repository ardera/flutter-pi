#ifndef ANDROID_AUTO_H_
#define ANDROID_AUTO_H_

#include <stdbool.h>
#include <libusb.h>

#include <openssl/ssl.h>
#include <openssl/engine.h>
#include <openssl/err.h>
#include <openssl/conf.h>

#include <aasdk/ChannelDescriptorData.pb-c.h>

#define GOOGLE_VENDOR_ID 0x18D1
#define AOAP_PRODUCT_ID 0x2D00
#define AOAP_WITH_ADB_PRODUCT_ID 0x2D01

#define TRANSFER_TIMEOUT_MS 1000

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

struct aoa_switcher_args {
    libusb_context *context;
    libusb_device *device;
};

struct aoa_device {
    struct aaplugin *aaplugin;
    libusb_device *device;
};

#define AOA_DESCRIPTION "Android Auto for Flutter"
#define AOA_MANUFACTURER ""

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

enum aa_xfer_buffer_type {
    kXferBufferHeap,
    kXferBufferLibusbDevMem,
    kXferBufferUserManaged
};

struct aa_xfer_buffer {
    enum aa_xfer_buffer_type type;

    uint8_t *pointer;
    size_t size;

    struct libusb_device_handle *libusb_device_handle;
};

enum aa_transfer_direction {
    kTransferDirectionIn,
    kTransferDirectionOut
};

struct aa_msg_assembly_data {
    bool is_constructing;
    struct aa_xfer_buffer buffer;
    size_t offset;
    size_t total_payload_size;
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

struct aa_channel {
    struct aa_device *device;
    int (*message_callback)(struct aa_channel *channel, const struct aa_msg *msg);
    int (*destroy_callback)(struct aa_channel *channel);
    int (*fill_features_callback)(struct aa_channel *channel, F1x__Aasdk__Proto__Data__ChannelDescriptor *channel_descriptor);
    void (*after_fill_features_callback)(struct aa_channel *channel, F1x__Aasdk__Proto__Data__ChannelDescriptor *channel_descriptor);
    void *userdata;
    enum aa_channel_id id;
};

struct aa_msg {
    struct aa_xfer_buffer payload;
    enum aa_channel_id channel;
    uint8_t flags;
};

enum aa_device_connection {
    kUSB,
    kWifi,
    kBluetooth
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

int get_errno_for_libusb_error(int libusb_error);

const char *get_str_for_libusb_error(int libusb_error);

/**
 * transfer buffers
 */
int aa_xfer_buffer_initialize_for_device(struct aa_xfer_buffer *out, struct aa_device *dev, size_t size);

int aa_xfer_buffer_initialize_from_pointer(struct aa_xfer_buffer *out, void *pointer, size_t size);

void aa_xfer_buffer_free(struct aa_xfer_buffer *msg);

#define define_xfer_buffer_on_stack(var_name, size) \
    uint16_t var_name##__buffer[(size) >> 1]; \
    struct aa_xfer_buffer var_name; \
    aa_xfer_buffer_initialize_from_pointer(&(var_name), var_name##__buffer, size);

#define define_and_setup_aa_msg_on_stack(var_name, __payload_size, __channel, __flags) \
    uint16_t var_name##__buffer[((__payload_size) +1) >> 1]; \
    struct aa_msg var_name = { \
        .channel = (__channel), \
        .flags = (__flags) \
    }; \
    aa_xfer_buffer_initialize_from_pointer(&(var_name.payload), var_name##__buffer, __payload_size);

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

int aa_device_receive_msg(struct aa_device *device, struct aa_msg *msg_out);

int aa_device_receive_msg_from_channel(struct aa_device *device, enum aa_channel_id channel, struct aa_msg *msg_out);

int aa_dev_manage(struct aa_device *device);

void *aa_dev_mgr_entry(void *arg);

/**
 * plugin stuff
 */
int aaplugin_init(void);

int aaplugin_deinit(void);

#endif