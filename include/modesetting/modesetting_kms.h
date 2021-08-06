#ifndef _FLUTTERPI_INCLUDE_MODESETTING_KMS_H
#define _FLUTTERPI_INCLUDE_MODESETTING_KMS_H

#ifndef HAS_KMS
#   error "KMS needs to be present to include modesetting_kms.h."
#endif // HAS_KMS

#include <stdbool.h>
#include <drm.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define DRM_NO_PROPERTY_ID (uint32_t) 0xFFFFFFFF

struct kmsdev;

struct kms_cursor;

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

struct kms_display_config {
    char connector_name[32];
    bool has_explicit_mode;
    drmModeModeInfo explicit_mode;
    const enum kmsdev_mode_preference *preferences;
    bool has_explicit_dimensions;
    int width_mm, height_mm;
};

struct kms_config {
    struct kms_display_config *display_configs;
    size_t n_display_configs;
};

struct kms_interface {
    int (*add_fd_callback)(int fd, void (*callback)(int fd, void *userdata), void *userdata);
};

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

struct kmsdev *kmsdev_new_from_fd(const struct kms_interface *interface, int fd);

struct kmsdev *kmsdev_new_from_path(const struct kms_interface *interface, const char *path);

struct kmsdev *kmsdev_new_auto(const struct kms_interface *interface);

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

void kmsdev_get_displays(struct kmsdev *dev, struct display *const **displays_out, size_t *n_displays_out);

#endif // _FLUTTERPI_INCLUDE_MODESETTING_KMS_H