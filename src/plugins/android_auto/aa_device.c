#define _XOPEN_SOURCE 500
#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
#include <memory.h>
#include <alloca.h>
#include <string.h>

#include <collection.h>
#include <plugins/android_auto/android_auto.h>
#include <aasdk/ControlMessageIdsEnum.pb-c.h>
#include <aasdk/VersionResponseStatusEnum.pb-c.h>
#include <aasdk/AuthCompleteIndicationMessage.pb-c.h>
#include <aasdk/ServiceDiscoveryRequestMessage.pb-c.h>
#include <aasdk/ServiceDiscoveryResponseMessage.pb-c.h>
#include <aasdk/AudioFocusRequestMessage.pb-c.h>
#include <aasdk/AudioFocusResponseMessage.pb-c.h>

static void aa_device_on_libusb_transfer_completed(struct libusb_transfer *xfer) {
    bool *completed = (bool*) xfer->user_data;

    *completed = true;
}

int aa_device_transfer(
    struct aa_device *dev,
    enum aa_transfer_direction direction,
    struct aa_xfer_buffer *buffer,
    size_t offset,
    size_t length,
    size_t *actually_transferred
) {
    struct libusb_transfer *xfer;
    bool completed;
    int ok;

    if (dev->connection == kUSB) {
        int actual_length;

        ok = libusb_bulk_transfer(
            dev->usb_handle,
            direction == kTransferDirectionOut ?
                dev->out_endpoint.bEndpointAddress :
                dev->in_endpoint.bEndpointAddress,
            buffer->pointer + offset,
            length,
            &actual_length,
            TRANSFER_TIMEOUT_MS
        );

        if (actually_transferred != NULL) {
            *actually_transferred = actual_length;
        }

        if (ok != 0) {
            fprintf(stderr, "[android-auto plugin] Error ocurred while executing USB bulk transfer. libusb_bulk_transfer: %s\n", get_str_for_libusb_error(ok));
            return get_errno_for_libusb_error(ok);
        }
        
        return 0;
    } else {
        return ENOTSUP;
    }

    return 0;
}

/**
 * Decryption/Encryption
 */

int aa_device_decrypt_write(struct aa_device *dev, uint8_t *source, size_t length) {
    size_t n_written;
    int ok;
    
    while (length > 0) {
        ok = BIO_write_ex(
            SSL_get_rbio(dev->ssl),
            source,
            length,
            &n_written
        );
        if (!ok) {
            ERR_print_errors_fp(stderr);
            return EIO;
        }

        length -= n_written;
        source += n_written;
    }

    return 0;
}

int aa_device_decrypt_pending(struct aa_device *dev) {
    SSL_read(dev->ssl, NULL, 0);
    return SSL_pending(dev->ssl);
}

int aa_device_decrypt_read(struct aa_device *dev, uint8_t *dest, size_t length) {
    size_t n_read;
    int ok;

    while (length > 0) {
        ok = SSL_read_ex(
            dev->ssl,
            dest,
            length,
            &n_read
        );
        if (!ok) {
            ERR_print_errors_fp(stderr);
            return EIO;
        }

        length -= n_read;
        dest += n_read;
    }

    return 0;
}

int aa_device_encrypt_write(struct aa_device *dev, uint8_t *source, size_t length) {
    size_t n_written;
    int ok;
    
    while (length > 0) {
        ok = SSL_write_ex(
            dev->ssl,
            source,
            length,
            &n_written
        );
        if (!ok) {
            ERR_print_errors_fp(stderr);
            return EIO;
        }

        length -= n_written;
        source += n_written;
    }

    return 0;
}

int aa_device_encrypt_pending(struct aa_device *dev) {
    return BIO_ctrl_pending(SSL_get_wbio(dev->ssl));
}

int aa_device_encrypt_read(struct aa_device *dev, uint8_t *dest, size_t length) {
    size_t n_read;
    int ok;

    while (length > 0) {
        ok = BIO_read_ex(SSL_get_wbio(dev->ssl), dest, length, &n_read);
        if (!ok) {
            ERR_print_errors_fp(stderr);
            return EIO;
        }

        length -= n_read;
        dest += n_read;
    }

    return 0;
}

/**
 * Sending Messages
 */

/**
 * 
 */
