#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include <unistd.h>

#include <drm_fourcc.h>
#include <gbm.h>
#include <gst/allocators/allocators.h>
#include <gst/video/video.h>

#include "flutter-pi.h"
#include "texture_registry.h"

// This will error if we don't have EGL / OpenGL ES support.
#include "gl_renderer.h"
#include "plugins/gstreamer_video_player.h"
#include "util/logging.h"
#include "util/refcounting.h"

#define MAX_N_PLANES 4

#define DRM_FOURCC_FORMAT "c%c%c%c"
#define DRM_FOURCC_ARGS(format) (format) & 0xFF, ((format) >> 8) & 0xFF, ((format) >> 16) & 0xFF, ((format) >> 24) & 0xFF

struct video_frame {
    GstSample *sample;

    struct frame_interface *interface;
    
    uint32_t drm_format;

    int n_dmabuf_fds;
    int dmabuf_fds[MAX_N_PLANES];

    EGLImageKHR image;

    struct gl_texture_frame gl_frame;
};

struct frame_interface {
    struct gbm_device *gbm_device;
    EGLDisplay display;

    pthread_mutex_t context_lock;
    EGLContext context;

    PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR;
    PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR;

    bool supports_external_target;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES;

    bool supports_extended_imports;
#ifdef EGL_EXT_image_dma_buf_import_modifiers
    PFNEGLQUERYDMABUFFORMATSEXTPROC eglQueryDmaBufFormatsEXT;
    PFNEGLQUERYDMABUFMODIFIERSEXTPROC eglQueryDmaBufModifiersEXT;
#endif

    int n_formats;
    struct egl_modified_format *formats;

    refcount_t n_refs;
};

#ifdef EGL_EXT_image_dma_buf_import_modifiers
static bool query_formats(
    EGLDisplay display,
    PFNEGLQUERYDMABUFFORMATSEXTPROC egl_query_dmabuf_formats,
    PFNEGLQUERYDMABUFMODIFIERSEXTPROC egl_query_dmabuf_modifiers,
    int *n_formats_out,
    struct egl_modified_format **formats_out
) {
    // EGLuint64KHR is defined by EGL_KHR_stream
    #ifndef EGL_KHR_stream
        #error "EGL header definitions for extension EGL_KHR_stream are required."
    #endif

    struct egl_modified_format *modified_formats;
    EGLBoolean *external_only;
    EGLuint64KHR *modifiers;
    EGLint *formats;
    EGLint egl_ok, n_modifiers;
    int n_formats, n_modified_formats, max_n_modifiers;

    egl_ok = egl_query_dmabuf_formats(display, 0, NULL, &n_formats);
    if (egl_ok != EGL_TRUE) {
        LOG_ERROR("Could not query number of dmabuf formats supported by EGL.\n");
        goto fail;
    }

    formats = malloc(n_formats * sizeof *formats);
    if (formats == NULL) {
        goto fail;
    }

    egl_ok = egl_query_dmabuf_formats(display, n_formats, formats, &n_formats);
    if (egl_ok != EGL_TRUE) {
        LOG_ERROR("Could not query dmabuf formats supported by EGL.\n");
        goto fail_free_formats;
    }

    // first, count how many modifiers we have for each format.
    n_modified_formats = 0;
    max_n_modifiers = 0;
    for (int i = 0; i < n_formats; i++) {
        egl_ok = egl_query_dmabuf_modifiers(display, formats[i], 0, NULL, NULL, &n_modifiers);
        if (egl_ok != EGL_TRUE) {
            LOG_ERROR("Could not query dmabuf formats supported by EGL.\n");
            goto fail_free_formats;
        }

        n_modified_formats += n_modifiers;

        if (n_modifiers > max_n_modifiers) {
            max_n_modifiers = n_modifiers;
        }
    }

    modified_formats = malloc(n_modified_formats * sizeof *modified_formats);
    if (modified_formats == NULL) {
        goto fail_free_formats;
    }

    modifiers = malloc(max_n_modifiers * sizeof *modifiers);
    if (modifiers == NULL) {
        goto fail_free_modified_formats;
    }

    external_only = malloc(max_n_modifiers * sizeof *external_only);
    if (external_only == NULL) {
        goto fail_free_modifiers;
    }

    LOG_DEBUG("supported formats for EGL import: ");
    for (int i = 0, j = 0; i < n_formats; i++) {
        egl_ok = egl_query_dmabuf_modifiers(display, formats[i], max_n_modifiers, modifiers, external_only, &n_modifiers);
        if (egl_ok != EGL_TRUE) {
            LOG_ERROR("Could not query dmabuf formats supported by EGL.\n");
            goto fail_free_formats;
        }

        LOG_DEBUG_UNPREFIXED("%" DRM_FOURCC_FORMAT ", ", DRM_FOURCC_ARGS(formats[i]));

        for (int k = 0; k < n_modifiers; k++, j++) {
            modified_formats[j].format = formats[i];
            modified_formats[j].modifier = modifiers[k];
            modified_formats[j].external_only = external_only[k];
        }
    }

    LOG_DEBUG_UNPREFIXED("\n");

    free(formats);
    free(modifiers);
    free(external_only);

    *n_formats_out = n_modified_formats;
    *formats_out = modified_formats;
    return true;

fail_free_modifiers:
    free(modifiers);

fail_free_modified_formats:
    free(modified_formats);

fail_free_formats:
    free(formats);

fail:
    *n_formats_out = 0;
    *formats_out = NULL;
    return false;
}
#endif

struct frame_interface *frame_interface_new(struct gl_renderer *renderer) {
    struct frame_interface *interface;
    struct gbm_device *gbm_device;
    EGLBoolean egl_ok;
    EGLContext context;
    EGLDisplay display;
    struct egl_modified_format *formats;
    bool supports_extended_imports, supports_external_target;
    int n_formats;

    interface = malloc(sizeof *interface);
    if (interface == NULL) {
        return NULL;
    }

    if (!gl_renderer_supports_egl_extension(renderer, "EGL_EXT_image_dma_buf_import")) {
        LOG_ERROR("EGL does not support EGL_EXT_image_dma_buf_import extension. Video frames cannot be uploaded.\n");
        goto fail_free;
    }

