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

#ifndef _FLUTTERPI_SRC_MODESETTING_H
#define _FLUTTERPI_SRC_MODESETTING_H

#include <stdbool.h>

#include <pthread.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#include "pixel_format.h"
#include "util/collection.h"
#include "util/geometry.h"
#include "util/refcounting.h"

#define DRM_ID_NONE ((uint32_t) 0xFFFFFFFF)

#define DRM_ID_IS_VALID(id) ((id) != 0 && (id) != DRM_ID_NONE)

// All commented out properties are not present on the RPi.
// Some of not-commented out properties we don't make usage of,
// but they could be useful in the future.
//
// keep in sync with: https://drmdb.emersion.fr/properties?object-type=3233857728
#define DRM_CONNECTOR_PROPERTIES(V)                                 \
    V("Broadcast RGB", broadcast_rgb)                               \
    /* V("CONNECTOR_ID", connector_id) */                           \
    V("CRTC_ID", crtc_id)                                           \
    V("Colorspace", colorspace)                                     \
    /* V("Content Protection", content_protection) */               \
    V("DPMS", dpms)                                                 \
    V("EDID", edid)                                                 \
    /* V("HDCP Content Type", hdcp_content_type) */                 \
    V("HDR_OUTPUT_METADATA", hdr_output_metadata)                   \
    /* V("HDR_PANEL_METADATA", hdr_panel_metadata) */               \
    /* V("HDR_SOURCE_METADATA", hdr_source_metadata) */             \
    /* V("NEXT_HDR_SINK_DATA", next_hdr_sink_data) */               \
    V("Output format", output_format)                               \
    /* V("PATH", path) */                                           \
    V("TILE", tile)                                                 \
    /* V("USER_SPLIT_MODE", user_split_mode) */                     \
    V("WRITEBACK_FB_ID", writeback_fb_id)                           \
    V("WRITEBACK_OUT_FENCE_PTR", writeback_out_fence_ptr)           \
    V("WRITEBACK_PIXEL_FORMATS", writeback_pixel_formats)           \
    /* V("abm level", abm_level) */                                 \
    /* V("allm_capacity", allm_capacity) */                         \
    /* V("allm_enable", allm_enable) */                             \
    /* V("aspect ratio", aspect_ratio) */                           \
    /* V("audio", audio) */                                         \
    /* V("backlight", backlight) */                                 \
    V("bottom margin", bottom_margin)                               \
    /* V("bpc", bpc) */                                             \
    /* V("brightness", brightness) */                               \
    /* V("coherent", coherent) */                                   \
    /* V("color vibrance", color_vibrance) */                       \
    /* V("color depth", color_depth) */                             \
    /* V("color depth caps", color_depth_caps) */                   \
    /* V("color format", color_format) */                           \
    /* V("color format caps", color_format_caps) */                 \
    /* V("content type", content_type) */                           \
    /* V("contrast", contrast) */                                   \
    /* V("dither", dither) */                                       \
    /* V("dithering depth", dithering_depth) */                     \
    /* V("dithering mode", dithering_mode) */                       \
    /* V("flicker reduction", flicker_reduction) */                 \
    /* V("hdmi_color_depth_capacity", hdmi_color_depth_capacity) */ \
    /* V("hdmi_output_colorimetry", hdmi_output_colorimetry) */     \
    /* V("hdmi_output_depth", hdmi_output_depth) */                 \
    /* V("hdmi_output_mode_capacity", hdmi_output_mode_capacity) */ \
    /* V("hotplug_mode_update", hotplug_mode_update) */             \
    /* V("hue", hue) */                                             \
    V("left margin", left_margin)                                   \
    V("link-status", link_status)                                   \
    /* V("load detection", load_detection) */                       \
    V("max bpc", max_bpc)                                           \
    V("mode", mode)                                                 \
    V("non-desktop", non_desktop)                                   \
    /* V("output_csc", output_csc) */                               \
    /* V("output_hdmi_dvi", output_hdmi_dvi) */                     \
    /* V("output_type_capacity", output_type_capacity) */           \
    /* V("overscan", overscan) */                                   \
    /* V("panel orientation", panel_orientation) */                 \
    /* V("privacy-screen hw-state", privacy_screen_hw_state) */     \
    /* V("privacy-screen sw-state", privacy_screen_sw_state) */     \
    V("right margin", right_margin)                                 \
    /* V("saturation", saturation) */                               \
    /* V("scaling mode", scaling_mode) */                           \
    /* V("select subconnector", select_subconnector) */             \
    /* V("subconnector", subconnector) */                           \
    /* V("suggested X", suggested_x) */                             \
    /* V("suggested Y", suggested_y) */                             \
    /* V("sync", sync) */                                           \
    V("top margin", top_margin)                                     \
    /* V("tv standard", tv_standard) */                             \
    /* V("underscan", underscan) */                                 \
    /* V("underscan hborder", underscan_hborder) */                 \
    /* V("underscan vborder", underscan_vborder) */                 \
    /* V("vibrant hue", vibrant_hue) */                             \
    /* V("vrr_capable", vrr_capable) */

