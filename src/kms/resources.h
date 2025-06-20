#ifndef _FLUTTERPI_MODESETTING_RESOURCES_H
#define _FLUTTERPI_MODESETTING_RESOURCES_H

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <pthread.h>

#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <libudev.h>

#include "pixel_format.h"
#include "util/bitset.h"
#include "util/list.h"
#include "util/lock_ops.h"
#include "util/logging.h"
#include "util/macros.h"
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

#define DRM_BLEND_ALPHA_OPAQUE 0xFFFF

enum drm_blend_mode {
    DRM_BLEND_MODE_PREMULTIPLIED,
    DRM_BLEND_MODE_COVERAGE,
    DRM_BLEND_MODE_NONE,

    DRM_BLEND_MODE_MAX = DRM_BLEND_MODE_NONE,
    DRM_BLEND_MODE_COUNT = DRM_BLEND_MODE_MAX + 1
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
    DRM_PRIMARY_PLANE = DRM_PLANE_TYPE_PRIMARY,
    DRM_OVERLAY_PLANE = DRM_PLANE_TYPE_OVERLAY,
    DRM_CURSOR_PLANE = DRM_PLANE_TYPE_CURSOR
};

enum drm_connector_type {
    DRM_CONNECTOR_TYPE_UNKNOWN = DRM_MODE_CONNECTOR_Unknown,
    DRM_CONNECTOR_TYPE_VGA = DRM_MODE_CONNECTOR_VGA,
    DRM_CONNECTOR_TYPE_DVII = DRM_MODE_CONNECTOR_DVII,
    DRM_CONNECTOR_TYPE_DVID = DRM_MODE_CONNECTOR_DVID,
    DRM_CONNECTOR_TYPE_DVIA = DRM_MODE_CONNECTOR_DVIA,
    DRM_CONNECTOR_TYPE_COMPOSITE = DRM_MODE_CONNECTOR_Composite,
    DRM_CONNECTOR_TYPE_SVIDEO = DRM_MODE_CONNECTOR_SVIDEO,
    DRM_CONNECTOR_TYPE_LVDS = DRM_MODE_CONNECTOR_LVDS,
    DRM_CONNECTOR_TYPE_COMPONENT = DRM_MODE_CONNECTOR_Component,
    DRM_CONNECTOR_TYPE_DIN = DRM_MODE_CONNECTOR_9PinDIN,
    DRM_CONNECTOR_TYPE_DISPLAYPORT = DRM_MODE_CONNECTOR_DisplayPort,
    DRM_CONNECTOR_TYPE_HDMIA = DRM_MODE_CONNECTOR_HDMIA,
    DRM_CONNECTOR_TYPE_HDMIB = DRM_MODE_CONNECTOR_HDMIB,
    DRM_CONNECTOR_TYPE_TV = DRM_MODE_CONNECTOR_TV,
    DRM_CONNECTOR_TYPE_EDP = DRM_MODE_CONNECTOR_eDP,
    DRM_CONNECTOR_TYPE_VIRTUAL = DRM_MODE_CONNECTOR_VIRTUAL,
    DRM_CONNECTOR_TYPE_DSI = DRM_MODE_CONNECTOR_DSI,
    DRM_CONNECTOR_TYPE_DPI = DRM_MODE_CONNECTOR_DPI,
    DRM_CONNECTOR_TYPE_WRITEBACK = DRM_MODE_CONNECTOR_WRITEBACK,
#ifdef DRM_MODE_CONNECTOR_SPI
    DRM_CONNECTOR_TYPE_SPI = DRM_MODE_CONNECTOR_SPI,
#endif
#ifdef DRM_MODE_CONNECTOR_USB
    DRM_CONNECTOR_TYPE_USB = DRM_MODE_CONNECTOR_USB,
#endif
};

enum drm_connection_state {
    DRM_CONNSTATE_CONNECTED = DRM_MODE_CONNECTED,
    DRM_CONNSTATE_DISCONNECTED = DRM_MODE_DISCONNECTED,
    DRM_CONNSTATE_UNKNOWN = DRM_MODE_UNKNOWNCONNECTION
};