    if (gl_renderer_supports_egl_extension(renderer, "EGL_EXT_image_dma_buf_import_modifiers")) {
#ifndef EGL_EXT_image_dma_buf_import_modifiers
        LOG_ERROR(
            "EGL supports EGL_EXT_image_dma_buf_import_modifiers, "
            "but EGL headers didn't contain definitions for EGL_EXT_image_dma_buf_import_modifiers."
            "Extended imports and pixel format information will not be used.\n"
        );
        supports_extended_imports = false;
#else
        supports_extended_imports = true;
#endif
    } else {
        supports_extended_imports = false;
    }

    if (gl_renderer_supports_gl_extension(renderer, "GL_OES_EGL_image_external")) {
        supports_external_target = true;
    } else {
        supports_external_target = false;
    }

    display = gl_renderer_get_egl_display(renderer);
    if (display == EGL_NO_DISPLAY) {
        goto fail_free;
    }

    context = gl_renderer_create_context(renderer);
    if (context == EGL_NO_CONTEXT) {
        goto fail_free;
    }

    PFNEGLCREATEIMAGEKHRPROC create_image = (PFNEGLCREATEIMAGEKHRPROC) gl_renderer_get_proc_address(renderer, "eglCreateImageKHR");
    if (create_image == NULL) {
        LOG_ERROR("Could not resolve eglCreateImageKHR EGL procedure.\n");
        goto fail_destroy_context;
    }

    PFNEGLDESTROYIMAGEKHRPROC destroy_image = (PFNEGLDESTROYIMAGEKHRPROC) gl_renderer_get_proc_address(renderer, "eglDestroyImageKHR");
    if (destroy_image == NULL) {
        LOG_ERROR("Could not resolve eglDestroyImageKHR EGL procedure.\n");
        goto fail_destroy_context;
    }

    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC gl_egl_image_target_texture2d = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC
    ) gl_renderer_get_proc_address(renderer, "glEGLImageTargetTexture2DOES");
    if (gl_egl_image_target_texture2d == NULL) {
        LOG_ERROR("Could not resolve glEGLImageTargetTexture2DOES EGL procedure.\n");
        goto fail_destroy_context;
    }

    // These two are optional.
    // Might be useful in the future.
#ifdef EGL_EXT_image_dma_buf_import_modifiers
    PFNEGLQUERYDMABUFFORMATSEXTPROC egl_query_dmabuf_formats = (PFNEGLQUERYDMABUFFORMATSEXTPROC
    ) gl_renderer_get_proc_address(renderer, "eglQueryDmaBufFormatsEXT");
    if (egl_query_dmabuf_formats == NULL && supports_extended_imports) {
        LOG_ERROR("Could not resolve eglQueryDmaBufFormatsEXT egl procedure, even though it is listed as supported.\n");
        supports_extended_imports = false;
    }

    PFNEGLQUERYDMABUFMODIFIERSEXTPROC egl_query_dmabuf_modifiers = (PFNEGLQUERYDMABUFMODIFIERSEXTPROC
    ) gl_renderer_get_proc_address(renderer, "eglQueryDmaBufModifiersEXT");
    if (egl_query_dmabuf_modifiers == NULL && supports_extended_imports) {
        LOG_ERROR("Could not resolve eglQueryDmaBufModifiersEXT egl procedure, even though it is listed as supported.\n");
        supports_extended_imports = false;
    }
#endif

    gbm_device = gl_renderer_get_gbm_device(renderer);
    if (gbm_device == NULL) {
        LOG_ERROR("GL Render doesn't have a GBM device associated with it, which is necessary for importing the video frames.\n");
        goto fail_destroy_context;
    }

    if (supports_extended_imports) {
#ifdef EGL_EXT_image_dma_buf_import_modifiers
        query_formats(display, egl_query_dmabuf_formats, egl_query_dmabuf_modifiers, &n_formats, &formats);
#else
        UNREACHABLE();
#endif
    } else {
        n_formats = 0;
        formats = NULL;
    }

    interface->gbm_device = gbm_device;
    interface->display = display;
    pthread_mutex_init(&interface->context_lock, NULL);
    interface->context = context;
    interface->eglCreateImageKHR = create_image;
    interface->eglDestroyImageKHR = destroy_image;
    interface->supports_external_target = supports_external_target;
    interface->glEGLImageTargetTexture2DOES = gl_egl_image_target_texture2d;
    interface->supports_extended_imports = supports_extended_imports;
#ifdef EGL_EXT_image_dma_buf_import_modifiers
    interface->eglQueryDmaBufFormatsEXT = egl_query_dmabuf_formats;
    interface->eglQueryDmaBufModifiersEXT = egl_query_dmabuf_modifiers;
#endif
    interface->n_formats = n_formats;
    interface->formats = formats;
    interface->n_refs = REFCOUNT_INIT_1;
    return interface;

fail_destroy_context:
    egl_ok = eglDestroyContext(display, context);
    ASSERT_EGL_TRUE(egl_ok);
    (void) egl_ok;

fail_free:
    free(interface);
    return NULL;
}

void frame_interface_destroy(struct frame_interface *interface) {
    EGLBoolean egl_ok;

    pthread_mutex_destroy(&interface->context_lock);
    egl_ok = eglDestroyContext(interface->display, interface->context);
    ASSERT_EGL_TRUE(egl_ok);
    (void) egl_ok;
    if (interface->formats != NULL) {
        free(interface->formats);
    }
    free(interface);
}

ATTR_PURE int frame_interface_get_n_formats(struct frame_interface *interface) {
    return interface->n_formats;
}

ATTR_PURE const struct egl_modified_format *frame_interface_get_format(struct frame_interface *interface, int index) {
    assert(index < interface->n_formats);
    return interface->formats + index;
}

DEFINE_LOCK_OPS(frame_interface, context_lock)

DEFINE_REF_OPS(frame_interface, n_refs)

/**
 * @brief Create a dmabuf fd from the given GstBuffer.
 *
 * Calls gst_buffer_map on the buffer, so buffer could have changed after the call.
 *
 */
