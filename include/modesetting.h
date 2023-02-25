// SPDX-License-Identifier: MIT
/*
 * KMS Modesetting
 *
 * - implements the interface to linux kernel modesetting
 * - allows querying connected screens, crtcs, planes, etc
 * - allows setting video modes, showing things on screen
 *
 * Copyright (c) 2022, Hannes Winkler <hanneswinkler2000@web.de>
 */

#ifndef _FLUTTERPI_INCLUDE_MODESETTING_H
#define _FLUTTERPI_INCLUDE_MODESETTING_H

#include <stdbool.h>

#include <pthread.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#include <collection.h>
#include <pixel_format.h>

#define DRM_ID_NONE ((uint32_t) 0xFFFFFFFF)

#define DRM_ID_IS_VALID(id) ((id) != 0 && (id) != DRM_ID_NONE)

// All commented out properties are not present on the RPi.
// Some of not-commented out properties we don't make usage of,
// but they could be useful in the future.
#define DRM_CONNECTOR_PROPERTIES(V)                       \
    V("Broadcast RGB", broadcast_rgb)                     \
    V("CRTC_ID", crtc_id)                                 \
    V("Colorspace", colorspace)                           \
    /* V("Content Protection", content_protection) */     \
    V("DPMS", dpms)                                       \
    V("EDID", edid)                                       \
    /* V("HDCP Content Type", hdcp_content_type) */       \
    V("HDR_OUTPUT_METADATA", hdr_output_metadata)         \
    /* V("HDR_SOURCE_METADATA", hdr_source_metadata) */   \
    /* V("PATH", path) */                                 \
    V("TILE", tile)                                       \
    V("WRITEBACK_FB_ID", writeback_fb_id)                 \
    V("WRITEBACK_OUT_FENCE_PTR", writeback_out_fence_ptr) \
    V("WRITEBACK_PIXEL_FORMATS", writeback_pixel_formats) \
    /* V("abm level", abm_level) */                       \
    /* V("aspect ratio", aspect_ratio) */                 \
    /* V("audio", audio) */                               \
    /* V("backlight", backlight) */                       \
    V("bottom margin", bottom_margin)                     \
    /* V("coherent", coherent) */                         \
    /* V("color vibrance", color_vibrance) */             \
    /* V("content type", content_type) */                 \
    /* V("dither", dither) */                             \
    /* V("dithering depth", dithering_depth) */           \
    /* V("dithering mode", dithering_mode) */             \
    /* V("flicker reduction", flicker_reduction) */       \
    /* V("hotplug_mode_update", hotplug_mode_update) */   \
    /* V("hue", hue) */                                   \
    V("left margin", left_margin)                         \
    V("link-status", link_status)                         \
    /* V("load detection", load_detection) */             \
    V("max bpc", max_bpc)                                 \
    V("mode", mode)                                       \
    V("non-desktop", non_desktop)                         \
    /* V("output_csc", output_csc) */                     \
    /* V("overscan", overscan) */                         \
    /* V("panel orientation", panel_orientation) */       \
    V("right margin", right_margin)                       \
    /* V("saturation", saturation) */                     \
    /* V("scaling mode", scaling_mode) */                 \
    /* V("select subconnector", select_subconnector) */   \
    /* V("subconnector", subconnector) */                 \
    /* V("suggested X", suggested_x) */                   \
    /* V("suggested Y", suggested_y) */                   \
    V("top margin", top_margin)                           \
    /* V("tv standard", tv_standard) */                   \
    /* V("underscan", underscan) */                       \
    /* V("underscan hborder", underscan_hborder) */       \
    /* V("underscan vborder", underscan_vborder) */       \
    /* V("vibrant hue", vibrant_hue) */                   \
    /* V("vrr_capable", vrr_capable) */

#define DRM_CRTC_PROPERTIES(V)                    \
    V("ACTIVE", active)                           \
    V("CTM", ctm)                                 \
    /* V("DEGAMMA_LUT", degamma_lut) */           \
    /* V("DEGAMMA_LUT_SIZE", degamma_lut_size) */ \
    V("GAMMA_LUT", gamma_lut)                     \
    V("GAMMA_LUT_SIZE", gamma_lut_size)           \
    V("MODE_ID", mode_id)                         \
    V("OUT_FENCE_PTR", out_fence_ptr)             \
    /* V("SCALING_FILTER", scaling_filter) */     \
    V("VRR_ENABLED", vrr_enabled)                 \
    V("rotation", rotation)                       \
    /* V("zorder", zorder) */

