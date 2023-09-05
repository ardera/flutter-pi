// SPDX-License-Identifier: MIT
/*
 * Pixel Formats
 *
 * A list of pixel formats that flutter-pi supports, with details
 * about their composition.
 *
 * Provides a translations between DRM, EGL/GL, fbdev, vulkan and
 * flutter software pixel foramts.
 *
 * Copyright (c) 2023, Hannes Winkler <hanneswinkler2000@web.de>
 */

#ifndef _FLUTTERPI_SRC_PIXEL_FORMAT_H
#define _FLUTTERPI_SRC_PIXEL_FORMAT_H

#include <stdbool.h>

#include "util/asserts.h"
#include "util/collection.h"

#include "config.h"

#ifdef HAVE_FBDEV
    #include <linux/fb.h>

/**
 * @brief Description of a fbdev pixel format.
 *
 */
struct fbdev_pixfmt {
    struct fb_bitfield r, g, b, a;
};

#endif

#ifdef HAVE_GBM
    #include <gbm.h>
#endif

#ifdef HAVE_KMS
    #include <drm_fourcc.h>
#endif

#ifdef HAVE_VULKAN
    #include <vulkan.h>
#endif

/**
 * @brief A specific pixel format. Use @ref get_pixfmt_info to get information
 * about this pixel format.
 *
 */
enum pixfmt {
    PIXFMT_RGB565,
    PIXFMT_ARGB4444,
    PIXFMT_XRGB4444,
    PIXFMT_ARGB1555,
    PIXFMT_XRGB1555,
    PIXFMT_ARGB8888,
    PIXFMT_XRGB8888,
    PIXFMT_BGRA8888,
    PIXFMT_BGRX8888,
    PIXFMT_RGBA8888,
    PIXFMT_RGBX8888,
    PIXFMT_MAX = PIXFMT_RGBX8888,
    PIXFMT_COUNT = PIXFMT_MAX + 1
};

// Just a pedantic check so we don't update the pixfmt enum without changing PIXFMT_MAX
COMPILE_ASSERT(PIXFMT_MAX == PIXFMT_RGBX8888);