int aa_device_send(
    struct aa_device *dev,
    const struct aa_msg *msg
) {
    struct aa_xfer_buffer *payload_buffer;
    uint16_t frame_size;
    bool is_multi_frame;
    int ok;

    if (msg->flags & AA_MSG_FLAG_ENCRYPTED) {
        ok = aa_device_encrypt_write(dev, msg->payload->pointer, msg->payload->size);
        if (ok != 0) {
            return ok;
        }

        ok = aa_device_encrypt_pending(dev);
        
        payload_buffer = aa_xfer_buffer_new_for_device(
            dev,
            ok
        );
        if (payload_buffer == NULL) {
            return ENOMEM;
        }

        ok = aa_device_encrypt_read(dev, payload_buffer->pointer, payload_buffer->size);
        if (ok != 0) {
            aa_xfer_buffer_unrefp(&payload_buffer);
            return ok;
        }
    } else {
        payload_buffer = (struct aa_xfer_buffer *) msg->payload;
    }
    
    printf("sending message on channel %hhu...\n", msg->channel);

    frame_size = min(payload_buffer->size, UINT16_MAX);
    is_multi_frame = frame_size < payload_buffer->size;

    define_xfer_buffer_on_stack(frame_header_and_size, 4 + (is_multi_frame? 4 : 0))

    // initialize the frame header
    frame_header_and_size.pointer[0] = msg->channel;
    frame_header_and_size.pointer[1] = msg->flags | (is_multi_frame? kFrameTypeFirst : kFrameTypeBulk);

    // initialize the frame size
    *(uint16_t*) (frame_header_and_size.pointer + 2) = cpu_to_be16(frame_size);
    
    // If we have multiple frames, also initialize the total_size value of the frame size
    if (is_multi_frame) {
        *(uint32_t*) (frame_header_and_size.pointer + 4) = cpu_to_be32(payload_buffer->size);
    }

    // send over the frame header
    ok = aa_device_transfer(
        dev,
        kTransferDirectionOut,
        &frame_header_and_size,
        0,
        frame_header_and_size.size,
        NULL
    );
    if (ok != 0) {
        goto fail_maybe_unref_msg;
    }

    size_t offset = 0;
    size_t remaining = payload_buffer->size;
    while (remaining > 0) {
        size_t length = min(remaining, frame_size);
        // TODO: this is hacky. The continuation frame header should be sent after
        // [frame_size] bytes were sent. Currently, the continuation frame header after each individual
        // USB transfer, if the offset is != 0.
        if (offset != 0) {
            define_xfer_buffer_on_stack(continuation_frame_header, 4);

            continuation_frame_header.pointer[0] = msg->channel;
            continuation_frame_header.pointer[1] = msg->flags | ((length == remaining)? kFrameTypeLast : kFrameTypeMiddle);
            *(uint16_t*) (continuation_frame_header.pointer + 2) = cpu_to_be16(length);

            ok = aa_device_transfer(
                dev,
                kTransferDirectionOut,
                &continuation_frame_header,
                0,
                continuation_frame_header.size,
                NULL
            );
            if (ok != 0) {
                goto fail_maybe_unref_msg;
            }
        }

        ok = aa_device_transfer(
            dev,
            kTransferDirectionOut,
            payload_buffer,
            offset,
            length,
            &length
        );
        if (ok != 0) {
            goto fail_maybe_unref_msg;
        }

        offset += length;
        remaining -= length;
    }

    if (msg->flags & AA_MSG_FLAG_ENCRYPTED) {
        aa_xfer_buffer_unrefp(&payload_buffer);
    }

    printf("done.\n");

    return 0;

    fail_maybe_unref_msg:
    if (msg->flags & AA_MSG_FLAG_ENCRYPTED) {
        aa_xfer_buffer_unrefp(&payload_buffer);
    }
    
    fail_return_ok:
    return ok;
}

/**
 * Receiving Messages
 */
int aa_device_receive_raw(struct aa_device *dev, struct aa_xfer_buffer *buffer, size_t offset, size_t *n_received) {
    int ok;

    if ((buffer->size - offset) >= dev->receive_buffers[0].size) {
        ok = aa_device_transfer(dev, kTransferDirectionIn, buffer, offset, dev->receive_buffers[0].size, n_received);
        if (ok != 0) {
            return ok;
        }
    } else {
        return EINVAL;
    }

    return 0;
}

void aa_device_fill_xfer_buffer(
    struct aa_device *dev,
    struct aa_xfer_buffer *dest,
    int receive_buffer_index,
    size_t offset,
    size_t length
) {
    int i = receive_buffer_index;

    struct aa_xfer_buffer *rbuf = dev->receive_buffers + i;
    size_t *rbuf_start = &dev->receive_buffer_info[i].start;
    size_t *rbuf_length = &dev->receive_buffer_info[i].length;

    memcpy(dest->pointer + offset, rbuf->pointer + *rbuf_start, length);

    *rbuf_start = *rbuf_start + length;
    *rbuf_length -= length;

    if (*rbuf_length == 0) {
        *rbuf_start = 0;
    }
}

