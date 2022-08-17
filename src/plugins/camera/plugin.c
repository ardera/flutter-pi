#define _GNU_SOURCE

#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdio.h>
#include <inttypes.h>

#include <flutter-pi.h>
#include <collection.h>
#include <pluginregistry.h>
#include <platformchannel.h>
#include <texture_registry.h>
#include <notifier_listener.h>
#include <plugins/gstreamer_video_player.h>

FILE_DESCR("camera plugin")

static struct plugin {
    struct flutterpi *flutterpi;
    bool initialized;
} plugin;

/// The direction the camera is facing.
enum camera_lens_direction {
  /// Front facing camera (a user looking at the screen is seen by the camera).
  kFront_CameraLensDirection,

  /// Back facing camera (a user looking at the screen is not seen by the camera).
  kBack_CameraLensDirection,

  /// External camera which may not be mounted to the device.
  kExternal_CameraLensDirection,
};

/// Group of image formats that are comparable across Android and iOS platforms.
enum image_format_group {
  /// The image format does not fit into any specific group.
  kUnknown_ImageFormatGroup,

  /// Multi-plane YUV 420 format.
  ///
  /// This format is a generic YCbCr format, capable of describing any 4:2:0
  /// chroma-subsampled planar or semiplanar buffer (but not fully interleaved),
  /// with 8 bits per color sample.
  ///
  /// On Android, this is `android.graphics.ImageFormat.YUV_420_888`. See
  /// https://developer.android.com/reference/android/graphics/ImageFormat.html#YUV_420_888
  ///
  /// On iOS, this is `kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange`. See
  /// https://developer.apple.com/documentation/corevideo/1563591-pixel_format_identifiers/kcvpixelformattype_420ypcbcr8biplanarvideorange?language=objc
  kYUV420_ImageFormatGroup,

  /// 32-bit BGRA.
  ///
  /// On iOS, this is `kCVPixelFormatType_32BGRA`. See
  /// https://developer.apple.com/documentation/corevideo/1563591-pixel_format_identifiers/kcvpixelformattype_32bgra?language=objc
  kBGRA8888_ImageFormatGroup,

  /// 32-big RGB image encoded into JPEG bytes.
  ///
  /// On Android, this is `android.graphics.ImageFormat.JPEG`. See
  /// https://developer.android.com/reference/android/graphics/ImageFormat#JPEG
  kJPEG_ImageFormatGroup,
};

/// The possible flash modes that can be set for a camera
enum flash_mode {
    /// Do not use the flash when taking a picture.
    kOff_FlashMode,

    /// Let the device decide whether to flash the camera when taking a picture.
    kAuto_FlashMode,

    /// Always use the flash when taking a picture.
    kAlways_FlashMode,

    /// Always use the flash when taking a picture.
    kTorch_FlashMode
};

/// The possible focus modes that can be set for a camera.
enum focus_mode {
    /// Automatically determine focus settings.
    kAuto_FocusMode,

    /// Lock the currently determined focus settings.
    kLocked_FocusMode
};

/// The possible exposure modes that can be set for a camera.
enum exposure_mode {
  /// Automatically determine exposure settings.
  kAuto_ExposureMode,

  /// Lock the currently determined exposure settings.
  kLocked_ExposureMode
};

/// Affect the quality of video recording and image capture:
///
/// If a preset is not available on the camera being used a preset of lower quality will be selected automatically.
enum resolution_present {
  /// 352x288 on iOS, 240p (320x240) on Android and Web
  kLow_ResolutionPreset,

  /// 480p (640x480 on iOS, 720x480 on Android and Web)
  kMedium_ResolutionPreset,

  /// 720p (1280x720)
  kHigh_ResolutionPreset,

  /// 1080p (1920x1080)
  kVeryHigh_ResolutionPreset,

  /// 2160p (3840x2160 on Android and iOS, 4096x2160 on Web)
  kUltraHigh_ResolutionPreset,

  /// The highest resolution available.
  kMax_ResolutionPreset
};

#define CAMERA_DEVICE_METHOD_CHANNEL "flutter.io/cameraPlugin/device"
#define CAMERA_METHOD_CHANNEL "plugins.flutter.io/camera"

