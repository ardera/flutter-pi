#define _XOPEN_SOURCE 700
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <alloca.h>
#include <inttypes.h>
#include <limits.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <event_loop.h>
#include <math.h>

#include <modesetting/modesetting.h>
#include <modesetting/modesetting_private.h>

#define LOG_ERROR(fmtstring, ...) LOG_ERROR_WITH_PREFIX("[modesetting]", fmstring, __VA_ARGS__)

/*****************************************************************************
 *                               PRESENTERS                                  *
 *****************************************************************************/
struct display *presenter_get_display(struct presenter *presenter) {
    DEBUG_ASSERT_NOT_NULL(presenter);
    DEBUG_ASSERT_NOT_NULL(presenter->display);
    return presenter->display;
}

/**
 * @brief Add a callback to all the layer for this CRTC are now present on screen.
 */
int presenter_set_scanout_callback(struct presenter *presenter, presenter_scanout_callback_t cb, void *userdata) {
    DEBUG_ASSERT_NOT_NULL(presenter);
    DEBUG_ASSERT_NOT_NULL(presenter->set_scanout_callback);
    return presenter->set_scanout_callback(presenter, cb, userdata);
}

/**
 * @brief Sets the zpos (which will be used for any new pushed planes) to the zpos corresponding to @ref logical_zpos.
 * Only supported for KMS presenters.
 * 
 * Actual hardware zpos ranges are driver-specific, sometimes they range from 0 to 127, sometimes 1 to 256, etc.
 * The logical zpos always starts at 0 and ends at (hw_zpos_max - hw_zpos_min) inclusive.
 */
int presenter_set_logical_zpos(struct presenter *presenter, int logical_zpos) {
    DEBUG_ASSERT_NOT_NULL(presenter);
    DEBUG_ASSERT_NOT_NULL(presenter->set_logical_zpos);
    return presenter->set_logical_zpos(presenter, logical_zpos);
}

/**
 * @brief Gets the current (actual) zpos. Only supported by KMS presenters, and even then not supported all the time.
 */
int presenter_get_zpos(struct presenter *presenter) {
    DEBUG_ASSERT_NOT_NULL(presenter);
    DEBUG_ASSERT_NOT_NULL(presenter->set_logical_zpos);
    return presenter->get_zpos(presenter);
}

/**
 * @brief Presents a software framebuffer (i.e. some malloced memory).
 * 
 * Can only be used for fbdev presenting.
 */
int presenter_push_sw_fb_layer(
    struct presenter *presenter,
    const struct sw_fb_layer *layer
) {
    DEBUG_ASSERT_NOT_NULL(presenter);
    DEBUG_ASSERT_NOT_NULL(layer);
    DEBUG_ASSERT_NOT_NULL(presenter->push_sw_fb_layer);
    return presenter->push_sw_fb_layer(presenter, layer);
}

int presenter_push_display_buffer_layer(
    struct presenter *presenter,
    const struct display_buffer_layer *layer
) {
    DEBUG_ASSERT_NOT_NULL(presenter);
    DEBUG_ASSERT_NOT_NULL(layer);
    DEBUG_ASSERT(presenter->push_display_buffer_layer);
    return presenter->push_display_buffer_layer(presenter, layer);
}

/**
 * @brief Push a placeholder layer. Increases the zpos by @ref n_reserved_layers for kms presenters.
 * Returns EOVERFLOW if the zpos before incrementing is higher than the maximum supported one by the hardware.
 */
int presenter_push_placeholder_layer(struct presenter *presenter, int n_reserved_layers) {
    DEBUG_ASSERT_NOT_NULL(presenter);
    DEBUG_ASSERT_NOT_NULL(presenter->push_placeholder_layer);

    return presenter->push_placeholder_layer(presenter, n_reserved_layers);
}

/**
 * @brief Makes sure all the output operations are applied. This is NOT the "point of no return", that
 * point is way earlier.
 */
int presenter_flush(struct presenter *presenter) {
    DEBUG_ASSERT_NOT_NULL(presenter);
    DEBUG_ASSERT_NOT_NULL(presenter->flush);
    return presenter->flush(presenter);
}

/**
 * @brief Destroy a presenter, freeing all the allocated resources.
 */
void presenter_destroy(struct presenter *presenter) {
    DEBUG_ASSERT_NOT_NULL(presenter);
    DEBUG_ASSERT_NOT_NULL(presenter->destroy);
    return presenter->destroy(presenter);
}


void display_destroy(struct display *display) {
    DEBUG_ASSERT_NOT_NULL(display);
    DEBUG_ASSERT_NOT_NULL(display->destroy);
    return display->destroy(display);
}