#define DRM_PLANE_PROPERTIES(V)                 \
    V("COLOR_ENCODING", color_encoding)         \
    V("COLOR_RANGE", color_range)               \
    V("CRTC_ID", crtc_id)                       \
    V("CRTC_H", crtc_h)                         \
    V("CRTC_W", crtc_w)                         \
    V("CRTC_X", crtc_x)                         \
    V("CRTC_Y", crtc_y)                         \
    /* V("FB_DAMAGE_CLIPS", fb_damage_clips) */ \
    V("FB_ID", fb_id)                           \
    V("IN_FENCE_FD", in_fence_fd)               \
    V("IN_FORMATS", in_formats)                 \
    /* V("SCALING_FILTER", scaling_filter) */   \
    V("SRC_H", src_h)                           \
    V("SRC_W", src_w)                           \
    V("SRC_X", src_x)                           \
    V("SRC_Y", src_y)                           \
    V("alpha", alpha)                           \
    /* V("brightness", brightness) */           \
    /* V("colorkey", colorkey) */               \
    /* V("contrast", contrast) */               \
    /* V("hue", hue) */                         \
    V("pixel blend mode", pixel_blend_mode)     \
    V("rotation", rotation)                     \
    /* V("saturation", saturation) */           \
    V("type", type)                             \
    /* V("zorder", zorder) */                   \
    V("zpos", zpos)

#define DECLARE_PROP_ID_AS_UINT32(prop_name, prop_var_name) uint32_t prop_var_name;

#define DRM_BLEND_ALPHA_OPAQUE 0xFFFF

enum drm_blend_mode {
    kPremultiplied_DrmBlendMode,
    kCoverage_DrmBlendMode,
    kNone_DrmBlendMode,

    kMax_DrmBlendMode = kNone_DrmBlendMode,
    kCount_DrmBlendMode = kMax_DrmBlendMode + 1
};

struct drm_connector_prop_ids {
    DRM_CONNECTOR_PROPERTIES(DECLARE_PROP_ID_AS_UINT32)
};

static inline void drm_connector_prop_ids_init(struct drm_connector_prop_ids *ids) {
    memset(ids, 0xFF, sizeof(*ids));
}

struct drm_crtc_prop_ids {
    DRM_CRTC_PROPERTIES(DECLARE_PROP_ID_AS_UINT32)
};

static inline void drm_crtc_prop_ids_init(struct drm_crtc_prop_ids *ids) {
    memset(ids, 0xFF, sizeof(*ids));
}

struct drm_plane_prop_ids {
    DRM_PLANE_PROPERTIES(DECLARE_PROP_ID_AS_UINT32)
};

static inline void drm_plane_prop_ids_init(struct drm_plane_prop_ids *ids) {
    memset(ids, 0xFF, sizeof(*ids));
}

#undef DECLARE_PROP_ID_AS_UINT32

// This is quite hacky, but if it works it pretty nice.
// There's asserts in modesetting.c (fn assert_rotations_work()) that make sure these rotations all work as expected.
// If any of these fail we gotta use a more conservative approach instead.
typedef struct {
    union {
        struct {
            bool rotate_0 : 1;
            bool rotate_90 : 1;
            bool rotate_180 : 1;
            bool rotate_270 : 1;
            bool reflect_x : 1;
            bool reflect_y : 1;
        };
        uint32_t u32;
        uint64_t u64;
    };
} drm_plane_transform_t;

#define PLANE_TRANSFORM_NONE ((const drm_plane_transform_t){ .u64 = 0 })
#define PLANE_TRANSFORM_ROTATE_0 ((const drm_plane_transform_t){ .u32 = DRM_MODE_ROTATE_0 })
#define PLANE_TRANSFORM_ROTATE_90 ((const drm_plane_transform_t){ .u32 = DRM_MODE_ROTATE_90 })
#define PLANE_TRANSFORM_ROTATE_180 ((const drm_plane_transform_t){ .u32 = DRM_MODE_ROTATE_180 })
#define PLANE_TRANSFORM_ROTATE_270 ((const drm_plane_transform_t){ .u32 = DRM_MODE_ROTATE_270 })
#define PLANE_TRANSFORM_REFLECT_X ((const drm_plane_transform_t){ .u32 = DRM_MODE_REFLECT_X })
#define PLANE_TRANSFORM_REFLECT_Y ((const drm_plane_transform_t){ .u32 = DRM_MODE_REFLECT_Y })