int aa_device_receive(
    struct aa_device *dev,
    struct aa_xfer_buffer *buffer,
    size_t offset,
    size_t length
) {
    int ok;

    while (length > 0) {
        size_t n_added;

        if (dev->receive_buffer_info[dev->receive_buffer_index].length == 0) {
            if (length >= dev->receive_buffers[dev->receive_buffer_index].size) {
                ok = aa_device_receive_raw(
                    dev,
                    buffer,
                    offset,
                    &n_added
                );
                if (ok != 0) {
                    return ok;
                }
            } else {
                ok = aa_device_receive_raw(
                    dev,
                    dev->receive_buffers + dev->receive_buffer_index,
                    dev->receive_buffer_info[dev->receive_buffer_index].start,
                    &dev->receive_buffer_info[dev->receive_buffer_index].length
                );
                if (ok != 0) {
                    return ok;
                }
            }
        }

        if (dev->receive_buffer_info[dev->receive_buffer_index].length > 0) {
            n_added = min(length, dev->receive_buffer_info[dev->receive_buffer_index].length);
            aa_device_fill_xfer_buffer(dev, buffer, dev->receive_buffer_index, offset, n_added);
        }

        offset += n_added;
        length -= n_added;
    }

    return 0;
}

static int aa_device_receive_frame_header(struct aa_device *device, enum aa_channel_id *channel_out, uint8_t *flags_out) {
    define_xfer_buffer_on_stack(buffer, 2);
    int ok;

    ok = aa_device_receive(device, &buffer, 0, buffer.size);
    if (ok != 0) {
        return ok;
    }

    if (channel_out != NULL) {
        *channel_out = (enum aa_channel_id) buffer.pointer[0];
    }
    if (flags_out != NULL) {
        *flags_out = buffer.pointer[1];
    }

    return 0;
}

static int aa_device_receive_frame_size(struct aa_device *device, enum aa_msg_frame_size_type frame_size_type, uint16_t *frame_size_out, uint32_t *total_size_out) {
    define_xfer_buffer_on_stack(buffer, (frame_size_type == kFrameSizeShort) ? 2 : 6);
    int ok;

    ok = aa_device_receive(device, &buffer, 0, buffer.size);
    if (ok != 0) {
        return ok;
    }

    if (frame_size_out != NULL) {
        *frame_size_out = be16_to_cpu(*(uint16_t*) (buffer.pointer + 0));
    }
    if ((total_size_out != NULL) && (frame_size_type == kFrameSizeExtended)) {
        *total_size_out = be32_to_cpu(*(uint32_t*) (buffer.pointer + 2));
    }

    return 0;
}

static int aa_device_receive_frame_payload(struct aa_device *device, struct aa_xfer_buffer *buffer, size_t offset, size_t length, bool decrypt) {
    int ok;

    ok = aa_device_receive(device, buffer, offset, length);
    if (ok != 0) {
        return ok;
    }

    return 0;
}

