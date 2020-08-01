#ifndef _FLUTTERPI_H
#define _FLUTTERPI_H

#include <limits.h>
#include <linux/input.h>
#include <stdbool.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h> 
#include <stdio.h> 
#include <glob.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <libinput.h>
#include <systemd/sd-event.h>
#include <EGL/egl.h>
//#define  EGL_EGLEXT_PROTOTYPES
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
//#define  GL_GLEXT_PROTOTYPES
#include <GLES2/gl2ext.h>
#include <flutter_embedder.h>

#include <modesetting.h>
#include <collection.h>

#define LOAD_EGL_PROC(flutterpi_struct, name) \
    do { \
		char proc[256]; \
		snprintf(proc, 256, "%s", "egl" #name); \
		proc[3] = toupper(proc[3]); \
        (flutterpi_struct).egl.name = (void*) eglGetProcAddress(proc); \
        if ((flutterpi_struct).egl.name == NULL) { \
            fprintf(stderr, "[flutter-pi] FATAL: Could not resolve EGL procedure " #name "\n"); \
            return EINVAL; \
        } \
    } while (false)

#define LOAD_GL_PROC(flutterpi_struct, name) \
	do { \
		char proc_name[256]; \
		snprintf(proc_name, 256, "gl" #name); \
		proc_name[2] = toupper(proc_name[2]); \
		(flutterpi_struct).gl.name = (void*) eglGetProcAddress(proc_name); \
		if ((flutterpi_struct).gl.name == NULL) { \
			fprintf(stderr, "[flutter-pi] FATAL: Could not resolve GL procedure " #name "\n"); \
			return EINVAL; \
		} \
	} while (false)

enum device_orientation {
	kPortraitUp, kLandscapeLeft, kPortraitDown, kLandscapeRight
};

#define ANGLE_FROM_ORIENTATION(o) \
	((o) == kPortraitUp ? 0 : \
	 (o) == kLandscapeLeft ? 90 : \
	 (o) == kPortraitDown ? 180 : \
	 (o) == kLandscapeRight ? 270 : 0)
	
#define ANGLE_BETWEEN_ORIENTATIONS(o_start, o_end) \
	(ANGLE_FROM_ORIENTATION(o_end) \
	- ANGLE_FROM_ORIENTATION(o_start) \
	+ (ANGLE_FROM_ORIENTATION(o_start) > ANGLE_FROM_ORIENTATION(o_end) ? 360 : 0))

#define FLUTTER_TRANSLATION_TRANSFORMATION(translate_x, translate_y) ((FlutterTransformation) \
	{.scaleX = 1, .skewX  = 0, .transX = translate_x, \
	 .skewY  = 0, .scaleY = 1, .transY = translate_y, \
	 .pers0  = 0, .pers1  = 0, .pers2  = 1})

#define FLUTTER_ROTX_TRANSFORMATION(deg) ((FlutterTransformation) \
	{.scaleX = 1, .skewX  = 0,                                .transX = 0, \
	 .skewY  = 0, .scaleY = cos(((double) (deg))/180.0*M_PI), .transY = -sin(((double) (deg))/180.0*M_PI), \
	 .pers0  = 0, .pers1  = sin(((double) (deg))/180.0*M_PI), .pers2  = cos(((double) (deg))/180.0*M_PI)})

#define FLUTTER_ROTY_TRANSFORMATION(deg) ((FlutterTransformation) \
	{.scaleX = cos(((double) (deg))/180.0*M_PI),  .skewX  = 0, .transX = sin(((double) (deg))/180.0*M_PI), \
	 .skewY  = 0,                                 .scaleY = 1, .transY = 0, \
	 .pers0  = -sin(((double) (deg))/180.0*M_PI), .pers1  = 0, .pers2  = cos(((double) (deg))/180.0*M_PI)})

#define FLUTTER_ROTZ_TRANSFORMATION(deg) ((FlutterTransformation) \
	{.scaleX = cos(((double) (deg))/180.0*M_PI), .skewX  = -sin(((double) (deg))/180.0*M_PI), .transX = 0, \
	 .skewY  = sin(((double) (deg))/180.0*M_PI), .scaleY = cos(((double) (deg))/180.0*M_PI),  .transY = 0, \
	 .pers0  = 0,                                .pers1  = 0,                                 .pers2  = 1})