#define PLANE_TRANSFORM_IS_VALID(t) (((t).u64 & ~(DRM_MODE_ROTATE_MASK | DRM_MODE_REFLECT_MASK)) == 0)
#define PLANE_TRANSFORM_IS_ONLY_ROTATION(t) (((t).u64 & ~DRM_MODE_ROTATE_MASK) == 0 && (HWEIGHT((t).u64) == 1))
#define PLANE_TRANSFORM_IS_ONLY_REFLECTION(t) (((t).u64 & ~DRM_MODE_REFLECT_MASK) == 0 && (HWEIGHT((t).u64) == 1))

#define PLANE_TRANSFORM_ROTATE_CW(t)                               \
    (assert(PLANE_TRANSFORM_IS_ONLY_ROTATION(t)),                  \
     (t).u64 == DRM_MODE_ROTATE_0   ? PLANE_TRANSFORM_ROTATE_90 :  \
     (t).u64 == DRM_MODE_ROTATE_90  ? PLANE_TRANSFORM_ROTATE_180 : \
     (t).u64 == DRM_MODE_ROTATE_180 ? PLANE_TRANSFORM_ROTATE_270 : \
                                      PLANE_TRANSFORM_ROTATE_0)

#define PLANE_TRANSFORM_ROTATE_CCW(t)                              \
    (assert(PLANE_TRANSFORM_IS_ONLY_ROTATION(t)),                  \
     (t).u64 == DRM_MODE_ROTATE_0   ? PLANE_TRANSFORM_ROTATE_270 : \
     (t).u64 == DRM_MODE_ROTATE_90  ? PLANE_TRANSFORM_ROTATE_0 :   \
     (t).u64 == DRM_MODE_ROTATE_180 ? PLANE_TRANSFORM_ROTATE_90 :  \
                                      PLANE_TRANSFORM_ROTATE_180)

/*
enum drm_plane_rotation {
    kRotate0_DrmPlaneRotation = DRM_MODE_ROTATE_0,
    kRotate90_DrmPlaneRotation = DRM_MODE_ROTATE_90,
    kRotate180_DrmPlaneRotation = DRM_MODE_ROTATE_180,
    kRotate270_DrmPlaneRotation = DRM_MODE_ROTATE_270,
    kReflectX_DrmPlaneRotation = DRM_MODE_REFLECT_X,
    kReflectY_DrmPlaneRotation = DRM_MODE_REFLECT_Y
};
*/

enum drm_plane_type {
    kPrimary_DrmPlaneType = DRM_PLANE_TYPE_PRIMARY,
    kOverlay_DrmPlaneType = DRM_PLANE_TYPE_OVERLAY,
    kCursor_DrmPlaneType = DRM_PLANE_TYPE_CURSOR
};

struct drm_mode_blob {
    int drm_fd;
    uint32_t blob_id;
    drmModeModeInfo mode;
};

enum drm_connector_type {
    kUnknown_DrmConnectorType = DRM_MODE_CONNECTOR_Unknown,
    kVGA_DrmConnectorType = DRM_MODE_CONNECTOR_VGA,
    kDVII_DrmConnectorType = DRM_MODE_CONNECTOR_DVII,
    kDVID_DrmConnectorType = DRM_MODE_CONNECTOR_DVID,
    kDVIA_DrmConnectorType = DRM_MODE_CONNECTOR_DVIA,
    kComposite_DrmConnectorType = DRM_MODE_CONNECTOR_Composite,
    kSVIDEO_DrmConnectorType = DRM_MODE_CONNECTOR_SVIDEO,
    kLVDS_DrmConnectorType = DRM_MODE_CONNECTOR_LVDS,
    kComponent_DrmConnectorType = DRM_MODE_CONNECTOR_Component,
    k9PinDIN_DrmConnectorType = DRM_MODE_CONNECTOR_9PinDIN,
    kDisplayPort_DrmConnectorType = DRM_MODE_CONNECTOR_DisplayPort,
    kHDMIA_DrmConnectorType = DRM_MODE_CONNECTOR_HDMIA,
    kHDMIB_DrmConnectorType = DRM_MODE_CONNECTOR_HDMIB,
    kTV_DrmConnectorType = DRM_MODE_CONNECTOR_TV,
    keDP_DrmConnectorType = DRM_MODE_CONNECTOR_eDP,
    kVIRTUAL_DrmConnectorType = DRM_MODE_CONNECTOR_VIRTUAL,
    kDSI_DrmConnectorType = DRM_MODE_CONNECTOR_DSI,
    kDPI_DrmConnectorType = DRM_MODE_CONNECTOR_DPI,
    kWRITEBACK_DrmConnectorType = DRM_MODE_CONNECTOR_WRITEBACK,
#ifdef DRM_MODE_CONNECTOR_SPI
    kSPI_DrmConnectorType = DRM_MODE_CONNECTOR_SPI
#endif
};