static int aa_device_receive_frame(struct aa_device *device, struct aa_msg **msg_out) {
    enum aa_msg_frame_type frame_type;
    enum aa_channel_id channel;
    struct aa_msg *msg;
    uint32_t total_size;
    uint32_t offset;
    uint16_t frame_size;
    uint8_t flags;
    int ok;

    msg = NULL;
    *msg_out = NULL;

    ok = aa_device_receive_frame_header(device, &channel, &flags);
    if (ok != 0) {
        goto fail_return_ok;
    }

    frame_type = (enum aa_msg_frame_type) flags & AA_MSG_FRAME_TYPE_MASK;

    ok = aa_device_receive_frame_size(device, frame_type == kFrameTypeFirst ? kFrameSizeExtended : kFrameSizeShort, &frame_size, &total_size);
    if (ok != 0) {
        goto fail_reset_msg_construction_buffer;
    }

    if (frame_type != kFrameTypeFirst) {
        total_size = frame_size;
    }

    if ((frame_type == kFrameTypeFirst) || (frame_type == kFrameTypeBulk)) {
        msg = aa_msg_new_with_new_buffer_for_device(channel, flags & ~AA_MSG_FRAME_TYPE_MASK, device, total_size);
        if (msg == NULL) {
            goto fail_reset_msg_construction_buffer;
        }

        offset = 0;

        if (frame_type == kFrameTypeFirst) {
            device->msg_assembly_buffers[channel].is_constructing = true;
            device->msg_assembly_buffers[channel].msg = aa_msg_ref(msg);
            device->msg_assembly_buffers[channel].offset = offset;
        }
    } else {
        if (device->msg_assembly_buffers[channel].is_constructing == false) {
            fprintf(stderr, "[android-auto plugin] ERROR: It appears some of the data frames of an android auto message were missed.\n");
            return EINVAL;
        } else {
            msg = aa_msg_ref(device->msg_assembly_buffers[channel].msg);
            offset = device->msg_assembly_buffers[channel].offset;

            size_t remaining_space = msg->payload->size - offset;

            if (frame_size > remaining_space) {
                fprintf(
                    stderr,
                    "[android-auto plugin] Message buffer has less space remaining than the frame size reported by the android auto device.\n"
                    "                      Resizing the transfer buffer so the frame fits in. (This is expensive!)\n"
                    "                      (frame is %u bytes too large)\n",
                    frame_size - remaining_space
                );

                size_t new_total_size = offset + frame_size;

                ok = aa_xfer_buffer_resize(msg->payload, new_total_size, false);
                if (ok != 0) {
                    fprintf(stderr, "[android-auto plugin] Could not resize the transfer buffer!\n");
                    ok = EPROTO;
                    goto fail_unref_msg;
                }
            } else if ((frame_type == kFrameTypeLast) && (frame_size < remaining_space)) {
                fprintf(
                    stderr,
                    "[android-auto plugin] Frame size of the last frame of a message is smaller than the remaining space in the message.\n"
                    "                      Limit\n"
                    "                      (frame is %u bytes too large)\n",
                    frame_size - remaining_space
                );

                size_t new_total_size = offset + frame_size;

                ok = aa_xfer_buffer_resize(msg->payload, new_total_size, false);
                if (ok != 0) {
                    fprintf(stderr, "[android-auto plugin] Could not resize the transfer buffer!\n");
                    ok = EPROTO;
                    goto fail_unref_msg;
                }
            }
        }
    }

    ok = aa_device_receive_frame_payload(device, msg->payload, offset, frame_size, flags & AA_MSG_FLAG_ENCRYPTED);
    if (ok != 0) {
        goto fail_unref_msg;
    }

    offset += frame_size;

    if ((frame_type == kFrameTypeFirst) || (frame_type == kFrameTypeMiddle)) {
        device->msg_assembly_buffers[channel].offset = offset;
    } else if ((frame_type == kFrameTypeLast) || (frame_type == kFrameTypeBulk)) {
        if (flags & AA_MSG_FLAG_ENCRYPTED) {
            ok = aa_device_decrypt_write(device, msg->payload->pointer, msg->payload->size);
            if (ok != 0) {
                goto fail_unref_msg;
            }

            ok = aa_device_decrypt_pending(device);
            if (ok <= 0) {
                ok = EIO;
                goto fail_unref_msg;
            }

            if ((unsigned int) ok > msg->payload->size) {
                printf("[android-auto plugin] Decrypted message is larger than transfer buffer. The transfer buffer needs to be expanded to the decrypted contents fit in.\n"
                       "                      (decrypted size: %d, buffer size: %d)\n", ok, msg->payload->size);
            }

            ok = aa_xfer_buffer_resize(msg->payload, ok, false);
            if (ok != 0) {
                fprintf(stderr, "[android-auto plugin] Could not resize transfer buffer.\n");
                ok = EIO;
                goto fail_unref_msg;
            }

            ok = aa_device_decrypt_read(device, msg->payload->pointer, msg->payload->size);
            if (ok != 0) {
                goto fail_unref_msg;
            }
        }

        if (frame_type == kFrameTypeLast) {
            aa_msg_unrefp(&device->msg_assembly_buffers[channel].msg);
            device->msg_assembly_buffers[channel].is_constructing = false;
        }

        *msg_out = aa_msg_ref(msg);
    }

    aa_msg_unrefp(&msg);

    return 0;


    fail_unref_msg:
    if (msg != NULL) {
        aa_msg_unrefp(&msg);
    }

    fail_reset_msg_construction_buffer:
    device->msg_assembly_buffers[channel].is_constructing = false;

    fail_return_ok:
    return ok;
}

int aa_device_receive_msg(struct aa_device *device, struct aa_msg **msg_out) {
    int ok;

    do {
        ok = aa_device_receive_frame(device, msg_out);
        if (ok != 0) {
            return ok;
        }
    } while (*msg_out == NULL);

    return 0;
}

int aa_device_receive_msg_from_channel(struct aa_device *device, enum aa_channel_id channel, struct aa_msg **msg_out) {
    int ok;

    do {
        ok = aa_device_receive_msg(device, msg_out);
        if (ok != 0) {
            return ok;
        }
    } while ((*msg_out) == NULL && (*msg_out)->channel != channel);

    return 0;
}


