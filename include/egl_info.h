// SPDX-License-Identifier: MIT
/*
 * Utility for working with EGL and EGL Extensions
 *
 * Copyright (c) 2022, Hannes Winkler <hanneswinkler2000@web.de>
 */


#ifndef _FLUTTERPI_INCLUDE_EGL_INFO_H
#define _FLUTTERPI_INCLUDE_EGL_INFO_H

#include <egl.h>

#define EGL_EXTENSION_LIST(EXT, FUN) \
    EXT(EGL_KHR_no_config_context) \
    EXT(EGL_MESA_drm_image) \
    EXT(EGL_KHR_image) \
    EXT(EGL_KHR_image_base) \
    FUN(EGL_KHR_image_base, PFNEGLCREATEIMAGEKHRPROC, eglCreateImageKHR) \
    FUN(EGL_KHR_image_base, PFNEGLDESTROYIMAGEKHRPROC, eglDestroyImageKHR) \
    EXT(EGL_EXT_image_dma_buf_import_modifiers), \
    FUN(EGL_EXT_image_dma_buf_import_modifiers, PFNEGLQUERYDMABUFFORMATSEXTPROC, eglQueryDmaBufFormatsEXT) \
    FUN(EGL_EXT_image_dma_buf_import_modifiers, PFNEGLQUERYDMABUFMODIFIERSEXTPROC, eglQueryDmaBufModifiersEXT) \
    EXT(EGL_KHR_gl_renderbuffer_image) \
    EXT(EGL_EXT_image_dma_buf_import) \
    EXT(EGL_MESA_image_dma_buf_export) \
    FUN(EGL_MESA_image_dma_buf_export, PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC, eglExportDMABUFImageQueryMESA), \
    FUN(EGL_MESA_image_dma_buf_export, PFNEGLEXPORTDMABUFIMAGEMESAPROC, eglExportDMABUFImageMESA)

struct egl_display_info {
#define EXT(ext_name_str) bool supports_##ext_name_str;
#define FUN(ext_name_str, function_type, function_name) function_type function_name;
EGL_EXTENSION_LIST(EXT, FuN)
#undef FUN
#undef EXT
};

static inline void fill_display_info(struct egl_display_info *info, const char *egl_client_exts, const char *egl_display_exts, PFNEGLGETPROCADDRESSPROC get_proc_address) {
    const char *namestr;
    bool *supported;

#define EXT(ext_name_str) \
    info->supports_##ext_name_str = (strstr(egl_client_exts, #ext_name_str) != NULL) || (strstr(egl_display_exts, #ext_name_str) != NULL);
#define FUN(ext_name_str, function_type, function_name) \
    info->function_name = get_proc_address(#function_name); \
    if (info->supports_##ext_name_str && info->function_name == NULL) { \
        LOG_ERROR_UNPREFIXED("EGL Extension " #ext_name_str " is listed as supported but EGL procedure " #function_name " could not be resolved.\n"); \
    }

    EGL_EXTENSION_LIST(EXT, FUN)

#undef FUN
#undef EXT
}

#endif // _FLUTTERPI_INCLUDE_EGL_INFO_H