#define FLUTTER_MULTIPLIED_TRANSFORMATIONS(a, b) ((FlutterTransformation) \
	{.scaleX = a.scaleX * b.scaleX + a.skewX  * b.skewY  + a.transX * b.pers0, \
	 .skewX  = a.scaleX * b.skewX  + a.skewX  * b.scaleY + a.transX * b.pers1, \
	 .transX = a.scaleX * b.transX + a.skewX  * b.transY + a.transX * b.pers2, \
	 .skewY  = a.skewY  * b.scaleX + a.scaleY * b.skewY  + a.transY * b.pers0, \
	 .scaleY = a.skewY  * b.skewX  + a.scaleY * b.scaleY + a.transY * b.pers1, \
	 .transY = a.skewY  * b.transX + a.scaleY * b.transY + a.transY * b.pers2, \
	 .pers0  = a.pers0  * b.scaleX + a.pers1  * b.skewY  + a.pers2  * b.pers0, \
	 .pers1  = a.pers0  * b.skewX  + a.pers1  * b.scaleY + a.pers2  * b.pers1, \
	 .pers2  = a.pers0  * b.transX + a.pers1  * b.transY + a.pers2  * b.pers2})

#define FLUTTER_ADDED_TRANSFORMATIONS(a, b) ((FlutterTransformation) \
	{.scaleX = a.scaleX + b.scaleX, .skewX  = a.skewX  + b.skewX,  .transX = a.transX + b.transX, \
	 .skewY  = a.skewY  + b.skewY,  .scaleY = a.scaleY + b.scaleY, .transY = a.transY + b.transY, \
	 .pers0  = a.pers0  + b.pers0,  .pers1  = a.pers1  + b.pers1,  .pers2  = a.pers2  + b.pers2 \
	})

static inline void apply_flutter_transformation(
	const FlutterTransformation t,
	double *px,
	double *py
) {
	double x = px != NULL ? *px : 0;
	double y = py != NULL ? *py : 0;

	if (px != NULL) {
		*px = t.scaleX*x + t.skewX*y + t.transX;
	}

	if (py != NULL) {
		*py = t.skewY*x + t.scaleY*y + t.transY;
	}
}

#define FLUTTER_RESULT_TO_STRING(result) \
	((result) == kSuccess ? "Success." : \
	 (result) == kInvalidLibraryVersion ? "Invalid library version." : \
	 (result) == kInvalidArguments ? "Invalid arguments." : \
	 (result) == kInternalInconsistency ? "Internal inconsistency." : "(?)")

#define LIBINPUT_EVENT_IS_TOUCH(event_type) (\
	((event_type) == LIBINPUT_EVENT_TOUCH_DOWN) || \
	((event_type) == LIBINPUT_EVENT_TOUCH_UP) || \
	((event_type) == LIBINPUT_EVENT_TOUCH_MOTION) || \
	((event_type) == LIBINPUT_EVENT_TOUCH_CANCEL) || \
	((event_type) == LIBINPUT_EVENT_TOUCH_FRAME))

#define LIBINPUT_EVENT_IS_POINTER(event_type) (\
	((event_type) == LIBINPUT_EVENT_POINTER_MOTION) || \
	((event_type) == LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE) || \
	((event_type) == LIBINPUT_EVENT_POINTER_BUTTON) || \
	((event_type) == LIBINPUT_EVENT_POINTER_AXIS))

enum frame_state {
	kFramePending,
	kFrameRendering,
	kFrameRendered
};

struct frame {
	/// The current state of the frame.
	/// - Pending, when the frame was requested using the FlutterProjectArgs' vsync_callback.
	/// - Rendering, when the baton was returned to the engine
	/// - Rendered, when the frame has been / is visible on the display.
	enum frame_state state;

	/// The baton to be returned to the flutter engine when the frame can be rendered.
	intptr_t baton;
};

struct compositor;

enum flutter_runtime_mode {
	kDebug, kProfile, kRelease
};

struct flutterpi {
	/// graphics stuff
	struct {
		struct drmdev *drmdev;
		drmEventContext evctx;
		sd_event_source *drm_pageflip_event_source;
		bool platform_supports_get_sequence_ioctl;
	} drm;

	struct {
		struct gbm_device  *device;
		struct gbm_surface *surface;
		uint32_t 			format;
		uint64_t			modifier;
	} gbm;

	struct {
		EGLDisplay display;
		EGLConfig  config;
		EGLContext root_context;
		EGLContext flutter_render_context;
		EGLContext flutter_resource_uploading_context;
		EGLContext compositor_context;
		EGLSurface surface;

		char      *renderer;