UNUSED int dup_gst_buffer_range_as_dmabuf(struct gbm_device *gbm_device, GstBuffer *buffer, unsigned int memory_index, int n_memories) {
    struct gbm_bo *bo;
    GstMapInfo map_info;
    uint32_t stride;
    gboolean gst_ok;
    void *map, *map_data;
    int fd;

    gst_ok = gst_buffer_map_range(buffer, memory_index, n_memories, &map_info, GST_MAP_READ);
    if (gst_ok == FALSE) {
        LOG_ERROR("Couldn't map gstreamer video frame buffer to copy it into a dma buffer.\n");
        return -1;
    }

    // Create a square texture large enough to fit our bytes instead of one with only one huge row,
    // because some drivers have limitations on the row length. (Intel)
    uint32_t dim = (uint32_t) ceil(sqrt(map_info.size));

    bo = gbm_bo_create(gbm_device, dim, dim, GBM_FORMAT_R8, GBM_BO_USE_LINEAR);
    if (bo == NULL) {
        LOG_ERROR("Couldn't create GBM BO to copy video frame into.\n");
        goto fail_unmap_buffer;
    }

    map_data = NULL;
    map = gbm_bo_map(bo, 0, 0, dim, dim, GBM_BO_TRANSFER_WRITE, &stride, &map_data);
    if (map == NULL) {
        LOG_ERROR("Couldn't mmap GBM BO to copy video frame into it.\n");
        goto fail_destroy_bo;
    }

    memcpy(map, map_info.data, map_info.size);

    gbm_bo_unmap(bo, map_data);

    fd = gbm_bo_get_fd(bo);
    if (fd < 0) {
        LOG_ERROR("Couldn't filedescriptor of video frame GBM BO.\n");
        goto fail_destroy_bo;
    }

    /// TODO: Should we dup the fd before we destroy the bo?
    gbm_bo_destroy(bo);
    gst_buffer_unmap(buffer, &map_info);
    return fd;

fail_destroy_bo:
    gbm_bo_destroy(bo);

fail_unmap_buffer:
    gst_buffer_unmap(buffer, &map_info);
    return -1;
}

/**
 * @brief Create a dmabuf fd from the given GstMemory.
 *
 * Calls gst_memory_map on the memory.
 *
 */
UNUSED int dup_gst_memory_as_dmabuf(struct gbm_device *gbm_device, GstMemory *memory) {
    struct gbm_bo *bo;
    GstMapInfo map_info;
    uint32_t stride;
    gboolean gst_ok;
    void *map, *map_data;
    int fd;

    gst_ok = gst_memory_map(memory, &map_info, GST_MAP_READ);
    if (gst_ok == FALSE) {
        LOG_ERROR("Couldn't map gstreamer video frame memory to copy it into a dma buffer.\n");
        return -1;
    }

    // Create a square texture large enough to fit our bytes instead of one with only one huge row,
    // because some drivers have limitations on the row length. (Intel)
    uint32_t dim = (uint32_t) ceil(sqrt(map_info.size));

    bo = gbm_bo_create(gbm_device, dim, dim, GBM_FORMAT_R8, GBM_BO_USE_LINEAR);
    if (bo == NULL) {
        LOG_ERROR("Couldn't create GBM BO to copy video frame into.\n");
        goto fail_unmap_buffer;
    }

    map_data = NULL;
    map = gbm_bo_map(bo, 0, 0, dim, dim, GBM_BO_TRANSFER_WRITE, &stride, &map_data);
    if (map == NULL) {
        LOG_ERROR("Couldn't mmap GBM BO to copy video frame into it.\n");
        goto fail_destroy_bo;
    }

    memcpy(map, map_info.data, map_info.size);

    gbm_bo_unmap(bo, map_data);

    fd = gbm_bo_get_fd(bo);
    if (fd < 0) {
        LOG_ERROR("Couldn't filedescriptor of video frame GBM BO.\n");
        goto fail_destroy_bo;
    }

    /// TODO: Should we dup the fd before we destroy the bo?
    gbm_bo_destroy(bo);
    gst_memory_unmap(memory, &map_info);
    return fd;

fail_destroy_bo:
    gbm_bo_destroy(bo);

fail_unmap_buffer:
    gst_memory_unmap(memory, &map_info);
    return -1;
}

struct plane_info {
    int fd;
    uint32_t offset;
    uint32_t pitch;
    bool has_modifier;
    uint64_t modifier;
};

#if THIS_GSTREAMER_VER < GSTREAMER_VER(1, 14, 0)
    #error "Unsupported gstreamer version."
#endif

#if THIS_GSTREAMER_VER >= GSTREAMER_VER(1, 18, 0)
static bool get_plane_sizes_from_meta(const GstVideoMeta *meta, size_t plane_sizes_out[4]) {
    GstVideoMeta *meta_non_const;
    gboolean gst_ok;

    #ifdef DEBUG
    GstVideoMeta _meta_non_const;
    meta_non_const = &_meta_non_const;
    memcpy(meta_non_const, meta, sizeof *meta);
    #else
    meta_non_const = (GstVideoMeta *) meta;
    #endif

    gst_ok = gst_video_meta_get_plane_size(meta_non_const, plane_sizes_out);
    if (gst_ok != TRUE) {
        LOG_ERROR("Could not query video frame plane size. gst_video_meta_get_plane_size\n");
        return false;
    }

    return true;
}

static bool get_plane_sizes_from_video_info(const GstVideoInfo *info, size_t plane_sizes_out[4]) {
    GstVideoAlignment alignment;
    GstVideoInfo *info_non_const;
    gboolean gst_ok;

    gst_video_alignment_reset(&alignment);

    #ifdef DEBUG
    info_non_const = gst_video_info_copy(info);
    #else
    info_non_const = (GstVideoInfo *) info;
    #endif

    gst_ok = gst_video_info_align_full(info_non_const, &alignment, plane_sizes_out);
    if (gst_ok != TRUE) {
        LOG_ERROR("Could not query video frame plane size. gst_video_info_align_full\n");
        return false;
    }

    #ifdef DEBUG
    assert(gst_video_info_is_equal(info, info_non_const));
    gst_video_info_free(info_non_const);
    #endif

    return true;
}