static int on_get_available_cameras(const struct std_value *arg, FlutterPlatformMessageResponseHandle *responsehandle) {
    (void) arg;
    (void) responsehandle;

    // See: https://github.com/flutter/plugins/blob/main/packages/camera/camera_platform_interface/lib/src/method_channel/method_channel_camera.dart#L64-L84
    UNIMPLEMENTED();
    
    return 0;
}

static int on_create_camera(const struct std_value *arg, FlutterPlatformMessageResponseHandle *responsehandle) {
    (void) arg;
    (void) responsehandle;

    // See: https://github.com/flutter/plugins/blob/main/packages/camera/camera_platform_interface/lib/src/method_channel/method_channel_camera.dart#L87-L106
    UNIMPLEMENTED();

    return 0;
}

static int on_initialize_camera(const struct std_value *arg, FlutterPlatformMessageResponseHandle *responsehandle) {
    (void) arg;
    (void) responsehandle;

    // See: https://github.com/flutter/plugins/blob/main/packages/camera/camera_platform_interface/lib/src/method_channel/method_channel_camera.dart#L108-L151
    UNIMPLEMENTED();

    return 0;
}

static int on_initialize_camera(const struct std_value *arg, FlutterPlatformMessageResponseHandle *responsehandle) {
    (void) arg;
    (void) responsehandle;

    // See: https://github.com/flutter/plugins/blob/main/packages/camera/camera_platform_interface/lib/src/method_channel/method_channel_camera.dart#L108-L151
    UNIMPLEMENTED();

    return 0;
}

static int on_lock_capture_orientation(const struct std_value *arg, FlutterPlatformMessageResponseHandle *responsehandle) {
    (void) arg;
    (void) responsehandle;

    // See: https://github.com/flutter/plugins/blob/main/packages/camera/camera_platform_interface/lib/src/method_channel/method_channel_camera.dart#L198-L210
    UNIMPLEMENTED();

    return 0;
}

static int on_unlock_capture_orientation(const struct std_value *arg, FlutterPlatformMessageResponseHandle *responsehandle) {
    (void) arg;
    (void) responsehandle;

    // See: https://github.com/flutter/plugins/blob/main/packages/camera/camera_platform_interface/lib/src/method_channel/method_channel_camera.dart#L213-L218
    UNIMPLEMENTED();

    return 0;
}

static int on_take_picture(const struct std_value *arg, FlutterPlatformMessageResponseHandle *responsehandle) {
    (void) arg;
    (void) responsehandle;

    // See: https://github.com/flutter/plugins/blob/main/packages/camera/camera_platform_interface/lib/src/method_channel/method_channel_camera.dart#L220-L235
    UNIMPLEMENTED();

    return 0;
}

static int on_prepare_for_video_recording(const struct std_value *arg, FlutterPlatformMessageResponseHandle *responsehandle) {
    (void) arg;
    (void) responsehandle;

    // See: https://github.com/flutter/plugins/blob/main/packages/camera/camera_platform_interface/lib/src/method_channel/method_channel_camera.dart#L237-L239
    UNIMPLEMENTED();

    return 0;
}

static int on_start_video_recording(const struct std_value *arg, FlutterPlatformMessageResponseHandle *responsehandle) {
    (void) arg;
    (void) responsehandle;

    // See: https://github.com/flutter/plugins/blob/main/packages/camera/camera_platform_interface/lib/src/method_channel/method_channel_camera.dart#L241-L251
    UNIMPLEMENTED();

    return 0;
}

static int on_stop_video_recording(const struct std_value *arg, FlutterPlatformMessageResponseHandle *responsehandle) {
    (void) arg;
    (void) responsehandle;

    // See: https://github.com/flutter/plugins/blob/main/packages/camera/camera_platform_interface/lib/src/method_channel/method_channel_camera.dart#L253-L268
    UNIMPLEMENTED();

    return 0;
}

static int on_pause_video_recording(const struct std_value *arg, FlutterPlatformMessageResponseHandle *responsehandle) {
    (void) arg;
    (void) responsehandle;

    // See: https://github.com/flutter/plugins/blob/main/packages/camera/camera_platform_interface/lib/src/method_channel/method_channel_camera.dart#L270-L274
    UNIMPLEMENTED();

    return 0;
}

static int on_resume_video_recording(const struct std_value *arg, FlutterPlatformMessageResponseHandle *responsehandle) {
    (void) arg;
    (void) responsehandle;

    // See: https://github.com/flutter/plugins/blob/main/packages/camera/camera_platform_interface/lib/src/method_channel/method_channel_camera.dart#L276-L281
    UNIMPLEMENTED();

    return 0;
}

