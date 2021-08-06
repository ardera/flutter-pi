#ifndef _FLUTTERPI_INCLUDE_MODESETTING_H
#define _FLUTTERPI_INCLUDE_MODESETTING_H

#include <stdbool.h>
#include <stdio.h>
#include <pthread.h>

#ifdef HAS_FBDEV
#   include "modesetting_fbdev.h"
#endif
#ifdef HAS_KMS
#   include "modesetting_kms.h"
#endif
#ifdef HAS_GBM
#   include <gbm.h>
#endif

#include <collection.h>
#include <pixel_format.h>

#define LOG_MODESETTING_ERROR(format_str, ...) fprintf(stderr, "[modesetting] %s: " format_str, __PRETTY_FUNCTION__, ##__VA_ARGS__)



struct gbm_device;
struct gbm_surface;
struct display;
struct display_buffer;
struct display_buffer_backend;
struct presenter;

typedef void (*display_buffer_destroy_callback_t)(struct display *display, const struct display_buffer_backend *backend, void *userdata);

typedef void (*display_buffer_release_callback_t)(struct display_buffer *buffer, const struct display_buffer_backend *backend, void *userdata);

typedef void (*presenter_scanout_callback_t)(struct display *display, uint64_t ns, void *userdata);

enum display_buffer_type {
    kDisplayBufferTypeSw = 0,
    kDisplayBufferTypeGbmBo = 1,
    kDisplayBufferTypeGemBo = 2,
    kDisplayBufferTypeEglImage = 3,
    kDisplayBufferTypeLast = kDisplayBufferTypeEglImage
};

struct display_buffer_backend {
    enum display_buffer_type type;
    union {
        struct {
            int width, height, stride;
            enum pixfmt format;
            uint8_t *vmem;
        } sw;
#ifdef HAS_GBM
        struct {
            struct gbm_bo *bo;
        } gbm_bo;
#endif
        struct {
            int width, height, stride;
            enum pixfmt format;
            uint32_t gem_bo_handle;
        } gem_bo;
#ifdef HAS_EGL
        struct {
            void *egl_image;
        };
#endif
    };
};

struct sw_fb_layer {
    uint8_t *vmem;
    int width, height, pitch;
    enum pixfmt format;
};

enum dspbuf_layer_rotation {
    kDspBufLayerRotationNone     = 1 << 0,
    kDspBufLayerRotation90       = 1 << 1,
    kDspBufLayerRotation180      = 1 << 2,
    kDspBufLayerRotation270      = 1 << 3,
    kDspBufLayerRotationReflectX = 1 << 4,
    kDspBufLayerRotationReflectY = 1 << 5
};

struct display_buffer_layer {
    struct display_buffer *buffer;

    int buffer_x, buffer_y, buffer_w, buffer_h;
    int display_x, display_y, display_w, display_h;

    enum dspbuf_layer_rotation rotation;

    display_buffer_release_callback_t on_release;
    void *userdata;
};

/************
 * DISPLAYS *
 ************/
void display_destroy(struct display *display);

double display_get_refreshrate(struct display *display);

void display_get_size(struct display *display, int *width_out, int *height_out);

static inline int display_get_width(struct display *display) {
    int width;
    display_get_size(display, &width, NULL);
    return width;
}

static inline int display_get_height(struct display *display) {
    int height;
    display_get_size(display, NULL, &height);
    return height;
}

bool display_has_dimensions(struct display *display);

void display_get_dimensions(struct display *display, int *width_mm_out, int *height_mm_out);

double display_get_flutter_pixel_ratio(struct display *display);

bool display_supports_gbm(struct display *display);

struct gbm_device *display_get_gbm_device(struct display *display);

bool display_supports_sw_buffers(struct display *display);

void display_get_supported_formats(struct display *display, const enum pixfmt **formats_out, size_t *n_formats_out);

struct presenter *display_create_presenter(struct display *display);

struct display_buffer *display_create_buffer(
    struct display *display,
    int width, int height, int stride,
    uint32_t pixel_format,
    uint32_t flags
);

bool display_supports_importing_buffer_type(struct display *display, enum display_buffer_type type);

struct display_buffer *display_import_buffer(
    struct display *display,
    const struct display_buffer_backend *source,
    display_buffer_destroy_callback_t destroy_callback,
    void *userdata
);

const struct display_buffer_backend *display_buffer_get_backend(struct display_buffer *buffer);

void display_buffer_destroy(struct display_buffer *buffer);

/**************
 * PRESENTERS *
 **************/
int presenter_set_scanout_callback(
    struct presenter *presenter,
    presenter_scanout_callback_t cb,
    void *userdata
);

int presenter_set_logical_zpos(struct presenter *presenter, int zpos);

int presenter_set_zpos(struct presenter *presenter, int zpos);

int presenter_get_zpos(struct presenter *presenter);

struct display *presenter_get_display(struct presenter *presenter);

int presenter_push_sw_fb_layer(
    struct presenter *presenter,
    const struct sw_fb_layer *layer
);

int presenter_push_display_buffer_layer(
    struct presenter *presenter,
    const struct display_buffer_layer *layer
);

int presenter_push_placeholder_layer(struct presenter *presenter, int n_reserved_layers);

int presenter_flush(struct presenter *presenter);

void presenter_destroy(struct presenter *presenter);



#endif // _FLUTTERPI_INCLUDE_MODESETTING_H
