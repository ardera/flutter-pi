// SPDX-License-Identifier: MIT
/*
 * Just a shim for including EGL headers, and disabling EGL function prototypes if EGL is not present
 *
 * Copyright (c) 2022, Hannes Winkler <hanneswinkler2000@web.de>
 */

#ifndef _FLUTTERPI_SRC_EGL_H
#define _FLUTTERPI_SRC_EGL_H

#include <stdbool.h>
#include <string.h>

#include "config.h"

#ifndef HAVE_EGL
    #error "egl.h was included but EGL support is disabled."
#endif

#ifdef LINT_EGL_HEADERS

    // This makes sure we only use EGL 1.4 definitions and prototypes.
    #define EGL_VERSION_1_5 1
    #include <EGL/egl.h>
    #undef EGL_VERSION_1_5

    // Every extension with a #define 1 here is disabled.
    // Once eglext.h is included, only the extensions that didn't have
    // such a define have their prototypes, types defined.

    #define EGL_KHR_cl_event 1
    // #define EGL_KHR_cl_event2 1
    #define EGL_KHR_client_get_all_proc_addresses 1
    #define EGL_KHR_config_attribs 1
    #define EGL_KHR_context_flush_control 1
    #define EGL_KHR_create_context 1
    #define EGL_KHR_create_context_no_error 1
    #define EGL_KHR_debug 1
    #define EGL_KHR_display_reference 1
    #define EGL_KHR_fence_sync 1
    #define EGL_KHR_get_all_proc_addresses 1
    #define EGL_KHR_gl_colorspace 1
    #define EGL_KHR_gl_renderbuffer_image 1
    #define EGL_KHR_gl_texture_2D_image 1
    #define EGL_KHR_gl_texture_3D_image 1
    #define EGL_KHR_gl_texture_cubemap_image 1
    // #define EGL_KHR_image 1
    // #define EGL_KHR_image_base 1
    #define EGL_KHR_image_pixmap 1
    #define EGL_KHR_lock_surface 1
    #define EGL_KHR_lock_surface2 1
    #define EGL_KHR_lock_surface3 1
    #define EGL_KHR_mutable_render_buffer 1
    // #define EGL_KHR_no_config_context 1
    #define EGL_KHR_partial_update 1
    #define EGL_KHR_platform_android 1
    // #define EGL_KHR_platform_gbm 1
    #define EGL_KHR_platform_wayland 1
    #define EGL_KHR_platform_x11 1
    #define EGL_KHR_reusable_sync 1
    // #define EGL_KHR_stream 1
    #define EGL_KHR_stream_attrib 1
    #define EGL_KHR_stream_consumer_gltexture 1
    #define EGL_KHR_stream_cross_process_fd 1
    #define EGL_KHR_stream_fifo 1
    #define EGL_KHR_stream_producer_aldatalocator 1
    #define EGL_KHR_stream_producer_eglsurface 1
    #define EGL_KHR_surfaceless_context 1
    #define EGL_KHR_swap_buffers_with_damage 1
    #define EGL_KHR_vg_parent_image 1
    #define EGL_KHR_wait_sync 1
    #define EGL_ANDROID_GLES_layers 1
    #define EGL_ANDROID_blob_cache 1
    #define EGL_ANDROID_create_native_client_buffer 1
    #define EGL_ANDROID_framebuffer_target 1
    #define EGL_ANDROID_front_buffer_auto_refresh 1
    #define EGL_ANDROID_get_frame_timestamps 1
    #define EGL_ANDROID_get_native_client_buffer 1
    #define EGL_ANDROID_image_native_buffer 1
    #define EGL_ANDROID_native_fence_sync 1
    #define EGL_ANDROID_presentation_time 1
    #define EGL_ANDROID_recordable 1
    #define EGL_ANGLE_d3d_share_handle_client_buffer 1
    #define EGL_ANGLE_device_d3d 1
    #define EGL_ANGLE_query_surface_pointer 1
    #define EGL_ANGLE_surface_d3d_texture_2d_share_handle 1
    #define EGL_ANGLE_sync_control_rate 1
    #define EGL_ANGLE_window_fixed_size 1
    #define EGL_ARM_image_format 1
    #define EGL_ARM_implicit_external_sync 1
    #define EGL_ARM_pixmap_multisample_discard 1
    #define EGL_EXT_bind_to_front 1
    #define EGL_EXT_buffer_age 1
    #define EGL_EXT_client_extensions 1
    #define EGL_EXT_client_sync 1
    #define EGL_EXT_compositor 1
    #define EGL_EXT_config_select_group 1
    #define EGL_EXT_create_context_robustness 1
    #define EGL_EXT_device_base 1
    #define EGL_EXT_device_drm 1
    #define EGL_EXT_device_drm_render_node 1
    #define EGL_EXT_device_enumeration 1
    #define EGL_EXT_device_openwf 1
    #define EGL_EXT_device_persistent_id 1
    #define EGL_EXT_device_query 1
    #define EGL_EXT_device_query_name 1
    #define EGL_EXT_gl_colorspace_bt2020_linear 1
    #define EGL_EXT_gl_colorspace_bt2020_pq 1
    #define EGL_EXT_gl_colorspace_display_p3 1
    #define EGL_EXT_gl_colorspace_display_p3_linear 1
    #define EGL_EXT_gl_colorspace_display_p3_passthrough 1
    #define EGL_EXT_gl_colorspace_scrgb 1
    #define EGL_EXT_gl_colorspace_scrgb_linear 1
    // #define EGL_EXT_image_dma_buf_import 1
    #define EGL_EXT_image_dma_buf_import_modifiers 1
    #define EGL_EXT_image_gl_colorspace 1
    #define EGL_EXT_image_implicit_sync_control 1
    #define EGL_EXT_multiview_window 1
    #define EGL_EXT_output_base 1
    #define EGL_EXT_output_drm 1
    #define EGL_EXT_output_openwf 1
    #define EGL_EXT_pixel_format_float 1
    #define EGL_EXT_platform_base 1
    #define EGL_EXT_platform_device 1
    #define EGL_EXT_platform_wayland 1
    #define EGL_EXT_platform_x11 1
    #define EGL_EXT_platform_xcb 1
    #define EGL_EXT_present_opaque 1
    #define EGL_EXT_protected_content 1
    #define EGL_EXT_protected_surface 1
    #define EGL_EXT_stream_consumer_egloutput 1
    #define EGL_EXT_surface_CTA861_3_metadata 1
    #define EGL_EXT_surface_SMPTE2086_metadata 1
    #define EGL_EXT_surface_compression 1
    #define EGL_EXT_swap_buffers_with_damage 1
    #define EGL_EXT_sync_reuse 1
    #define EGL_EXT_yuv_surface 1
    #define EGL_HI_clientpixmap 1
    #define EGL_HI_colorformats 1
    #define EGL_IMG_context_priority 1
    #define EGL_IMG_image_plane_attribs 1
    #define EGL_MESA_drm_image 1
    #define EGL_MESA_image_dma_buf_export 1
    #define EGL_MESA_platform_gbm 1
    #define EGL_MESA_platform_surfaceless 1
    #define EGL_NV_quadruple_buffer 1
    #define EGL_NV_robustness_video_memory_purge 1
    #define EGL_NV_stream_consumer_eglimage 1
    #define EGL_NV_stream_consumer_gltexture_yuv 1
    #define EGL_NV_stream_cross_display 1
    #define EGL_NV_stream_cross_object 1
    #define EGL_NV_stream_cross_partition 1
    #define EGL_NV_stream_cross_process 1
    #define EGL_NV_stream_cross_system 1
    #define EGL_NV_stream_dma 1
    #define EGL_NV_stream_fifo_next 1
    #define EGL_NV_stream_fifo_synchronous 1
    #define EGL_NV_stream_flush 1
    #define EGL_NV_stream_frame_limits 1
    #define EGL_NV_stream_metadata 1
    #define EGL_NV_stream_origin 1
    #define EGL_NV_stream_remote 1
    #define EGL_NV_stream_reset 1
    #define EGL_NV_stream_socket 1
    #define EGL_NV_stream_socket_inet 1
    #define EGL_NV_stream_socket_unix 1
    #define EGL_NV_stream_sync 1
    #define EGL_NV_sync 1
    #define EGL_NV_system_time 1
    #define EGL_NV_triple_buffer 1
    #define EGL_TIZEN_image_native_buffer 1
    #define EGL_TIZEN_image_native_surface 1
    #define EGL_WL_bind_wayland_display 1
    #define EGL_WL_create_wayland_buffer_from_image 1
    #define EGL_NV_native_query 1
    #define EGL_NV_post_convert_rounding 1
    #define EGL_NV_post_sub_buffer 1
    #define EGL_NV_quadruple_buffer 1
    #define EGL_NV_robustness_video_memory_purge 1
    #define EGL_NV_stream_consumer_eglimage 1
    #define EGL_NV_stream_consumer_gltexture_yuv 1
    #define EGL_NV_stream_cross_display 1
    #define EGL_NV_stream_cross_object 1
    #define EGL_NV_stream_cross_partition 1
    #define EGL_NV_stream_cross_process 1
    #define EGL_NV_stream_cross_system 1
    #define EGL_NV_stream_dma 1
    #define EGL_NV_stream_fifo_next 1
    #define EGL_NV_stream_fifo_synchronous 1
    #define EGL_NV_stream_flush 1
    #define EGL_NV_stream_frame_limits 1
    #define EGL_NV_stream_metadata 1
    #define EGL_NV_stream_origin 1
    #define EGL_NV_stream_remote 1
    #define EGL_NV_stream_reset 1
    #define EGL_NV_stream_socket 1
    #define EGL_NV_stream_socket_inet 1
    #define EGL_NV_stream_socket_unix 1
    #define EGL_NV_stream_sync 1
    #define EGL_NV_sync 1
    #define EGL_NV_system_time 1
    #define EGL_NV_triple_buffer 1
    #define EGL_TIZEN_image_native_buffer 1
    #define EGL_TIZEN_image_native_surface 1
    #define EGL_WL_bind_wayland_display 1
    #define EGL_WL_create_wayland_buffer_from_image 1

    // Actually include eglext.h
    #include <EGL/eglext.h>

    #undef EGL_KHR_cl_event
    // #undef EGL_KHR_cl_event2
    #undef EGL_KHR_client_get_all_proc_addresses
    #undef EGL_KHR_config_attribs
    #undef EGL_KHR_context_flush_control
    #undef EGL_KHR_create_context
    #undef EGL_KHR_create_context_no_error
    #undef EGL_KHR_debug
    #undef EGL_KHR_display_reference
    #undef EGL_KHR_fence_sync
    #undef EGL_KHR_get_all_proc_addresses
    #undef EGL_KHR_gl_colorspace
    #undef EGL_KHR_gl_renderbuffer_image
    #undef EGL_KHR_gl_texture_2D_image
    #undef EGL_KHR_gl_texture_3D_image
    #undef EGL_KHR_gl_texture_cubemap_image
    // #undef EGL_KHR_image
    // #undef EGL_KHR_image_base
    #undef EGL_KHR_image_pixmap
    #undef EGL_KHR_lock_surface
    #undef EGL_KHR_lock_surface2
    #undef EGL_KHR_lock_surface3
    #undef EGL_KHR_mutable_render_buffer
    // #undef EGL_KHR_no_config_context
    #undef EGL_KHR_partial_update
    #undef EGL_KHR_platform_android
    // #undef EGL_KHR_platform_gbm
    #undef EGL_KHR_platform_wayland
    #undef EGL_KHR_platform_x11
    #undef EGL_KHR_reusable_sync
    // #undef EGL_KHR_stream
    #undef EGL_KHR_stream_attrib
    #undef EGL_KHR_stream_consumer_gltexture
    #undef EGL_KHR_stream_cross_process_fd
    #undef EGL_KHR_stream_fifo
    #undef EGL_KHR_stream_producer_aldatalocator
    #undef EGL_KHR_stream_producer_eglsurface
    #undef EGL_KHR_surfaceless_context
    #undef EGL_KHR_swap_buffers_with_damage
    #undef EGL_KHR_vg_parent_image
    #undef EGL_KHR_wait_sync
    #undef EGL_ANDROID_GLES_layers
    #undef EGL_ANDROID_blob_cache
    #undef EGL_ANDROID_create_native_client_buffer
    #undef EGL_ANDROID_framebuffer_target
    #undef EGL_ANDROID_front_buffer_auto_refresh
    #undef EGL_ANDROID_get_frame_timestamps
    #undef EGL_ANDROID_get_native_client_buffer
    #undef EGL_ANDROID_image_native_buffer
    #undef EGL_ANDROID_native_fence_sync
    #undef EGL_ANDROID_presentation_time
    #undef EGL_ANDROID_recordable
    #undef EGL_ANGLE_d3d_share_handle_client_buffer
    #undef EGL_ANGLE_device_d3d
    #undef EGL_ANGLE_query_surface_pointer
    #undef EGL_ANGLE_surface_d3d_texture_2d_share_handle
    #undef EGL_ANGLE_sync_control_rate
    #undef EGL_ANGLE_window_fixed_size
    #undef EGL_ARM_image_format
    #undef EGL_ARM_implicit_external_sync
    #undef EGL_ARM_pixmap_multisample_discard
    #undef EGL_EXT_bind_to_front
    #undef EGL_EXT_buffer_age
    #undef EGL_EXT_client_extensions
    #undef EGL_EXT_client_sync
    #undef EGL_EXT_compositor
    #undef EGL_EXT_config_select_group
    #undef EGL_EXT_create_context_robustness
    #undef EGL_EXT_device_base
    #undef EGL_EXT_device_drm
    #undef EGL_EXT_device_drm_render_node
    #undef EGL_EXT_device_enumeration
    #undef EGL_EXT_device_openwf
    #undef EGL_EXT_device_persistent_id
    #undef EGL_EXT_device_query
    #undef EGL_EXT_device_query_name
    #undef EGL_EXT_gl_colorspace_bt2020_linear
    #undef EGL_EXT_gl_colorspace_bt2020_pq
    #undef EGL_EXT_gl_colorspace_display_p3
    #undef EGL_EXT_gl_colorspace_display_p3_linear
    #undef EGL_EXT_gl_colorspace_display_p3_passthrough
    #undef EGL_EXT_gl_colorspace_scrgb
    #undef EGL_EXT_gl_colorspace_scrgb_linear
    // #undef EGL_EXT_image_dma_buf_import
    #undef EGL_EXT_image_dma_buf_import_modifiers
    #undef EGL_EXT_image_gl_colorspace
    #undef EGL_EXT_image_implicit_sync_control
    #undef EGL_EXT_multiview_window
    #undef EGL_EXT_output_base
    #undef EGL_EXT_output_drm
    #undef EGL_EXT_output_openwf
    #undef EGL_EXT_pixel_format_float
    #undef EGL_EXT_platform_base
    #undef EGL_EXT_platform_device
    #undef EGL_EXT_platform_wayland
    #undef EGL_EXT_platform_x11
    #undef EGL_EXT_platform_xcb
    #undef EGL_EXT_present_opaque
    #undef EGL_EXT_protected_content
    #undef EGL_EXT_protected_surface
    #undef EGL_EXT_stream_consumer_egloutput
    #undef EGL_EXT_surface_CTA861_3_metadata
    #undef EGL_EXT_surface_SMPTE2086_metadata
    #undef EGL_EXT_surface_compression
    #undef EGL_EXT_swap_buffers_with_damage
    #undef EGL_EXT_sync_reuse
    #undef EGL_EXT_yuv_surface
    #undef EGL_HI_clientpixmap
    #undef EGL_HI_colorformats
    #undef EGL_IMG_context_priority
    #undef EGL_IMG_image_plane_attribs
    #undef EGL_MESA_drm_image
    #undef EGL_MESA_image_dma_buf_export
    #undef EGL_MESA_platform_gbm
    #undef EGL_MESA_platform_surfaceless
    #undef EGL_NV_quadruple_buffer
    #undef EGL_NV_robustness_video_memory_purge
    #undef EGL_NV_stream_consumer_eglimage
    #undef EGL_NV_stream_consumer_gltexture_yuv
    #undef EGL_NV_stream_cross_display
    #undef EGL_NV_stream_cross_object
    #undef EGL_NV_stream_cross_partition
    #undef EGL_NV_stream_cross_process
    #undef EGL_NV_stream_cross_system
    #undef EGL_NV_stream_dma
    #undef EGL_NV_stream_fifo_next
    #undef EGL_NV_stream_fifo_synchronous
    #undef EGL_NV_stream_flush
    #undef EGL_NV_stream_frame_limits
    #undef EGL_NV_stream_metadata
    #undef EGL_NV_stream_origin
    #undef EGL_NV_stream_remote
    #undef EGL_NV_stream_reset
    #undef EGL_NV_stream_socket
    #undef EGL_NV_stream_socket_inet
    #undef EGL_NV_stream_socket_unix
    #undef EGL_NV_stream_sync
    #undef EGL_NV_sync
    #undef EGL_NV_system_time
    #undef EGL_NV_triple_buffer
    #undef EGL_TIZEN_image_native_buffer
    #undef EGL_TIZEN_image_native_surface
    #undef EGL_WL_bind_wayland_display
    #undef EGL_WL_create_wayland_buffer_from_image
    #undef EGL_NV_native_query
    #undef EGL_NV_post_convert_rounding
    #undef EGL_NV_post_sub_buffer
    #undef EGL_NV_quadruple_buffer
    #undef EGL_NV_robustness_video_memory_purge
    #undef EGL_NV_stream_consumer_eglimage
    #undef EGL_NV_stream_consumer_gltexture_yuv
    #undef EGL_NV_stream_cross_display
    #undef EGL_NV_stream_cross_object
    #undef EGL_NV_stream_cross_partition
    #undef EGL_NV_stream_cross_process
    #undef EGL_NV_stream_cross_system
    #undef EGL_NV_stream_dma
    #undef EGL_NV_stream_fifo_next
    #undef EGL_NV_stream_fifo_synchronous
    #undef EGL_NV_stream_flush
    #undef EGL_NV_stream_frame_limits
    #undef EGL_NV_stream_metadata
    #undef EGL_NV_stream_origin
    #undef EGL_NV_stream_remote
    #undef EGL_NV_stream_reset
    #undef EGL_NV_stream_socket
    #undef EGL_NV_stream_socket_inet
    #undef EGL_NV_stream_socket_unix
    #undef EGL_NV_stream_sync
    #undef EGL_NV_sync
    #undef EGL_NV_system_time
    #undef EGL_NV_triple_buffer
    #undef EGL_TIZEN_image_native_buffer
    #undef EGL_TIZEN_image_native_surface
    #undef EGL_WL_bind_wayland_display
    #undef EGL_WL_create_wayland_buffer_from_image