static int on_set_flash_mode(const struct std_value *arg, FlutterPlatformMessageResponseHandle *responsehandle) {
    (void) arg;
    (void) responsehandle;

    // See: https://github.com/flutter/plugins/blob/main/packages/camera/camera_platform_interface/lib/src/method_channel/method_channel_camera.dart#L329-L337
    UNIMPLEMENTED();

    return 0;
}

static int on_set_exposure_mode(const struct std_value *arg, FlutterPlatformMessageResponseHandle *responsehandle) {
    (void) arg;
    (void) responsehandle;

    // See: https://github.com/flutter/plugins/blob/main/packages/camera/camera_platform_interface/lib/src/method_channel/method_channel_camera.dart#L339-L347
    UNIMPLEMENTED();

    return 0;
}

static int on_set_exposure_point(const struct std_value *arg, FlutterPlatformMessageResponseHandle *responsehandle) {
    (void) arg;
    (void) responsehandle;

    // See: https://github.com/flutter/plugins/blob/main/packages/camera/camera_platform_interface/lib/src/method_channel/method_channel_camera.dart#L349-L363
    UNIMPLEMENTED();

    return 0;
}

static int on_get_min_exposure_offset(const struct std_value *arg, FlutterPlatformMessageResponseHandle *responsehandle) {
    (void) arg;
    (void) responsehandle;

    // See: https://github.com/flutter/plugins/blob/main/packages/camera/camera_platform_interface/lib/src/method_channel/method_channel_camera.dart#L365-L373
    UNIMPLEMENTED();

    return 0;
}

static int on_get_max_exposure_offset(const struct std_value *arg, FlutterPlatformMessageResponseHandle *responsehandle) {
    (void) arg;
    (void) responsehandle;

    // See: https://github.com/flutter/plugins/blob/main/packages/camera/camera_platform_interface/lib/src/method_channel/method_channel_camera.dart#L375-L383
    UNIMPLEMENTED();

    return 0;
}

static int on_get_exposure_offset_step_size(const struct std_value *arg, FlutterPlatformMessageResponseHandle *responsehandle) {
    (void) arg;
    (void) responsehandle;

    // See: https://github.com/flutter/plugins/blob/main/packages/camera/camera_platform_interface/lib/src/method_channel/method_channel_camera.dart#L385-L393
    UNIMPLEMENTED();

    return 0;
}

static int on_set_exposure_offset(const struct std_value *arg, FlutterPlatformMessageResponseHandle *responsehandle) {
    (void) arg;
    (void) responsehandle;

    // See: https://github.com/flutter/plugins/blob/main/packages/camera/camera_platform_interface/lib/src/method_channel/method_channel_camera.dart#L395-L406
    UNIMPLEMENTED();

    return 0;
}

static int on_set_focus_mode(const struct std_value *arg, FlutterPlatformMessageResponseHandle *responsehandle) {
    (void) arg;
    (void) responsehandle;

    // See: https://github.com/flutter/plugins/blob/main/packages/camera/camera_platform_interface/lib/src/method_channel/method_channel_camera.dart#L408-L416
    UNIMPLEMENTED();

    return 0;
}

static int on_set_focus_point(const struct std_value *arg, FlutterPlatformMessageResponseHandle *responsehandle) {
    (void) arg;
    (void) responsehandle;

    // See: https://github.com/flutter/plugins/blob/main/packages/camera/camera_platform_interface/lib/src/method_channel/method_channel_camera.dart#L418-L432
    UNIMPLEMENTED();

    return 0;
}

static int on_get_max_zoom_level(const struct std_value *arg, FlutterPlatformMessageResponseHandle *responsehandle) {
    (void) arg;
    (void) responsehandle;

    // See: https://github.com/flutter/plugins/blob/main/packages/camera/camera_platform_interface/lib/src/method_channel/method_channel_camera.dart#L434-L442
    UNIMPLEMENTED();

    return 0;
}

static int on_get_min_zoom_level(const struct std_value *arg, FlutterPlatformMessageResponseHandle *responsehandle) {
    (void) arg;
    (void) responsehandle;

    // See: https://github.com/flutter/plugins/blob/main/packages/camera/camera_platform_interface/lib/src/method_channel/method_channel_camera.dart#L444-L452
    UNIMPLEMENTED();

    return 0;
}