static bool calculate_plane_size(const GstVideoInfo *info, int plane_index, size_t *plane_size_out) {
    // Taken from: https://github.com/GStreamer/gstreamer/blob/621604aa3e4caa8db27637f63fa55fac2f7721e5/subprojects/gst-plugins-base/gst-libs/gst/video/video-info.c#L1278-L1301

    #if THIS_GSTREAMER_VER >= GSTREAMER_VER(1, 21, 3)
    if (GST_VIDEO_FORMAT_INFO_IS_TILED(info->finfo)) {
        guint x_tiles = GST_VIDEO_TILE_X_TILES(info->stride[plane_index]);
        guint y_tiles = GST_VIDEO_TILE_Y_TILES(info->stride[plane_index]);
        *plane_size_out = x_tiles * y_tiles * GST_VIDEO_FORMAT_INFO_TILE_SIZE(info->finfo, plane_index);
        return true;
    }
    #endif

    gint comp[GST_VIDEO_MAX_COMPONENTS];
    guint plane_height;

    /* Convert plane index to component index */
    gst_video_format_info_component(info->finfo, plane_index, comp);
    plane_height = GST_VIDEO_FORMAT_INFO_SCALE_HEIGHT(info->finfo, comp[0], GST_VIDEO_INFO_FIELD_HEIGHT(info));

    *plane_size_out = plane_height * GST_VIDEO_INFO_PLANE_STRIDE(info, plane_index);
    return true;
}

#else
static bool get_plane_sizes_from_meta(UNUSED const GstVideoMeta *meta, UNUSED size_t plane_sizes_out[4]) {
    return false;
}
static bool get_plane_sizes_from_video_info(UNUSED const GstVideoInfo *info, UNUSED size_t plane_sizes_out[4]) {
    return false;
}
static bool calculate_plane_size(UNUSED const GstVideoInfo *info, UNUSED int plane_index, UNUSED size_t *plane_size_out) {
    return false;
}
#endif

static int
get_plane_infos(GstBuffer *buffer, const GstVideoInfo *info, struct gbm_device *gbm_device, struct plane_info plane_infos[MAX_N_PLANES]) {
    GstVideoMeta *meta;
    GstMemory *memory;
    gboolean gst_ok;
    size_t plane_sizes[4] = { 0 };
    bool has_plane_sizes;
    int n_planes;

    n_planes = GST_VIDEO_INFO_N_PLANES(info);

    // There's so many ways to get the plane sizes.
    // 1. Preferably we should use the video meta.
    // 2. If that doesn't work, we'll use gst_video_info_align_full() with the video info.
    // 3. If that doesn't work, we'll calculate them ourselves.
    // 4. If that doesn't work, we can't determine the plane sizes.
    //    In that case, we'll error if we have more than one plane.
    has_plane_sizes = false;
    meta = gst_buffer_get_video_meta(buffer);
    if (meta != NULL) {
        has_plane_sizes = get_plane_sizes_from_meta(meta, plane_sizes);
    }

    if (!has_plane_sizes) {
        has_plane_sizes = get_plane_sizes_from_video_info(info, plane_sizes);
    }

    if (!has_plane_sizes) {
        has_plane_sizes = true;
        for (int i = 0; i < n_planes; i++) {
            has_plane_sizes = has_plane_sizes && calculate_plane_size(info, i, plane_sizes + i);
        }
    }

    if (!has_plane_sizes) {
        // We couldn't determine the plane sizes.
        // We can still continue if we have only one plane.
        if (n_planes != 1) {
            LOG_ERROR(
                "Couldn't determine video frame plane sizes. Without plane sizes, only planar framebuffer formats are supported, but the "
                "supplied format was not planar.\n"
            );
            return EINVAL;
        }

        // We'll just assume the first plane spans the entire buffer.
        plane_sizes[0] = gst_buffer_get_size(buffer);
        has_plane_sizes = true;
    }

    for (int i = 0; i < n_planes; i++) {
        size_t offset_in_memory = 0;
        size_t offset_in_buffer = 0;
        unsigned memory_index = 0;
        unsigned n_memories = 0;
        int stride, ok;

        if (meta) {
            offset_in_buffer = meta->offset[i];
            stride = meta->stride[i];
        } else {
            offset_in_buffer = GST_VIDEO_INFO_PLANE_OFFSET(info, i);
            stride = GST_VIDEO_INFO_PLANE_STRIDE(info, i);
        }

        gst_ok = gst_buffer_find_memory(buffer, offset_in_buffer, plane_sizes[i], &memory_index, &n_memories, &offset_in_memory);
        if (gst_ok != TRUE) {
            LOG_ERROR("Could not find video frame memory for plane.\n");
            ok = EIO;
            goto fail_close_fds;
        }

        static bool logged_dmabuf_feedback = false;

        if (n_memories != 1) {
            if (!logged_dmabuf_feedback) {
                LOG_DEBUG("INFO: Flutter-Pi is using manual dmabuf uploads to show video frames. This can result in poor performance.\n");
                logged_dmabuf_feedback = true;
            }

            ok = dup_gst_buffer_range_as_dmabuf(gbm_device, buffer, memory_index, n_memories);
            if (ok < 0) {
                LOG_ERROR("Could not upload gstreamer buffer range into dmabufs.\n");
                ok = EIO;
                goto fail_close_fds;
            }


            plane_infos[i].fd = ok;
        } else {
            memory = gst_buffer_peek_memory(buffer, memory_index);
            if (gst_is_dmabuf_memory(memory)) {
                ok = gst_dmabuf_memory_get_fd(memory);
                if (ok < 0) {
                    LOG_ERROR("Could not get gstreamer memory as dmabuf.\n");
                    ok = EIO;
                    goto fail_close_fds;
                }

                ok = dup(ok);
                if (ok < 0) {
                    ok = errno;
                    LOG_ERROR("Could not dup fd. dup: %s\n", strerror(ok));
                    goto fail_close_fds;
                }

                plane_infos[i].fd = ok;
            } else {
                if (!logged_dmabuf_feedback) {
                    LOG_DEBUG("INFO: Flutter-Pi is using manual dmabuf uploads to show video frames. This can result in poor performance.\n");
                    logged_dmabuf_feedback = true;
                }

                /// TODO: When duping, duplicate all non-dmabuf memories into one
                /// gbm buffer instead.
                ok = dup_gst_memory_as_dmabuf(gbm_device, memory);
                if (ok < 0) {
                    LOG_ERROR("Could not upload gstreamer memory into dmabuf.\n");
                    ok = EIO;
                    goto fail_close_fds;
                }

                plane_infos[i].fd = ok;
            }

            offset_in_memory += memory->offset;
        }

        plane_infos[i].offset = offset_in_memory;
        plane_infos[i].pitch = stride;

        /// TODO: Detect modifiers here
        /// Modifiers will be supported in future gstreamer, see:
        /// https://gstreamer.freedesktop.org/documentation/additional/design/dmabuf.html?gi-language=c
        plane_infos[i].has_modifier = false;
        plane_infos[i].modifier = DRM_FORMAT_MOD_LINEAR;
        continue;

fail_close_fds:
        for (int j = i - 1; j > 0; j--) {
            close(plane_infos[i].fd);
        }
        return ok;
    }

    return 0;
}