		PFNEGLGETPLATFORMDISPLAYEXTPROC getPlatformDisplay;
		PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC createPlatformWindowSurface;
		PFNEGLCREATEPLATFORMPIXMAPSURFACEEXTPROC createPlatformPixmapSurface;
		PFNEGLCREATEDRMIMAGEMESAPROC createDRMImageMESA;
		PFNEGLEXPORTDRMIMAGEMESAPROC exportDRMImageMESA;
	} egl;

	struct  {
		PFNGLEGLIMAGETARGETTEXTURE2DOESPROC EGLImageTargetTexture2DOES;
		PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC EGLImageTargetRenderbufferStorageOES;
	} gl;

	struct {
		/// width & height of the display in pixels.
		int width, height;

		/// physical width & height of the display in millimeters
		/// the physical size can only be queried for HDMI displays (and even then, most displays will
		///   probably return bogus values like 160mm x 90mm).
		/// for DSI displays, the physical size of the official 7-inch display will be set in init_display.
		/// init_display will only update width_mm and height_mm if they are set to zero, allowing you
		///   to hardcode values for you individual display.
		int width_mm, height_mm;

		int refresh_rate;

		/// The pixel ratio used by flutter.
		/// This is computed inside init_display using width_mm and height_mm.
		/// flutter only accepts pixel ratios >= 1.0
		double pixel_ratio;
	} display;

	struct {
		/// This is false when the value in the "orientation" field is
		/// unset. (Since we can't just do orientation = null)
		bool has_orientation;

		/// The current device orientation.
		/// The initial device orientation is based on the width & height data from drm,
		/// or given as command line arguments.
		enum device_orientation orientation;

		bool has_rotation;

		/// The angle between the initial device orientation and the current device orientation in degrees.
		/// (applied as a rotation to the flutter window in transformation_callback, and also
		/// is used to determine if width/height should be swapped when sending a WindowMetrics event to flutter)
		int rotation;
		
		/// width & height of the flutter view. These are the dimensions send to flutter using
		/// [FlutterEngineSendWindowMetricsEvent]. (So, for example, with rotation == 90, these
		/// dimensions are swapped compared to the display dimensions)
		int width, height;

		int width_mm, height_mm;
		
		/// Used by flutter to transform the flutter view to fill the display.
		/// Matrix that transforms flutter view coordinates to display coordinates.
		FlutterTransformation view_to_display_transform;

		/// Used by the touch input to transform raw display coordinates into flutter view coordinates.
		/// Matrix that transforms display coordinates into flutter view coordinates
		FlutterTransformation display_to_view_transform;
	} view;

	struct concurrent_queue frame_queue;

	struct compositor *compositor;

	/// IO
	struct {
		bool use_paths;
		glob_t input_devices_glob;
		struct udev *udev;
		struct libinput *libinput;
		sd_event_source *libinput_event_source;
		sd_event_source *stdin_event_source;
		int64_t next_unused_flutter_device_id;
	} input;
	
	/// flutter stuff
	struct {
		char *asset_bundle_path;
		char *kernel_blob_path;
		char *app_elf_path;
		void *app_elf_handle;
		char *icu_data_path;

		int engine_argc;
		char **engine_argv;
		enum flutter_runtime_mode runtime_mode;
		FlutterEngine engine;
	} flutter;
	
	/// main event loop
	pthread_t event_loop_thread;
	pthread_mutex_t event_loop_mutex;
	sd_event *event_loop;
	int wakeup_event_loop_fd;

	/// flutter-pi internal stuff
	struct plugin_registry *plugin_registry;
};

struct platform_task {
	int (*callback)(void *userdata);
	void *userdata;
};

struct platform_message {
	bool is_response;
	union {
		FlutterPlatformMessageResponseHandle *target_handle;
		struct {
			char *target_channel;
			FlutterPlatformMessageResponseHandle *response_handle;
		};
	};
	uint8_t *message;
	size_t message_size;
};

extern struct flutterpi flutterpi;

struct input_device_data {
	int64_t flutter_device_id_offset;
	double x, y;
	int64_t buttons;
	uint64_t timestamp;
};

int flutterpi_fill_view_properties(
	bool has_orientation,
	enum device_orientation orientation,
	bool has_rotation,
	int rotation
);

int flutterpi_send_platform_message(
	const char *channel,
	const uint8_t *restrict message,
	size_t message_size,
	FlutterPlatformMessageResponseHandle *responsehandle
);

int flutterpi_respond_to_platform_message(
	FlutterPlatformMessageResponseHandle *handle,
	const uint8_t *restrict message,
	size_t message_size
);

#endif