static int on_set_zoom_level(const struct std_value *arg, FlutterPlatformMessageResponseHandle *responsehandle) {
    (void) arg;
    (void) responsehandle;

    // See: https://github.com/flutter/plugins/blob/main/packages/camera/camera_platform_interface/lib/src/method_channel/method_channel_camera.dart#L454-L467
    UNIMPLEMENTED();

    return 0;
}

static int on_pause_preview(const struct std_value *arg, FlutterPlatformMessageResponseHandle *responsehandle) {
    (void) arg;
    (void) responsehandle;

    // See: https://github.com/flutter/plugins/blob/main/packages/camera/camera_platform_interface/lib/src/method_channel/method_channel_camera.dart#L469-L475
    UNIMPLEMENTED();

    return 0;
}

static int on_resume_preview(const struct std_value *arg, FlutterPlatformMessageResponseHandle *responsehandle) {
    (void) arg;
    (void) responsehandle;

    // See: https://github.com/flutter/plugins/blob/main/packages/camera/camera_platform_interface/lib/src/method_channel/method_channel_camera.dart#L477-L483
    UNIMPLEMENTED();

    return 0;
}

static int send_device_orientation_changed_event(enum device_orientation orientation) {
    (void) orientation;
    
    // See: https://github.com/flutter/plugins/blob/main/packages/camera/camera_platform_interface/lib/src/method_channel/method_channel_camera.dart#L530-L540
    UNIMPLEMENTED();

    return 0;
}

MAYBE_UNUSED static int send_initialized_event(
    int64_t camera_id,
    double preview_width,
    double preview_height,
    enum exposure_mode exposure_mode,
    bool exposure_point_supported,
    enum focus_mode focus_mode,
    bool focus_point_supported
) {
    (void) preview_width;
    (void) preview_height;
    (void) exposure_mode;
    (void) exposure_point_supported;
    (void) focus_mode;
    (void) focus_point_supported;

    // See: https://github.com/flutter/plugins/blob/main/packages/camera/camera_platform_interface/lib/src/method_channel/method_channel_camera.dart#L549-L559
    UNIMPLEMENTED();

    return 0;
}

MAYBE_UNUSED static int send_resolution_changed_event(
    int64_t camera_id,
    double capture_width,
    double capture_height
) {
    (void) camera_id;
    (void) capture_width;
    (void) capture_height;

    // See: https://github.com/flutter/plugins/blob/main/packages/camera/camera_platform_interface/lib/src/method_channel/method_channel_camera.dart#L560-L566
    UNIMPLEMENTED();

    return 0;
}

MAYBE_UNUSED static int send_camera_closing_event(int64_t camera_id) {
    (void) camera_id;

    // See: https://github.com/flutter/plugins/blob/main/packages/camera/camera_platform_interface/lib/src/method_channel/method_channel_camera.dart#L567-L571
    UNIMPLEMENTED();

    return 0;
}

MAYBE_UNUSED static int send_video_recorded_event(
    int64_t camera_id,
    const char *filepath,
    bool has_max_video_duration, int64_t max_video_duration_ms
) {
    (void) camera_id;
    (void) filepath;
    (void) has_max_video_duration;
    (void) max_video_duration_ms;

    // See: https://github.com/flutter/plugins/blob/main/packages/camera/camera_platform_interface/lib/src/method_channel/method_channel_camera.dart#L572-L581
    UNIMPLEMENTED();

    return 0;
}

MAYBE_UNUSED static int send_camera_error_event(
    int64_t camera_id,
    const char *description
) {
    (void) camera_id;
    (void) description;

    // See: https://github.com/flutter/plugins/blob/main/packages/camera/camera_platform_interface/lib/src/method_channel/method_channel_camera.dart#L582-L587
    UNIMPLEMENTED();

    return 0;
}