static uint32_t drm_format_from_gst_info(const GstVideoInfo *info) {
    switch (GST_VIDEO_INFO_FORMAT(info)) {
        case GST_VIDEO_FORMAT_YUY2: return DRM_FORMAT_YUYV;
        case GST_VIDEO_FORMAT_YVYU: return DRM_FORMAT_YVYU;
        case GST_VIDEO_FORMAT_UYVY: return DRM_FORMAT_UYVY;
        case GST_VIDEO_FORMAT_VYUY: return DRM_FORMAT_VYUY;
        case GST_VIDEO_FORMAT_AYUV: return DRM_FORMAT_AYUV;
#if THIS_GSTREAMER_VER >= GSTREAMER_VER(1, 16, 0)
        case GST_VIDEO_FORMAT_VUYA: return DRM_FORMAT_AYUV;
#endif
        case GST_VIDEO_FORMAT_NV12: return DRM_FORMAT_NV12;
        case GST_VIDEO_FORMAT_NV21: return DRM_FORMAT_NV21;
        case GST_VIDEO_FORMAT_NV16: return DRM_FORMAT_NV16;
        case GST_VIDEO_FORMAT_NV61: return DRM_FORMAT_NV61;
        case GST_VIDEO_FORMAT_NV24: return DRM_FORMAT_NV24;
        case GST_VIDEO_FORMAT_YUV9: return DRM_FORMAT_YUV410;
        case GST_VIDEO_FORMAT_YVU9: return DRM_FORMAT_YVU410;
        case GST_VIDEO_FORMAT_Y41B: return DRM_FORMAT_YUV411;
        case GST_VIDEO_FORMAT_I420: return DRM_FORMAT_YUV420;
        case GST_VIDEO_FORMAT_YV12: return DRM_FORMAT_YVU420;
        case GST_VIDEO_FORMAT_Y42B: return DRM_FORMAT_YUV422;
        case GST_VIDEO_FORMAT_Y444: return DRM_FORMAT_YUV444;
        case GST_VIDEO_FORMAT_RGB16: return DRM_FORMAT_RGB565;
        case GST_VIDEO_FORMAT_BGR16: return DRM_FORMAT_BGR565;
        case GST_VIDEO_FORMAT_RGBA: return DRM_FORMAT_ABGR8888;
        case GST_VIDEO_FORMAT_RGBx: return DRM_FORMAT_XBGR8888;
        case GST_VIDEO_FORMAT_BGRA: return DRM_FORMAT_ARGB8888;
        case GST_VIDEO_FORMAT_BGRx: return DRM_FORMAT_XRGB8888;
        case GST_VIDEO_FORMAT_ARGB: return DRM_FORMAT_BGRA8888;
        case GST_VIDEO_FORMAT_xRGB: return DRM_FORMAT_BGRX8888;
        case GST_VIDEO_FORMAT_ABGR: return DRM_FORMAT_RGBA8888;
        case GST_VIDEO_FORMAT_xBGR: return DRM_FORMAT_RGBX8888;
        default: return DRM_FORMAT_INVALID;
    }
}

ATTR_CONST GstVideoFormat gst_video_format_from_drm_format(uint32_t drm_format) {
    switch (drm_format) {
        case DRM_FORMAT_YUYV: return GST_VIDEO_FORMAT_YUY2;
        case DRM_FORMAT_YVYU: return GST_VIDEO_FORMAT_YVYU;
        case DRM_FORMAT_UYVY: return GST_VIDEO_FORMAT_UYVY;
        case DRM_FORMAT_VYUY: return GST_VIDEO_FORMAT_VYUY;
        case DRM_FORMAT_AYUV: return GST_VIDEO_FORMAT_AYUV;
#if THIS_GSTREAMER_VER >= GSTREAMER_VER(1, 16, 0)
            // GST_VIDEO_FORMAT_AYUV and _VUYA both map to DRM_FORMAT_AYUV
            // case DRM_FORMAT_AYUV: return GST_VIDEO_FORMAT_VUYA;
#endif
        case DRM_FORMAT_NV12: return GST_VIDEO_FORMAT_NV12;
        case DRM_FORMAT_NV21: return GST_VIDEO_FORMAT_NV21;
        case DRM_FORMAT_NV16: return GST_VIDEO_FORMAT_NV16;
        case DRM_FORMAT_NV61: return GST_VIDEO_FORMAT_NV61;
        case DRM_FORMAT_NV24: return GST_VIDEO_FORMAT_NV24;
        case DRM_FORMAT_YUV410: return GST_VIDEO_FORMAT_YUV9;
        case DRM_FORMAT_YVU410: return GST_VIDEO_FORMAT_YVU9;
        case DRM_FORMAT_YUV411: return GST_VIDEO_FORMAT_Y41B;
        case DRM_FORMAT_YUV420: return GST_VIDEO_FORMAT_I420;
        case DRM_FORMAT_YVU420: return GST_VIDEO_FORMAT_YV12;
        case DRM_FORMAT_YUV422: return GST_VIDEO_FORMAT_Y42B;
        case DRM_FORMAT_YUV444: return GST_VIDEO_FORMAT_Y444;
        case DRM_FORMAT_RGB565: return GST_VIDEO_FORMAT_RGB16;
        case DRM_FORMAT_BGR565: return GST_VIDEO_FORMAT_BGR16;
        case DRM_FORMAT_ABGR8888: return GST_VIDEO_FORMAT_RGBA;
        case DRM_FORMAT_XBGR8888: return GST_VIDEO_FORMAT_RGBx;
        case DRM_FORMAT_ARGB8888: return GST_VIDEO_FORMAT_BGRA;
        case DRM_FORMAT_XRGB8888: return GST_VIDEO_FORMAT_BGRx;
        case DRM_FORMAT_BGRA8888: return GST_VIDEO_FORMAT_ARGB;
        case DRM_FORMAT_BGRX8888: return GST_VIDEO_FORMAT_xRGB;
        case DRM_FORMAT_RGBA8888: return GST_VIDEO_FORMAT_ABGR;
        case DRM_FORMAT_RGBX8888: return GST_VIDEO_FORMAT_xBGR;
        default: return GST_VIDEO_FORMAT_UNKNOWN;
    }
}