enum drm_connection_state {
    kConnected_DrmConnectionState = DRM_MODE_CONNECTED,
    kDisconnected_DrmConnectionState = DRM_MODE_DISCONNECTED,
    kUnknown_DrmConnectionState = DRM_MODE_UNKNOWNCONNECTION
};

enum drm_subpixel_layout {
    kUnknown_DrmSubpixelLayout = DRM_MODE_SUBPIXEL_UNKNOWN,
    kHorizontalRRB_DrmSubpixelLayout = DRM_MODE_SUBPIXEL_HORIZONTAL_RGB,
    kHorizontalBGR_DrmSubpixelLayout = DRM_MODE_SUBPIXEL_HORIZONTAL_BGR,
    kVerticalRGB_DrmSubpixelLayout = DRM_MODE_SUBPIXEL_VERTICAL_RGB,
    kVerticalBGR_DrmSubpixelLayout = DRM_MODE_SUBPIXEL_VERTICAL_BGR,
    kNone_DrmSubpixelLayout = DRM_MODE_SUBPIXEL_NONE
};

struct drm_connector {
    uint32_t id;

    enum drm_connector_type type;
    uint32_t type_id;

    struct drm_connector_prop_ids ids;

    int n_encoders;
    uint32_t encoders[32];

    struct {
        enum drm_connection_state connection_state;
        enum drm_subpixel_layout subpixel_layout;
        uint32_t width_mm, height_mm;
        uint32_t n_modes;
        drmModeModeInfo *modes;
    } variable_state;

    struct {
        uint32_t crtc_id;
        uint32_t encoder_id;
    } committed_state;
};

struct drm_encoder {
    drmModeEncoder *encoder;
};

struct drm_crtc {
    uint32_t id;
    uint32_t bitmask;
    uint8_t index;

    struct drm_crtc_prop_ids ids;

    struct {
        bool has_mode;
        drmModeModeInfo mode;
        struct drm_mode_blob *mode_blob;
    } committed_state;
};

struct modified_format {
    enum pixfmt format;
    uint64_t modifier;
};

struct drm_plane {
    uint32_t id;

    /**
     * @brief Bitmap of the indexes of the CRTCs that this plane can be scanned out on.
     * 
     * i.e. if bit 0 is set, this plane can be scanned out on the CRTC with index 0.
     * if bit 0 is not set, this plane can not be scanned out on that CRTC.
     * 
     */
    uint32_t possible_crtcs;

    /// The ids of all properties associated with this plane.
    /// Any property that is not supported has the value DRM_PLANE_ID_NONE
    struct drm_plane_prop_ids ids;

    /// The type of this plane (primary, overlay, cursor)
    /// The type has some influence on what you can do with the plane.
    /// For example, it's possible the driver enforces the primary plane to be
    /// the bottom-most plane or have an opaque pixel format.
    enum drm_plane_type type;

    /// True if this plane has a zpos property, whether readonly (hardcoded)
    /// or read/write.
    /// The docs say if one plane has a zpos property, all planes should have one.
    bool has_zpos;

    /// The minimum and maximum possible zpos, if @ref has_zpos is true.
    /// If @ref has_hardcoded_zpos is true, min_zpos should equal max_zpos.
    int64_t min_zpos, max_zpos;

    /// True if this plane has a hardcoded zpos that can't
    /// be changed by userspace.
    bool has_hardcoded_zpos;

    /// The specific hardcoded zpos of this plane. Only valid if
    /// @ref has_hardcoded_zpos is true.
    int64_t hardcoded_zpos;

    /// True if this plane has a rotation property.
    bool has_rotation;

    /// Query the set booleans of the supported_rotations struct
    /// to find out of a given rotation is supported.
    /// It is assumed if both a and b are listed as supported in this struct,
    /// a rotation value of a | b is supported as well.
    /// Only valid if @ref has_rotation is supported as well.
    drm_plane_transform_t supported_rotations;

