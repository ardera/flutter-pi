#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <inttypes.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <texture_registry.h>
#include <gst/video/video.h>
#include <gst/allocators/allocators.h>
#include <plugins/gstreamer_video_player.h>

#define LOG_ERROR(...) fprintf(stderr, "[gstreamer video player] " __VA_ARGS__)

#define MAX_N_PLANES 4

struct video_frame {
    GstBuffer *buffer;

    struct frame_interface interface;

    uint32_t drm_format;

    int n_dmabuf_fds;
    int dmabuf_fds[MAX_N_PLANES];

    EGLImage image;
    size_t width, height;

    struct gl_texture_frame gl_frame;
};

struct video_frame *frame_new(
    const struct frame_interface *interface,
    const struct frame_info *info,
    GstBuffer *buffer
) {
#   define PUT_ATTR(_key, _value) do { *attr_cursor++ = _key; *attr_cursor++ = _value; } while (false)
    struct video_frame *frame;
    GstVideoMeta *meta;
    EGLBoolean egl_ok;
    EGLImage egl_image;
    GstMemory *memory;
    GLenum gl_error;
    EGLint attributes[2*7 + MAX_N_PLANES*2*5 + 1], *attr_cursor;
    GLuint texture;
    EGLint egl_error;
    bool is_dmabuf_memory;
    int dmabuf_fd, n_mems, n_planes, width, height;

    struct {
        int fd;
        int offset;
        int pitch;
        bool has_modifier;
        uint64_t modifier;
    } planes[MAX_N_PLANES];

    frame = malloc(sizeof *frame);
    if (frame == NULL) {
        goto fail_unref_buffer;
    }

    memory = gst_buffer_peek_memory(buffer, 0);
    is_dmabuf_memory = gst_is_dmabuf_memory(memory);
    n_mems = gst_buffer_n_memory(buffer);

    if (!is_dmabuf_memory) {
        LOG_ERROR("Only dmabuf memory is supported for video frame buffers right now, but gstreamer didn't provide a dmabuf memory buffer.\n");
        goto fail_free_frame;
    }

    if (n_mems > 1) {
        LOG_ERROR("Multiple dmabufs for a single frame buffer is not supported right now.\n");
        goto fail_free_frame;
    }

    dmabuf_fd = dup(gst_dmabuf_memory_get_fd(memory));

    width = GST_VIDEO_INFO_WIDTH(info->gst_info);
    height = GST_VIDEO_INFO_HEIGHT(info->gst_info);
    n_planes = GST_VIDEO_INFO_N_PLANES(info->gst_info);

    meta = gst_buffer_get_video_meta(buffer);
    if (meta != NULL) {
        for (int i = 0; i < n_planes; i++) {
            planes[i].fd = dmabuf_fd;
            planes[i].offset = meta->offset[i];
            planes[i].pitch = meta->stride[i];
            planes[i].has_modifier = false;
            planes[i].modifier = DRM_FORMAT_MOD_LINEAR;
        }
    } else {
        for (int i = 0; i < n_planes; i++) {
            planes[i].fd = dmabuf_fd;
            planes[i].offset = GST_VIDEO_INFO_PLANE_OFFSET(info->gst_info, i);
            planes[i].pitch = GST_VIDEO_INFO_PLANE_STRIDE(info->gst_info, i);
            planes[i].has_modifier = false;
            planes[i].modifier = DRM_FORMAT_MOD_LINEAR;
        }
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

    egl_ok = eglMakeCurrent(interface->display, EGL_NO_SURFACE, EGL_NO_SURFACE, interface->context);
    if (egl_ok == EGL_FALSE) {
        egl_error = eglGetError();
        LOG_ERROR("Could not make EGL context current. eglMakeCurrent: %" PRId32 "\n", egl_error);
        goto fail_destroy_egl_image;
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

    frame->buffer = buffer;
    memcpy(&frame->interface, interface, sizeof *interface);
    frame->drm_format = info->drm_format;
    frame->n_dmabuf_fds = 1;
    frame->dmabuf_fds[0] = dmabuf_fd;
    frame->image = egl_image;
    frame->gl_frame.target = GL_TEXTURE_EXTERNAL_OES;
    frame->gl_frame.name = texture;
    frame->gl_frame.format = GL_NONE;
    frame->gl_frame.width = 0;
    frame->gl_frame.height = 0;

    return frame;

    fail_delete_texture:
    glDeleteTextures(1, &texture);

    fail_clear_context:
    eglMakeCurrent(interface->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    fail_destroy_egl_image:
    interface->eglDestroyImageKHR(interface->display, egl_image);

    fail_close_dmabuf_fd:
    close(dmabuf_fd);

    fail_free_frame:
    free(frame);

    fail_unref_buffer:
    gst_buffer_unref(buffer);
    return NULL;

#   undef PUT_ATTR
}

void frame_destroy(struct video_frame *frame) {
    gst_buffer_unref(frame->buffer);
    eglMakeCurrent(frame->interface.display, EGL_NO_SURFACE, EGL_NO_SURFACE, frame->interface.context);
    glDeleteTextures(1, &frame->gl_frame.name);
    eglMakeCurrent(frame->interface.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    frame->interface.eglDestroyImageKHR(frame->interface.display, frame->image);
    for (int i = 0; i < frame->n_dmabuf_fds; i++)
        close(frame->dmabuf_fds[i]);
    free(frame);
}

const struct gl_texture_frame *frame_get_gl_frame(struct video_frame *frame) {
    return &frame->gl_frame;
}