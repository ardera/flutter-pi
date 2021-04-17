#ifndef _MODESETTING_H
#define _MODESETTING_H

#include <stdbool.h>
#include <pthread.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#include <collection.h>

#define LOG_MODESETTING_ERROR(format_str, ...) fprintf(stderr, "[modesetting] %s: " format_str, __func__, ##__VA_ARGS__)

#define DRM_NO_PROPERTY_ID ((uint32_t) 0xFFFFFFFF)

struct kms_display_config {
    char connector_name[32];
    bool has_explicit_mode;
    drmModeModeInfo explicit_mode;
    bool has_explicit_dimensions;
    int width_mm, height_mm;
};

struct kms_config {
    struct kms_display_config *display_configs;
    size_t n_display_configs;
};

struct kmsdev;

struct kms_cursor;

struct fbdev_display_config {
    bool has_explicit_dimensions;
    int width_mm, height_mm;
};

struct display;

struct presenter;

typedef void (*presenter_scanout_callback_t)(int crtc_index, unsigned int tv_sec, unsigned int tv_usec, void *userdata);

typedef void (*drm_fb_release_callback_t)(int32_t fb_id, void *userdata);

typedef void (*gbm_bo_release_callback_t)(struct gbm_bo *bo, void *userdata);

struct drm_fb_layer {
    int32_t fb_id;
    uint32_t src_x, src_y, src_w, src_h;
    int32_t crtc_x, crtc_y, crtc_w, crtc_h;

    bool has_rotation;
    uint8_t rotation;
    
    drm_fb_release_callback_t on_release;
    void *userdata;
};

struct gbm_bo_layer {
    struct gbm_bo *bo;
    uint32_t src_x, src_y, src_w, src_h;
    int32_t crtc_x, crtc_y, crtc_w, crtc_h;

    bool has_rotation;
    uint8_t rotation;
    
    gbm_bo_release_callback_t on_release;
    void *userdata;
};

struct sw_fb_layer {
    uint8_t *vmem;
    uint32_t width, height, pitch;
    uint32_t format;
};

enum kmsdev_mode_preference {
    kKmsdevModePreferenceNone,
    kKmsdevModePreferencePreferred,
    kKmsdevModePreferenceHighestResolution,
    kKmsdevModePreferenceLowestResolution,
    kKmsdevModePreferenceHighestRefreshrate,
    kKmsdevModePreferenceLowestRefreshrate,
    kKmsdevModePreferenceProgressive,
    kKmsdevModePreferenceInterlaced
};

/*********************
 * UTILITY FUNCTIONS *
 *********************/
static inline float mode_get_vrefresh(const drmModeModeInfo *mode) {
    return mode->clock * 1000.0 / (mode->htotal * mode->vtotal);
}

static inline uint32_t mode_get_display_area(const drmModeModeInfo *mode) {
    return ((uint32_t) mode->hdisplay) * mode->vdisplay;
}

static inline bool mode_is_interlaced(const drmModeModeInfo *mode) {
    return mode->flags & DRM_MODE_FLAG_INTERLACE;
}

static inline bool mode_is_preferred(const drmModeModeInfo *mode) {
    return mode->type & DRM_MODE_TYPE_PREFERRED;
}

bool fd_is_kmsfd(int fd);

/**********
 * KMSDEV *
 **********/
struct kmsdev *kmsdev_new_from_fd(struct event_loop *loop, int fd);

struct kmsdev *kmsdev_new_from_path(struct event_loop *loop, const char *path);

struct kmsdev *kmsdev_new_auto(struct event_loop *loop);

void kmsdev_destroy(struct kmsdev *dev);

int kmsdev_get_n_crtcs(struct kmsdev *dev);

int kmsdev_get_n_connectors(struct kmsdev *dev);

bool kmsdev_is_connector_connected(struct kmsdev *dev, int connector_index);

int kmsdev_configure_crtc(
    struct kmsdev *dev,
    int crtc_index,
    int connector_index,
    drmModeModeInfo *mode
);

