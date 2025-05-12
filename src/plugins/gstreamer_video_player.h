#ifndef _FLUTTERPI_INCLUDE_PLUGINS_GSTREAMER_VIDEO_PLAYER_H
#define _FLUTTERPI_INCLUDE_PLUGINS_GSTREAMER_VIDEO_PLAYER_H

#include "util/collection.h"
#include "util/lock_ops.h"
#include "util/refcounting.h"

#include <gst/video/video-format.h>

#include "config.h"

#if !defined(HAVE_EGL_GLES2)
    #error "gstreamer video player requires EGL and OpenGL ES2 support."
#else
    #include "egl.h"
    #include "gles.h"
#endif

#if !defined(HAVE_GSTREAMER_VIDEO_PLAYER)
    #error "gstreamer_video_player.h can't be used when building without gstreamer video player."
#endif

struct video_frame;
struct gl_renderer;

struct egl_modified_format {
    uint32_t format;
    uint64_t modifier;
    bool external_only;
};

struct frame_interface;

struct frame_interface *frame_interface_new(struct gl_renderer *renderer);

ATTR_PURE int frame_interface_get_n_formats(struct frame_interface *interface);

ATTR_PURE const struct egl_modified_format *frame_interface_get_format(struct frame_interface *interface, int index);

#define for_each_format_in_frame_interface(index, format, interface)                                                          \
    for (const struct egl_modified_format *format = frame_interface_get_format((interface), 0), *guard = NULL; guard == NULL; \
         guard = (void *) 1)                                                                                                  \
        for (size_t index = 0; index < frame_interface_get_n_formats(interface); index++,                                     \
                    format = (index) < frame_interface_get_n_formats(interface) ? frame_interface_get_format((interface), (index)) : NULL)

DECLARE_LOCK_OPS(frame_interface)

DECLARE_REF_OPS(frame_interface)

typedef struct _GstVideoInfo GstVideoInfo;
typedef struct _GstVideoMeta GstVideoMeta;


struct _GstSample;

ATTR_CONST GstVideoFormat gst_video_format_from_drm_format(uint32_t drm_format);

struct video_frame *frame_new(struct frame_interface *interface, GstSample *sample, const GstVideoInfo *info);

void frame_destroy(struct video_frame *frame);

struct gl_texture_frame;

const struct gl_texture_frame *frame_get_gl_frame(struct video_frame *frame);

struct texture;
struct gl_renderer;
typedef struct _GstElement GstElement;
GstElement *flutter_gl_texture_sink_new(struct texture *texture, struct gl_renderer *renderer);

#endif
