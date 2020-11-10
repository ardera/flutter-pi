#include <stdlib.h>
#include <errno.h>

#include <plugins/android_auto/android_auto.h>

int aa_xfer_buffer_initialize_on_stack_for_device(struct aa_xfer_buffer *out, struct aa_device *dev, size_t size) {
    if (out == NULL) {
        return EINVAL;
    }

    out->pointer = NULL;
    out->size = size;
    out->allocated_size = size;
    out->libusb_device_handle = dev->usb_handle;
    out->is_allocated = false;
    out->n_refs = 0;

    if (dev->connection == kUSB && false) {
        uint8_t *buffer = libusb_dev_mem_alloc(dev->usb_handle, size);
        if (buffer != NULL) {
            out->type = kXferBufferLibusbDevMem;
            out->pointer = buffer;
        }
    }

    if (out->pointer == NULL) {
        uint8_t *buffer = malloc(size);
        if (buffer != NULL) {
            out->type = kXferBufferHeap;
            out->pointer = buffer;
        } else {   
            return ENOMEM;
        }
    }

    return 0;
}

int aa_xfer_buffer_initialize_on_stack_from_pointer(struct aa_xfer_buffer *out, void *pointer, size_t size) {
    if (out == NULL) {
        return EINVAL;
    }

    out->type = kXferBufferUserManaged;
    out->pointer = pointer;
    out->size = size;
    out->allocated_size = size;
    out->libusb_device_handle = NULL;
    out->is_allocated = false;
    out->n_refs = 0;

    return 0;
}

struct aa_xfer_buffer *aa_xfer_buffer_new_for_device(struct aa_device *dev, size_t size) {
    struct aa_xfer_buffer *buf;

    buf = malloc(sizeof *buf);
    if (buf == NULL) {
        return buf;
    }

    buf->pointer = NULL;
    buf->size = size;
    buf->allocated_size = size;
    buf->libusb_device_handle = dev->usb_handle;
    buf->is_allocated = true;
    buf->n_refs = 1;

    if (dev->connection == kUSB && false) {
        uint8_t *buffer = libusb_dev_mem_alloc(dev->usb_handle, size);
        if (buffer != NULL) {
            buf->type = kXferBufferLibusbDevMem;
            buf->pointer = buffer;
        }
    }

    if (buf->pointer == NULL) {
        uint8_t *buffer = malloc(size);
        if (buffer != NULL) {
            buf->type = kXferBufferHeap;
            buf->pointer = buffer;
        } else {   
            return NULL;
        }
    }

    return buf;
}

struct aa_xfer_buffer *aa_xfer_buffer_new_from_pointer(struct aa_device *dev, void *pointer, size_t size) {
    struct aa_xfer_buffer *buf;

    buf = malloc(sizeof *buf);
    if (buf == NULL) {
        return NULL;
    }

    buf->type = kXferBufferUserManaged;
    buf->pointer = pointer;
    buf->size = size;
    buf->allocated_size = size;
    buf->libusb_device_handle = dev->usb_handle;
    buf->is_allocated = true;
    buf->n_refs = 1;

    return buf;
}

struct aa_xfer_buffer *aa_xfer_buffer_ref(struct aa_xfer_buffer *buffer) {
    if (!buffer->is_allocated) {
        return NULL;
    }

    buffer->n_refs++;
    return buffer;
}

static uint8_t *libusb_dev_mem_realloc(libusb_device_handle *handle, uint8_t *buffer, size_t new_size, size_t old_size) {
    uint8_t *new_pointer = (uint8_t*) libusb_dev_mem_alloc(handle, new_size);
    if (new_pointer == NULL) {
        return NULL;
    }

    memcpy(new_pointer, buffer, min(old_size, new_size));

    libusb_dev_mem_free(handle, buffer, old_size);

    return new_pointer;
}