    /// True if this plane has a hardcoded rotation.
    bool has_hardcoded_rotation;

    /// The specific hardcoded rotation, only valid if @ref has_hardcoded_rotation is true.
    drm_plane_transform_t hardcoded_rotation;

    /// The framebuffer formats this plane supports. (Assuming no modifier)
    /// For example, kARGB8888_FpiPixelFormat is supported if supported_formats[kARGB8888_FpiPixelFormat] is true.
    bool supported_formats[kCount_PixFmt];

    /// True if this plane has an IN_FORMATS property attached an
    /// supports scanning out buffers with explicit format modifiers.
    bool supports_modifiers;

    /// The number of entries in the @ref supported_format_modifier_pairs array below.
    int n_supported_modified_formats;

    /// A pair of pixel format / modifier that is definitely supported.
    /// DRM_FORMAT_MOD_LINEAR is supported for most (but not all pixel formats).
    /// There are some format & modifier pairs that may be faster to scanout by the GPU.
    struct modified_format *supported_modified_formats;

    /// Whether this plane has a mutable alpha property we can set.
    bool has_alpha;

    /// Whether this plane has a pixel blend mode we can set.
    bool has_blend_mode;

    /// The supported blend modes, if @ref has_blend_mode is true.
    bool supported_blend_modes[kCount_DrmBlendMode];

    struct {
        uint32_t crtc_id;
        uint32_t fb_id;
        uint32_t src_x, src_y, src_w, src_h;
        uint32_t crtc_x, crtc_y, crtc_w, crtc_h;
        int64_t zpos;
        drm_plane_transform_t rotation;
        uint16_t alpha;
        enum drm_blend_mode blend_mode;
    } committed_state;
};

struct drmdev;
struct _drmModeModeInfo;

struct drmdev_interface {
    int (*open)(const char *path, int flags, void **fd_metadata_out, void *userdata);
    void (*close)(int fd, void *fd_metadata, void *userdata);
};

struct drmdev *drmdev_new_from_fd(int fd, const struct drmdev_interface *interface, void *userdata);

struct drmdev *drmdev_new_from_path(const char *path, const struct drmdev_interface *interface, void *userdata);

void drmdev_destroy(struct drmdev *drmdev);

DECLARE_REF_OPS(drmdev)

struct drmdev;
struct _drmModeModeInfo;

int drmdev_get_fd(struct drmdev *drmdev);
int drmdev_get_event_fd(struct drmdev *drmdev);
int drmdev_on_event_fd_ready(struct drmdev *drmdev);
const struct drm_connector *drmdev_get_selected_connector(struct drmdev *drmdev);
const struct drm_encoder *drmdev_get_selected_encoder(struct drmdev *drmdev);
const struct drm_crtc *drmdev_get_selected_crtc(struct drmdev *drmdev);
const struct _drmModeModeInfo *drmdev_get_selected_mode(struct drmdev *drmdev);

struct gbm_device *drmdev_get_gbm_device(struct drmdev *drmdev);

uint32_t drmdev_add_fb(
    struct drmdev *drmdev,
    uint32_t width,
    uint32_t height,
    enum pixfmt pixel_format,
    uint32_t bo_handle,
    uint32_t pitch,
    uint32_t offset,
    bool has_modifier,
    uint64_t modifier
);

uint32_t drmdev_add_fb_multiplanar(
    struct drmdev *drmdev,
    uint32_t width,
    uint32_t height,
    enum pixfmt pixel_format,
    uint32_t bo_handles[4],
    uint32_t pitches[4],
    uint32_t offsets[4],
    bool has_modifiers,
    uint64_t modifiers[4]
);

uint32_t drmdev_add_fb_from_dmabuf(
    struct drmdev *drmdev,
    uint32_t width,
    uint32_t height,
    enum pixfmt pixel_format,
    int prime_fd,
    uint32_t pitch,
    uint32_t offset,
    bool has_modifier,
    uint64_t modifier
);

uint32_t drmdev_add_fb_from_dmabuf_multiplanar(
    struct drmdev *drmdev,
    uint32_t width,
    uint32_t height,
    enum pixfmt pixel_format,
    int prime_fds[4],
    uint32_t pitches[4],
    uint32_t offsets[4],
    bool has_modifiers,
    uint64_t modifiers[4]
);

int drmdev_rm_fb(struct drmdev *drmdev, uint32_t fb_id);