static int do_version_request(struct aa_device *device) {
    int ok;

    // send version request.
    {
        define_and_setup_aa_msg_on_stack(msg, 6, kAndroidAutoChannelControl, 0);

        uint16_t *msg_payload = (uint16_t*) msg.payload->pointer;
        msg_payload[0] = cpu_to_be16(F1X__AASDK__PROTO__IDS__CONTROL_MESSAGE__ENUM__VERSION_REQUEST);
        msg_payload[1] = cpu_to_be16(1);
        msg_payload[2] = cpu_to_be16(1);

        ok = aa_device_send(device, &msg);
        if (ok != 0) {
            return ok;
        }
    }
    
    // receive version response.
    {
        struct aa_msg *msg;
        ok = aa_device_receive_msg_from_channel(
            device,
            kAndroidAutoChannelControl,
            &msg
        );
        if (ok != 0) {
            return ok;
        }

        uint16_t *payload = (uint16_t*) msg->payload->pointer;

        uint16_t msg_id = be16_to_cpu(payload[0]);

        if (msg_id != F1X__AASDK__PROTO__IDS__CONTROL_MESSAGE__ENUM__VERSION_RESPONSE) {
            fprintf(stderr, "[android-auto plugin] Error: android auto device didn't return a version response.\n");
            return EPROTO;
        }

        size_t n_msg_elements = msg->payload->size / sizeof(uint16_t);
        uint16_t *msg_elements = payload + 1;

        if (n_msg_elements >= 3) {
            uint16_t major = be16_to_cpu(msg_elements[0]);
            uint16_t minor = be16_to_cpu(msg_elements[1]);
            F1x__Aasdk__Proto__Enums__VersionResponseStatus__Enum status = be16_to_cpu(msg_elements[2]);

            aa_msg_unrefp(&msg);

            if (status == F1X__AASDK__PROTO__ENUMS__VERSION_RESPONSE_STATUS__ENUM__MISMATCH) {
                fprintf(stderr, "[android-auto plugin] Error: android auto device returned a version mismatch.\n");
                return EINVAL;
            } else if (status == F1X__AASDK__PROTO__ENUMS__VERSION_RESPONSE_STATUS__ENUM__MATCH) {
                printf("[android-auto plugin] Android auto device returned version match.\n");
            }
        } else {
            aa_msg_unrefp(&msg);

            fprintf(stderr, "[android-auto plugin] Error: android auto device returned invalid version response.\n");
            return EINVAL;
        }
    }

    return 0;
}

int aa_transport_bio_write(BIO *bio, const char *data, size_t s, size_t *ss) {
    return 1;
}

int aa_transport_bio_read(BIO *bio, char *data, size_t s, size_t *ss) {
    return 1;
}

int aa_transport_bio_puts(BIO *bio, const char *str) {
    return 1;
}

int aa_transport_bio_gets(BIO *bio, char *str_out, int length) {
    return 1;
}

long aa_transport_bio_ctrl(BIO *bio, int cmd, long larg, void *parg) {
    return 1;
}

int aa_transport_bio_create(BIO *bio) {
    //BIO_set_shutdown(bio, 1);
    //BIO_set_init(bio, 1);
    return 1;
}

int aa_transport_bio_destroy(BIO *bio) {
    return 1;
}

BIO *BIO_new_aa_transport(struct aa_device *device) {
    return NULL;
}