static EGLint egl_color_space_from_gst_info(const GstVideoInfo *info) {
    if (gst_video_colorimetry_matches(&GST_VIDEO_INFO_COLORIMETRY(info), GST_VIDEO_COLORIMETRY_BT601)) {
        return EGL_ITU_REC601_EXT;
    } else if (gst_video_colorimetry_matches(&GST_VIDEO_INFO_COLORIMETRY(info), GST_VIDEO_COLORIMETRY_BT709)) {
        return EGL_ITU_REC709_EXT;
    } else if (gst_video_colorimetry_matches(&GST_VIDEO_INFO_COLORIMETRY(info), GST_VIDEO_COLORIMETRY_BT2020)) {
        return EGL_ITU_REC2020_EXT;
    } else {
        LOG_DEBUG("Unsupported video colorimetry: %s\n", gst_video_colorimetry_to_string(&GST_VIDEO_INFO_COLORIMETRY(info)));
        return EGL_NONE;
    }
}

static EGLint egl_sample_range_hint_from_gst_info(const GstVideoInfo *info) {
    if (GST_VIDEO_INFO_COLORIMETRY(info).range == GST_VIDEO_COLOR_RANGE_0_255) {
        return EGL_YUV_FULL_RANGE_EXT;
    } else if (GST_VIDEO_INFO_COLORIMETRY(info).range == GST_VIDEO_COLOR_RANGE_16_235) {
        return EGL_YUV_NARROW_RANGE_EXT;
    } else {
        return EGL_NONE;
    }
}

static EGLint egl_horizontal_chroma_siting_from_gst_info(const GstVideoInfo *info) {
    GstVideoChromaSite chroma_site = GST_VIDEO_INFO_CHROMA_SITE(info);

    if (chroma_site == GST_VIDEO_CHROMA_SITE_H_COSITED || chroma_site == GST_VIDEO_CHROMA_SITE_COSITED) {
        return EGL_YUV_CHROMA_SITING_0_EXT;
    } else if (chroma_site == GST_VIDEO_CHROMA_SITE_V_COSITED || chroma_site == GST_VIDEO_CHROMA_SITE_NONE) {
        return EGL_YUV_CHROMA_SITING_0_5_EXT;
    } else {
        return EGL_NONE;
    }
}

static EGLint egl_vertical_chroma_siting_from_gst_info(const GstVideoInfo *info) {
    GstVideoChromaSite chroma_site = GST_VIDEO_INFO_CHROMA_SITE(info);

    if (chroma_site == GST_VIDEO_CHROMA_SITE_V_COSITED || chroma_site == GST_VIDEO_CHROMA_SITE_COSITED) {
        return EGL_YUV_CHROMA_SITING_0_EXT;
    } else if (chroma_site == GST_VIDEO_CHROMA_SITE_H_COSITED || chroma_site == GST_VIDEO_CHROMA_SITE_NONE) {
        return EGL_YUV_CHROMA_SITING_0_5_EXT;
    } else {
        return EGL_NONE;
    }
}

static struct video_frame *frame_new_egl_imported(struct frame_interface *interface, GstSample *sample, const GstVideoInfo *info) {
#define PUT_ATTR(_key, _value)                            \
    do {                                                  \
        assert(attr_index + 2 <= ARRAY_SIZE(attributes)); \
        attributes[attr_index++] = (_key);                \
        attributes[attr_index++] = (_value);              \
    } while (false)
    struct video_frame *frame;
    struct plane_info planes[MAX_N_PLANES];
    GstVideoInfo video_info;
    EGLBoolean egl_ok;
    GstBuffer *buffer;
    EGLImageKHR egl_image;
    gboolean gst_ok;
    GstCaps *caps;
    GLuint texture;
    GLenum gl_error;
    EGLint egl_error;
    EGLint attributes[2 * 7 + MAX_N_PLANES * 2 * 5 + 1];
    EGLint egl_color_space, egl_sample_range_hint, egl_horizontal_chroma_siting, egl_vertical_chroma_siting;
    int ok, width, height, n_planes, attr_index;

    buffer = gst_sample_get_buffer(sample);
    if (buffer == NULL) {
        LOG_ERROR("Could not get buffer from video sample.\n");
        return NULL;
    }

    // If we don't have an explicit info given, we determine it from the sample caps.
    if (info == NULL) {
        caps = gst_sample_get_caps(sample);
        if (caps == NULL) {
            return NULL;
        }

        gst_ok = gst_video_info_from_caps(&video_info, caps);
        if (gst_ok == FALSE) {
            LOG_ERROR("Could not get video info from caps.\n");
            return NULL;
        }

        info = &video_info;
    } else {
        caps = NULL;
    }

    // Determine some basic frame info.
    width = GST_VIDEO_INFO_WIDTH(info);
    height = GST_VIDEO_INFO_HEIGHT(info);
    n_planes = GST_VIDEO_INFO_N_PLANES(info);

    uint64_t drm_modifier = DRM_FORMAT_MOD_LINEAR;
    uint32_t drm_format = drm_format_from_gst_info(info);
    if (drm_format == DRM_FORMAT_INVALID) {
        LOG_ERROR("Video format has no EGL equivalent.\n");
        return NULL;
    }
    

    bool external_only;
    for_each_format_in_frame_interface(i, format, interface) {
        if (format->format == drm_format && format->modifier == drm_modifier) {
            external_only = format->external_only;
            goto format_supported;
        }
    }

    LOG_ERROR(
        "Video format is not supported by EGL: %" DRM_FOURCC_FORMAT " (modifier: %" PRIu64 ").\n",
        DRM_FOURCC_ARGS(drm_format),
        (uint64_t) drm_modifier
    );
    return NULL;

format_supported:

