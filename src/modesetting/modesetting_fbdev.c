#include <modesetting/modesetting_fbdev.h>
#include <modesetting/modesetting_private.h>

#define DISPLAY_PRIVATE_FBDEV(p_display) CAST_DISPLAY_PRIVATE(p_display, struct fbdev_display)
#define PRESENTER_PRIVATE_FBDEV(p_presenter) CAST_PRESENTER_PRIVATE(p_presenter, struct fbdev_presenter)

#define LOG_ERROR(fmtstring, ...) LOG_ERROR_WITH_PREFIX("[fbdev modesetting]", fmtstring, __VA_ARGS__)

struct fbdev_display {
    int fd;
    
    /**
     * @brief fixinfo is the info that is fixed per framebuffer and can't be changed.
     */
    struct fb_fix_screeninfo fixinfo;

    /**
     * @brief varinfo is the info that can potentially be changed, but we don't do that and
     * instead rely on something like `fbset` configuring the fbdev for us.
     */
    struct fb_var_screeninfo varinfo;

    /**
     * @brief The physical width & height of the frame buffer and the pixel format as a fourcc code.
     */
    int width, height;
    
    /**
     * @brief The pixel format of the display (ARGB8888, RGB565, etc) as a enum pixfmt.
     */
    enum pixfmt format;
    
    /**
     * @brief The mapped video memory of this fbdev.
     * Basically the result of mmap(fd)
     */ 
    uint8_t *vmem;

    /**
     * @brief How many bytes of vmem we mapped.
     */
    size_t size_vmem;
};

struct fbdev_presenter {
    struct fbdev_display *fbdev_display;
    struct display *display;

    presenter_scanout_callback_t scanout_cb;
    void *scanout_cb_userdata;
};

static int fbdev_presenter_set_scanout_callback(struct presenter *presenter, presenter_scanout_callback_t cb, void *userdata) {
    struct fbdev_presenter *private = PRESENTER_PRIVATE_FBDEV(presenter);

    private->scanout_cb = cb;
    private->scanout_cb_userdata = userdata;
    
    return 0;
}

static int fbdev_presenter_set_logical_zpos(struct presenter *presenter, int logical_zpos) {
    (void) presenter;

    if ((logical_zpos == 0) || (logical_zpos == -1)) {
        return 0;
    } else {
        return EINVAL;
    }
}

static int fbdev_presenter_get_zpos(struct presenter *presenter) {
    (void) presenter;
    return 0;
}

static int fbdev_presenter_push_sw_fb_layer(struct presenter *presenter, const struct sw_fb_layer *layer) {
    struct fbdev_presenter *private = PRESENTER_PRIVATE_FBDEV(presenter);

    // formats should probably be handled by the code using this
    if (layer->format != private->fbdev_display->format) {
        return EINVAL;
    }

    if (layer->width != private->fbdev_display->width) {
        return EINVAL;
    }

    if (layer->height != private->fbdev_display->height) {
        return EINVAL;
    }

    // If the pitches don't match, we'd need to copy line by line.
    // We can support that in the future but it's probably unnecessary right now.
    if ((int) private->fbdev_display->fixinfo.line_length != layer->pitch) {
        LOG_MODESETTING_ERROR("Rendering software framebuffers into fbdev with non-matching buffer pitches is not supported.\n");
        return EINVAL;
    }

    size_t offset = private->fbdev_display->varinfo.yoffset * private->fbdev_display->fixinfo.line_length + private->fbdev_display->varinfo.xoffset;

    memcpy(
        private->fbdev_display->vmem + offset,
        layer->vmem,
        min(private->fbdev_display->size_vmem - offset, (size_t) (layer->height) * (size_t) layer->pitch)
    );

    return 0;
}

static int fbdev_presenter_push_placeholder_layer(struct presenter *presenter, int n_reserved_layers) {
    (void) presenter;
    (void) n_reserved_layers;
    return EOPNOTSUPP;
}

static int fbdev_presenter_flush(struct presenter *presenter) {
    (void) presenter;
    /// TODO: Implement
    return 0;
}

static void fbdev_presenter_destroy(struct presenter *presenter) {
    free(presenter);
}

static inline bool fb_bitfield_equals(const struct fb_bitfield *a, const struct fb_bitfield *b) {
    return (a->offset == b->offset) && (a->length == b->length) && ((a->msb_right != 0) == (b->msb_right != 0));
}