static int do_handshake(struct aa_device *device) {
    int ok;

    while (1) {
        ok = SSL_do_handshake(device->ssl);
        if (ok == 1) {
            printf("[android-auto plugin] SSL handshake completed!\n");
            break;
        } else if (ok == 0) {
            printf("[android-auto plugin] SSL handshake error!\n");
            ERR_print_errors_fp(stderr);
            break;
        } else if (ok < 0) {
            ok = SSL_get_error(device->ssl, ok);
            if (ok == SSL_ERROR_WANT_READ) {
                printf("flushing SSL buffers to android auto device...\n");
                {
                    // We should send the data from the wbio to the android auto device.
                    size_t pending = BIO_ctrl_pending(SSL_get_wbio(device->ssl));

                    define_and_setup_aa_msg_on_stack(msg, pending + 2, kAndroidAutoChannelControl, 0);

                    *(uint16_t*) msg.payload->pointer = cpu_to_be16(F1X__AASDK__PROTO__IDS__CONTROL_MESSAGE__ENUM__SSL_HANDSHAKE);

                    size_t offset = 2;
                    size_t remaining = pending;
                    while (remaining > 0) {
                        ok = BIO_read(SSL_get_wbio(device->ssl), msg.payload->pointer + offset, remaining);
                        if (ok <= 0) {
                            ERR_print_errors_fp(stderr);
                            return EIO;
                        }

                        remaining -= ok;
                        offset += ok;
                    }

                    ok = aa_device_send(device, &msg);
                    if (ok != 0) {
                        return ok;
                    }
                }

                {
                    // We should read the data from the android auto device into the rbio.
                    struct aa_msg *msg;

                    ok = aa_device_receive_msg_from_channel(device, kAndroidAutoChannelControl, &msg);
                    if (ok != 0) {
                        return ok;
                    }

                    uint16_t message_id = be16_to_cpu(*(uint16_t*) msg->payload->pointer);

                    if (message_id == F1X__AASDK__PROTO__IDS__CONTROL_MESSAGE__ENUM__SSL_HANDSHAKE) {
                        size_t remaining = msg->payload->size - 2;
                        size_t offset = 2;
                        
                        while (remaining > 0) {
                            size_t written;
                            ok = BIO_write_ex(SSL_get_rbio(device->ssl), msg->payload->pointer + offset, remaining, &written);
                            if (!ok) {
                                ERR_print_errors_fp(stderr);
                                aa_msg_unrefp(&msg);
                                return EIO;
                            }

                            remaining -= written;
                            offset += written;
                        }
                    } else {
                        printf(
                            "[android-auto plugin] SSL handshake successful. Got message id %s\n",
                            protobuf_c_enum_descriptor_get_value(
                                &f1x__aasdk__proto__ids__control_message__enum__descriptor,
                                message_id
                            )->name
                        );
                        break;
                    }

                    aa_msg_unrefp(&msg);
                }
                printf("finished flushing buffers!\n");
            } else {
                fprintf(
                    stderr,
                    "[android-auto plugin] Error while performing SSL handshake. SSL_do_handshake: %s\n",
                    (ok == SSL_ERROR_NONE) ? "SSL_ERROR_NONE" :
                    (ok == SSL_ERROR_WANT_READ) ? "SSL_ERROR_WANT_READ" :
                    (ok == SSL_ERROR_WANT_WRITE) ? "SSL_ERROR_WANT_WRITE" :
                    (ok == SSL_ERROR_WANT_X509_LOOKUP) ? "SSL_ERROR_WANT_X509_LOOKUP" :
                    (ok == SSL_ERROR_SYSCALL) ? "SSL_ERROR_SYSCALL" :
                    (ok == SSL_ERROR_ZERO_RETURN) ? "SSL_ERROR_ZERO_RETURN" :
                    (ok == SSL_ERROR_WANT_CONNECT) ? "SSL_ERROR_WANT_CONNECT" :
                    (ok == SSL_ERROR_WANT_ACCEPT) ? "SSL_ERROR_WANT_ACCEPT" :
                    (ok == SSL_ERROR_WANT_ASYNC) ? "SSL_ERROR_WANT_ASYNC" :
                    (ok == SSL_ERROR_WANT_ASYNC_JOB) ? "SSL_ERROR_WANT_ASYNC_JOB" :
                    (ok == SSL_ERROR_WANT_CLIENT_HELLO_CB) ? "SSL_ERROR_WANT_CLIENT_HELLO_CB" :
                    "?"
                );
            }
        }
    }
    
    {
        F1x__Aasdk__Proto__Messages__AuthCompleteIndication auth_complete = F1X__AASDK__PROTO__MESSAGES__AUTH_COMPLETE_INDICATION__INIT;

        size_t size = f1x__aasdk__proto__messages__auth_complete_indication__get_packed_size(&auth_complete);
        
        define_and_setup_aa_msg_on_stack(msg, size + 2, kAndroidAutoChannelControl, 0);

        *(uint16_t *) msg.payload->pointer = cpu_to_be16(F1X__AASDK__PROTO__IDS__CONTROL_MESSAGE__ENUM__AUTH_COMPLETE);
        f1x__aasdk__proto__messages__auth_complete_indication__pack(&auth_complete, msg.payload->pointer + 2);

        ok = aa_device_send(device, &msg);
        if (ok != 0) {
            return ok;
        }
    }

    return 0;
}

static int create_channels(struct aa_device *device) {
    pset_put(&device->channels, aa_audio_input_channel_new(device));

    /*
    pset_put(
        &device->channels,
        aa_audio_channel_new(
            device,
            kAndroidAutoChannelMediaAudio,
            F1X__AASDK__PROTO__ENUMS__AUDIO_TYPE__ENUM__MEDIA,
            48000,
            16,
            2
        )
    );

    pset_put(
        &device->channels,
        aa_audio_channel_new(
            device,
            kAndroidAutoChannelSpeechAudio,
            F1X__AASDK__PROTO__ENUMS__AUDIO_TYPE__ENUM__SPEECH,
            16000,
            16,
            1
        )
    );
    */

    pset_put(
        &device->channels,
        aa_audio_channel_new(
            device,
            kAndroidAutoChannelSpeechAudio,
            F1X__AASDK__PROTO__ENUMS__AUDIO_TYPE__ENUM__SYSTEM,
            16000,
            16,
            1
        )
    );
    
    pset_put(&device->channels, aa_sensor_channel_new(device));

    pset_put(&device->channels, aa_video_channel_new(device));

    pset_put(&device->channels, aa_input_channel_new(device));

    pset_put(&device->channels, aa_wifi_channel_new(device));

    return 0;
}

