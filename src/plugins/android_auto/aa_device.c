#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
#include <memory.h>
#include <alloca.h>

#include <collection.h>
#include <plugins/android_auto/android_auto.h>
#include <aasdk/ControlMessageIdsEnum.pb-c.h>
#include <aasdk/VersionResponseStatusEnum.pb-c.h>
#include <aasdk/AuthCompleteIndicationMessage.pb-c.h>
#include <aasdk/ServiceDiscoveryRequestMessage.pb-c.h>
#include <aasdk/ServiceDiscoveryResponseMessage.pb-c.h>

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

    /*if (dev->connection == kUSB) {
        int actual_length;

        ok = libusb_bulk_transfer(
            dev->usb_handle,
            direction == kTransferDirectionOut ?
                dev->out_endpoint.bEndpointAddress :
                dev->in_endpoint.bEndpointAddress,
            buffer->pointer + offset,
            length,
            &actual_length,
            10000
        );

        if (actually_transferred != NULL) {
            *actually_transferred = actual_length;
        }

        if (ok != 0) {
            fprintf(stderr, "[android-auto plugin] Error ocurred while executing USB bulk transfer. libusb_bulk_transfer: %s\n", get_str_for_libusb_error(ok));
            return get_errno_for_libusb_error(ok);
        }
        
        return 0;
    } else */if (dev->connection == kUSB) {
        xfer = libusb_alloc_transfer(0);
        if (xfer == NULL) {
            return ENOMEM;
        }
    
        completed = false;
        libusb_fill_bulk_transfer(
            xfer,
            dev->usb_handle,
            direction == kTransferDirectionOut ?
                dev->out_endpoint.bEndpointAddress :
                dev->in_endpoint.bEndpointAddress,
            buffer->pointer + offset,
            length,
            aa_device_on_libusb_transfer_completed,
            &completed,
            60000
        );

        libusb_lock_event_waiters(dev->aaplugin->libusb_context);

        ok = libusb_submit_transfer(xfer);
        if (ok != 0) {
            fprintf(stderr, "[android-auto plugin] Couldn't submit USB bulk transfer. libusb_submit_transfer: %s\n", get_str_for_libusb_error(ok));
            libusb_free_transfer(xfer);
            return get_errno_for_libusb_error(ok);
        }

        while (completed == false) {
            ok = libusb_wait_for_event(dev->aaplugin->libusb_context, NULL);
            if (ok == 1) {
                libusb_unlock_event_waiters(dev->aaplugin->libusb_context);
                libusb_free_transfer(xfer);
                fprintf(stderr, "[android-auto plugin] USB bulk transfer timed out.\n");
                return ETIMEDOUT;
            }
        }
        libusb_unlock_event_waiters(dev->aaplugin->libusb_context);

        if (xfer->status != LIBUSB_TRANSFER_COMPLETED) {
            fprintf(stderr, "[android-auto plugin] Error ocurred while executing USB bulk transfer. transfer_status = %s\n", get_str_for_libusb_error(xfer->status));
            libusb_free_transfer(xfer);
            return get_errno_for_libusb_error(xfer->status);
        }

        if (actually_transferred != NULL) {
            *actually_transferred = xfer->actual_length;
        }

        libusb_free_transfer(xfer);

        return 0;
    } else {
        return ENOTSUP;
    }

    return 0;
}

/**
 * Sending Messages
 */
int aa_device_send(
    struct aa_device *dev,
    const struct aa_msg *msg
) {
    bool is_multi_frame;
    uint16_t frame_size;
    int ok;
    
    frame_size = min(msg->payload.size, UINT16_MAX);
    is_multi_frame = frame_size < msg->payload.size;

    define_xfer_buffer_on_stack(frame_header_and_size, 4 + (is_multi_frame? 4 : 0))

    // initialize the frame header
    frame_header_and_size.pointer[0] = msg->channel;
    frame_header_and_size.pointer[1] = msg->flags | (is_multi_frame? kFrameTypeFirst : kFrameTypeBulk);

    // initialize the frame size
    *(uint16_t*) (frame_header_and_size.pointer + 2) = cpu_to_be16(frame_size);
    
    // If we have multiple frames, also initialize the total_size value of the frame size
    if (is_multi_frame) {
        *(uint32_t*) (frame_header_and_size.pointer + 4) = cpu_to_be32(msg->payload.size);
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
        return ok;
    }

    size_t offset = 0;
    size_t remaining = msg->payload.size;
    while (remaining > 0) {
        size_t length = min(remaining, frame_size);

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
                return ok;
            }
        }

        ok = aa_device_transfer(
            dev,
            kTransferDirectionOut,
            (struct aa_xfer_buffer*) &msg->payload,
            offset,
            length,
            &length
        );
        if (ok != 0) {
            return ok;
        }

        offset += length;
        remaining -= length;
    }

    return 0;
}

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