static void fbdev_display_destroy(struct display *display) {
    struct fbdev_display *private;

    DEBUG_ASSERT(display != NULL);

    private = DISPLAY_PRIVATE_FBDEV(display);

    munmap(private->vmem, private->size_vmem);
    close(private->fd);
    free(private);
    free(display);
}

static void fbdev_display_get_supported_formats(struct display *display, const enum pixfmt **formats_out, size_t *n_formats_out) {
    struct fbdev_display *private = DISPLAY_PRIVATE_FBDEV(display);

    *formats_out = &private->format;
    *n_formats_out = 1;
}

static void fbdev_display_on_destroy_mapped_buffer(struct display *display, const struct display_buffer_backend *backend, void *userdata) {
    (void) display;
    (void) userdata;
    
    free(backend->sw.vmem);
}

static int fbdev_display_make_mapped_buffer(struct display_buffer *buffer) {
    uint8_t *vmem;
    int bytes_per_pixel, stride;

    bytes_per_pixel = (get_pixfmt_info(buffer->backend.sw.format)->bits_per_pixel + 7) / 8;
    
    stride = buffer->backend.sw.width * bytes_per_pixel;
    vmem = malloc(buffer->backend.sw.height * stride);
    if (vmem == NULL) {
        return ENOMEM;
    }

    buffer->backend.sw.stride = stride;
    buffer->backend.sw.vmem = vmem;
    buffer->destroy_callback = fbdev_display_on_destroy_mapped_buffer;
    buffer->userdata = NULL;
    return 0;
}

static int fbdev_display_import_sw_buffer(struct display_buffer *buffer) {
    struct fbdev_display *private = DISPLAY_PRIVATE_FBDEV(buffer->display);
    
    if (buffer->backend.sw.width != private->width) {
        LOG_MODESETTING_ERROR("sw buffer has wrong width.\n");
        return EINVAL;
    }

    if (buffer->backend.sw.height != private->height) {
        LOG_MODESETTING_ERROR("sw buffer has wrong height.\n");
        return EINVAL;
    }

    if (buffer->backend.sw.format != private->format) {
        LOG_MODESETTING_ERROR("sw buffer has wrong pixel format.\n");
        return EINVAL;
    }

    return 0;
}

static struct presenter *fbdev_display_create_presenter(struct display *display) {
    struct fbdev_presenter *presenter_private;
    struct fbdev_display *private;
    struct presenter *presenter;

    DEBUG_ASSERT(display != NULL);
    private = DISPLAY_PRIVATE_FBDEV(display);

    presenter = malloc(sizeof *presenter);
    if (presenter == NULL) {
        return NULL;
    }

    presenter_private = malloc(sizeof *presenter_private);
    if (presenter_private == NULL) {
        free(presenter);
        return NULL;
    }

    presenter_private->fbdev_display = private;
    presenter_private->display = display;
    presenter_private->scanout_cb = NULL;
    presenter_private->scanout_cb_userdata = NULL;
    presenter->private = (struct presenter_private *) presenter_private;
    presenter->set_logical_zpos = fbdev_presenter_set_logical_zpos;
    presenter->get_zpos = fbdev_presenter_get_zpos;
    presenter->set_scanout_callback = fbdev_presenter_set_scanout_callback;
    presenter->push_sw_fb_layer = fbdev_presenter_push_sw_fb_layer;
    presenter->push_placeholder_layer = fbdev_presenter_push_placeholder_layer;
    presenter->flush = fbdev_presenter_flush;
    presenter->destroy = fbdev_presenter_destroy;
    presenter->display = display;

    return presenter;
}

struct display *fbdev_display_new_from_fd(int fd, const struct fbdev_display_config *config) {
    struct fbdev_display *private;
    struct display *display;
    unsigned int i;
    uint32_t format;
    void *vmem;
    int ok;

    DEBUG_ASSERT(config != NULL);

    display = malloc(sizeof *display);
    if (display == NULL) {
        goto fail_return_null;
    }

    private = malloc(sizeof *private);
    if (private == NULL) {
        goto fail_free_display;
    }

    ok = ioctl(fd, FBIOGET_FSCREENINFO, &private->fixinfo);
    if (ok < 0) {
        LOG_MODESETTING_ERROR("Couldn't get fbdev fix_screeninfo. ioctl: %s\n", strerror(-ok));
        goto fail_free_private;
    }