// again, crtc properties that are not available on pi 4
// are commented out.
//
// keep in sync with:
//   https://drmdb.emersion.fr/properties?object-type=3435973836
#define DRM_CRTC_PROPERTIES(V)                              \
    /* V("ACLK", aclk) */                                   \
    V("ACTIVE", active)                                     \
    /* V("ALPHA_SCALE", alpha_scale) */                     \
    /* V("BACKGROUND", background) */                       \
    /* V("BG_COLOR", bg_color) */                           \
    /* V("CABC_CALC_PIXEL_NUM", cabc_calc_pixel_num) */     \
    /* V("CABC_GLOBAL_DN", cabc_global_dn) */               \
    /* V("CABC_LUT", cabc_lut) */                           \
    /* V("CABC_MODE", cabc_mode) */                         \
    /* V("CABC_STAGE_DOWN", cabc_stage_down) */             \
    /* V("CABC_STAGE_UP", cabc_stage_up) */                 \
    V("CTM", ctm)                                           \
    /* V("DEGAMMA_LUT", degamma_lut) */                     \
    /* V("DEGAMMA_LUT_SIZE", degamma_lut_size) */           \
    /* V("DITHER_ENABLED", dither_enabled) */               \
    /* V("FEATURE", feature) */                             \
    V("GAMMA_LUT", gamma_lut)                               \
    V("GAMMA_LUT_SIZE", gamma_lut_size)                     \
    /* V("LINE_FLAG1", line_flag1) */                       \
    V("MODE_ID", mode_id)                                   \
    V("OUT_FENCE_PTR", out_fence_ptr)                       \
    /* V("PLANE_MASK", plane_mask) */                       \
    /* V("PORT_ID", port_id) */                             \
    /* V("SCALING_FILTER", scaling_filter) */               \
    /* V("SOC_ID", soc_id) */                               \
    /* V("SYNC_ENABLED", sync_enabled) */                   \
    V("VRR_ENABLED", vrr_enabled)                           \
    /* V("bg_c0", bg_c0) */                                 \
    /* V("bg_c1", bg_c1) */                                 \
    /* V("bg_c2", bg_c2) */                                 \
    /* V("bottom margin", bottom_margin) */                 \
    /* V("left margin", left_margin) */                     \
    /* V("max refresh rate", max_refresh_rate) */           \
    /* V("min refresh rate", min_refresh_rate) */           \
    /* V("output_color", output_color) */                   \
    /* V("right margin", right_margin) */                   \
    V("rotation", rotation)                                 \
    /* V("top margin", top_margin) */                       \
    /* V("variable refresh rate", variable_refresh_rate) */ \
    V("zorder", zorder)