int aa_device_decrypt_write(struct aa_device *dev, uint8_t *source, size_t length) {
    size_t remaining, offset, n_written;
    int ok;
    
    remaining = length;
    offset = 0;
    while (remaining > 0) {
        ok = BIO_write_ex(
            SSL_get_rbio(dev->ssl),
            source + offset,
            remaining,
            &n_written
        );
        if (!ok) {
            ERR_print_errors_fp(stderr);
            return EIO;
        }

        remaining -= n_written;
        offset += n_written;
    }

    return 0;
}

int aa_device_decrypt_pending(struct aa_device *dev) {
    SSL_read(dev->ssl, NULL, 0);

    return SSL_pending(dev->ssl);
}

int aa_device_decrypt_read(struct aa_device *dev, uint8_t *dest, size_t length) {
    size_t remaining, offset, n_read;
    int ok;

    remaining = SSL_pending(dev->ssl);
    offset = 0;
    while (remaining > 0) {
        ok = SSL_read_ex(
            dev->ssl,
            dest + offset,
            remaining,
            &n_read
        );
        if (!ok) {
            ERR_print_errors_fp(stderr);
            return EIO;
        }

        remaining -= n_read;
        offset += n_read;
    }

    return 0;
}

int aa_device_encrypt_write(struct aa_device *dev, uint8_t *source, size_t length) {
    size_t remaining, offset, n_written;
    int ok;
    
    remaining = length;
    offset = 0;
    while (remaining > 0) {
        ok = BIO_write_ex(
            SSL_get_rbio(dev->ssl),
            source + offset,
            remaining,
            &n_written
        );
        if (!ok) {
            ERR_print_errors_fp(stderr);
            return EIO;
        }

        remaining -= n_written;
        offset += n_written;
    }

    return 0;
}

int aa_device_encrypt_pending(struct aa_device *dev) {
    return SSL_pending(dev->ssl);
}

int aa_device_encrypt_read(struct aa_device *dev, uint8_t *dest, size_t length, size_t *n_read) {
    return SSL_read_ex(dev->ssl, dest, length, n_read);
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

static int aa_device_receive_frame(struct aa_device *device, bool *msg_completed, struct aa_msg *msg_out) {
    enum aa_msg_frame_type frame_type;
    enum aa_channel_id channel;
    struct aa_xfer_buffer buffer;
    uint32_t total_size;
    uint32_t offset;
    uint16_t frame_size;
    uint8_t flags;
    bool should_free;
    int ok;

    should_free = false;

    if (msg_completed) {
        *msg_completed = false;
    }

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
        ok = aa_xfer_buffer_initialize_for_device(&buffer, device, total_size);
        if (ok != 0) {
            goto fail_reset_msg_construction_buffer;
        }

        should_free = true;
        offset = 0;

        if (frame_type == kFrameTypeFirst) {
            device->msg_assembly_buffers[channel].is_constructing = true;
            device->msg_assembly_buffers[channel].buffer = buffer;
            device->msg_assembly_buffers[channel].offset = offset;
            device->msg_assembly_buffers[channel].total_payload_size = total_size;
        }
    } else {
        if (device->msg_assembly_buffers[channel].is_constructing == false) {
            fprintf(stderr, "[android-auto plugin] ERROR: It appears some of the data frames of an android auto message were missed.\n");
            return EINVAL;
        } else {
            buffer = device->msg_assembly_buffers[channel].buffer;
            offset = device->msg_assembly_buffers[channel].offset;

            if (frame_size > (device->msg_assembly_buffers[channel].total_payload_size - device->msg_assembly_buffers[channel].offset)) {
                fprintf(stderr, "[android-auto plugin] Message buffer has less space remaining than the frame size reported by the android auto device. Limiting frame size to remaining buffer space.\n");
                frame_size = device->msg_assembly_buffers[channel].total_payload_size - device->msg_assembly_buffers[channel].offset;
            }
        }
    }

    ok = aa_device_receive_frame_payload(device, &buffer, offset, frame_size, flags & AA_MSG_FLAG_ENCRYPTED);
    if (ok != 0) {
        goto fail_free_msg;
    }

    offset += frame_size;

    if ((frame_type == kFrameTypeFirst) || (frame_type == kFrameTypeMiddle)) {
        device->msg_assembly_buffers[channel].offset = offset;
    } else if ((frame_type == kFrameTypeLast) || (frame_type == kFrameTypeBulk)) {
        if (flags & AA_MSG_FLAG_ENCRYPTED) {
            ok = aa_device_decrypt_write(device, buffer.pointer, buffer.size);
            if (ok != 0) return ok;

            ok = aa_device_decrypt_pending(device);
            if (ok <= 0) return EIO;

            buffer.size = ok;

            ok = aa_device_decrypt_read(device, buffer.pointer, ok);
            if (ok != 0) return ok;
        }

        if (frame_type == kFrameTypeLast) {
            device->msg_assembly_buffers[channel].is_constructing = false;
        }

        if (msg_completed) {
            *msg_completed = true;
        }

        if (msg_out) {
            *msg_out = (struct aa_msg) {
                .payload = buffer,
                .channel = channel,
                .flags = flags & ~(AA_MSG_FRAME_TYPE_MASK)
            };
        }
    }

    return 0;


    fail_free_msg:
    if (should_free) {
        aa_xfer_buffer_free(&buffer);
    }

    fail_reset_msg_construction_buffer:
    device->msg_assembly_buffers[channel].is_constructing = false;

    fail_return_ok:
    return ok;
}


