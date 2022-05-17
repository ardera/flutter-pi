#ifndef _FLUTTERPI_INCLUDE_PLUGINS_OMXPLAYER_VIDEO_PLUGIN_H
#define _FLUTTERPI_INCLUDE_PLUGINS_OMXPLAYER_VIDEO_PLUGIN_H

#include <collection.h>
#include <egl.h>
#include <gles.h>

enum format_hint {
    kNoFormatHint,
    kMpegDash_FormatHint,
    kHLS_FormatHint,
    kSS_FormatHint,
    kOther_FormatHint
};

enum buffering_mode {
    kStream,
    kDownload,
    kTimeshift,
    kLive
};

struct buffering_range {
    int64_t start_ms;
    int64_t stop_ms;
};

struct buffering_state {
    // The percentage that the buffer is filled.
    // If this is 100 playback will resume.
    int percent;

    // The buffering mode currently used by the pipeline.
    enum buffering_mode mode;

    // The average input / consumption speed in bytes per second.
    int avg_in, avg_out;

    // Time left till buffering finishes, in ms. 
    // 0 means not buffering right now.
    int64_t time_left_ms;

    // The ranges of already buffered video.
    // For the kDownload and kTimeshift buffering modes, this specifies the ranges
    // where efficient seeking is possible.
    // For the kStream and kLive buffering modes, this describes the oldest and
    // newest item in the buffer.
    int n_ranges;

    // Flexible array member.
    // For example, if n_ranges is 2, just allocate using 
    // `state = malloc(sizeof(struct buffering_state) + 2*sizeof(struct buffering_range))`
    // and we can use state->ranges[0] and so on. 
    // This is cool because we don't need to allocate two blocks of memory and we can just call
    // `free` once to free the whole thing.
    // More precisely, we don't need to define a new function we can give to value_notifier_init
    // as the value destructor, we can just use `free`.
    struct buffering_range ranges[];
};

#define BUFFERING_STATE_SIZE(n_ranges) (sizeof(struct buffering_state) + (n_ranges)*sizeof(struct buffering_range))

struct video_info;
struct gstplayer;
struct flutterpi;

/// Create a gstreamer video player that loads the video from a flutter asset.
///     @arg asset_path     The path of the asset inside the asset bundle.
///     @arg package_name   The name of the package containing the asset
///     @arg userdata       The userdata associated with this player
struct gstplayer *gstplayer_new_from_asset(
    struct flutterpi *flutterpi,
    const char *asset_path,
    const char *package_name,
    void *userdata
);

/// Create a gstreamer video player that loads the video from a network URI.
///     @arg uri          The URI to the video. (for example, http://, https://, rtmp://, rtsp://)
///     @arg format_hint  A hint to the format of the video. kNoFormatHint means there's no hint.
///     @arg userdata     The userdata associated with this player.
struct gstplayer *gstplayer_new_from_network(
    struct flutterpi *flutterpi,
    const char *uri,
    enum format_hint format_hint,
    void *userdata
);

/// Create a gstreamer video player that loads the video from a file URI.
///     @arg uri        The file:// URI to the video.
///     @arg userdata   The userdata associated with this player.
struct gstplayer *gstplayer_new_from_file(
    struct flutterpi *flutterpi,
    const char *uri,
    void *userdata
);

/// Destroy this gstreamer player instance and the resources
/// associated with it. (texture, gstreamer pipeline, etc)
///
/// Should be called on the flutterpi main/platform thread,
/// because otherwise destroying the gstreamer event bus listener
/// might be a race condition.
void gstplayer_destroy(struct gstplayer *player);

DECLARE_LOCK_OPS(gstplayer)

/// Set the generic userdata associated with this gstreamer player instance.
/// Overwrites the userdata set in the constructor and any userdata previously
/// set using @ref gstplayer_set_userdata_locked.
///     @arg userdata The new userdata that should be associated with this player.
void gstplayer_set_userdata_locked(struct gstplayer *player, void *userdata);

/// Get the userdata that was given to the constructor or was previously set using
/// @ref gstplayer_set_userdata_locked.
///     @returns userdata associated with this player.
void *gstplayer_get_userdata_locked(struct gstplayer *player);

/// Get the id of the flutter external texture that this player is rendering into.
int64_t gstplayer_get_texture_id(struct gstplayer *player);

//void gstplayer_set_info_callback(struct gstplayer *player, gstplayer_info_callback_t cb, void *userdata);

//void gstplayer_set_buffering_callback(struct gstplayer *player, gstplayer_buffering_callback_t callback, void *userdata);