#else

    #include <EGL/egl.h>
    #include <EGL/eglext.h>

#endif

// Older egl.h doesn't define typedefs for standard EGL functions, for example on debian buster.
// Define them ourselves if necessary.
//
// We don't use the function typedefs for functions part of 1.4, since
// those should be present at all times and we just statically use them.
//
// For functions part of EGL 1.5 we dynamically resolve the functions at
// runtime, since we can't be sure they're actually present.
#if defined(EGL_VERSION_1_5) && !defined(EGL_EGL_PROTOTYPES)

// clang-format off
typedef EGLSync(EGLAPIENTRYP PFNEGLCREATESYNCPROC)(EGLDisplay dpy, EGLenum type, const EGLAttrib *attrib_list);
typedef EGLBoolean(EGLAPIENTRYP PFNEGLDESTROYSYNCPROC)(EGLDisplay dpy, EGLSync sync);
typedef EGLint(EGLAPIENTRYP PFNEGLCLIENTWAITSYNCPROC)(EGLDisplay dpy, EGLSync sync, EGLint flags, EGLTime timeout);
typedef EGLBoolean(EGLAPIENTRYP PFNEGLGETSYNCATTRIBPROC)(EGLDisplay dpy, EGLSync sync, EGLint attribute, EGLAttrib *value);
typedef EGLImage(EGLAPIENTRYP PFNEGLCREATEIMAGEPROC)(
    EGLDisplay dpy,
    EGLContext ctx,
    EGLenum target,
    EGLClientBuffer buffer,
    const EGLAttrib *attrib_list
);
typedef EGLBoolean(EGLAPIENTRYP PFNEGLDESTROYIMAGEPROC)(EGLDisplay dpy, EGLImage image);
typedef EGLDisplay(EGLAPIENTRYP PFNEGLGETPLATFORMDISPLAYPROC)(EGLenum platform, void *native_display, const EGLAttrib *attrib_list);
typedef EGLSurface(EGLAPIENTRYP PFNEGLCREATEPLATFORMWINDOWSURFACEPROC)(
    EGLDisplay dpy,
    EGLConfig config,
    void *native_window,
    const EGLAttrib *attrib_list
);
typedef EGLSurface(EGLAPIENTRYP PFNEGLCREATEPLATFORMPIXMAPSURFACEPROC)(
    EGLDisplay dpy,
    EGLConfig config,
    void *native_pixmap,
    const EGLAttrib *attrib_list
);
typedef EGLBoolean(EGLAPIENTRYP PFNEGLWAITSYNCPROC)(EGLDisplay dpy, EGLSync sync, EGLint flags);
    // clang-format on

