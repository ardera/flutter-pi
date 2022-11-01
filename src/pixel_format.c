#include <pixel_format.h>
#include <vulkan.h>

#ifdef HAS_FBDEV
#   define FBDEV_FORMAT_FIELD_INITIALIZER(r_length, r_offset, g_length, g_offset, b_length, b_offset, a_length, a_offset) \
        .fbdev_format = { \
            .r = {.length = r_length, .offset = r_offset, .msb_right = 0}, \
            .g = {.length = g_length, .offset = g_offset, .msb_right = 0}, \
            .b = {.length = b_length, .offset = b_offset, .msb_right = 0}, \
            .a = {.length = a_length, .offset = a_offset, .msb_right = 0}, \
        },
#else
#   define FBDEV_FORMAT_FIELD_INITIALIZER(r_length, r_offset, g_length, g_offset, b_length, b_offset, a_length, a_offset)
#endif

#ifdef HAS_GBM
#   include <gbm.h>
#   define GBM_FORMAT_FIELD_INITIALIZER(_gbm_format) .gbm_format = _gbm_format,
#else
#   define GBM_FORMAT_FIELD_INITIALIZER(_gbm_format)
#endif

#ifdef HAS_KMS
#   include <drm_fourcc.h>
#   define DRM_FORMAT_FIELD_INITIALIZER(_drm_format) .drm_format = _drm_format,
#else
#   define DRM_FORMAT_FIELD_INITIALIZER(_drm_format)
#endif

#ifdef HAS_VULKAN
#   include <vulkan.h>
#   define VK_FORMAT_FIELD_INITIALIZER(_vk_format) .vk_format = _vk_format,
#else
#   define VK_FORMAT_FIELD_INITIALIZER(_vk_format)
#endif

#define PIXFMT_MAPPING(_name, _arg_name, _format, _bpp, _bit_depth, _is_opaque, _vk_format, r_length, r_offset, g_length, g_offset, b_length, b_offset, a_length, a_offset, _gbm_format, _drm_format) \
    { \
        .name = _name, \
        .arg_name = _arg_name, \
        .format = _format, \
        .bits_per_pixel = _bpp, \
        .bit_depth = _bit_depth, \
        .is_opaque = _is_opaque, \
        VK_FORMAT_FIELD_INITIALIZER(_vk_format) \
        FBDEV_FORMAT_FIELD_INITIALIZER(r_length, r_offset, g_length, g_offset, b_length, b_offset, a_length, a_offset) \
        GBM_FORMAT_FIELD_INITIALIZER(_gbm_format) \
        DRM_FORMAT_FIELD_INITIALIZER(_drm_format) \
    },

const struct pixfmt_info pixfmt_infos[] = {
    PIXFMT_LIST(PIXFMT_MAPPING)
};

/// hack so we can use COMPILE_ASSERT.
enum {
    n_pixfmt_infos_constexpr = sizeof(pixfmt_infos) / sizeof(*pixfmt_infos)
};

const size_t n_pixfmt_infos = n_pixfmt_infos_constexpr;

COMPILE_ASSERT(n_pixfmt_infos_constexpr == kMax_PixFmt+1);

#ifdef DEBUG
void assert_pixfmt_list_valid() {
    for (enum pixfmt format = 0; format < kCount_PixFmt; format++) {
        assert(pixfmt_infos[format].format == format);
    }
}
#endif
