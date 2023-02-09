#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <inttypes.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <gbm.h>
#include <gst/video/video.h>
#include <gst/allocators/allocators.h>

#include <flutter-pi.h>
#include <texture_registry.h>
#include <gl_renderer.h>
#include <plugins/gstreamer_video_player.h>

FILE_DESCR("gstreamer video_player")

#define MAX_N_PLANES 4

struct video_frame {
    GstSample *sample;

    struct frame_interface *interface;

    uint32_t drm_format;

    int n_dmabuf_fds;
    int dmabuf_fds[MAX_N_PLANES];

    EGLImage image;
    size_t width, height;

    struct gl_texture_frame gl_frame;
};

struct frame_interface *frame_interface_new(struct gl_renderer *renderer) {
    struct frame_interface *interface;
    struct gbm_device *gbm_device;
    EGLBoolean egl_ok;
    EGLContext context;
    EGLDisplay display;

    interface = malloc(sizeof *interface);
    if (interface == NULL) {
        return NULL;
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

    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC gl_egl_image_target_texture2d = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC) gl_renderer_get_proc_address(renderer, "glEGLImageTargetTexture2DOES");
    if (gl_egl_image_target_texture2d == NULL) {
        LOG_ERROR("Could not resolve glEGLImageTargetTexture2DOES EGL procedure.\n");
        goto fail_destroy_context;
    }

    // These two are optional.
    // Might be useful in the future.
    PFNEGLQUERYDMABUFFORMATSEXTPROC egl_query_dmabuf_formats = (PFNEGLQUERYDMABUFFORMATSEXTPROC) gl_renderer_get_proc_address(renderer, "eglQueryDmaBufFormatsEXT");
    PFNEGLQUERYDMABUFMODIFIERSEXTPROC egl_query_dmabuf_modifiers = (PFNEGLQUERYDMABUFMODIFIERSEXTPROC) gl_renderer_get_proc_address(renderer, "eglQueryDmaBufModifiersEXT");

    gbm_device = gl_renderer_get_gbm_device(renderer);
    if (gbm_device == NULL) {
        LOG_ERROR("GL Render doesn't have a GBM device associated with it, which is necessary for importing the video frames.\n");
        goto fail_destroy_context;
    }

    interface->gbm_device = gbm_device;
    interface->display = display;
    pthread_mutex_init(&interface->context_lock, NULL); 
    interface->context = context;
    interface->eglCreateImageKHR = create_image;
    interface->eglDestroyImageKHR = destroy_image;
    interface->glEGLImageTargetTexture2DOES = gl_egl_image_target_texture2d;
    interface->supports_extended_imports = false;
    interface->eglQueryDmaBufFormatsEXT = egl_query_dmabuf_formats;
    interface->eglQueryDmaBufModifiersEXT = egl_query_dmabuf_modifiers;
    interface->n_refs = REFCOUNT_INIT_1;
    return interface;


    fail_destroy_context:
    egl_ok = eglDestroyContext(display, context);
    DEBUG_ASSERT_EGL_TRUE(egl_ok);
    (void) egl_ok;

    fail_free:
    free(interface);
    return NULL;
}

void frame_interface_destroy(struct frame_interface *interface) {
    EGLBoolean egl_ok;

    pthread_mutex_destroy(&interface->context_lock);
    egl_ok = eglDestroyContext(interface->display, interface->context);
    DEBUG_ASSERT_EGL_TRUE(egl_ok); (void) egl_ok;
    free(interface);
}

DEFINE_REF_OPS(frame_interface, n_refs)

/**
 * @brief Create a dmabuf fd from the given GstBuffer.
 * 
 * Calls gst_buffer_map on the buffer, so buffer could have changed after the call.
 * 
 */