static int on_receive_method_channel(
    char *channel,
    struct platch_obj *object,
    FlutterPlatformMessageResponseHandle *responsehandle
) {
    const char *method;

    (void) channel;

    method = object->method;

    if (strcmp("availableCameras", method) == 0) {
        return on_get_available_cameras(&object->std_arg, responsehandle);
    } else if (strcmp("create", method) == 0) {
        return on_create_camera(&object->std_arg, responsehandle);
    } else if (strcmp("initialize", method) == 0) {
        return on_initialize_camera(&object->std_arg, responsehandle);
    } else if (strcmp("dispose", method) == 0) {
        return on_dispose_camera(&object->std_arg, responsehandle);
    } else if (strcmp("lockCaptureOrientation", method) == 0) {
        return on_lock_capture_orientation(&object->std_arg, responsehandle);
    } else if (strcmp("unlockCaptureOrientation", method) == 0) {
        return on_unlock_capture_orientation(&object->std_arg, responsehandle);
    } else if (strcmp("takePicture", method) == 0) {
        return on_take_picture(&object->std_arg, responsehandle);
    } else if (strcmp("prepareForVideoRecording", method) == 0) {
        return on_prepare_for_video_recording(&object->std_arg, responsehandle);
    } else if (strcmp("startVideoRecording", method) == 0) {
        return on_start_video_recording(&object->std_arg, responsehandle);
    } else if (strcmp("stopVideoRecording", method) == 0) {
        return on_stop_video_recording(&object->std_arg, responsehandle);
    } else if (strcmp("pauseVideoRecording", method) == 0) {
        return on_pause_video_recording(&object->std_arg, responsehandle);
    } else if (strcmp("resumeVideoRecording", method) == 0) {
        return on_resume_video_recording(&object->std_arg, responsehandle);
    } else if (strcmp("setFlashMode", method) == 0) {
        return on_set_flash_mode(&object->std_arg, responsehandle);
    } else if (strcmp("setExposureMode", method) == 0) {
        return on_set_exposure_mode(&object->std_arg, responsehandle);
    } else if (strcmp("setExposurePoint", method) == 0) {
        return on_set_exposure_point(&object->std_arg, responsehandle);
    } else if (strcmp("getMinExposureOffset", method) == 0) {
        return on_get_min_exposure_offset(&object->std_arg, responsehandle);
    } else if (strcmp("getMaxExposureOffset", method) == 0) {
        return on_get_max_exposure_offset(&object->std_arg, responsehandle);
    } else if (strcmp("getExposureOffsetStepSize", method) == 0) {
        return on_get_exposure_offset_step_size(&object->std_arg, responsehandle);
    } else if (strcmp("setExposureOffset", method) == 0) {
        return on_set_exposure_offset(&object->std_arg, responsehandle);
    } else if (strcmp("setFocusMode", method) == 0) {
        return on_set_focus_mode(&object->std_arg, responsehandle);
    } else if (strcmp("setFocusPoint", method) == 0) {
        return on_set_focus_point(&object->std_arg, responsehandle);
    } else if (strcmp("getMaxZoomLevel", method) == 0) {
        return on_get_max_zoom_level(&object->std_arg, responsehandle);
    } else if (strcmp("getMinZoomLevel", method) == 0) {
        return on_get_min_zoom_level(&object->std_arg, responsehandle);
    } else if (strcmp("setZoomLevel", method) == 0) {
        return on_set_zoom_level(&object->std_arg, responsehandle);
    } else if (strcmp("pausePreview", method) == 0) {
        return on_pause_preview(&object->std_arg, responsehandle);
    } else if (strcmp("resumePreview", method) == 0) {
        return on_resume_preview(&object->std_arg, responsehandle);
    } else {
        return platch_respond_not_implemented(responsehandle);
    }
}

static enum plugin_init_result plugin_init(struct flutterpi *flutterpi, void **userdata_out) {
    int ok;

    ok = plugin_registry_set_receiver(CAMERA_METHOD_CHANNEL, kStandardMethodCall, on_receive_method_channel);
    if (ok != 0) {
        LOG_ERROR("Couldn't set receiver for camera method channel \"" CAMERA_METHOD_CHANNEL "\".\n");
        return kError_PluginInitResult;
    }

    plugin.flutterpi = flutterpi;
    plugin.initialized = true;
    userdata_out = &plugin;

    return kInitialized_PluginInitResult;
}

static void plugin_deinit(struct flutterpi *flutterpi, void *userdata) {
    int ok;
    
    (void) flutterpi;
    (void) userdata;
    /// TODO: Deinitialize all cameras here

    plugin_registry_remove_receiver(CAMERA_METHOD_CHANNEL);
}

FLUTTERPI_PLUGIN(
    "camera",
    camera,
    plugin_init,
    plugin_deinit
)
