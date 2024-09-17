#ifndef _FLUTTERPI_SRC_WINDOW_PRIVATE_H
#define _FLUTTERPI_SRC_WINDOW_PRIVATE_H

#define _GNU_SOURCE
#include <errno.h>
#include <stdlib.h>

#include <pthread.h>

#include <flutter_embedder.h>

#include "compositor_ng.h"
#include "cursor.h"
#include "flutter-pi.h"
#include "frame_scheduler.h"
#include "kms/req_builder.h"
#include "kms/resources.h"
#include "render_surface.h"
#include "surface.h"
#include "tracer.h"
#include "util/collection.h"
#include "util/logging.h"
#include "util/refcounting.h"
#include "window.h"

#include "config.h"

#ifdef HAVE_EGL_GLES2
    #include "egl_gbm_render_surface.h"
    #include "gl_renderer.h"
#endif

#ifdef HAVE_VULKAN
    #include "vk_gbm_render_surface.h"
    #include "vk_renderer.h"
#endif

struct user_input_device;

struct window_ops {
    void (*deinit)(struct window *window);

    int (*push_composition)(struct window *window, struct fl_layer_composition *composition);
    struct render_surface *(*get_render_surface)(struct window *window, struct vec2i size);

#ifdef HAVE_EGL_GLES2
    bool (*has_egl_surface)(struct window *window);
    EGLSurface (*get_egl_surface)(struct window *window);
#endif

    int (*set_cursor_locked)(
        struct window *window,
        bool has_enabled,
        bool enabled,
        bool has_kind,
        enum pointer_kind kind,
        bool has_pos,
        struct vec2i pos
    );

    input_device_match_score_t (*match_input_device)(struct window *window, struct user_input_device *device);
};

struct gl_renderer;
struct vk_renderer;

struct window {
    pthread_mutex_t lock;
    refcount_t n_refs;

    /**
     * @brief Event tracing interface.
     *
     * Used to report timing information to the dart observatory.
     *
     */
    struct tracer *tracer;

    /**
     * @brief Manages the frame scheduling for this window.
     *
     */
    struct frame_scheduler *frame_scheduler;

    /**
     * @brief Refresh rate of the selected video mode / display.
     *
     */
    float refresh_rate;

    /**
     * @brief Flutter device pixel ratio (in the horizontal axis). Number of physical pixels per logical pixel.
     *
     * There are always 38 logical pixels per cm, or 96 per inch. This is roughly equivalent to DPI / 100.
     * A device pixel ratio of 1.0 is roughly a dpi of 96, which is the most common dpi for full-hd desktop displays.
     * To calculate this, the physical dimensions of the display are required. If there are no physical dimensions,
     * this will default to 1.0.
     */
    float pixel_ratio;

    /**
     * @brief Whether we have physical screen dimensions and @ref width_mm and @ref height_mm contain usable values.
     *
     */
    bool has_dimensions;

    /**
     * @brief Width, height of the display in millimeters.
     *
     */
    int width_mm, height_mm;

    /**
     * @brief The size of the view, as reported to flutter, in pixels.
     *
     * If no rotation and scaling is applied, this probably equals the display size.
     * For example, if flutter-pi should render at 1/2 the resolution of a full-hd display, this would be
     * 960x540 and the display size 1920x1080.
     */
    struct vec2f view_size;

    /**
     * @brief The actual size of the view on the display, pixels.
     *
     */
    struct vec2f display_size;

    /**
     * @brief The rotation we should apply to the flutter layers to present them on screen.
     */
    drm_plane_transform_t rotation;

    /**
     * @brief The current device orientation and the original (startup) device orientation.
     *
     * @ref original_orientation is kLandscapeLeft for displays that are more wide than high, and kPortraitUp for displays that are
     * more high than wide. Though this can also be anything else theoretically, if the user specifies weird combinations of rotation
     * and orientation via cmdline arguments.
     *
     * @ref orientation should always equal to rotating @ref original_orientation clock-wise by the angle in the @ref rotation field.
     */
    enum device_orientation orientation, original_orientation;

    /**
     * @brief Matrix for transforming display coordinates to view coordinates.
     *
     * For example for transforming pointer events (which are in the display coordinate space) to flutter coordinates.
     * Useful if for example flutter has specified a custom device orientation (for example kPortraitDown), in that case we of course
     * also need to transform the touch coords.
     *
     */
    struct mat3f display_to_view_transform;

    /**
     * @brief Matrix for transforming view coordinates to display coordinates.
     *
     * Can be used as a root surface transform, for fitting the flutter view into the desired display frame.
     *
     * Useful if for example flutter has specified a custom device orientation (for example kPortraitDown),
     * because we need to rotate the flutter view in that case.
     */
    struct mat3f view_to_display_transform;

    /**
     * @brief Matrix for transforming normalized device coordinates to view coordinates.
     *
     */
    struct mat3f ndc_to_view_transform;

    /**
     * @brief True if we should use a specific pixel format.
     *
     */
    bool has_forced_pixel_format;

    /**
     * @brief The forced pixel format if @ref has_forced_pixel_format is true.
     *
     */
    enum pixfmt forced_pixel_format;

    /**
     * @brief The current flutter layer composition that should be output on screen.
     *
     */
    struct fl_layer_composition *composition;

    /**
     * @brief The type of rendering that should be used. (gl, vk)
     *
     */
    enum renderer_type renderer_type;

    /**
     * @brief The OpenGL renderer if OpenGL rendering should be used.
     *
     */
    struct gl_renderer *gl_renderer;

    /**
     * @brief The Vulkan renderer if Vulkan rendering should be used.
     *
     */
    struct vk_renderer *vk_renderer;

    /**
     * @brief Our main render surface, if we have one yet.
     *
     * Otherwise a new one should be created using the render surface interface.
     *
     */
    struct render_surface *render_surface;

#ifdef HAVE_EGL_GLES2
    /**
     * @brief The EGLSurface of this window, if any.
     *
     * Should be EGL_NO_SURFACE if this window is not associated with any EGL surface.
     * This is really just a workaround because flutter doesn't support arbitrary EGL surfaces as render targets right now.
     * (Just one global EGLSurface)
     *
     */
    EGLSurface egl_surface;
#endif

    /**
     * @brief Whether this window currently shows a mouse cursor.
     *
     */
    bool cursor_enabled;

    /**
     * @brief The position of the mouse cursor.
     *
     */
    struct vec2f cursor_pos;

    struct window_ops ops;
};

int window_init(
    // clang-format off
    struct window *window,
    struct tracer *tracer,
    struct frame_scheduler *scheduler,
    bool has_rotation, drm_plane_transform_t rotation,
    bool has_orientation, enum device_orientation orientation,
    int width, int height,
    bool has_dimensions, int width_mm, int height_mm,
    float refresh_rate,
    bool has_forced_pixel_format, enum pixfmt forced_pixel_format,
    enum renderer_type renderer_type,
    struct gl_renderer *gl_renderer,
    struct vk_renderer *vk_renderer
    // clang-format on
);

void window_deinit(struct window *window);

#endif  // _FLUTTERPI_SRC_WINDOW_PRIVATE_H