int aa_xfer_buffer_resize(struct aa_xfer_buffer *buffer, size_t new_size, bool allow_unused_memory) {
    // TODO: Make libusb dev mem transfer buffers zero-copy shrinkable as well
    if (new_size < buffer->size) {
        if (allow_unused_memory) {
            if (buffer->type == kXferBufferLibusbDevMem) {
                uint8_t *new_pointer = libusb_dev_mem_realloc(buffer->libusb_device_handle, buffer->pointer, new_size, buffer->size);
                if (new_pointer == NULL) {
                    return ENOMEM;
                }

                buffer->size = new_size;
                buffer->pointer = new_pointer;
            } else if (buffer->type == kXferBufferHeap) {
                uint8_t *new_pointer = realloc(buffer->pointer, new_size);
                if (new_pointer == NULL) {
                    return ENOMEM;
                }

                buffer->size = new_size;
                buffer->pointer = new_pointer;
            } else {
                return EINVAL;
            }
        } else {
            buffer->size = new_size;
        }
    } else if (new_size > buffer->size) {
        if (!allow_unused_memory && new_size <= buffer->allocated_size) {
            buffer->size = new_size;
        } else {
            if (buffer->type == kXferBufferLibusbDevMem) {
                uint8_t *new_pointer = libusb_dev_mem_realloc(buffer->libusb_device_handle, buffer->pointer, new_size, buffer->size);
                if (new_pointer == NULL) {
                    return ENOMEM;
                }

                buffer->size = new_size;
                buffer->pointer = new_pointer;
            } else if (buffer->type == kXferBufferHeap) {
                uint8_t *new_pointer = realloc(buffer->pointer, new_size);
                if (new_pointer == NULL) {
                    return ENOMEM;
                }

                buffer->size = new_size;
                buffer->pointer = new_pointer;
            } else if (buffer->type == kXferBufferUserManaged) {
                return EINVAL;
            }
        }
    }

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

    if (buffer->is_allocated) {
        free(buffer);
    }
}

void aa_xfer_buffer_unref(struct aa_xfer_buffer *buffer) {
    if (buffer->is_allocated == false) {
        return;
    }

    buffer->n_refs--;
    if (buffer->n_refs == 0) {
        aa_xfer_buffer_free(buffer);
    }
}

void aa_xfer_buffer_unrefp(struct aa_xfer_buffer **buffer) {
    aa_xfer_buffer_unref(*buffer);
    *buffer = NULL;
}


struct aa_msg *aa_msg_new(enum aa_channel_id channel_id, uint8_t flags, struct aa_xfer_buffer *payload) {
    struct aa_msg *msg = malloc(sizeof(struct aa_msg));
    if (msg == NULL) {
        return NULL;
    }

    msg->channel = channel_id;
    msg->flags = flags;
    msg->payload = aa_xfer_buffer_ref(payload) ?: payload;
    msg->is_allocated = true;
    msg->n_refs = 1;

    return msg;
}

struct aa_msg *aa_msg_new_with_new_buffer_for_device(enum aa_channel_id channel_id, uint8_t flags, struct aa_device *dev, size_t size) {
    struct aa_xfer_buffer *buffer = aa_xfer_buffer_new_for_device(dev, size);
    if (buffer == NULL) return NULL;

    struct aa_msg *msg = aa_msg_new(channel_id, flags, buffer);
    
    aa_xfer_buffer_unrefp(&buffer);

    return msg;
}

struct aa_msg *aa_msg_new_with_new_buffer_from_pointer(enum aa_channel_id channel_id, uint8_t flags, struct aa_device *dev, void *pointer, size_t size) {
    struct aa_xfer_buffer *buffer = aa_xfer_buffer_new_from_pointer(dev, pointer, size);
    if (buffer == NULL) return NULL;

    struct aa_msg *msg = aa_msg_new(channel_id, flags, buffer);
    
    aa_xfer_buffer_unrefp(&buffer);

    return msg;
}

struct aa_msg *aa_msg_ref(struct aa_msg *msg) {
    if (msg->is_allocated == false) {
        return NULL;
    }

    msg->n_refs++;
    return msg;
}

void aa_msg_unref(struct aa_msg *msg) {
    if (msg->is_allocated == false) {
        return;
    }

    msg->n_refs--;

    if (msg->n_refs == 0) {
        aa_xfer_buffer_unref(msg->payload);
        free(msg);
    }
}

void aa_msg_unrefp(struct aa_msg **msg) {
    aa_msg_unref(*msg);
    *msg = NULL;
}