enum drm_subpixel_layout {
    DRM_SUBPIXEL_UNKNOWN = DRM_MODE_SUBPIXEL_UNKNOWN,
    DRM_SUBPIXEL_HORIZONTAL = DRM_MODE_SUBPIXEL_HORIZONTAL_RGB,
    DRM_SUBPIXEL_HORIZONTAL_BGR = DRM_MODE_SUBPIXEL_HORIZONTAL_BGR,
    DRM_SUBPIXEL_VERTICAL_RGB = DRM_MODE_SUBPIXEL_VERTICAL_RGB,
    DRM_SUBPIXEL_VERTICAL_BGR = DRM_MODE_SUBPIXEL_VERTICAL_BGR,
    DRM_SUBPIXEL_NONE = DRM_MODE_SUBPIXEL_NONE
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

enum drm_encoder_type {
    DRM_ENCODER_TYPE_NONE = DRM_MODE_ENCODER_NONE,
    DRM_ENCODER_TYPE_TMDS = DRM_MODE_ENCODER_TMDS,
    DRM_ENCODER_TYPE_DAC = DRM_MODE_ENCODER_DAC,
    DRM_ENCODER_TYPE_LVDS = DRM_MODE_ENCODER_LVDS,
    DRM_ENCODER_TYPE_TVDAC = DRM_MODE_ENCODER_TVDAC,
    DRM_ENCODER_TYPE_VIRTUAL = DRM_MODE_ENCODER_VIRTUAL,
    DRM_ENCODER_TYPE_DSI = DRM_MODE_ENCODER_DSI,
    DRM_ENCODER_TYPE_DPMST = DRM_MODE_ENCODER_DPMST,
    DRM_ENCODER_TYPE_DPI = DRM_MODE_ENCODER_DPI,
    DRM_ENCODER_TYPE_MAX = DRM_MODE_ENCODER_DPI,
};

struct drm_encoder {
    uint32_t id;
	enum drm_encoder_type type;
	
	uint32_t possible_crtcs;
	uint32_t possible_clones;

    struct {
        uint32_t crtc_id;
    } variable_state;
};

struct drm_crtc {
    uint32_t id;
    uint32_t bitmask;
    uint8_t index;

    struct drm_crtc_prop_ids ids;

    struct {
        bool has_mode;
        drmModeModeInfo mode;
        struct drm_blob *mode_blob;
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

    /// @brief Whether this plane has a mutable pixel blend mode we can set.
    bool has_blend_mode;

    /// @brief The supported blend modes.
    ///
    /// Only valid if @ref has_blend_mode is true.
    bool supported_blend_modes[DRM_BLEND_MODE_COUNT];

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

/**
 * @brief A set of DRM resources, e.g. connectors, encoders, CRTCs, planes.
 * 
 * This struct is refcounted, so you should use @ref drm_resources_ref and @ref drm_resources_unref
 * to manage its lifetime.
 * 
 * DRM resources can change, e.g. when a monitor is plugged in or out, or a connector is added.
 * You can update the resources with @ref drm_resources_update.
 * 
 * @attention DRM resources are not thread-safe. They should only be accessed on a single thread
 * in their entire lifetime. This includes updates using @ref drm_resources_update.
 * 
 */
struct drm_resources {
    refcount_t n_refs;

    bool have_filter;
    struct {
        uint32_t connector_id;
        uint32_t encoder_id;
        uint32_t crtc_id;

        size_t n_planes;
        uint32_t plane_ids[32];
    } filter;

    uint32_t min_width, max_width;
	uint32_t min_height, max_height;

    size_t n_connectors;
    struct drm_connector *connectors;

    size_t n_encoders;
    struct drm_encoder *encoders;

    size_t n_crtcs;
    struct drm_crtc *crtcs;