// again, plane properties that are not present on pi 4 are commented out.
//
// keep in sync with:
//   https://drmdb.emersion.fr/properties?object-type=4008636142
#define DRM_PLANE_PROPERTIES(V)                               \
    /* V("ASYNC_COMMIT", async_commit) */                     \
    /* V("BLEND_MODE", blend_mode) */                         \
    /* V("CHROMA_SITING_H", chroma_siting_h) */               \
    /* V("CHROMA_SITING_V", chroma_siting_v) */               \
    /* V("COLOR_CONFIG", color_config) */                     \
    V("COLOR_ENCODING", color_encoding)                       \
    V("COLOR_RANGE", color_range)                             \
    /* V("COLOR_SPACE", color_space) */                       \
    V("CRTC_H", crtc_h)                                       \
    V("CRTC_ID", crtc_id)                                     \
    V("CRTC_W", crtc_w)                                       \
    V("CRTC_X", crtc_x)                                       \
    V("CRTC_Y", crtc_y)                                       \
    /* V("DEGAMMA_MODE", degamma_mode) */                     \
    /* V("EOTF", eotf) */                                     \
    /* V("FB_DAMAGE_CLIPS", fb_damage_clips) */               \
    V("FB_ID", fb_id)                                         \
    /* V("FEATURE", feature) */                               \
    /* V("GLOBAL_ALPHA", global_alpha) */                     \
    /* V("INPUT_HEIGHT", input_height) */                     \
    /* V("INPUT_WIDTH", input_width) */                       \
    V("IN_FENCE_FD", in_fence_fd)                             \
    V("IN_FORMATS", in_formats)                               \
    /* V("NAME", name) */                                     \
    /* V("NV_HDR_STATIC_METADATA", nv_hdr_static_metadata) */ \
    /* V("NV_INPUT_COLORSPACE", nv_input_colorspace) */       \
    /* V("OUTPUT_HEIGHT", output_height) */                   \
    /* V("OUTPUT_WIDTH", output_width) */                     \
    /* V("ROI", roi) */                                       \
    /* V("SCALE_RATE", scale_rate) */                         \
    /* V("SCALING_FILTER", scaling_filter) */                 \
    /* V("SHARE_ID", share_id) */                             \
    V("SRC_H", src_h)                                         \
    V("SRC_W", src_w)                                         \
    V("SRC_X", src_x)                                         \
    V("SRC_Y", src_y)                                         \
    /* V("WATERMARK", watermark) */                           \
    /* V("ZPOS", zpos) */                                     \
    V("alpha", alpha)                                         \
    /* V("brightness", brightness) */                         \
    /* V("colorkey", colorkey) */                             \
    /* V("contrast", contrast) */                             \
    /* V("g_alpha_en", g_alpha_en) */                         \
    /* V("hue", hue) */                                       \
    V("pixel blend mode", pixel_blend_mode)                   \
    V("rotation", rotation)                                   \
    /* V("saturation", saturation) */                         \
    /* V("tpg", tpg) */                                       \
    V("type", type)                                           \
    /* V("zorder", zorder) */                                 \
    V("zpos", zpos)