// Vulkan doesn't support that many sRGB formats actually.
// There's two more (one packed and one non-packed) that aren't listed here.
/// TODO: We could support other formats as well though with manual colorspace conversions.
#define PIXFMT_LIST(V)                           \
    V("RGB 5:6:5",                               \
      "RGB565",                                  \
      PIXFMT_RGB565,                             \
      /*bpp*/ 16,                                \
      /*bit_depth*/ 16,                          \
      /*opaque*/ true,                           \
      /*Vulkan format*/ VK_FORMAT_UNDEFINED,     \
      /*R*/ 5,                                   \
      11,                                        \
      /*G*/ 6,                                   \
      5,                                         \
      /*B*/ 5,                                   \
      0,                                         \
      /*A*/ 0,                                   \
      0,                                         \
      /*GBM fourcc*/ GBM_FORMAT_RGB565,          \
      /*DRM fourcc*/ DRM_FORMAT_RGB565)          \
    V("ARGB 4:4:4:4",                            \
      "ARGB4444",                                \
      PIXFMT_ARGB4444,                           \
      /*bpp*/ 16,                                \
      /*bit_depth*/ 12,                          \
      /*opaque*/ false,                          \
      /*Vulkan format*/ VK_FORMAT_UNDEFINED,     \
      /*R*/ 4,                                   \
      8,                                         \
      /*G*/ 4,                                   \
      4,                                         \
      /*B*/ 4,                                   \
      0,                                         \
      /*A*/ 4,                                   \
      12,                                        \
      /*GBM fourcc*/ GBM_FORMAT_ARGB4444,        \
      /*DRM fourcc*/ DRM_FORMAT_ARGB4444)        \
    V("XRGB 4:4:4:4",                            \
      "XRGB4444",                                \
      PIXFMT_XRGB4444,                           \
      /*bpp*/ 16,                                \
      /*bit_depth*/ 12,                          \
      /*opaque*/ true,                           \
      /*Vulkan format*/ VK_FORMAT_UNDEFINED,     \
      /*R*/ 4,                                   \
      8,                                         \
      /*G*/ 4,                                   \
      4,                                         \
      /*B*/ 4,                                   \
      0,                                         \
      /*A*/ 0,                                   \
      0,                                         \
      /*GBM fourcc*/ GBM_FORMAT_XRGB4444,        \
      /*DRM fourcc*/ DRM_FORMAT_XRGB4444)        \
    V("ARGB 1:5:5:5",                            \
      "ARGB1555",                                \
      PIXFMT_ARGB1555,                           \
      /*bpp*/ 16,                                \
      /*bit_depth*/ 15,                          \
      /*opaque*/ false,                          \
      /*Vulkan format*/ VK_FORMAT_UNDEFINED,     \
      /*R*/ 5,                                   \
      10,                                        \
      /*G*/ 5,                                   \
      5,                                         \
      /*B*/ 5,                                   \
      0,                                         \
      /*A*/ 1,                                   \
      15,                                        \
      /*GBM fourcc*/ GBM_FORMAT_ARGB1555,        \
      /*DRM fourcc*/ DRM_FORMAT_ARGB1555)        \
    V("XRGB 1:5:5:5",                            \
      "XRGB1555",                                \
      PIXFMT_XRGB1555,                           \
      /*bpp*/ 16,                                \
      /*bit_depth*/ 15,                          \
      /*opaque*/ true,                           \
      /*Vulkan format*/ VK_FORMAT_UNDEFINED,     \
      /*R*/ 5,                                   \
      10,                                        \
      /*G*/ 5,                                   \
      5,                                         \
      /*B*/ 5,                                   \
      0,                                         \
      /*A*/ 0,                                   \
      0,                                         \
      /*GBM fourcc*/ GBM_FORMAT_XRGB1555,        \
      /*DRM fourcc*/ DRM_FORMAT_XRGB1555)        \
    V("ARGB 8:8:8:8",                            \
      "ARGB8888",                                \
      PIXFMT_ARGB8888,                           \
      /*bpp*/ 32,                                \
      /*bit_depth*/ 24,                          \
      /*opaque*/ false,                          \
      /*Vulkan format*/ VK_FORMAT_B8G8R8A8_SRGB, \
      /*R*/ 8,                                   \
      16,                                        \
      /*G*/ 8,                                   \
      8,                                         \
      /*B*/ 8,                                   \
      0,                                         \
      /*A*/ 8,                                   \
      24,                                        \
      /*GBM fourcc*/ GBM_FORMAT_ARGB8888,        \
      /*DRM fourcc*/ DRM_FORMAT_ARGB8888)        \
    V("XRGB 8:8:8:8",                            \
      "XRGB8888",                                \
      PIXFMT_XRGB8888,                           \
      /*bpp*/ 32,                                \
      /*bit_depth*/ 24,                          \
      /*opaque*/ true,                           \
      /*Vulkan format*/ VK_FORMAT_UNDEFINED,     \
      /*R*/ 8,                                   \
      16,                                        \
      /*G*/ 8,                                   \
      8,                                         \
      /*B*/ 8,                                   \
      0,                                         \
      /*A*/ 0,                                   \
      24,                                        \
      /*GBM fourcc*/ GBM_FORMAT_XRGB8888,        \
      /*DRM fourcc*/ DRM_FORMAT_XRGB8888)        \
    V("BGRA 8:8:8:8",                            \
      "BGRA8888",                                \
      PIXFMT_BGRA8888,                           \
      /*bpp*/ 32,                                \
      /*bit_depth*/ 24,                          \
      /*opaque*/ false,                          \
      /*Vulkan format*/ VK_FORMAT_UNDEFINED,     \
      /*R*/ 8,                                   \
      8,                                         \
      /*G*/ 8,                                   \
      16,                                        \
      /*B*/ 8,                                   \
      24,                                        \
      /*A*/ 8,                                   \
      0,                                         \
      /*GBM fourcc*/ GBM_FORMAT_BGRA8888,        \
      /*DRM fourcc*/ DRM_FORMAT_BGRA8888)        \
    V("BGRX 8:8:8:8",                            \
      "BGRX8888",                                \
      PIXFMT_BGRX8888,                           \
      /*bpp*/ 32,                                \
      /*bit_depth*/ 24,                          \
      /*opaque*/ true,                           \
      /*Vulkan format*/ VK_FORMAT_UNDEFINED,     \
      /*R*/ 8,                                   \
      8,                                         \
      /*G*/ 8,                                   \
      16,                                        \
      /*B*/ 8,                                   \
      24,                                        \
      /*A*/ 0,                                   \
      0,                                         \
      /*GBM fourcc*/ GBM_FORMAT_BGRX8888,        \
      /*DRM fourcc*/ DRM_FORMAT_BGRX8888)        \
    V("RGBA 8:8:8:8",                            \
      "RGBA8888",                                \
      PIXFMT_RGBA8888,                           \
      /*bpp*/ 32,                                \
      /*bit_depth*/ 24,                          \
      /*opaque*/ false,                          \
      /*Vulkan format*/ VK_FORMAT_UNDEFINED,     \
      /*R*/ 8,                                   \
      24,                                        \
      /*G*/ 8,                                   \
      16,                                        \
      /*B*/ 8,                                   \
      8,                                         \
      /*A*/ 8,                                   \
      0,                                         \
      /*GBM fourcc*/ GBM_FORMAT_RGBA8888,        \
      /*DRM fourcc*/ DRM_FORMAT_RGBA8888)        \
    V("RGBX 8:8:8:8",                            \
      "RGBX8888",                                \
      PIXFMT_RGBX8888,                           \
      /*bpp*/ 32,                                \
      /*bit_depth*/ 24,                          \
      /*opaque*/ true,                           \
      /*Vulkan format*/ VK_FORMAT_UNDEFINED,     \
      /*R*/ 8,                                   \
      24,                                        \
      /*G*/ 8,                                   \
      16,                                        \
      /*B*/ 8,                                   \
      8,                                         \
      /*A*/ 0,                                   \
      0,                                         \
      /*GBM fourcc*/ GBM_FORMAT_RGBX8888,        \
      /*DRM fourcc*/ DRM_FORMAT_RGBX8888)