    // query the color space for this sample
    egl_color_space = egl_color_space_from_gst_info(info);

    // check the sample range
    egl_sample_range_hint = egl_sample_range_hint_from_gst_info(info);

    // check the chroma siting
    egl_horizontal_chroma_siting = egl_horizontal_chroma_siting_from_gst_info(info);
    egl_vertical_chroma_siting = egl_vertical_chroma_siting_from_gst_info(info);

    frame = malloc(sizeof *frame);
    if (frame == NULL) {
        return NULL;
    }

    ok = get_plane_infos(buffer, info, interface->gbm_device, planes);
    if (ok != 0) {
        goto fail_free_frame;
    }

    // Start putting together the EGL attributes.
    attr_index = 0;

#ifndef EGL_EXT_image_dma_buf_import
    #error "EGL header definitions for extension EGL_EXT_image_dma_buf_import are required."
#endif

    // first, put some of our basic attributes like
    // frame size and format
    PUT_ATTR(EGL_WIDTH, width);
    PUT_ATTR(EGL_HEIGHT, height);
    PUT_ATTR(EGL_LINUX_DRM_FOURCC_EXT, drm_format);

    // if we have a color space, put that too
    // could be one of EGL_ITU_REC601_EXT, EGL_ITU_REC709_EXT or EGL_ITU_REC2020_EXT
    if (egl_color_space != EGL_NONE) {
        PUT_ATTR(EGL_YUV_COLOR_SPACE_HINT_EXT, egl_color_space);
    }

    // if we have information about the sample range, put that into the attributes too
    if (egl_sample_range_hint != EGL_NONE) {
        PUT_ATTR(EGL_SAMPLE_RANGE_HINT_EXT, egl_sample_range_hint);
    }

    // chroma siting
    if (egl_horizontal_chroma_siting != EGL_NONE) {
        PUT_ATTR(EGL_YUV_CHROMA_HORIZONTAL_SITING_HINT_EXT, egl_horizontal_chroma_siting);
    }

    if (egl_vertical_chroma_siting != EGL_NONE) {
        PUT_ATTR(EGL_YUV_CHROMA_VERTICAL_SITING_HINT_EXT, egl_vertical_chroma_siting);
    }

    // add plane 1
    PUT_ATTR(EGL_DMA_BUF_PLANE0_FD_EXT, planes[0].fd);
    PUT_ATTR(EGL_DMA_BUF_PLANE0_OFFSET_EXT, planes[0].offset);
    PUT_ATTR(EGL_DMA_BUF_PLANE0_PITCH_EXT, planes[0].pitch);
    if (planes[0].has_modifier) {
        if (interface->supports_extended_imports) {
#ifdef EGL_EXT_image_dma_buf_import_modifiers
            PUT_ATTR(EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, uint32_to_int32(planes[0].modifier & 0xFFFFFFFFlu));
            PUT_ATTR(EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, uint32_to_int32(planes[0].modifier >> 32));
#else
            UNREACHABLE();
#endif
        } else {
            LOG_ERROR(
                "video frame buffer uses modified format but EGL doesn't support the EGL_EXT_image_dma_buf_import_modifiers extension.\n"
            );
            goto fail_release_planes;
        }
    }

    // add plane 2 (if present)
    if (n_planes >= 2) {
        PUT_ATTR(EGL_DMA_BUF_PLANE1_FD_EXT, planes[1].fd);
        PUT_ATTR(EGL_DMA_BUF_PLANE1_OFFSET_EXT, planes[1].offset);
        PUT_ATTR(EGL_DMA_BUF_PLANE1_PITCH_EXT, planes[1].pitch);
        if (planes[1].has_modifier) {
            if (interface->supports_extended_imports) {
#ifdef EGL_EXT_image_dma_buf_import_modifiers
                PUT_ATTR(EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT, uint32_to_int32(planes[1].modifier & 0xFFFFFFFFlu));
                PUT_ATTR(EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT, uint32_to_int32(planes[1].modifier >> 32));
#else
                UNREACHABLE();
#endif
            } else {
                LOG_ERROR(
                    "video frame buffer uses modified format but EGL doesn't support the EGL_EXT_image_dma_buf_import_modifiers "
                    "extension.\n"
                );
                goto fail_release_planes;
            }
        }
    }

    // add plane 3 (if present)
    if (n_planes >= 3) {
        PUT_ATTR(EGL_DMA_BUF_PLANE2_FD_EXT, planes[2].fd);
        PUT_ATTR(EGL_DMA_BUF_PLANE2_OFFSET_EXT, planes[2].offset);
        PUT_ATTR(EGL_DMA_BUF_PLANE2_PITCH_EXT, planes[2].pitch);
        if (planes[2].has_modifier) {
            if (interface->supports_extended_imports) {
#ifdef EGL_EXT_image_dma_buf_import_modifiers
                PUT_ATTR(EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, uint32_to_int32(planes[2].modifier & 0xFFFFFFFFlu));
                PUT_ATTR(EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT, uint32_to_int32(planes[2].modifier >> 32));
#else
                UNREACHABLE();
#endif
            } else {
                LOG_ERROR(
                    "video frame buffer uses modified format but EGL doesn't support the EGL_EXT_image_dma_buf_import_modifiers "
                    "extension.\n"
                );
                goto fail_release_planes;
            }
        }
    }

    // add plane 4 (if present)
    if (n_planes >= 4) {
        if (!interface->supports_extended_imports) {
            LOG_ERROR(
                "The video frame has more than 3 planes but that can't be imported as a GL texture if EGL doesn't support the "
                "EGL_EXT_image_dma_buf_import_modifiers extension.\n"
            );
            goto fail_release_planes;
        }

#ifdef EGL_EXT_image_dma_buf_import_modifiers
        PUT_ATTR(EGL_DMA_BUF_PLANE3_FD_EXT, planes[3].fd);
        PUT_ATTR(EGL_DMA_BUF_PLANE3_OFFSET_EXT, planes[3].offset);
        PUT_ATTR(EGL_DMA_BUF_PLANE3_PITCH_EXT, planes[3].pitch);
        if (planes[3].has_modifier) {
            PUT_ATTR(EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT, uint32_to_int32(planes[3].modifier & 0xFFFFFFFFlu));
            PUT_ATTR(EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT, uint32_to_int32(planes[3].modifier >> 32));
        }
#else
        UNREACHABLE();
#endif
    }