#define DECLARE_PROP_ID_AS_UINT32(prop_name, prop_var_name) uint32_t prop_var_name;

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
#define PLANE_TRANSFORM_ROTATE_0 ((const drm_plane_transform_t){ .u64 = DRM_MODE_ROTATE_0 })
#define PLANE_TRANSFORM_ROTATE_90 ((const drm_plane_transform_t){ .u64 = DRM_MODE_ROTATE_90 })
#define PLANE_TRANSFORM_ROTATE_180 ((const drm_plane_transform_t){ .u64 = DRM_MODE_ROTATE_180 })
#define PLANE_TRANSFORM_ROTATE_270 ((const drm_plane_transform_t){ .u64 = DRM_MODE_ROTATE_270 })
#define PLANE_TRANSFORM_REFLECT_X ((const drm_plane_transform_t){ .u64 = DRM_MODE_REFLECT_X })
#define PLANE_TRANSFORM_REFLECT_Y ((const drm_plane_transform_t){ .u64 = DRM_MODE_REFLECT_Y })

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
    /// @brief The DRM id of this plane.
    uint32_t id;

    /// @brief Bitmap of the indexes of the CRTCs that this plane can be scanned out on.
    ///
    /// I.e. if bit 0 is set, this plane can be scanned out on the CRTC with index 0.
    /// if bit 0 is not set, this plane can not be scanned out on that CRTC.
    uint32_t possible_crtcs;

    /// @brief The ids of all properties associated with this plane.
    ///
    /// Any property that is not supported has the value @ref DRM_PROP_ID_NONE.
    struct drm_plane_prop_ids ids;

    /// @brief The type of this plane (primary, overlay, cursor)
    ///
    /// The type has some influence on what you can do with the plane.
    /// For example, it's possible the driver enforces the primary plane to be
    /// the bottom-most plane or have an opaque pixel format.
    enum drm_plane_type type;

    /// @brief True if this plane has a zpos property.
    ///
    /// This does not mean it is changeable by userspace. Check
    /// @ref has_hardcoded_zpos for that.
    ///
    /// The docs say if one plane has a zpos property, all planes should have one.
    bool has_zpos;

    /// @brief The minimum and maximum possible zpos
    ///
    /// Only valid if @ref has_zpos is true.
    ///
    /// If @ref has_hardcoded_zpos is true, min_zpos should equal max_zpos.
    int64_t min_zpos, max_zpos;

    /// @brief True if this plane has a hardcoded zpos that can't
    /// be changed by userspace.
    bool has_hardcoded_zpos;

    /// @brief The specific hardcoded zpos of this plane.
    ///
    /// Only valid if @ref has_hardcoded_zpos is true.
    int64_t hardcoded_zpos;

    /// @brief True if this plane has a rotation property.
    ///
    /// This does not mean that it is mutable. Check @ref has_hardcoded_rotation
    /// and @ref supported_rotations for that.
    bool has_rotation;

    /// @brief The set of rotations that are supported by this plane.
    ///
    /// Query the set booleans of the supported_rotations struct
    /// to find out of a given rotation is supported.
    ///
    /// It is assumed if both a and b are listed as supported in this struct,
    /// a rotation value of a | b is supported as well.
    /// Only valid if @ref has_rotation is supported as well.
    drm_plane_transform_t supported_rotations;

    /// @brief True if this plane has a hardcoded rotation.
    bool has_hardcoded_rotation;

    /// @brief The specific hardcoded rotation.
    ///
    /// Only valid if @ref has_hardcoded_rotation is true.
    drm_plane_transform_t hardcoded_rotation;

    /// @brief The framebuffer formats this plane supports, assuming no
    /// (implicit) modifier.
    ///
    /// For example, @ref PIXFMT_ARGB8888 is supported if
    /// supported_formats[PIXFMT_ARGB8888] is true.
    bool supported_formats[PIXFMT_COUNT];

    /// @brief True if this plane has an IN_FORMATS property attached and
    /// supports scanning out buffers with explicit format modifiers.
    bool supports_modifiers;

    /// @brief A pair of pixel format / modifier that is definitely supported.
    ///
    /// DRM_FORMAT_MOD_LINEAR is supported for most (but not all pixel formats).
    /// There are some format & modifier pairs that may be faster to scanout by the GPU.
    ///
    /// Is NULL if the plane didn't specify an IN_FORMATS property.
    ///
    /// Use @ref drm_plane_for_each_modified_format to iterate over the supported modified
    /// formats.
    struct drm_format_modifier_blob *supported_modified_formats_blob;

    /// @brief Whether this plane has a mutable alpha property we can set.
    bool has_alpha;

    /// @brief The minimum and maximum alpha values.
    ///
    /// Only valid if @ref has_alpha is true.
    ///
    /// This should be 0x0000..0xFFFF, but the xilinx driver uses different values.
    uint16_t min_alpha, max_alpha;

    /// @brief Whether this plane has a mutable pixel blend mode we can set.
    bool has_blend_mode;

    /// @brief The supported blend modes.
    ///
    /// Only valid if @ref has_blend_mode is true.
    bool supported_blend_modes[kCount_DrmBlendMode];

    struct {
        /// @brief The committed CRTC id.
        ///
        /// The id of the CRTC this plane is associated with, right now.
        uint32_t crtc_id;

        /// @brief The committed framebuffer id.
        ///
        /// The id of the framebuffer this plane is scanning out, right now.
        uint32_t fb_id;

        /// @brief The committed source rect from the framebuffer.
        ///
        /// Only valid when using atomic modesetting.
        uint32_t src_x, src_y, src_w, src_h;

        /// @brief The committed destination rect, on the CRTC.
        ///
        /// Only valid when using atomic modesetting.
        uint32_t crtc_x, crtc_y, crtc_w, crtc_h;

        /// @brief The committed plane zpos.
        ///
        /// Only valid if @ref drm_plane.has_zpos is true.
        int64_t zpos;

        /// @brief The committed plane rotation.
        ///
        /// Only valid if @ref drm_plane.has_rotation is true.
        drm_plane_transform_t rotation;

        /// @brief The committed alpha property.
        ///
        /// Only valid if @ref drm_plane.has_alpha is true.
        uint16_t alpha;

        /// @brief  The committed blend mode.
        ///
        /// Only valid if @ref drm_plane.has_blend_mode is true.
        enum drm_blend_mode blend_mode;

        /// @brief If false, we don't know about the committed format.
        ///
        /// This can be false on debian buster for example, because we don't
        /// have drmModeGetFB2 here, which is required for querying the pixel
        /// format of a framebuffer. When a plane is associated with our own
        /// framebuffer (created via @ref drmdev_add_fb, for example), we can
        /// still determine the pixel format because we track the pixel formats
        /// of each added drm fb.
        ///
        /// But for foreign framebuffers, i.e. the ones that are set on
        /// the plane by fbcon when flutter-pi is starting up, we simply can't
        /// tell.
        ///
        /// We need to know though because we need to call @ref drmModeSetCrtc
        /// if the pixel format of a drm plane has changed.
        bool has_format;

        /// @brief The pixel format of the currently committed framebuffer.
        ///
        /// Only valid if @ref has_format is true.
        enum pixfmt format;
    } committed_state;
};

