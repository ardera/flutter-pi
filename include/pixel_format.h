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
    kARGB8888,
    kXRGB8888,
    kBGRA8888,
    kRGBA8888,
    kMax_PixFmt = kRGBA8888,
    kCount_PixFmt = kMax_PixFmt + 1
};

// Just a pedantic check so we don't update the pixfmt enum without changing kMax_PixFmt
COMPILE_ASSERT(kMax_PixFmt == kRGBA8888);

#define PIXFMT_LIST(V) \
    V( "RGB 5:6:5",    "RGB565",  kRGB565,   /*bpp*/ 16, /*opaque*/ true,  /*R*/ 5, 11, /*G*/ 6, 5,  /*B*/ 5, 0,  /*A*/ 0, 0,  /*GBM fourcc*/ GBM_FORMAT_RGB565,   /*DRM fourcc*/ DRM_FORMAT_RGB565) \
    V("ARGB 8:8:8:8", "ARGB8888", kARGB8888, /*bpp*/ 32, /*opaque*/ false, /*R*/ 8, 16, /*G*/ 8, 8,  /*B*/ 8, 0,  /*A*/ 8, 24, /*GBM fourcc*/ GBM_FORMAT_ARGB8888, /*DRM fourcc*/ DRM_FORMAT_RGB565) \
    V("XRGB 8:8:8:8", "XRGB8888", kXRGB8888, /*bpp*/ 32, /*opaque*/ true,  /*R*/ 8, 16, /*G*/ 8, 8,  /*B*/ 8, 0,  /*A*/ 0, 24, /*GBM fourcc*/ GBM_FORMAT_XRGB8888, /*DRM fourcc*/ DRM_FORMAT_XRGB8888) \
    V("BGRA 8:8:8:8", "BGRA8888", kBGRA8888, /*bpp*/ 32, /*opaque*/ false, /*R*/ 8,  8, /*G*/ 8, 16, /*B*/ 8, 24, /*A*/ 8, 0,  /*GBM fourcc*/ GBM_FORMAT_BGRA8888, /*DRM fourcc*/ DRM_FORMAT_BGRA8888) \
    V("RGBA 8:8:8:8", "RGBA8888", kRGBA8888, /*bpp*/ 32, /*opaque*/ false, /*R*/ 8, 24, /*G*/ 8, 16, /*B*/ 8, 8,  /*A*/ 8, 0,  /*GBM fourcc*/ GBM_FORMAT_RGBA8888, /*DRM fourcc*/ DRM_FORMAT_RGBA8888)

// make sure the macro list we defined has as many elements as the pixfmt enum.
#define __COUNT(...) +1
COMPILE_ASSERT(0 PIXFMT_LIST(__COUNT) == kMax_PixFmt+1);
#undef __COUNT

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

/**
 * @brief A list of known pixel-formats, with some details about them.
 * 
 */
extern const struct pixfmt_info pixfmt_infos[];
extern const size_t n_pixfmt_infos;

/**
 * @brief Get the pixel format info for a specific pixel format.
 * 
 */
static inline const struct pixfmt_info *get_pixfmt_info(enum pixfmt format) {
    DEBUG_ASSERT(format > 0 && format <= kMax_PixFmt);
    return pixfmt_infos + format;
}

#endif // _FLUTTERPI_INCLUDE_PIXEL_FORMAT_H