int dup_gst_buffer_as_dmabuf(struct gbm_device *gbm_device, GstBuffer *buffer) {
    struct gbm_bo *bo;
    GstMapInfo map_info;
    uint32_t stride;
    gboolean gst_ok;
    void *map, *map_data;
    int fd;
    
    gst_ok = gst_buffer_map(buffer, &map_info, GST_MAP_READ);
    if (gst_ok == FALSE) {
        LOG_ERROR("Couldn't map gstreamer video frame buffer to copy it into a dma buffer.\n");
        return -1;
    }

    bo = gbm_bo_create(gbm_device, map_info.size, 1, GBM_FORMAT_R8, GBM_BO_USE_LINEAR);
    if (bo == NULL) {
        LOG_ERROR("Couldn't create GBM BO to copy video frame into.\n");
        goto fail_unmap_buffer;
    }

    map_data = NULL;
    map = gbm_bo_map(bo, 0, 0, map_info.size, 1, GBM_BO_TRANSFER_WRITE, &stride, &map_data);
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

struct video_frame *frame_new(
    struct frame_interface *interface,
    const struct frame_info *info,
    GstSample *sample
) {
#   define PUT_ATTR(_key, _value) do { *attr_cursor++ = _key; *attr_cursor++ = _value; } while (false)
    struct video_frame *frame;
    GstVideoMeta *meta;
    EGLBoolean egl_ok;
    EGLImage egl_image;
    GstBuffer *buffer;
    GstMemory *memory;
    gboolean gst_ok;
    GLenum gl_error;
    EGLint attributes[2*7 + MAX_N_PLANES*2*5 + 1], *attr_cursor;
    GLuint texture;
    EGLint egl_error;
    bool is_dmabuf_memory;
    int dmabuf_fd, n_planes, width, height;

    struct {
        int fd;
        int offset;
        int pitch;
        bool has_modifier;
        uint64_t modifier;
    } planes[MAX_N_PLANES];

    buffer = gst_sample_get_buffer(sample);

    frame = malloc(sizeof *frame);
    if (frame == NULL) {
        goto fail_unref_buffer;
    }

    memory = gst_buffer_peek_memory(buffer, 0);
    is_dmabuf_memory = gst_is_dmabuf_memory(memory);

    /// TODO: Do we really need to dup() here?
    if (is_dmabuf_memory) {
        //dmabuf_fd = dup(gst_dmabuf_memory_get_fd(memory));
        dmabuf_fd = -1;
    } else {
        dmabuf_fd = dup_gst_buffer_as_dmabuf(interface->gbm_device, buffer);
        
        //LOG_ERROR("Only dmabuf memory is supported for video frame buffers right now, but gstreamer didn't provide a dmabuf memory buffer.\n");
        //goto fail_free_frame;
    }

    width = GST_VIDEO_INFO_WIDTH(info->gst_info);
    height = GST_VIDEO_INFO_HEIGHT(info->gst_info);
    n_planes = GST_VIDEO_INFO_N_PLANES(info->gst_info);

    size_t plane_sizes[4] = {0};

    meta = gst_buffer_get_video_meta(buffer);
    if (meta != NULL) {
        gst_ok = gst_video_meta_get_plane_size(meta, plane_sizes);
        if (gst_ok != TRUE) {
            LOG_ERROR("Could not query video frame plane size.\n");
            goto fail_close_dmabuf_fd;
        }
    } else {
        // Taken from: https://github.com/GStreamer/gstreamer/blob/621604aa3e4caa8db27637f63fa55fac2f7721e5/subprojects/gst-plugins-base/gst-libs/gst/video/video-info.c#L1278-L1301
        for (int i = 0; i < GST_VIDEO_MAX_PLANES; i++) {
            if (i < GST_VIDEO_INFO_N_PLANES(info->gst_info)) {
                gint comp[GST_VIDEO_MAX_COMPONENTS];
                guint plane_height;

                gst_video_format_info_component(info->gst_info->finfo, i, comp);
                plane_height = GST_VIDEO_FORMAT_INFO_SCALE_HEIGHT(
                    info->gst_info->finfo,
                    comp[0],
                    GST_VIDEO_INFO_FIELD_HEIGHT(info->gst_info)
                );
                plane_sizes[i] = plane_height * GST_VIDEO_INFO_PLANE_STRIDE(info->gst_info, i);
            } else {
                plane_sizes[i] = 0;
            }
        }
    }

    for (int i = 0; i < n_planes; i++) {
        unsigned memory_index = 0, n_memories = 0;
        size_t offset_in_memory = 0;

        gst_ok = gst_buffer_find_memory(
            buffer,
            meta ? meta->offset[i] : GST_VIDEO_INFO_PLANE_OFFSET(info->gst_info, i),
            plane_sizes[i],
            &memory_index,
            &n_memories,
            &offset_in_memory
        );
        if (gst_ok != TRUE) {
            LOG_ERROR("Could not find video frame memory for plane.\n");
            goto fail_close_dmabuf_fd;
        }

        if (n_memories != 1) {
            LOG_ERROR("Gstreamer Image planes can't span multiple dmabufs.\n");
            goto fail_close_dmabuf_fd;
        }

        planes[i].fd = dup(gst_dmabuf_memory_get_fd(gst_buffer_peek_memory(buffer, memory_index)));
        planes[i].offset = offset_in_memory;
        planes[i].pitch = meta ? meta->stride[i] : GST_VIDEO_INFO_PLANE_STRIDE(info->gst_info, i);
        planes[i].has_modifier = false;
        planes[i].modifier = DRM_FORMAT_MOD_LINEAR;
    }

    attr_cursor = attributes;

    // first, put some of our basic attributes like
    // frame size and format
    PUT_ATTR(EGL_WIDTH, width);
    PUT_ATTR(EGL_HEIGHT, height);
    PUT_ATTR(EGL_LINUX_DRM_FOURCC_EXT, info->drm_format);

    // if we have a color space, put that too
    // could be one of EGL_ITU_REC601_EXT, EGL_ITU_REC709_EXT or EGL_ITU_REC2020_EXT
    if (info->egl_color_space != EGL_NONE) {
        PUT_ATTR(EGL_YUV_COLOR_SPACE_HINT_EXT, info->egl_color_space);
    }

    // if we have information about the sample range, put that into the attributes too
    if (GST_VIDEO_INFO_COLORIMETRY(info->gst_info).range == GST_VIDEO_COLOR_RANGE_0_255) {
        PUT_ATTR(EGL_SAMPLE_RANGE_HINT_EXT, EGL_YUV_FULL_RANGE_EXT);
    } else if (GST_VIDEO_INFO_COLORIMETRY(info->gst_info).range == GST_VIDEO_COLOR_RANGE_16_235) {
        PUT_ATTR(EGL_SAMPLE_RANGE_HINT_EXT, EGL_YUV_NARROW_RANGE_EXT);
    }

    // Check that we can actually represent that siting info using the attributes EGL gives us.
    // For example, we can't represent GST_VIDEO_CHROMA_SITE_ALT_LINE.
    if ((GST_VIDEO_INFO_CHROMA_SITE(info->gst_info) & ~(GST_VIDEO_CHROMA_SITE_H_COSITED | GST_VIDEO_CHROMA_SITE_V_COSITED)) == 0) {
        if (GST_VIDEO_INFO_CHROMA_SITE(info->gst_info) & GST_VIDEO_CHROMA_SITE_H_COSITED) {
            PUT_ATTR(EGL_YUV_CHROMA_HORIZONTAL_SITING_HINT_EXT, EGL_YUV_CHROMA_SITING_0_EXT);
        } else {
            PUT_ATTR(EGL_YUV_CHROMA_HORIZONTAL_SITING_HINT_EXT, EGL_YUV_CHROMA_SITING_0_5_EXT);
        }
        if (GST_VIDEO_INFO_CHROMA_SITE(info->gst_info) & GST_VIDEO_CHROMA_SITE_V_COSITED) {
            PUT_ATTR(EGL_YUV_CHROMA_VERTICAL_SITING_HINT_EXT, EGL_YUV_CHROMA_SITING_0_EXT);
        } else {
            PUT_ATTR(EGL_YUV_CHROMA_VERTICAL_SITING_HINT_EXT, EGL_YUV_CHROMA_SITING_0_5_EXT);
        }
    }
    
    // now begin with putting in information about plane memory
    PUT_ATTR(EGL_DMA_BUF_PLANE0_FD_EXT, planes[0].fd);
    PUT_ATTR(EGL_DMA_BUF_PLANE0_OFFSET_EXT, planes[0].offset);
    PUT_ATTR(EGL_DMA_BUF_PLANE0_PITCH_EXT, planes[0].pitch);
    if (planes[0].has_modifier) {
        if (interface->supports_extended_imports) {
            PUT_ATTR(EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, uint32_to_int32(planes[0].modifier & 0xFFFFFFFFlu));
            PUT_ATTR(EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, uint32_to_int32(planes[0].modifier >> 32));
        } else {
            LOG_ERROR("video frame buffer uses modified format but EGL doesn't support the EGL_EXT_image_dma_buf_import_modifiers extension.\n");
            goto fail_close_dmabuf_fd;
        }
    }

    if (n_planes >= 2) {
        PUT_ATTR(EGL_DMA_BUF_PLANE1_FD_EXT, planes[1].fd);
        PUT_ATTR(EGL_DMA_BUF_PLANE1_OFFSET_EXT, planes[1].offset);
        PUT_ATTR(EGL_DMA_BUF_PLANE1_PITCH_EXT, planes[1].pitch);
        if (planes[1].has_modifier) {
            if (interface->supports_extended_imports) {
                PUT_ATTR(EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT, uint32_to_int32(planes[1].modifier & 0xFFFFFFFFlu));
                PUT_ATTR(EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT, uint32_to_int32(planes[1].modifier >> 32));
            } else {
                LOG_ERROR("video frame buffer uses modified format but EGL doesn't support the EGL_EXT_image_dma_buf_import_modifiers extension.\n");
                goto fail_close_dmabuf_fd;
            }
        }
    }

    if (n_planes >= 3) {
        PUT_ATTR(EGL_DMA_BUF_PLANE2_FD_EXT, planes[2].fd);
        PUT_ATTR(EGL_DMA_BUF_PLANE2_OFFSET_EXT, planes[2].offset);
        PUT_ATTR(EGL_DMA_BUF_PLANE2_PITCH_EXT, planes[2].pitch);
        if (planes[2].has_modifier) {
            if (interface->supports_extended_imports) {
                PUT_ATTR(EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, uint32_to_int32(planes[2].modifier & 0xFFFFFFFFlu));
                PUT_ATTR(EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT, uint32_to_int32(planes[2].modifier >> 32));
            } else {
                LOG_ERROR("video frame buffer uses modified format but EGL doesn't support the EGL_EXT_image_dma_buf_import_modifiers extension.\n");
                goto fail_close_dmabuf_fd;
            }
        }
    }

    if (n_planes >= 4) {
        if (!interface->supports_extended_imports) {
            LOG_ERROR("The video frame has more than 3 planes but that can't be imported as a GL texture if EGL doesn't support the EGL_EXT_image_dma_buf_import_modifiers extension.\n");
            goto fail_close_dmabuf_fd;
        }

        PUT_ATTR(EGL_DMA_BUF_PLANE3_FD_EXT, planes[3].fd);
        PUT_ATTR(EGL_DMA_BUF_PLANE3_OFFSET_EXT, planes[3].offset);
        PUT_ATTR(EGL_DMA_BUF_PLANE3_PITCH_EXT, planes[3].pitch);
        if (planes[3].has_modifier) {
            if (interface->supports_extended_imports) {
                PUT_ATTR(EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT, uint32_to_int32(planes[3].modifier & 0xFFFFFFFFlu));
                PUT_ATTR(EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT, uint32_to_int32(planes[3].modifier >> 32));
            } else {
                LOG_ERROR("video frame buffer uses modified format but EGL doesn't support the EGL_EXT_image_dma_buf_import_modifiers extension.\n");
                goto fail_close_dmabuf_fd;
            }
        }
    }

    // add a EGL_NONE to mark the end of the buffer
    *attr_cursor++ = EGL_NONE;

    egl_image = interface->eglCreateImageKHR(interface->display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attributes);
    if (egl_image == EGL_NO_IMAGE_KHR) {
        goto fail_close_dmabuf_fd;
    }

    frame_interface_lock(interface);

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

    glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture);
    interface->glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, egl_image);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);

    egl_ok = eglMakeCurrent(interface->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (egl_ok == EGL_FALSE) {
        egl_error = eglGetError();
        LOG_ERROR("Could not clear EGL context. eglMakeCurrent: %" PRId32 "\n", egl_error);
        goto fail_delete_texture;
    }

    frame_interface_unlock(interface);

    frame->sample = sample;
    frame->interface = frame_interface_ref(interface);
    frame->drm_format = info->drm_format;
    frame->n_dmabuf_fds = 1;
    frame->dmabuf_fds[0] = dmabuf_fd;
    frame->image = egl_image;
    frame->gl_frame.target = GL_TEXTURE_EXTERNAL_OES;
    frame->gl_frame.name = texture;
    frame->gl_frame.format = GL_RGBA8_OES;
    frame->gl_frame.width = 0;
    frame->gl_frame.height = 0;

    return frame;

    fail_delete_texture:
    glDeleteTextures(1, &texture);

    fail_clear_context:
    eglMakeCurrent(interface->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    fail_unlock_interface:
    frame_interface_unlock(interface);
    interface->eglDestroyImageKHR(interface->display, egl_image);

    fail_close_dmabuf_fd:
    close(dmabuf_fd);
    free(frame);

    fail_unref_buffer:
    gst_sample_unref(sample);
    return NULL;

#   undef PUT_ATTR
}

void frame_destroy(struct video_frame *frame) {
    EGLBoolean egl_ok;
    int ok;

    /// TODO: See TODO in frame_new 
    gst_sample_unref(frame->sample);

    frame_interface_lock(frame->interface);
    egl_ok = eglMakeCurrent(frame->interface->display, EGL_NO_SURFACE, EGL_NO_SURFACE, frame->interface->context);
    DEBUG_ASSERT_EGL_TRUE(egl_ok); (void) egl_ok;
    glDeleteTextures(1, &frame->gl_frame.name);
    DEBUG_ASSERT(GL_NO_ERROR == glGetError());
    egl_ok = eglMakeCurrent(frame->interface->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    DEBUG_ASSERT_EGL_TRUE(egl_ok);
    frame_interface_unlock(frame->interface);
    
    egl_ok = frame->interface->eglDestroyImageKHR(frame->interface->display, frame->image);
    DEBUG_ASSERT_EGL_TRUE(egl_ok);
    frame_interface_unref(frame->interface);
    for (int i = 0; i < frame->n_dmabuf_fds; i++) {
        ok = close(frame->dmabuf_fds[i]);
        DEBUG_ASSERT(ok == 0); (void) ok;
    }
    free(frame);
}

const struct gl_texture_frame *frame_get_gl_frame(struct video_frame *frame) {
    return &frame->gl_frame;
}