/**
 * @brief Callback that will be called on each iteration of
 * @ref drm_plane_for_each_modified_format.
 *
 * Should return true if looping should continue. False if iterating should be
 * stopped.
 *
 * @param plane The plane that was passed to @ref drm_plane_for_each_modified_format.
 * @param index The index of the pixel format. Is incremented for each call of the callback.
 * @param pixel_format The pixel format.
 * @param modifier The modifier of this pixel format.
 * @param userdata Userdata that was passed to @ref drm_plane_for_each_modified_format.
 */
typedef bool (*drm_plane_modified_format_callback_t)(
    struct drm_plane *plane,
    int index,
    enum pixfmt pixel_format,
    uint64_t modifier,
    void *userdata
);

struct drmdev;

/**
 * @brief Iterates over every supported pixel-format & modifier pair.
 *
 * See @ref drm_plane_modified_format_callback_t for documentation on the callback.
 */
void drm_plane_for_each_modified_format(struct drm_plane *plane, drm_plane_modified_format_callback_t callback, void *userdata);

bool drm_plane_supports_modified_format(struct drm_plane *plane, enum pixfmt format, uint64_t modifier);

bool drm_plane_supports_unmodified_format(struct drm_plane *plane, enum pixfmt format);

bool drm_crtc_any_plane_supports_format(struct drmdev *drmdev, struct drm_crtc *crtc, enum pixfmt pixel_format);

struct _drmModeModeInfo;

struct drmdev_interface {
    int (*open)(const char *path, int flags, void **fd_metadata_out, void *userdata);
    void (*close)(int fd, void *fd_metadata, void *userdata);
};

struct drmdev *drmdev_new_from_interface_fd(int fd, void *fd_metadata, const struct drmdev_interface *interface, void *userdata);

struct drmdev *drmdev_new_from_path(const char *path, const struct drmdev_interface *interface, void *userdata);

DECLARE_REF_OPS(drmdev)

struct drmdev;
struct _drmModeModeInfo;

int drmdev_get_fd(struct drmdev *drmdev);
int drmdev_get_event_fd(struct drmdev *drmdev);
bool drmdev_supports_dumb_buffers(struct drmdev *drmdev);
int drmdev_create_dumb_buffer(
    struct drmdev *drmdev,
    int width,
    int height,
    int bpp,
    uint32_t *gem_handle_out,
    uint32_t *pitch_out,
    size_t *size_out
);
void drmdev_destroy_dumb_buffer(struct drmdev *drmdev, uint32_t gem_handle);
void *drmdev_map_dumb_buffer(struct drmdev *drmdev, uint32_t gem_handle, size_t size);
void drmdev_unmap_dumb_buffer(struct drmdev *drmdev, void *map, size_t size);
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
    const uint32_t bo_handles[4],
    const uint32_t pitches[4],
    const uint32_t offsets[4],
    bool has_modifiers,
    const uint64_t modifiers[4]
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
    const int prime_fds[4],
    const uint32_t pitches[4],
    const uint32_t offsets[4],
    bool has_modifiers,
    const uint64_t modifiers[4]
);