    ok = ioctl(fd, FBIOGET_VSCREENINFO, &private->varinfo);
    if (ok < 0) {
        LOG_MODESETTING_ERROR("Couldn't get fbdev var_screeninfo. ioctl: %s\n", strerror(-ok));
        goto fail_free_private;
    }

    if (private->fixinfo.visual != FB_TYPE_PACKED_PIXELS) {
        LOG_MODESETTING_ERROR("A fbdev type other than FB_TYPE_PACKED_PIXELS is not supported.\n");
        goto fail_free_private;
    }
    
    if (private->fixinfo.visual != FB_VISUAL_TRUECOLOR) {
        LOG_MODESETTING_ERROR("A fbdev visual other than FB_VISUAL_TRUECOLOR is not supported.\n");
        goto fail_free_private;
    }

    // look for the fourcc code for this fbdev format
    format = 0;
    for (i = 0; i < n_pixfmt_infos; i++) {
        if (fb_bitfield_equals(&private->varinfo.red, &pixfmt_infos[i].fbdev_format.r) &&
            fb_bitfield_equals(&private->varinfo.green, &pixfmt_infos[i].fbdev_format.g) &&
            fb_bitfield_equals(&private->varinfo.blue, &pixfmt_infos[i].fbdev_format.b) &&
            fb_bitfield_equals(&private->varinfo.transp, &pixfmt_infos[i].fbdev_format.a)
        ) {
            format = pixfmt_infos[i].format;
            break;
        }
    }

    // if the format is still 0 we couldn't find a corresponding DRM fourcc format.
    if (format == 0) {
        LOG_MODESETTING_ERROR(
            "Didn't find a corresponding fourcc format for fbdev format rgba = %" PRIu32 "/%" PRIu32 ",%" PRIu32 "/%" PRIu32 ",%" PRIu32 "/%" PRIu32 ",%" PRIu32"/%" PRIu32 ".\n",
            private->varinfo.red.length, private->varinfo.red.offset,
            private->varinfo.green.length, private->varinfo.green.offset,
            private->varinfo.blue.length, private->varinfo.blue.offset,
            private->varinfo.transp.length, private->varinfo.transp.offset
        );
        goto fail_free_private;
    }

    vmem = mmap(
        NULL,
        private->varinfo.yres_virtual * private->fixinfo.line_length,
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        private->fd,
        0
    );
    if (vmem == MAP_FAILED) {
        LOG_MODESETTING_ERROR("Couldn't map fbdev. mmap: %s\n", strerror(errno));
        goto fail_free_private;
    }

    private->fd = fd;
    private->width = private->varinfo.width;
    private->height = private->varinfo.height;
    private->format = format;
    private->vmem = vmem;
    display->private = (struct display_private*) private;
    display->get_supported_formats = fbdev_display_get_supported_formats;
    display->import_sw_buffer = fbdev_display_import_sw_buffer;
    display->make_mapped_buffer = fbdev_display_make_mapped_buffer;
    display->create_presenter = fbdev_display_create_presenter;
    display->destroy = fbdev_display_destroy;
    display->width = private->varinfo.width;
    display->height = private->varinfo.height;
    display->has_dimensions = config->has_explicit_dimensions;
    display->width_mm = config->width_mm;
    display->height_mm = config->height_mm;
    display->flutter_pixel_ratio = config->has_explicit_dimensions
        ? (10.0 * private->varinfo.width) / (config->width_mm * 38.0)
        : 1.0f;
    display->supports_gbm = false;
    display->gbm_device = NULL;
    display->supported_buffer_types_for_import[kDisplayBufferTypeSw] = true;
    display->supported_buffer_types_for_import[kDisplayBufferTypeGbmBo] = false;
    display->supported_buffer_types_for_import[kDisplayBufferTypeGemBo] = false;
    display->supported_buffer_types_for_import[kDisplayBufferTypeEglImage] = false;

    return display;


    fail_free_private:
    free(private);

    fail_free_display:
    free(display);

    fail_return_null:
    return NULL;
}

struct display *fbdev_display_new_from_path(const char *path, const struct fbdev_display_config *config) {
    struct display *display;
    int fd;

    fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        LOG_MODESETTING_ERROR("Couldn't open fb device. open: %s\n", strerror(errno));
        return NULL;
    }

    display = fbdev_display_new_from_fd(fd, config);
    if (display == NULL) {
        close(fd);
        return NULL;
    }

    return display;
}