static int on_service_discovery_request(struct aa_device *device, size_t payload_size, uint8_t *payload) {
    F1x__Aasdk__Proto__Messages__ServiceDiscoveryResponse sdrep = F1X__AASDK__PROTO__MESSAGES__SERVICE_DISCOVERY_RESPONSE__INIT;
    F1x__Aasdk__Proto__Messages__ServiceDiscoveryRequest *sdreq;
    struct aa_channel *channel;
    struct aaplugin *aaplugin;
    size_t response_payload_size;
    int ok, i;

    aaplugin = device->aaplugin;

    sdreq = f1x__aasdk__proto__messages__service_discovery_request__unpack(NULL, payload_size, payload);
    if (sdreq == NULL) {
        fprintf(stderr, "[android-auto plugin] [control channel] Could not unpack service discovery request.\n");
        return EINVAL;
    }
    
    printf("[android-auto plugin] Got Service Discovery Request. device name: %s, device_brand: %s\n", sdreq->device_name, sdreq->device_brand);

    device->device_name = strdup(sdreq->device_name);
    device->device_brand = strdup(sdreq->device_brand);

    f1x__aasdk__proto__messages__service_discovery_request__free_unpacked(sdreq, NULL);
    
    sdrep.head_unit_name = aaplugin->hu_info.headunit_name;
    sdrep.car_model = aaplugin->hu_info.car_model;
    sdrep.car_year = aaplugin->hu_info.car_year;
    sdrep.car_serial = aaplugin->hu_info.car_serial;
    sdrep.left_hand_drive_vehicle = aaplugin->hu_info.left_hand_drive_vehicle;
    sdrep.headunit_manufacturer = aaplugin->hu_info.headunit_manufacturer;
    sdrep.sw_build = aaplugin->hu_info.sw_build;
    sdrep.sw_version = aaplugin->hu_info.sw_version;
    sdrep.can_play_native_media_during_vr = aaplugin->hu_info.can_play_native_media_during_vr;
    sdrep.has_hide_clock = true;
    sdrep.hide_clock = aaplugin->hu_info.hide_clock;

    
    F1x__Aasdk__Proto__Data__ChannelDescriptor channels[pset_get_count_pointers(&device->channels)];
    F1x__Aasdk__Proto__Data__ChannelDescriptor *pchannels[pset_get_count_pointers(&device->channels)];

    i = 0;
    for_each_pointer_in_pset(&device->channels, channel) {
        channels[i] = (F1x__Aasdk__Proto__Data__ChannelDescriptor) F1X__AASDK__PROTO__DATA__CHANNEL_DESCRIPTOR__INIT;

        ok = aa_channel_fill_features(channel, channels + i);
        if (ok != 0) {
            int j = 0;
            for_each_pointer_in_pset(&device->channels, channel) {
                if (j == i) break;
                aa_channel_after_fill_features(channel, channels + j);
                j++;
            }

            return ok;
        }

        pchannels[i] = channels + i;
        i++;
    }
    
    sdrep.n_channels = pset_get_count_pointers(&device->channels);
    sdrep.channels = pchannels;

    response_payload_size = f1x__aasdk__proto__messages__service_discovery_response__get_packed_size(&sdrep);
    
    define_and_setup_aa_msg_on_stack(response_msg, response_payload_size + 2, kAndroidAutoChannelControl, AA_MSG_FLAG_ENCRYPTED);

    *(uint16_t *) response_msg.payload->pointer = cpu_to_be16(F1X__AASDK__PROTO__IDS__CONTROL_MESSAGE__ENUM__SERVICE_DISCOVERY_RESPONSE);
    f1x__aasdk__proto__messages__service_discovery_response__pack(&sdrep, response_msg.payload->pointer + 2);
    
    ok = aa_device_send(device, &response_msg);

    i = 0;
    for_each_pointer_in_pset(&device->channels, channel) {
        aa_channel_after_fill_features(channel, channels + i);
        pchannels[i] = channels + i;
        i++;
    }

    return ok;
}