uint32_t drmdev_add_fb_from_gbm_bo(struct drmdev *drmdev, struct gbm_bo *bo, bool cast_opaque);

int drmdev_rm_fb_locked(struct drmdev *drmdev, uint32_t fb_id);

int drmdev_rm_fb(struct drmdev *drmdev, uint32_t fb_id);

int drmdev_get_last_vblank(struct drmdev *drmdev, uint32_t crtc_id, uint64_t *last_vblank_ns_out);

bool drmdev_can_modeset(struct drmdev *drmdev);

void drmdev_suspend(struct drmdev *drmdev);

int drmdev_resume(struct drmdev *drmdev);

int drmdev_move_cursor(struct drmdev *drmdev, uint32_t crtc_id, struct vec2i pos);

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

    bool prefer_cursor;
};

typedef void (*kms_fb_release_cb_t)(void *userdata);

typedef void (*kms_deferred_fb_release_cb_t)(void *userdata, int syncfile_fd);

struct kms_req_builder;

struct kms_req_builder *drmdev_create_request_builder(struct drmdev *drmdev, uint32_t crtc_id);

DECLARE_REF_OPS(kms_req_builder);

/**
 * @brief Gets the @ref drmdev associated with this KMS request builder.
 *
 * @param builder The KMS request builder.
 * @returns The drmdev associated with this KMS request builder.
 */
struct drmdev *kms_req_builder_get_drmdev(struct kms_req_builder *builder);

/**
 * @brief Gets the CRTC associated with this KMS request builder.
 *
 * @param builder The KMS request builder.
 * @returns The CRTC associated with this KMS request builder.
 */
struct drm_crtc *kms_req_builder_get_crtc(struct kms_req_builder *builder);

/**
 * @brief Adds a property to the KMS request that will set the given video mode
 * on this CRTC on commit, regardless of whether the currently committed output
 * mode is the same.
 *
 * @param builder The KMS request builder.
 * @param mode The output mode to set (on @ref kms_req_commit)
 * @returns Zero if successful, positive errno-style error on failure.
 */
int kms_req_builder_set_mode(struct kms_req_builder *builder, const drmModeModeInfo *mode);

/**
 * @brief Adds a property to the KMS request that will unset the configured
 * output mode for this CRTC on commit, regardless of whether the currently
 * committed output mdoe is already unset.
 *
 * @param builder The KMS request builder.
 * @returns Zero if successful, positive errno-style error on failure.
 */
int kms_req_builder_unset_mode(struct kms_req_builder *builder);

/**
 * @brief Adds a property to the KMS request that will change the connector
 * that this CRTC is displaying content on to @param connector_id.
 *
 * @param builder The KMS request builder.
 * @param connector_id The connector that this CRTC should display contents on.
 * @returns Zero if successful, EINVAL if the @param connector_id is invalid.
 */
int kms_req_builder_set_connector(struct kms_req_builder *builder, uint32_t connector_id);

/**
 * @brief True if the next layer pushed using @ref kms_req_builder_push_fb_layer
 * should be opaque, i.e. use a framebuffer which has a pixel format that has no
 * alpha channel.
 *
 * This is true for the bottom-most layer. There are some display controllers
 * that don't support non-opaque pixel formats for the bottom-most (primary)
 * plane. So ignoring this might lead to an EINVAL on commit.
 *
 * @param builder The KMS request builder.
 * @returns True if the next layer should preferably be opaque, false if there's
 *          no preference.
 */
bool kms_req_builder_prefer_next_layer_opaque(struct kms_req_builder *builder);