/// Add a http header (consisting of a string key and value) to the list of http headers that
/// gstreamer will use when playing back from a HTTP/S URI.
/// This has no effect after @ref gstplayer_initialize was called.
void gstplayer_put_http_header(struct gstplayer *player, const char *key, const char *value);

/// Initializes the video playback, i.e. boots up the gstreamer pipeline, starts
/// buffering the video.
///     @returns 0 if initialization was successfull, errno-style error code if an error ocurred.
int gstplayer_initialize(struct gstplayer *player);

/// Get the video info. If the video info (format, size, etc) is already known, @arg callback will be called
/// synchronously, inside this call. If the video info is not known, @arg callback will be called on the flutter-pi
/// platform thread as soon as the info is known.
///     @returns The handle for the deferred callback.
//struct sd_event_source_generic *gstplayer_probe_video_info(struct gstplayer *player, gstplayer_info_callback_t callback, void *userdata);

/// Set the current playback state to "playing" if that's not the case already.
///     @returns 0 if initialization was successfull, errno-style error code if an error ocurred.
int gstplayer_play(struct gstplayer *player);

/// Sets the current playback state to "paused" if that's not the case already.
///     @returns 0 if initialization was successfull, errno-style error code if an error ocurred.
int gstplayer_pause(struct gstplayer *player);

/// Get the current playback position.
///     @returns Current playback position, in milliseconds from the beginning of the video.
int64_t gstplayer_get_position(struct gstplayer *player);

/// Set whether the video should loop.
///     @arg looping    Whether the video should start playing from the beginning when the
///                     end is reached.
int gstplayer_set_looping(struct gstplayer *player, bool looping);

/// Set the playback volume.
///     @arg volume     Desired volume as a value between 0 and 1.
int gstplayer_set_volume(struct gstplayer *player, double volume);

/// Seek to a specific position in the video.
///     @arg position            Position to seek to in milliseconds from the beginning of the video.
///     @arg nearest_keyframe    If true, seek to the nearest keyframe instead. Might be faster but less accurate.
int gstplayer_seek_to(struct gstplayer *player, int64_t position, bool nearest_keyframe);

/// Set the playback speed of the player.
///   1.0: normal playback speed
///   0.5: half playback speed
///   2.0: double playback speed
int gstplayer_set_playback_speed(struct gstplayer *player, double playback_speed);

int gstplayer_step_forward(struct gstplayer *player);

int gstplayer_step_backward(struct gstplayer *player);

/// @brief Get the value notifier for the video info. 
/// 
/// Gets notified with a value of type `struct video_info*` when the video info changes.
/// The listeners will be called on an internal gstreamer thread.
/// So you need to make sure you do the proper rethreading in the listener callback.
struct notifier *gstplayer_get_video_info_notifier(struct gstplayer *player);

/// @brief Get the value notifier for the buffering state.
///
/// Gets notified with a value of type `struct buffering_state*` when the buffering state changes. 
/// The listeners will be called on the main flutterpi platform thread.
struct notifier *gstplayer_get_buffering_state_notifier(struct gstplayer *player);

/// @brief Get the change notifier for errors.
///
/// Gets notified when an error happens. (Not yet implemented)
struct notifier *gstplayer_get_error_notifier(struct gstplayer *player);



struct video_frame;
struct gl_renderer;

struct frame_interface {
    struct gbm_device *gbm_device;
    EGLDisplay display;

    pthread_mutex_t context_lock;
    EGLContext context;
    PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR;
    PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES;

    bool supports_extended_imports;
    PFNEGLQUERYDMABUFFORMATSEXTPROC eglQueryDmaBufFormatsEXT;
    PFNEGLQUERYDMABUFMODIFIERSEXTPROC eglQueryDmaBufModifiersEXT;

    refcount_t n_refs;
};

struct frame_interface *frame_interface_new(struct gl_renderer *renderer);

DEFINE_INLINE_LOCK_OPS(frame_interface, context_lock)

DECLARE_REF_OPS(frame_interface)

typedef struct _GstVideoInfo GstVideoInfo;
typedef struct _GstVideoMeta GstVideoMeta;

struct video_info {
    int width, height;
    double fps;
    int64_t duration_ms;
    bool can_seek;
    int64_t seek_begin_ms, seek_end_ms;
};

struct frame_info {
    const GstVideoInfo *gst_info;
    uint32_t drm_format;
    EGLint egl_color_space;
};

struct _GstSample;

struct video_frame *frame_new(
    struct frame_interface *interface,
    const struct frame_info *meta,
    struct _GstSample *sample
);

void frame_destroy(struct video_frame *frame);

struct gl_texture_frame;

const struct gl_texture_frame *frame_get_gl_frame(struct video_frame *frame);

#endif