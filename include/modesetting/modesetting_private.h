#ifndef _FLUTTERPI_INCLUDE_MODESETTING_PRIVATE_H
#define _FLUTTERPI_INCLUDE_MODESETTING_PRIVATE_H

#include <modesetting/modesetting.h>

struct presenter_private;

struct presenter {
    struct presenter_private *private;
    int (*set_logical_zpos)(struct presenter *presenter, int logical_zpos);
    int (*get_zpos)(struct presenter *presenter);
    int (*set_scanout_callback)(struct presenter *presenter, presenter_scanout_callback_t cb, void *userdata);
    int (*push_sw_fb_layer)(struct presenter *presenter, const struct sw_fb_layer *layer);
    int (*push_placeholder_layer)(struct presenter *presenter, int n_reserved_layers);
    int (*push_display_buffer_layer)(struct presenter *presenter, const struct display_buffer_layer *layer);
    int (*flush)(struct presenter *presenter);
    void (*destroy)(struct presenter *presenter);
    struct display *display;
};

struct display {
    struct display_private *private;

    void (*get_supported_formats)(struct display *display, const enum pixfmt **formats_out, size_t *n_formats_out);
    int (*make_mapped_buffer)(struct display_buffer *buffer);
    int (*import_sw_buffer)(struct display_buffer *buffer);
    int (*import_gbm_bo)(struct display_buffer *buffer);
    int (*import_gem_bo)(struct display_buffer *buffer);
    int (*import_egl_image)(struct display_buffer *buffer);
    struct presenter *(*create_presenter)(struct display *display);
    void (*destroy)(struct display *display);

    int width, height;
    double refresh_rate;
    bool has_dimensions;
    int width_mm, height_mm;
    double flutter_pixel_ratio;
    bool supports_gbm;
    struct gbm_device *gbm_device;
    bool supported_buffer_types_for_import[kDisplayBufferTypeLast + 1];
};

struct display_buffer {
    struct {
        uint32_t kms_fb_id;
    } resources;

    struct display *display;
    struct display_buffer_backend backend;
    display_buffer_destroy_callback_t destroy_callback;
    void *userdata;
};

#define CAST_PRESENTER_PRIVATE(p_presenter, type) ((type*) (p_presenter->private))
#define CAST_DISPLAY_PRIVATE(p_display, type) ((type*) (p_display->private))

#endif // _FLUTTERPI_INCLUDE_MODESETTING_PRIVATE_H