#endif

#ifdef HAVE_EGL
static inline bool check_egl_extension(const char *client_ext_string, const char *display_ext_string, const char *extension) {
    size_t len = strlen(extension);

    if (client_ext_string != NULL) {
        const char *result = strstr(client_ext_string, extension);
        if (result != NULL && (result[len] == ' ' || result[len] == '\0')) {
            return true;
        }
    }

    if (display_ext_string != NULL) {
        const char *result = strstr(display_ext_string, extension);
        if (result != NULL && (result[len] == ' ' || result[len] == '\0')) {
            return true;
        }
    }

    return false;
}

static inline const char *egl_strerror(EGLenum result) {
    switch (result) {
        case EGL_SUCCESS: return "EGL_SUCCESS";
        case EGL_NOT_INITIALIZED: return "EGL_NOT_INITIALIZED";
        case EGL_BAD_ACCESS: return "EGL_BAD_ACCESS";
        case EGL_BAD_ALLOC: return "EGL_BAD_ALLOC";
        case EGL_BAD_ATTRIBUTE: return "EGL_BAD_ATTRIBUTE";
        case EGL_BAD_CONFIG: return "EGL_BAD_CONFIG";
        case EGL_BAD_CONTEXT: return "EGL_BAD_CONTEXT";
        case EGL_BAD_CURRENT_SURFACE: return "EGL_BAD_CURRENT_SURFACE";
        case EGL_BAD_DISPLAY: return "EGL_BAD_DISPLAY";
        case EGL_BAD_MATCH: return "EGL_BAD_MATCH";
        case EGL_BAD_NATIVE_PIXMAP: return "EGL_BAD_NATIVE_PIXMAP";
        case EGL_BAD_NATIVE_WINDOW: return "EGL_BAD_NATIVE_WINDOW";
        case EGL_BAD_PARAMETER: return "EGL_BAD_PARAMETER";
        case EGL_BAD_SURFACE: return "EGL_BAD_SURFACE";
        case EGL_CONTEXT_LOST: return "EGL_CONTEXT_LOST";
        default: return "<unknown result code>";
    }
}

    #define LOG_EGL_ERROR(result, fmt, ...) LOG_ERROR(fmt ": %s\n", __VA_ARGS__ egl_strerror(result))
#endif

#endif  // _FLUTTERPI_SRC_EGL_H
