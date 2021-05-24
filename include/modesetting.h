#ifndef _MODESETTING_H
#define _MODESETTING_H

#include <stdbool.h>
#include <pthread.h>

#ifdef HAS_FBDEV
#   include <linux/fb.h>
#endif
#ifdef HAS_KMS
#   include <drm.h>
#   include <drm_fourcc.h>
#   include <xf86drm.h>
#   include <xf86drmMode.h>
#endif
#ifdef HAS_GBM
#   include <gbm.h>
#endif

#include <collection.h>


#define LOG_MODESETTING_ERROR(format_str, ...) fprintf(stderr, "[modesetting] %s: " format_str, __func__, ##__VA_ARGS__)

struct presenter;
struct display_buffer_backend;
struct display_buffer;
struct display;

typedef void (*presenter_scanout_callback_t)(struct display *display, uint64_t ns, void *userdata);

typedef void (*display_buffer_destroy_callback_t)(struct display *display, const struct display_buffer_backend *backend, void *userdata);

enum pixfmt {
    kRGB565,
    kARGB8888,
    kXRGB8888,
    kBGRA8888,
    kRGBA8888,
};

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
    kDspBufLayerRotationNone = 1 << 0,
    kDspBufLayerRotation90 = 1 << 1,
    kDspBufLayerRotation180 = 1 << 2,
    kDspBufLayerRotation270 = 1 << 3,
    kDspBufLayerRotationReflectX = 1 << 4,
    kDspBufLayerRotationReflectY = 1 << 5
};

struct display_buffer_layer {
    struct display_buffer *buffer;

    int buffer_x, buffer_y, buffer_w, buffer_h;
    int display_x, display_y, display_w, display_h;

    enum dspbuf_layer_rotation rotation;
};

#ifdef HAS_FBDEV
struct fbdev_pixfmt {
    struct fb_bitfield r, g, b, a;
};
#endif

struct pixfmt_info {
    /**
     * @brief A descriptive, human-readable name for this pixel format.
     */
    const char *name;

    /**
     * @brief The pixel format that this struct provides information about.
     */
    enum pixfmt format;

    /**
     * @brief How many bits per pixel does this pixel format use?
     */
    int bits_per_pixel;

    /**
     * @brief True if there's no way to specify transparency with this format.
     */
    bool is_opaque;

#ifdef HAS_FBDEV
    /**
     * @brief The fbdev format equivalent to this pixel format.
     */
    struct fbdev_pixfmt fbdev_format;
#endif
#ifdef HAS_GBM
    /**
     * @brief The GBM format equivalent to this pixel format.
     */
    uint32_t gbm_format;
#endif
#ifdef HAS_KMS
    /**
     * @brief The GBM format equivalent to this pixel format.
     */
    uint32_t drm_format;
#endif
};

extern const struct pixfmt_info pixfmt_infos[];
extern const size_t n_pixfmt_infos;

static inline const struct pixfmt_info *get_pixfmt_info(enum pixfmt format) {
    return pixfmt_infos + format;
}

#ifdef HAS_GBM
typedef void (*gbm_bo_release_callback_t)(struct gbm_bo *bo, void *userdata);

struct gbm_bo_layer {
    struct gbm_bo *bo;
    uint32_t src_x, src_y, src_w, src_h;
    int32_t crtc_x, crtc_y, crtc_w, crtc_h;

    bool has_rotation;
    uint8_t rotation;
    
    gbm_bo_release_callback_t on_release;
    void *userdata;
};
#endif

#ifdef HAS_KMS
#include <xf86drm.h>
#include <xf86drmMode.h>

#define DRM_NO_PROPERTY_ID ((uint32_t) 0xFFFFFFFF)

typedef void (*drm_fb_release_callback_t)(int32_t fb_id, void *userdata);

struct drm_fb_layer {
    int32_t fb_id;
    uint32_t src_x, src_y, src_w, src_h;
    int32_t crtc_x, crtc_y, crtc_w, crtc_h;

    bool has_rotation;
    uint8_t rotation;
    
    drm_fb_release_callback_t on_release;
    void *userdata;
};
#endif

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

#ifdef HAS_KMS
int presenter_push_drm_fb_layer(
    struct presenter *presenter,
    const struct drm_fb_layer *layer
);
#endif

#ifdef HAS_GBM
int presenter_push_gbm_bo_layer(
    struct presenter *presenter,
    const struct gbm_bo_layer *layer
);
#endif

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

/************
 * DISPLAYS *
 ************/
void display_destroy(struct display *display);

double display_get_refreshrate(struct display *display);

void display_get_size(struct display *display, int *width_out, int *height_out);

bool display_has_dimensions(struct display *display);

void display_get_dimensions(struct display *display, int *width_mm_out, int *height_mm_out);

bool display_supports_gbm(struct display *display);

#ifdef HAS_GBM
struct gbm_device *display_get_gbm_device(struct display *display);

struct gbm_surface *display_get_gbm_surface(struct display *display);
#endif

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

#ifdef HAS_FBDEV
struct fbdev_display_config {
    bool has_explicit_dimensions;
    int width_mm, height_mm;
};

struct display *fbdev_display_new_from_fd(int fd, const struct fbdev_display_config *config);

struct display *fbdev_display_new_from_path(const char *path, const struct fbdev_display_config *config);
#endif

#ifdef HAS_KMS
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

void kmsdev_get_displays(struct kmsdev *dev, struct display *const **displays_out, size_t *n_displays_out);
#endif
#endif