    size_t n_planes;
    struct drm_plane *planes;
};

/**
 * @brief Iterates over every supported pixel-format & modifier pair.
 *
 * See @ref drm_plane_modified_format_callback_t for documentation on the callback.
 */
void drm_plane_for_each_modified_format(struct drm_plane *plane, drm_plane_modified_format_callback_t callback, void *userdata);

bool drm_plane_supports_modified_formats(struct drm_plane *plane);

bool drm_plane_supports_modified_format(struct drm_plane *plane, enum pixfmt format, uint64_t modifier);

bool drm_plane_supports_unmodified_format(struct drm_plane *plane, enum pixfmt format);

bool drm_resources_any_crtc_plane_supports_format(struct drm_resources *res, uint32_t crtc_id, enum pixfmt pixel_format);


/**
 * @brief Create a new drm_resources object
 */
struct drm_resources *drm_resources_new(int drm_fd);

struct drm_resources *drm_resources_new_filtered(int drm_fd, uint32_t connector_id, uint32_t encoder_id, uint32_t crtc_id, size_t n_planes, const uint32_t *plane_ids);

struct drm_resources *drm_resources_dup_filtered(struct drm_resources *res, uint32_t connector_id, uint32_t encoder_id, uint32_t crtc_id, size_t n_planes, const uint32_t *plane_ids);

void drm_resources_destroy(struct drm_resources *r);

DECLARE_REF_OPS(drm_resources)

/**
 * @brief Apply a workaround for the Rockchip DRM driver.
 *
 * The rockchip driver has special requirements as to which CRTCs can be used with which planes.
 * This function will restrict the possible_crtcs property for each plane to satisfy that requirement.
 * 
 * @attention This function can only be called on un-filtered resources, and should be called after each drm_resources_update.
 * 
 * @param r The resources to apply the workaround to.
 * 
 */
void drm_resources_apply_rockchip_workaround(struct drm_resources *r);


bool drm_resources_has_connector(struct drm_resources *r, uint32_t connector_id);

struct drm_connector *drm_resources_get_connector(struct drm_resources *r, uint32_t connector_id);

bool drm_resources_has_encoder(struct drm_resources *r, uint32_t encoder_id);

struct drm_encoder *drm_resources_get_encoder(struct drm_resources *r, uint32_t encoder_id);

bool drm_resources_has_crtc(struct drm_resources *r, uint32_t crtc_id);

struct drm_crtc *drm_resources_get_crtc(struct drm_resources *r, uint32_t crtc_id);

int64_t drm_resources_get_min_zpos_for_crtc(struct drm_resources *r, uint32_t crtc_id);

uint32_t drm_resources_get_possible_planes_for_crtc(struct drm_resources *r, uint32_t crtc_id);

bool drm_resources_has_plane(struct drm_resources *r, uint32_t plane_id);

struct drm_plane *drm_resources_get_plane(struct drm_resources *r, uint32_t plane_id);

unsigned int drm_resources_get_plane_index(struct drm_resources *r, uint32_t plane_id);


struct drm_connector *drm_resources_connector_first(struct drm_resources *r);

struct drm_connector *drm_resources_connector_end(struct drm_resources *r);

struct drm_connector *drm_resources_connector_next(struct drm_resources *r, struct drm_connector *current);


drmModeModeInfo *drm_connector_mode_first(struct drm_connector *c);

drmModeModeInfo *drm_connector_mode_end(struct drm_connector *c);

drmModeModeInfo *drm_connector_mode_next(struct drm_connector *c, drmModeModeInfo *current);


struct drm_encoder *drm_resources_encoder_first(struct drm_resources *r);

struct drm_encoder *drm_resources_encoder_end(struct drm_resources *r);

struct drm_encoder *drm_resources_encoder_next(struct drm_resources *r, struct drm_encoder *current);


struct drm_crtc *drm_resources_crtc_first(struct drm_resources *r);

struct drm_crtc *drm_resources_crtc_end(struct drm_resources *r);

struct drm_crtc *drm_resources_crtc_next(struct drm_resources *r, struct drm_crtc *current);


struct drm_plane *drm_resources_plane_first(struct drm_resources *r);

struct drm_plane *drm_resources_plane_end(struct drm_resources *r);

struct drm_plane *drm_resources_plane_next(struct drm_resources *r, struct drm_plane *current);


#define drm_resources_for_each_connector(res, connector) \
    for (struct drm_connector *(connector) = drm_resources_connector_first(res); (connector) != drm_resources_connector_end(res); (connector) = drm_resources_connector_next((res), (connector)))

#define drm_connector_for_each_mode(connector, mode) \
    for (drmModeModeInfo *(mode) = drm_connector_mode_first(connector); (mode) != drm_connector_mode_end(connector); (mode) = drm_connector_mode_next((connector), (mode)))

#define drm_resources_for_each_encoder(res, encoder) \
    for (struct drm_encoder *(encoder) = drm_resources_encoder_first(res); (encoder) != drm_resources_encoder_end(res); (encoder) = drm_resources_encoder_next((res), (encoder)))

#define drm_resources_for_each_crtc(res, crtc) \
    for (struct drm_crtc *(crtc) = drm_resources_crtc_first(res); (crtc) != drm_resources_crtc_end(res); (crtc) = drm_resources_crtc_next((res), (crtc)))

#define drm_resources_for_each_plane(res, plane) \
    for (struct drm_plane *(plane) = drm_resources_plane_first(res); (plane) != drm_resources_plane_end(res); (plane) = drm_resources_plane_next((res), (plane)))


struct drm_blob *drm_blob_new_mode(int drm_fd, const drmModeModeInfo *mode, bool dup_fd);

void drm_blob_destroy(struct drm_blob *blob);

int drm_blob_get_id(struct drm_blob *blob);

/**
 * @brief Get the precise refresh rate of a video mode.
 */
static inline double mode_get_vrefresh(const drmModeModeInfo *mode) {
    return mode->clock * 1000.0 / (mode->htotal * mode->vtotal);
}

#endif