/**
 * @brief Adds a new framebuffer (display) layer on top of the last layer.
 *
 * If this is the first layer, the framebuffer should cover the entire screen
 * (CRTC).
 *
 * To allow the use of explicit fencing, specify an in_fence_fd in @param layer
 * and a @param deferred_release_callback.
 *
 * If explicit fencing is supported:
 *   - the in_fence_fd should be a DRM syncobj fd that signals
 *     when the GPU has finished rendering to the framebuffer and is ready
 *     to be scanned out.
 *   - @param deferred_release_callback will be called
 *     with a DRM syncobj fd that is signaled once the framebuffer is no longer
 *     being displayed on screen (and can be rendered into again)
 *
 * If explicit fencing is not supported:
 *   - the in_fence_fd in @param layer will be closed by this procedure.
 *   - @param deferred_release_callback will NOT be called and
 *     @param release_callback will be called instead.
 *
 * Explicit fencing is supported: When atomic modesetting is being used and
 * the driver supports it. (Driver has IN_FENCE_FD plane and OUT_FENCE_PTR crtc
 * properties)
 *
 * @param builder          The KMS request builder.
 * @param layer            The exact details (src pos, output pos, rotation,
 *                         framebuffer) of the layer that should be shown on
 *                         screen.
 * @param release_callback Called when the framebuffer of this layer is no
 *                         longer being shown on screen. This is called with the
 *                         drmdev locked, so make sure to use _locked variants
 *                         of any drmdev calls.
 * @param deferred_release_callback (Unimplemented right now) If this is present,
 *                                  this callback might be called instead of
 *                                  @param release_callback.
 *                                  This is called with a DRM syncobj fd that is
 *                                  signaled when the framebuffer is no longer
 *                                  shown on screen.
 *                                  Legacy DRM modesetting does not support
 *                                  explicit fencing, in which case
 *                                  @param release_callback will be called
 *                                  instead.
 * @param userdata Userdata pointer that's passed to the release_callback or
 *                 deferred_release_callback as-is.
 * @returns Zero on success, otherwise:
 *            - EINVAL: if attempting to push a second framebuffer layer, if
 *                driver supports atomic modesetting but legacy modesetting is
 *                being used.
 *            - EIO: if no DRM plane could be found that supports displaying
 *                this framebuffer layer. Either the pixel format is not
 *                supported, the modifier, the rotation or the drm device
 *                doesn't have enough planes.
 *            - The error returned by @ref close if closing the in_fence_fd
 *              fails.
 */
int kms_req_builder_push_fb_layer(
    struct kms_req_builder *builder,
    const struct kms_fb_layer *layer,
    kms_fb_release_cb_t release_callback,
    kms_deferred_fb_release_cb_t deferred_release_callback,
    void *userdata
);

/**
 * @brief Push a "fake" layer that just keeps one zpos free, incase something
 * other than KMS wants to display contents there. (e.g. omxplayer)
 *
 * @param builder The KMS request builder.
 * @param zpos_out Filled with the zpos that won't be occupied by the request
 *                 builder.
 * @returns Zero.
 */
int kms_req_builder_push_zpos_placeholder_layer(struct kms_req_builder *builder, int64_t *zpos_out);

/**
 * @brief A KMS request (atomic or legacy modesetting) that can be committed to
 * change the state of a single CRTC.
 *
 * Only way to construct this is by building a KMS request using
 * @ref kms_req_builder and then calling @ref kms_req_builder_build.
 */
struct kms_req;

DECLARE_REF_OPS(kms_req);

/**
 * @brief Build the KMS request builder into an actual, immutable KMS request
 * that can be committed. Internally this doesn't do much at all.
 *
 * @param builder The KMS request builder that should be built.
 * @returns KMS request that can be committed using @ref kms_req_commit_blocking
 *          or @ref kms_req_commit_nonblocking.
 *          The returned KMS request has refcount 1. Unref using
 *          @ref kms_req_unref after usage.
 */
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

#define for_each_crtc_in_drmdev(drmdev, crtc) for (crtc = __next_crtc(drmdev, NULL); crtc != NULL; crtc = __next_crtc(drmdev, crtc))

#define for_each_plane_in_drmdev(drmdev, plane) for (plane = __next_plane(drmdev, NULL); plane != NULL; plane = __next_plane(drmdev, plane))

#define for_each_mode_in_connector(connector, mode) \
    for (mode = __next_mode(connector, NULL); mode != NULL; mode = __next_mode(connector, mode))

#define for_each_unreserved_plane_in_atomic_req(atomic_req, plane) for_each_pointer_in_pset(&(atomic_req)->available_planes, plane)

#endif  // _FLUTTERPI_SRC_MODESETTING_H