int kmsdev_configure_crtc_with_preferences(
    struct kmsdev *dev,
    int crtc_index,
    int connector_index,
    const enum kmsdev_mode_preference *preferences
);

const drmModeModeInfo *kmsdev_get_selected_mode(
    struct kmsdev *dev,
    int crtc_index
);

int kmsdev_add_fb(
    struct kmsdev *dev,
    uint32_t width, uint32_t height,
    uint32_t pixel_format,
    const uint32_t bo_handles[4],
    const uint32_t pitches[4],
    const uint32_t offsets[4],
    const uint64_t modifier[4],
    uint32_t *buf_id,
    uint32_t flags
);

static inline int kmsdev_add_fb_planar(
    struct kmsdev *dev,
    uint32_t width, uint32_t height,
    uint32_t pixel_format,
    uint32_t bo_handle,
    uint32_t pitch,
    uint32_t offset,
    uint32_t modifier,
    uint32_t *buf_id,
    uint32_t flags
) {
    return kmsdev_add_fb(
        dev,
        width, height,
        pixel_format,
        (const uint32_t[4]) {
            bo_handle, 0, 0, 0
        },
        (const uint32_t[4]) {
            pitch, 0, 0, 0
        },
        (const uint32_t[4]) {
            offset, 0, 0, 0
        },
        (const uint64_t[4]) {
            modifier, 0, 0, 0
        },
        buf_id,
        flags
    );
}

int kmsdev_destroy_fb(
    struct kmsdev *dev,
    uint32_t buf_id
);

/**
 * @brief Load raw cursor data into a cursor that can be used by KMS.
 */
struct kms_cursor *kmsdev_load_cursor(
    struct kmsdev *dev,
    int width, int height,
    uint32_t format,
    int hot_x, int hot_y,
    const uint8_t *data
);

/**
 * @brief Dispose this cursor, freeing all associated resources. Make sure
 * the cursor is now longer used on any crtc before disposing it.
 */
void kmsdev_dispose_cursor(struct kmsdev *dev, struct kms_cursor *cursor);

int kmsdev_set_cursor(struct kmsdev *dev, int crtc_index, struct kms_cursor *cursor);

int kmsdev_move_cursor(struct kmsdev *dev, int crtc_index, int x, int y);

int kmsdev_configure(struct kmsdev *dev, const struct kms_config *config);

struct display *kmsdev_get_display(struct kmsdev *dev, int display_index);

void kmsdev_get_displays(struct kmsdev *dev, struct display ***displays_out, size_t *n_displays_out);

/*********
 * FBDEV *
 *********/
struct display *fbdev_display_new_from_fd(int fd, const struct fbdev_display_config *config);

struct display *fbdev_display_new_from_path(const char *path, const struct fbdev_display_config *config);

/************
 * DISPLAYS *
 ************/
void display_destroy(struct display *display);

void display_get_size(struct display *display, int *width_out, int *height_out);

bool display_has_dimensions(struct display *display);

void display_get_dimensions(struct display *display, int *width_mm_out, int *height_mm_out);

bool display_supports_gbm(struct display *display);

struct gbm_device *display_get_gbm_device(struct display *display);

struct gbm_surface *display_get_gbm_surface(struct display *display);

bool display_supports_sw_buffers(struct display *display);

void display_get_supported_formats(struct display *display, const uint32_t **formats_out, size_t *n_formats_out);

struct presenter *display_create_presenter(struct display *display);

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

int presenter_push_drm_fb_layer(
    struct presenter *presenter,
    const struct drm_fb_layer *layer
);

int presenter_push_gbm_bo_layer(
    struct presenter *presenter,
    const struct gbm_bo_layer *layer
);

int presenter_push_sw_fb_layer(
    struct presenter *presenter,
    const struct sw_fb_layer *layer
);

int presenter_push_placeholder_layer(struct presenter *presenter, int n_reserved_layers);

int presenter_flush(struct presenter *presenter);

void presenter_destroy(struct presenter *presenter);

#endif