void display_get_size(struct display *display, int *width_out, int *height_out) {
    DEBUG_ASSERT_NOT_NULL(display);
    
    if (width_out != NULL) {
        *width_out = display->width;
    }
    if (height_out != NULL) {
        *height_out = display->height;
    }
}

double display_get_refreshrate(struct display *display) {
    DEBUG_ASSERT_NOT_NULL(display);
    return display->refresh_rate;
}

bool display_has_dimensions(struct display *display) {
    return display->has_dimensions;
}

void display_get_dimensions(struct display *display, int *width_mm_out, int *height_mm_out) {
    DEBUG_ASSERT(display->has_dimensions);
    if (width_mm_out) {
        *width_mm_out = display->width_mm;
    }
    if (height_mm_out) {
        *height_mm_out = display->height_mm;
    }
}

double display_get_flutter_pixel_ratio(struct display *display) {
    DEBUG_ASSERT_NOT_NULL(display);
    return display->flutter_pixel_ratio;
}

bool display_supports_gbm(struct display *display) {
    DEBUG_ASSERT_NOT_NULL(display);
    return display->supports_gbm;
}

struct gbm_device *display_get_gbm_device(struct display *display) {
    DEBUG_ASSERT_NOT_NULL(display);
    DEBUG_ASSERT(display->supports_gbm);
    return display->gbm_device;
}

/**
 * @brief Get the list of fourcc formats supported by this presenter.
 */
void display_get_supported_formats(struct display *display, const enum pixfmt **formats_out, size_t *n_formats_out) {
    DEBUG_ASSERT_NOT_NULL(display);
    DEBUG_ASSERT_NOT_NULL(formats_out);
    DEBUG_ASSERT_NOT_NULL(n_formats_out);
    return display->get_supported_formats(display, formats_out, n_formats_out);
}

double display_get_refresh_rate(struct display *display) {
    return display->refresh_rate;
}

struct presenter *display_create_presenter(struct display *display) {
    DEBUG_ASSERT_NOT_NULL(display);
    DEBUG_ASSERT_NOT_NULL(display->create_presenter);
    return display->create_presenter(display);
}

bool display_supports_importing_buffer_type(struct display *display, enum display_buffer_type type) {
    DEBUG_ASSERT_NOT_NULL(display);
    return display->supported_buffer_types_for_import[type];
}

struct display_buffer *display_create_mapped_buffer(
    struct display *display,
    int width, int height,
    enum pixfmt format
) {
    struct display_buffer *buffer;
    int ok;

    DEBUG_ASSERT_NOT_NULL(display);

    buffer = malloc(sizeof *buffer);
    if (buffer == NULL) {
        return NULL;
    }
    
    buffer->display = display;
    buffer->backend.sw.width = width;
    buffer->backend.sw.height = height;
    buffer->backend.sw.format = format;

    ok = display->make_mapped_buffer(buffer);
    if (ok != 0) {
        free(buffer);
        return NULL;
    }

    return buffer;
}

struct display_buffer *display_import_buffer(
    struct display *display,
    const struct display_buffer_backend *source,
    display_buffer_destroy_callback_t destroy_callback,
    void *userdata
) {
    struct display_buffer *buffer;
    int ok;

    DEBUG_ASSERT_NOT_NULL(display);
    DEBUG_ASSERT_NOT_NULL(source);
    DEBUG_ASSERT_NOT_NULL(destroy_callback);
    DEBUG_ASSERT(display->supported_buffer_types_for_import[source->type]);

    buffer = malloc(sizeof *buffer);
    if (buffer == NULL) {
        return NULL;
    }

    buffer->display = display;
    buffer->backend = *source;
    buffer->destroy_callback = destroy_callback;
    buffer->userdata = userdata;

    if (source->type == kDisplayBufferTypeSw) {
        ok = display->import_sw_buffer(buffer);
    } else if (source->type == kDisplayBufferTypeGbmBo) {
        ok = display->import_gbm_bo(buffer);
    } else if (source->type == kDisplayBufferTypeGemBo) {
        ok = display->import_gem_bo(buffer);
    } else if (source->type == kDisplayBufferTypeEglImage) {
        ok = display->import_egl_image(buffer);
    }

    if (ok != 0) {
        free(buffer);
        return NULL;
    }

    return buffer;
}

const struct display_buffer_backend *display_buffer_get_backend(struct display_buffer *buffer) {
    return &buffer->backend;
}

void display_buffer_destroy(struct display_buffer *buffer) {
    DEBUG_ASSERT_NOT_NULL(buffer);
    DEBUG_ASSERT_NOT_NULL(buffer->destroy_callback);
    return buffer->destroy_callback(buffer->display, &buffer->backend, buffer->userdata);
}