int aa_device_receive_msg(struct aa_device *device, struct aa_msg *msg_out) {
    bool completed = false;
    int ok;

    while (!completed) {
        ok = aa_device_receive_frame(device, &completed, msg_out);
        if (ok != 0) {
            return ok;
        }
    }

    return 0;
}

int aa_device_receive_msg_from_channel(struct aa_device *device, enum aa_channel_id channel, struct aa_msg *msg_out) {
    int ok;

    if (msg_out == NULL) {
        msg_out = alloca(sizeof(struct aa_msg));
    }

    do {
        ok = aa_device_receive_msg(device, msg_out);
        if (ok != 0) {
            return ok;
        }
    } while (msg_out->channel != channel);

    return 0;
}

static int do_version_request(struct aa_device *device) {
    int ok;

    // send version request.
    {
        define_and_setup_aa_msg_on_stack(msg, 6, kAndroidAutoChannelControl, 0);

        uint16_t *msg_payload = (uint16_t*) msg.payload.pointer;
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
        struct aa_msg msg;
        ok = aa_device_receive_msg_from_channel(
            device,
            kAndroidAutoChannelControl,
            &msg
        );
        if (ok != 0) {
            return ok;
        }

        uint16_t *payload = (uint16_t*) msg.payload.pointer;

        uint16_t msg_id = be16_to_cpu(payload[0]);

        if (msg_id != F1X__AASDK__PROTO__IDS__CONTROL_MESSAGE__ENUM__VERSION_RESPONSE) {
            fprintf(stderr, "[android-auto plugin] Error: android auto device didn't return a version response.\n");
            return EPROTO;
        }

        size_t n_msg_elements = msg.payload.size / sizeof(uint16_t);
        uint16_t *msg_elements = payload + 1;

        if (n_msg_elements >= 3) {
            uint16_t major = be16_to_cpu(msg_elements[0]);
            uint16_t minor = be16_to_cpu(msg_elements[1]);
            F1x__Aasdk__Proto__Enums__VersionResponseStatus__Enum status = be16_to_cpu(msg_elements[2]);

            if (status == F1X__AASDK__PROTO__ENUMS__VERSION_RESPONSE_STATUS__ENUM__MISMATCH) {
                fprintf(stderr, "[android-auto plugin] Error: android auto device returned a version mismatch.\n");
                return EINVAL;
            } else if (status == F1X__AASDK__PROTO__ENUMS__VERSION_RESPONSE_STATUS__ENUM__MATCH) {
                printf("[android-auto plugin] Android auto device returned version match.\n");
            }
        } else {
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

                    *(uint16_t*) msg.payload.pointer = cpu_to_be16(F1X__AASDK__PROTO__IDS__CONTROL_MESSAGE__ENUM__SSL_HANDSHAKE);

                    size_t offset = 2;
                    size_t remaining = pending;
                    while (remaining > 0) {
                        ok = BIO_read(SSL_get_wbio(device->ssl), msg.payload.pointer + offset, remaining);
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
                    struct aa_msg msg;

                    ok = aa_device_receive_msg_from_channel(device, kAndroidAutoChannelControl, &msg);
                    if (ok != 0) {
                        return ok;
                    }

                    uint16_t message_id = be16_to_cpu(*(uint16_t*) msg.payload.pointer);

                    if (message_id == F1X__AASDK__PROTO__IDS__CONTROL_MESSAGE__ENUM__SSL_HANDSHAKE) {
                        size_t remaining = msg.payload.size - 2;
                        size_t offset = 2;
                        
                        while (remaining > 0) {
                            size_t written;
                            ok = BIO_write_ex(SSL_get_rbio(device->ssl), msg.payload.pointer + offset, remaining, &written);
                            if (!ok) {
                                ERR_print_errors_fp(stderr);
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

        *(uint16_t *) msg.payload.pointer = cpu_to_be16(F1X__AASDK__PROTO__IDS__CONTROL_MESSAGE__ENUM__AUTH_COMPLETE);
        f1x__aasdk__proto__messages__auth_complete_indication__pack(&auth_complete, msg.payload.pointer + 2);

        ok = aa_device_send(device, &msg);
        if (ok != 0) {
            return ok;
        }
    }

    return 0;
}

int aa_dev_manage(struct aa_device *device) {
    struct aaplugin *aaplugin;
    struct aa_msg msg;
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

    printf("handling messages...\n");
    while (1) {
        ok = aa_device_receive_msg(device, &msg);
        if (ok != 0) {
            return ok;
        }

        printf("received message on channel %hhu, encrypted? %s\n", msg.channel, msg.flags & AA_MSG_FLAG_ENCRYPTED ? "yes" : "no");

        if (msg.channel == kAndroidAutoChannelControl) {
            uint16_t message_id = be16_to_cpu(*(uint16_t*) msg.payload.pointer);

            if (message_id == F1X__AASDK__PROTO__IDS__CONTROL_MESSAGE__ENUM__SERVICE_DISCOVERY_REQUEST) {
                F1x__Aasdk__Proto__Messages__ServiceDiscoveryRequest *request =
                    f1x__aasdk__proto__messages__service_discovery_request__unpack(
                        NULL,
                        msg.payload.size - 2,
                        msg.payload.pointer + 2
                    );
                
                printf("[android-auto plugin] Got Service Discovery Request. device name: %s, device_brand: %s\n", request->device_name, request->device_brand);

                F1x__Aasdk__Proto__Messages__ServiceDiscoveryResponse response = F1X__AASDK__PROTO__MESSAGES__SERVICE_DISCOVERY_RESPONSE__INIT;
                
                response.head_unit_name = aaplugin->hu_info.headunit_name;
                response.car_model = aaplugin->hu_info.car_model;
                response.car_year = aaplugin->hu_info.car_year;
                response.car_serial = aaplugin->hu_info.car_serial;
                response.left_hand_drive_vehicle = aaplugin->hu_info.left_hand_drive_vehicle;
                response.headunit_manufacturer = aaplugin->hu_info.headunit_manufacturer;
                response.sw_build = aaplugin->hu_info.sw_build;
                response.sw_version = aaplugin->hu_info.sw_version;
                response.can_play_native_media_during_vr = aaplugin->hu_info.can_play_native_media_during_vr;
                response.has_hide_clock = true;
                response.hide_clock = aaplugin->hu_info.hide_clock;

                /*
                F1x__Aasdk__Proto__Data__ChannelDescriptor channels[256];
                F1x__Aasdk__Proto__Data__ChannelDescriptor *pchannels[256];

                channels[0].channel_id = kAndroidAutoChannelInput;
                channels[0].input_channel = 

                for (int i = 0; i < 256; i++) {
                    channels[i].
                }
                

                response.n_channels = 256;
                response.channels = pchannels;
                */
            //} else if (message_id == F1X__AASDK__PROTO__IDS__CONTROL_MESSAGE__ENUM__AUDIO_FOCUS_REQUEST) {
            //} else if (message_id == F1X__AASDK__PROTO__IDS__CONTROL_MESSAGE__ENUM__SHUTDOWN_REQUEST) {
            //} else if (message_id == F1X__AASDK__PROTO__IDS__CONTROL_MESSAGE__ENUM__SHUTDOWN_RESPONSE) {
            //} else if (message_id == F1X__AASDK__PROTO__IDS__CONTROL_MESSAGE__ENUM__NAVIGATION_FOCUS_REQUEST) {
            //} else if (message_id == F1X__AASDK__PROTO__IDS__CONTROL_MESSAGE__ENUM__PING_RESPONSE) {
                
            } else {
                fprintf(stderr, "[android-auto plugin] Unhandled message on channel %hhu, message_id = %hu\n", msg.channel, message_id);
            }
        } else {
            fprintf(stderr, "[android-auto plugin] Unhandled message on channel %hhu\n", msg.channel);
        }
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