int drmdev_get_last_vblank(struct drmdev *drmdev, uint32_t crtc_id, uint64_t *last_vblank_ns_out);

bool drmdev_can_modeset(struct drmdev *drmdev);

void drmdev_suspend(struct drmdev *drmdev);

int drmdev_resume(struct drmdev *drmdev);

static inline double mode_get_vrefresh(const drmModeModeInfo *mode) {
    return mode->clock * 1000.0 / (mode->htotal * mode->vtotal);
}

typedef void (*kms_scanout_cb_t)(struct drmdev *drmdev, uint64_t vblank_ns, void *userdata);

struct kms_fb_layer {
    uint32_t drm_fb_id;
    enum pixfmt format;
    bool has_modifier;
    uint64_t modifier;

    int32_t src_x, src_y, src_w, src_h;
    int32_t dst_x, dst_y, dst_w, dst_h;

    bool has_rotation;
    drm_plane_transform_t rotation;

    bool has_in_fence_fd;
    int in_fence_fd;
};

typedef void (*kms_fb_release_cb_t)(void *userdata);

typedef void (*kms_deferred_fb_release_cb_t)(void *userdata, int syncfile_fd);

struct kms_req_builder;

struct kms_req_builder *drmdev_create_request_builder(struct drmdev *drmdev, uint32_t crtc_id);

void kms_req_builder_destroy(struct kms_req_builder *builder);

DECLARE_REF_OPS(kms_req_builder);

struct drmdev *kms_req_builder_get_drmdev(struct kms_req_builder *builder);

int kms_req_builder_set_mode(struct kms_req_builder *builder, const drmModeModeInfo *mode);

int kms_req_builder_unset_mode(struct kms_req_builder *builder);

int kms_req_builder_set_connector(struct kms_req_builder *builder, uint32_t connector_id);

bool kms_req_builder_prefer_next_layer_opaque(struct kms_req_builder *builder);

int kms_req_builder_push_fb_layer(
    struct kms_req_builder *builder,
    const struct kms_fb_layer *layer,
    kms_fb_release_cb_t release_callback,
    kms_deferred_fb_release_cb_t deferred_release_callback,
    void *userdata
);

int kms_req_builder_push_zpos_placeholder_layer(struct kms_req_builder *builder, int64_t *zpos_out);

struct kms_req;

DECLARE_REF_OPS(kms_req);

struct kms_req *kms_req_builder_build(struct kms_req_builder *builder);

int kms_req_commit_blocking(struct kms_req *req, uint64_t *vblank_ns_out);

int kms_req_commit_nonblocking(struct kms_req *req, kms_scanout_cb_t scanout_cb, void *userdata, void_callback_t destroy_cb);

struct drm_connector *__next_connector(const struct drmdev *drmdev, const struct drm_connector *connector);

struct drm_encoder *__next_encoder(const struct drmdev *drmdev, const struct drm_encoder *encoder);

struct drm_crtc *__next_crtc(const struct drmdev *drmdev, const struct drm_crtc *crtc);

struct drm_plane *__next_plane(const struct drmdev *drmdev, const struct drm_plane *plane);

drmModeModeInfo *__next_mode(const struct drm_connector *connector, const drmModeModeInfo *mode);

#define for_each_connector_in_drmdev(drmdev, connector) \
    for (connector = __next_connector(drmdev, NULL); connector != NULL; connector = __next_connector(drmdev, connector))

#define for_each_encoder_in_drmdev(drmdev, encoder) \
    for (encoder = __next_encoder(drmdev, NULL); encoder != NULL; encoder = __next_encoder(drmdev, encoder))

#define for_each_crtc_in_drmdev(drmdev, crtc) \
    for (crtc = __next_crtc(drmdev, NULL); crtc != NULL; crtc = __next_crtc(drmdev, crtc))

#define for_each_plane_in_drmdev(drmdev, plane) \
    for (plane = __next_plane(drmdev, NULL); plane != NULL; plane = __next_plane(drmdev, plane))

#define for_each_mode_in_connector(connector, mode) \
    for (mode = __next_mode(connector, NULL); mode != NULL; mode = __next_mode(connector, mode))

#define for_each_unreserved_plane_in_atomic_req(atomic_req, plane) \
    for_each_pointer_in_pset(&(atomic_req)->available_planes, plane)

#endif  // _FLUTTERPI_INCLUDE_MODESETTING_H
