#ifndef _FLUTTERPI_INCLUDE_PIXEL_FORMAT_H
#define _FLUTTERPI_INCLUDE_PIXEL_FORMAT_H

#include <collection.h>

#ifdef HAS_FBDEV
#include <linux/fb.h>

/**
 * @brief Description of a fbdev pixel format.
 * 
 */
struct fbdev_pixfmt {
    struct fb_bitfield r, g, b, a;
};

#endif

#ifdef HAS_GBM
#include <gbm.h>
#endif

#ifdef HAS_KMS
#include <drm_fourcc.h>
#endif

/**
 * @brief A specific pixel format. Use @ref get_pixfmt_info to get information
 * about this pixel format.
 * 
 */
enum pixfmt {
    kRGB565,
    kARGB4444,
    kXRGB4444,
    kARGB1555,
    kXRGB1555,
    kARGB8888,
    kXRGB8888,
    kBGRA8888,
    kBGRX8888,
    kRGBA8888,
    kRGBX8888,
    kMax_PixFmt = kRGBX8888,
    kCount_PixFmt = kMax_PixFmt + 1
};

// Just a pedantic check so we don't update the pixfmt enum without changing kMax_PixFmt
COMPILE_ASSERT(kMax_PixFmt == kRGBX8888);

#define PIXFMT_LIST(V) \
    V( "RGB 5:6:5",   "RGB565",   kRGB565,   /*bpp*/ 16, /*bit_depth*/ 16, /*opaque*/ true,  /*R*/ 5, 11, /*G*/ 6,  5, /*B*/ 5,  0, /*A*/ 0,  0, /*GBM fourcc*/ GBM_FORMAT_RGB565,   /*DRM fourcc*/ DRM_FORMAT_RGB565  ) \
    V("ARGB 4:4:4:4", "ARGB4444", kARGB4444, /*bpp*/ 16, /*bit_depth*/ 12, /*opaque*/ false, /*R*/ 4,  8, /*G*/ 4,  4, /*B*/ 4,  0, /*A*/ 4, 12, /*GBM fourcc*/ GBM_FORMAT_ARGB4444, /*DRM fourcc*/ DRM_FORMAT_ARGB4444) \
    V("XRGB 4:4:4:4", "XRGB4444", kXRGB4444, /*bpp*/ 16, /*bit_depth*/ 12, /*opaque*/ true,  /*R*/ 4,  8, /*G*/ 4,  4, /*B*/ 4,  0, /*A*/ 0,  0, /*GBM fourcc*/ GBM_FORMAT_XRGB4444, /*DRM fourcc*/ DRM_FORMAT_XRGB4444) \
    V("ARGB 1:5:5:5", "ARGB1555", kARGB1555, /*bpp*/ 16, /*bit_depth*/ 15, /*opaque*/ false, /*R*/ 5, 10, /*G*/ 5,  5, /*B*/ 5,  0, /*A*/ 1, 15, /*GBM fourcc*/ GBM_FORMAT_ARGB1555, /*DRM fourcc*/ DRM_FORMAT_ARGB1555) \
    V("XRGB 1:5:5:5", "XRGB1555", kXRGB1555, /*bpp*/ 16, /*bit_depth*/ 15, /*opaque*/ true,  /*R*/ 5, 10, /*G*/ 5,  5, /*B*/ 5,  0, /*A*/ 0,  0, /*GBM fourcc*/ GBM_FORMAT_XRGB1555, /*DRM fourcc*/ DRM_FORMAT_XRGB1555) \
    V("ARGB 8:8:8:8", "ARGB8888", kARGB8888, /*bpp*/ 32, /*bit_depth*/ 24, /*opaque*/ false, /*R*/ 8, 16, /*G*/ 8,  8, /*B*/ 8,  0, /*A*/ 8, 24, /*GBM fourcc*/ GBM_FORMAT_ARGB8888, /*DRM fourcc*/ DRM_FORMAT_ARGB8888) \
    V("XRGB 8:8:8:8", "XRGB8888", kXRGB8888, /*bpp*/ 32, /*bit_depth*/ 24, /*opaque*/ true,  /*R*/ 8, 16, /*G*/ 8,  8, /*B*/ 8,  0, /*A*/ 0, 24, /*GBM fourcc*/ GBM_FORMAT_XRGB8888, /*DRM fourcc*/ DRM_FORMAT_XRGB8888) \
    V("BGRA 8:8:8:8", "BGRA8888", kBGRA8888, /*bpp*/ 32, /*bit_depth*/ 24, /*opaque*/ false, /*R*/ 8,  8, /*G*/ 8, 16, /*B*/ 8, 24, /*A*/ 8,  0, /*GBM fourcc*/ GBM_FORMAT_BGRA8888, /*DRM fourcc*/ DRM_FORMAT_BGRA8888) \
    V("BGRX 8:8:8:8", "BGRX8888", kBGRX8888, /*bpp*/ 32, /*bit_depth*/ 24, /*opaque*/ true,  /*R*/ 8,  8, /*G*/ 8, 16, /*B*/ 8, 24, /*A*/ 0,  0, /*GBM fourcc*/ GBM_FORMAT_BGRX8888, /*DRM fourcc*/ DRM_FORMAT_BGRX8888) \
    V("RGBA 8:8:8:8", "RGBA8888", kRGBA8888, /*bpp*/ 32, /*bit_depth*/ 24, /*opaque*/ false, /*R*/ 8, 24, /*G*/ 8, 16, /*B*/ 8,  8, /*A*/ 8,  0, /*GBM fourcc*/ GBM_FORMAT_RGBA8888, /*DRM fourcc*/ DRM_FORMAT_RGBA8888) \
    V("RGBX 8:8:8:8", "RGBX8888", kRGBX8888, /*bpp*/ 32, /*bit_depth*/ 24, /*opaque*/ true,  /*R*/ 8, 24, /*G*/ 8, 16, /*B*/ 8,  8, /*A*/ 0,  0, /*GBM fourcc*/ GBM_FORMAT_RGBX8888, /*DRM fourcc*/ DRM_FORMAT_RGBX8888)

// make sure the macro list we defined has as many elements as the pixfmt enum.
#define __COUNT(...) +1
COMPILE_ASSERT(0 PIXFMT_LIST(__COUNT) == kMax_PixFmt+1);
#undef __COUNT

static inline enum pixfmt pixfmt_opaque(enum pixfmt format) {
    if (format == kARGB8888) {
        return kXRGB8888;
    } else if (format == kARGB4444) {
        return kXRGB4444;
    } else if (format == kARGB1555) {
        return kXRGB1555;
    } else if (format == kBGRA8888) {
        return kBGRX8888;
    } else if (format == kRGBA8888) {
        return kRGBX8888;
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
     * @brief The DRM format equivalent to this pixel format.
     */
    uint32_t drm_format;
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
static inline const struct pixfmt_info *get_pixfmt_info(enum pixfmt format) {
    DEBUG_ASSERT(format >= 0 && format <= kMax_PixFmt);
#ifdef DEBUG
    assert_pixfmt_list_valid();
#endif
    return pixfmt_infos + format;
}

#endif // _FLUTTERPI_INCLUDE_PIXEL_FORMAT_H
