#include <stdlib.h>
#include <errno.h>

#include <plugins/android_auto/android_auto.h>

int aa_xfer_buffer_initialize_for_device(struct aa_xfer_buffer *out, struct aa_device *dev, size_t size) {
    if (out == NULL) {
        return EINVAL;
    }

    if (dev->connection == kUSB) {
        uint8_t *buffer = libusb_dev_mem_alloc(dev->usb_handle, size);
        if (buffer != NULL) {
            out->type = kXferBufferLibusbDevMem;
            out->pointer = buffer;
            out->size = size;
            out->libusb_device_handle = dev->usb_handle;

            return 0;
        }
    }

    uint8_t *buffer = malloc(size);
    if (buffer == NULL) {
        return ENOMEM;
    }

    out->type = kXferBufferHeap;
    out->pointer = buffer;
    out->size = size;
    out->libusb_device_handle = dev->usb_handle;

    return 0;
}

int aa_xfer_buffer_initialize_from_pointer(struct aa_xfer_buffer *out, void *pointer, size_t size) {
    if (out == NULL) {
        return EINVAL;
    }

    out->type = kXferBufferUserManaged;
    out->pointer = pointer;
    out->size = size;
    out->libusb_device_handle = NULL;

    return 0;
}

void aa_xfer_buffer_free(struct aa_xfer_buffer *buffer) {
    switch (buffer->type) {
        case kXferBufferHeap:
            free(buffer->pointer);
            break;
        case kXferBufferLibusbDevMem:
            libusb_dev_mem_free(buffer->libusb_device_handle, buffer->pointer, buffer->size);
            break;
        case kXferBufferUserManaged:
            break;
        default: break;
    }
}