// make sure the macro list we defined has as many elements as the pixfmt enum.
#define __COUNT(...) +1
COMPILE_ASSERT(0 PIXFMT_LIST(__COUNT) == PIXFMT_MAX + 1);
#undef __COUNT

static inline enum pixfmt pixfmt_opaque(enum pixfmt format) {
    if (format == PIXFMT_ARGB8888) {
        return PIXFMT_XRGB8888;
    } else if (format == PIXFMT_ARGB4444) {
        return PIXFMT_XRGB4444;
    } else if (format == PIXFMT_ARGB1555) {
        return PIXFMT_XRGB1555;
    } else if (format == PIXFMT_BGRA8888) {
        return PIXFMT_BGRX8888;
    } else if (format == PIXFMT_RGBA8888) {
        return PIXFMT_RGBX8888;
    }

    /// TODO: We're potentially returning a non-opaque format here.
    return format;
}

/**
 * @brief Information about a pixel format.
 *
 */
struct pixfmt_info {
    /**
     * @brief A descriptive, human-readable name for this pixel format.
     *
     * Example: RGB 5:6:5
     */
    const char *name;

    /**
     * @brief A short, unique name for this pixel format, to use it as a commandline argument for example.
     *
     * Example: RGB565
     *
     */
    const char *arg_name;

    /**
     * @brief The pixel format that this struct provides information about.
     */
    enum pixfmt format;

    /**
     * @brief How many bits per pixel does this pixel format use?
     */
    int bits_per_pixel;

    /**
     * @brief How many bits of the @ref bits_per_pixel are used for color (R / G / B)?
     *
     */
    int bit_depth;

    /**
     * @brief True if there's no way to specify transparency with this format.
     */
    bool is_opaque;

#ifdef HAVE_FBDEV
    /**
     * @brief The fbdev format equivalent to this pixel format.
     */
    struct fbdev_pixfmt fbdev_format;
#endif
#ifdef HAVE_GBM
    /**
     * @brief The GBM format equivalent to this pixel format.
     */
    uint32_t gbm_format;
#endif
#ifdef HAVE_KMS
    /**
     * @brief The DRM format equivalent to this pixel format.
     */
    uint32_t drm_format;
#endif
#ifdef HAVE_VULKAN
    /**
     * @brief The vulkan equivalent of this pixel format.
     */
    VkFormat vk_format;
#endif
};

/**
 * @brief A list of known pixel-formats, with some details about them.
 *
 */
extern const struct pixfmt_info pixfmt_infos[];
extern const size_t n_pixfmt_infos;

#ifdef DEBUG
void assert_pixfmt_list_valid();
#endif

/**
 * @brief Get the pixel format info for a specific pixel format.
 *
 */
ATTR_CONST static inline const struct pixfmt_info *get_pixfmt_info(enum pixfmt format) {
    assert(format >= 0 && format <= PIXFMT_MAX);
#ifdef DEBUG
    assert_pixfmt_list_valid();
#endif
    return pixfmt_infos + format;
}

ATTR_CONST static inline bool has_pixfmt_for_drm_format(uint32_t fourcc) {
    for (int i = 0; i < n_pixfmt_infos; i++) {
        if (get_pixfmt_info(i)->drm_format == fourcc) {
            return true;
        }
    }

    return false;
}

ATTR_CONST static inline enum pixfmt get_pixfmt_for_drm_format(uint32_t fourcc) {
    for (int i = 0; i < n_pixfmt_infos; i++) {
        if (get_pixfmt_info(i)->drm_format == fourcc) {
            return i;
        }
    }

    ASSERT_MSG(false, "Check has_pixfmt_for_drm_format if an enum pixfmt exists for a specific DRM fourcc.");
    return PIXFMT_RGB565;
}

ATTR_CONST static inline bool has_pixfmt_for_gbm_format(uint32_t fourcc) {
    for (int i = 0; i < n_pixfmt_infos; i++) {
        if (get_pixfmt_info(i)->gbm_format == fourcc) {
            return true;
        }
    }

    return false;
}

ATTR_CONST static inline enum pixfmt get_pixfmt_for_gbm_format(uint32_t fourcc) {
    for (int i = 0; i < n_pixfmt_infos; i++) {
        if (get_pixfmt_info(i)->gbm_format == fourcc) {
            return i;
        }
    }

    ASSERT_MSG(false, "Check has_pixfmt_for_gbm_format if an enum pixfmt exists for a specific GBM fourcc.");
    return PIXFMT_RGB565;
}

COMPILE_ASSERT(PIXFMT_RGB565 == 0);

#define ASSERT_PIXFMT_VALID(format) ASSERT_MSG(format >= PIXFMT_RGB565 && format <= PIXFMT_MAX, "Invalid pixel format")
#define ASSUME_PIXFMT_VALID(format) ASSUME((format) >= PIXFMT_RGB565 && (format) <= PIXFMT_MAX)

#endif  // _FLUTTERPI_SRC_PIXEL_FORMAT_H