static int on_audio_focus_request(struct aa_device *device, size_t payload_size, uint8_t *payload) {
    F1x__Aasdk__Proto__Messages__AudioFocusResponse response = F1X__AASDK__PROTO__MESSAGES__AUDIO_FOCUS_RESPONSE__INIT;
    F1x__Aasdk__Proto__Messages__AudioFocusRequest *request;
    size_t response_packed_size;

    request = f1x__aasdk__proto__messages__audio_focus_request__unpack(NULL, payload_size, payload);
    if (request == NULL) {
        fprintf(stderr, "[android-auto plugin] [control channel] Could not unpack audio focus request.\n");
        return EPROTO;
    }

    printf(
        "[android-auto plugin] [control channel] audio focus request. audio_focus_type: %s\n",
        protobuf_c_enum_descriptor_get_value(&f1x__aasdk__proto__enums__audio_focus_type__enum__descriptor, request->audio_focus_type)->name
    );

    response.audio_focus_state = request->audio_focus_type == F1X__AASDK__PROTO__ENUMS__AUDIO_FOCUS_TYPE__ENUM__RELEASE?
        F1X__AASDK__PROTO__ENUMS__AUDIO_FOCUS_STATE__ENUM__LOSS :
        F1X__AASDK__PROTO__ENUMS__AUDIO_FOCUS_STATE__ENUM__GAIN;

    f1x__aasdk__proto__messages__audio_focus_request__free_unpacked(request, NULL);

    response_packed_size = f1x__aasdk__proto__messages__audio_focus_response__get_packed_size(&response);
    define_and_setup_aa_msg_on_stack(response_msg, response_packed_size + 2, kAndroidAutoChannelControl, AA_MSG_FLAG_ENCRYPTED);
    *(uint16_t *) response_msg.payload->pointer = cpu_to_be16(F1X__AASDK__PROTO__IDS__CONTROL_MESSAGE__ENUM__AUDIO_FOCUS_RESPONSE);
    f1x__aasdk__proto__messages__audio_focus_response__pack(&response, response_msg.payload->pointer + 2);

    return aa_device_send(device, &response_msg);
}

int aa_dev_manage(struct aa_device *device) {
    struct aa_channel *channel;
    struct aaplugin *aaplugin;
    struct aa_msg *msg;
    int ok;

    aaplugin = device->aaplugin;

    printf("managing aa device.\n");

    printf("sending version request.\n");
    ok = do_version_request(device);
    if (ok != 0) {
        return ok;
    }

    printf("doing handshake.\n");
    ok = do_handshake(device);
    if (ok != 0) {
        return ok;
    }

    ok = create_channels(device);
    if (ok != 0) {
        return ok;
    }

    printf("handling messages...\n");
    while (1) {
        ok = aa_device_receive_msg(device, &msg);
        if (ok != 0) {
            return ok;
        }

        printf("received message on channel %hhu, encrypted? %s\n", msg->channel, msg->flags & AA_MSG_FLAG_ENCRYPTED ? "yes" : "no");

        if (msg->channel == kAndroidAutoChannelControl) {
            uint16_t message_id = be16_to_cpu(*(uint16_t*) msg->payload->pointer);

            switch (message_id) {
                case F1X__AASDK__PROTO__IDS__CONTROL_MESSAGE__ENUM__SERVICE_DISCOVERY_REQUEST:
                    ok = on_service_discovery_request(device, msg->payload->size - 2, msg->payload->pointer + 2);
                    break;
                case F1X__AASDK__PROTO__IDS__CONTROL_MESSAGE__ENUM__AUDIO_FOCUS_REQUEST:
                    ok = on_audio_focus_request(device, msg->payload->size - 2, msg->payload->pointer + 2);
                    break;
                default:
                    fprintf(stderr, "[android-auto plugin] Unhandled control channel message. message_id = %hu\n", message_id);
                    ok = EINVAL;
                    break;
            }

            if (ok != 0) {
                return ok;
            }
        } else {
            for_each_pointer_in_pset(&device->channels, channel) {
                if (channel->id == msg->channel) {
                    break;
                }
            }

            if (channel != NULL) {
                ok = aa_channel_on_message(channel, aa_msg_ref(msg));
                if (ok != 0) {
                    fprintf(stderr, "[android-auto plugin] Error handling message for channel %hhu: %s\n", msg->channel, strerror(ok));
                }
            } else {
                fprintf(stderr, "[android-auto plugin] Unhandled message on channel %hhu\n", msg->channel);
            }
        }

        aa_msg_unrefp(&msg);
    }

    return 0;
}

void *aa_dev_mgr_entry(void *arg) {
    struct aa_device *aadev;
    int ok;

    aadev = arg;
    
    ok = aa_dev_manage(aadev);
    
    free(aadev);

    return (void*) ok;
}