    assert(attr_index < ARRAY_SIZE(attributes));
    attributes[attr_index++] = EGL_NONE;

    egl_image = interface->eglCreateImageKHR(interface->display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attributes);
    if (egl_image == EGL_NO_IMAGE_KHR) {
        LOG_ERROR("Couldn't create EGL image from video sample.\n");
        goto fail_release_planes;
    }

    frame_interface_lock(interface);

    /// TODO: Make a single EGL context current for the whole lifetime of a
    /// gstreamer thread, instead of making one current and clearing it after
    /// for the duration of this procedure, to save some cycles.
    ///
    /// Make context current in GST_STREAM_STATUS_TYPE_ENTER message, clear it
    /// in GST_STREAM_STATUS_TYPE_LEAVE message.
    ///
    /// See https://gstreamer.freedesktop.org/documentation/additional/design/stream-status.html
    ///
    /// Alternatively, don't unconditionally create & make current an EGL
    /// context in the ENTER message, but create one lazily as soon as we need
    /// one, but keep track of it an destroy it in the LEAVE message.
    ///
    /// The STREAM_STATUS messages need to be handled synchronously of course,
    /// on the streaming thread, using either:
    ///   - gst_bus_set_sync_handler
    ///     https://gstreamer.freedesktop.org/documentation/gstreamer/gstbus.html#gst_bus_set_sync_handler
    ///   - or GstBus sync-message signal
    ///     https://gstreamer.freedesktop.org/documentation/gstreamer/gstbus.html#GstBus::sync-message
    egl_ok = eglMakeCurrent(interface->display, EGL_NO_SURFACE, EGL_NO_SURFACE, interface->context);
    if (egl_ok == EGL_FALSE) {
        egl_error = eglGetError();
        LOG_ERROR("Could not make EGL context current. eglMakeCurrent: %" PRId32 "\n", egl_error);
        goto fail_unlock_interface;
    }

    glGenTextures(1, &texture);
    if (texture == 0) {
        gl_error = glGetError();
        LOG_ERROR("Could not create GL texture. glGenTextures: %" PRIu32 "\n", gl_error);
        goto fail_clear_context;
    }

    GLenum target;
    if (external_only) {
        target = GL_TEXTURE_EXTERNAL_OES;
    } else {
        target = GL_TEXTURE_2D;
    }

    glBindTexture(target, texture);

    interface->glEGLImageTargetTexture2DOES(target, egl_image);
    gl_error = glGetError();
    if (gl_error != GL_NO_ERROR) {
        LOG_ERROR("Couldn't attach EGL Image to OpenGL texture. glEGLImageTargetTexture2DOES: %" PRIu32 "\n", gl_error);
        goto fail_unbind_texture;
    }

    glBindTexture(target, 0);

    egl_ok = eglMakeCurrent(interface->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (egl_ok == EGL_FALSE) {
        egl_error = eglGetError();
        LOG_ERROR("Could not clear EGL context. eglMakeCurrent: %" PRId32 "\n", egl_error);
        goto fail_delete_texture;
    }

    frame_interface_unlock(interface);

    frame->sample = gst_sample_ref(sample);
    frame->interface = frame_interface_ref(interface);
    frame->drm_format = drm_format;
    frame->n_dmabuf_fds = n_planes;
    frame->dmabuf_fds[0] = planes[0].fd;
    frame->dmabuf_fds[1] = planes[1].fd;
    frame->dmabuf_fds[2] = planes[2].fd;
    frame->dmabuf_fds[3] = planes[3].fd;
    frame->image = egl_image;
    frame->gl_frame.target = target;
    frame->gl_frame.name = texture;
    frame->gl_frame.format = GL_RGBA8_OES;
    frame->gl_frame.width = 0;
    frame->gl_frame.height = 0;
    return frame;

fail_unbind_texture:
    glBindTexture(texture, 0);

fail_delete_texture:
    glDeleteTextures(1, &texture);

fail_clear_context:
    eglMakeCurrent(interface->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

fail_unlock_interface:
    frame_interface_unlock(interface);
    interface->eglDestroyImageKHR(interface->display, egl_image);

fail_release_planes:
    for (int i = 0; i < n_planes; i++)
        close(planes[i].fd);

fail_free_frame:
    free(frame);
    return NULL;
}

static struct video_frame *frame_new_egl_duped(struct frame_interface *interface, GstSample *sample, const GstVideoInfo *info) {
    (void) interface;
    (void) sample;
    (void) info;
    return NULL;
}

struct video_frame *frame_new(struct frame_interface *interface, GstSample *sample, const GstVideoInfo *info) {
    struct video_frame *frame;

    frame = frame_new_egl_imported(interface, sample, info);
    if (frame != NULL) {
        return frame;
    }

    frame = frame_new_egl_duped(interface, sample, info);
    if (frame != NULL) {
        return frame;
    }

    return NULL;
}

void frame_destroy(struct video_frame *frame) {
    EGLBoolean egl_ok;
    int ok;

    frame_interface_lock(frame->interface);
    egl_ok = eglMakeCurrent(frame->interface->display, EGL_NO_SURFACE, EGL_NO_SURFACE, frame->interface->context);
    ASSERT_EGL_TRUE(egl_ok);
    (void) egl_ok;
    glDeleteTextures(1, &frame->gl_frame.name);
    assert(GL_NO_ERROR == glGetError());
    egl_ok = eglMakeCurrent(frame->interface->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    ASSERT_EGL_TRUE(egl_ok);
    frame_interface_unlock(frame->interface);

    egl_ok = frame->interface->eglDestroyImageKHR(frame->interface->display, frame->image);
    ASSERT_EGL_TRUE(egl_ok);
    frame_interface_unref(frame->interface);
    for (int i = 0; i < frame->n_dmabuf_fds; i++) {
        ok = close(frame->dmabuf_fds[i]);
        assert(ok == 0);
        (void) ok;
    }

    gst_sample_unref(frame->sample);
    free(frame);
}

const struct gl_texture_frame *frame_get_gl_frame(struct video_frame *frame) {
    return &frame->gl_frame;
}
