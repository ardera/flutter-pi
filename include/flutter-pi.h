#ifndef _FLUTTERPI_H
#define _FLUTTERPI_H

#define LOG_FLUTTERPI_ERROR(...) fprintf(stderr, "[flutter-pi] " __VA_ARGS__)

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
#include <keyboard.h>

#define LOAD_EGL_PROC(flutterpi_struct, name, full_name) \
    do { \
        (flutterpi_struct).egl.name = (void*) eglGetProcAddress(#full_name); \
        if ((flutterpi_struct).egl.name == NULL) { \
            fprintf(stderr, "[flutter-pi] FATAL: Could not resolve EGL procedure " #full_name "\n"); \
            return EINVAL; \
        } \
    } while (false)

#define LOAD_GL_PROC(flutterpi_struct, name, full_name) \
	do { \
		(flutterpi_struct).gl.name = (void*) eglGetProcAddress(#full_name); \
		if ((flutterpi_struct).gl.name == NULL) { \
			fprintf(stderr, "[flutter-pi] FATAL: Could not resolve GL procedure " #full_name "\n"); \
			return EINVAL; \
		} \
	} while (false)

enum device_orientation {
	kPortraitUp, kLandscapeLeft, kPortraitDown, kLandscapeRight
};

/// TODO: Use flutter_embedder.h proctable instead
struct libflutter_engine {
	FlutterEngineResult (*FlutterEngineCreateAOTData)(const FlutterEngineAOTDataSource* source, FlutterEngineAOTData* data_out);
	FlutterEngineResult (*FlutterEngineCollectAOTData)(FlutterEngineAOTData data);
	FlutterEngineResult (*FlutterEngineRun)(size_t version, const FlutterRendererConfig* config, const FlutterProjectArgs* args, void* user_data, FlutterEngine *engine_out);
	FlutterEngineResult (*FlutterEngineShutdown)(FlutterEngine engine);
	FlutterEngineResult (*FlutterEngineInitialize)(size_t version, const FlutterRendererConfig* config, const FlutterProjectArgs* args, void* user_data, FlutterEngine *engine_out);
	FlutterEngineResult (*FlutterEngineDeinitialize)(FlutterEngine engine);
	FlutterEngineResult (*FlutterEngineRunInitialized)(FlutterEngine engine);
	FlutterEngineResult (*FlutterEngineSendWindowMetricsEvent)(FlutterEngine engine, const FlutterWindowMetricsEvent* event);
	FlutterEngineResult (*FlutterEngineSendPointerEvent)(FlutterEngine engine, const FlutterPointerEvent* events, size_t events_count);
	FlutterEngineResult (*FlutterEngineSendPlatformMessage)(FlutterEngine engine, const FlutterPlatformMessage* message);
	FlutterEngineResult (*FlutterPlatformMessageCreateResponseHandle)(FlutterEngine engine, FlutterDataCallback data_callback, void* user_data, FlutterPlatformMessageResponseHandle** response_out);
	FlutterEngineResult (*FlutterPlatformMessageReleaseResponseHandle)(FlutterEngine engine, FlutterPlatformMessageResponseHandle* response);
	FlutterEngineResult (*FlutterEngineSendPlatformMessageResponse)(FlutterEngine engine, const FlutterPlatformMessageResponseHandle* handle, const uint8_t* data, size_t data_length);
	FlutterEngineResult (*__FlutterEngineFlushPendingTasksNow)();
	FlutterEngineResult (*FlutterEngineRegisterExternalTexture)(FlutterEngine engine, int64_t texture_identifier);
	FlutterEngineResult (*FlutterEngineUnregisterExternalTexture)(FlutterEngine engine, int64_t texture_identifier);
	FlutterEngineResult (*FlutterEngineMarkExternalTextureFrameAvailable)(FlutterEngine engine, int64_t texture_identifier);
	FlutterEngineResult (*FlutterEngineUpdateSemanticsEnabled)(FlutterEngine engine, bool enabled);
	FlutterEngineResult (*FlutterEngineUpdateAccessibilityFeatures)(FlutterEngine engine, FlutterAccessibilityFeature features);
	FlutterEngineResult (*FlutterEngineDispatchSemanticsAction)(FlutterEngine engine, uint64_t id, FlutterSemanticsAction action, const uint8_t* data, size_t data_length);
	FlutterEngineResult (*FlutterEngineOnVsync)(FlutterEngine engine, intptr_t baton, uint64_t frame_start_time_nanos, uint64_t frame_target_time_nanos);
	FlutterEngineResult (*FlutterEngineReloadSystemFonts)(FlutterEngine engine);
	void (*FlutterEngineTraceEventDurationBegin)(const char* name);
	void (*FlutterEngineTraceEventDurationEnd)(const char* name);
	void (*FlutterEngineTraceEventInstant)(const char* name);
	FlutterEngineResult (*FlutterEnginePostRenderThreadTask)(FlutterEngine engine, VoidCallback callback, void* callback_data);
	uint64_t (*FlutterEngineGetCurrentTime)();
	FlutterEngineResult (*FlutterEngineRunTask)(FlutterEngine engine, const FlutterTask* task);
	FlutterEngineResult (*FlutterEngineUpdateLocales)(FlutterEngine engine, const FlutterLocale** locales, size_t locales_count);
	bool (*FlutterEngineRunsAOTCompiledDartCode)(void);
	FlutterEngineResult (*FlutterEnginePostDartObject)(FlutterEngine engine, FlutterEngineDartPort port, const FlutterEngineDartObject* object);
	FlutterEngineResult (*FlutterEngineNotifyLowMemoryWarning)(FlutterEngine engine);
	FlutterEngineResult (*FlutterEnginePostCallbackOnAllNativeThreads)(FlutterEngine engine, FlutterNativeThreadCallback callback, void* user_data);
	FlutterEngineResult (*FlutterEngineNotifyDisplayUpdate)(FlutterEngine engine, FlutterEngineDisplaysUpdateType update_type, const FlutterEngineDisplay* displays, size_t display_count);
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

/**
 * A flutter transformation that rotates any coords around the z-axis, counter-clockwise.
 */
#define FLUTTER_ROTZ_TRANSFORMATION(deg) ((FlutterTransformation) \
	{.scaleX = cos(((double) (deg))/180.0*M_PI), .skewX  = -sin(((double) (deg))/180.0*M_PI), .transX = 0, \
	 .skewY  = sin(((double) (deg))/180.0*M_PI), .scaleY = cos(((double) (deg))/180.0*M_PI),  .transY = 0, \
	 .pers0  = 0,                                .pers1  = 0,                                 .pers2  = 1})

/**
 * A transformation that is the result of multiplying a with b.
 */
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

/**
 * A transformation that is the result of adding a with b.
 */
#define FLUTTER_ADDED_TRANSFORMATIONS(a, b) ((FlutterTransformation) \
	{.scaleX = a.scaleX + b.scaleX, .skewX  = a.skewX  + b.skewX,  .transX = a.transX + b.transX, \
	 .skewY  = a.skewY  + b.skewY,  .scaleY = a.scaleY + b.scaleY, .transY = a.transY + b.transY, \
	 .pers0  = a.pers0  + b.pers0,  .pers1  = a.pers1  + b.pers1,  .pers2  = a.pers2  + b.pers2 \
	})

/**
 * A transformation that is the result of transponating a.
 */
#define FLUTTER_TRANSPONATED_TRANSFORMATION(a) ((FlutterTransformation) \
	{.scaleX = a.scaleX, .skewX  = a.skewY,  .transX = a.pers0, \
	 .skewY  = a.skewX,  .scaleY = a.scaleY, .transY = a.pers1, \
	 .pers0  = a.transX, .pers1  = a.transY, .pers2  = a.pers2, \
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

#define LIBINPUT_EVENT_IS_KEYBOARD(event_type) (\
	((event_type) == LIBINPUT_EVENT_KEYBOARD_KEY))

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

#define FLUTTER_RUNTIME_MODE_IS_JIT(runtime_mode) ((runtime_mode) == kDebug)
#define FLUTTER_RUNTIME_MODE_IS_AOT(runtime_mode) ((runtime_mode) == kProfile || (runtime_mode) == kRelease)

struct plugin_registry;
struct texture_registry;

struct flutter_paths {
	char *app_bundle_path;
	char *asset_bundle_path;
	char *app_elf_path;
	char *icudtl_path;
	char *kernel_blob_path;
	char *flutter_engine_path;
	char *flutter_engine_dlopen_name;
	char *flutter_engine_dlopen_name_fallback;
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
		
		/// Used to lock the @ref temp_context, to be sure we only try to make it current on
		/// one thread.
		pthread_mutex_t temp_context_lock;
		
		/// An EGL context that's only made current to create new contexts,
		/// for example when some native code calls @ref flutterpi_create_egl_context
		/// to get a new context.
		EGLContext temp_context;
		
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
	sd_event_source *user_input_event_source;
	struct user_input *user_input;

	/// Locales
	struct locales *locales;
	
	/// flutter stuff
	struct {
		char *bundle_path;
		struct flutter_paths *paths;
		void *app_elf_handle;

		FlutterLocale **locales;
		size_t n_locales;

		int engine_argc;
		char **engine_argv;
		enum flutter_runtime_mode runtime_mode;
		struct libflutter_engine libflutter_engine;
		FlutterEngine engine;
	} flutter;
	
	/// main event loop
	pthread_t event_loop_thread;
	pthread_mutex_t event_loop_mutex;
	sd_event *event_loop;
	int wakeup_event_loop_fd;

	/// flutter-pi internal stuff
	struct plugin_registry *plugin_registry;
	struct texture_registry *texture_registry;
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

int flutterpi_fill_view_properties(
	bool has_orientation,
	enum device_orientation orientation,
	bool has_rotation,
	int rotation
);

int flutterpi_post_platform_task(
	int (*callback)(void *userdata),
	void *userdata
);

int flutterpi_post_platform_task_with_time(
	int (*callback)(void *userdata),
	void *userdata,
	uint64_t target_time_usec
);

int flutterpi_sd_event_add_io(
	sd_event_source **source_out,
	int fd,
	uint32_t events,
	sd_event_io_handler_t callback,
	void *userdata
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

struct texture_registry *flutterpi_get_texture_registry(
	struct flutterpi *flutterpi
);

struct texture *flutterpi_create_texture(struct flutterpi *flutterpi);

const char *flutterpi_get_asset_bundle_path(
	struct flutterpi *flutterpi
);

int flutterpi_schedule_exit(void);

struct gbm_device *flutterpi_get_gbm_device(struct flutterpi *flutterpi);

EGLDisplay flutterpi_get_egl_display(struct flutterpi *flutterpi);

EGLContext flutterpi_create_egl_context(struct flutterpi *flutterpi);

void flutterpi_trace_event_instant(struct flutterpi *flutterpi, const char *name);

void flutterpi_trace_event_begin(struct flutterpi *flutterpi, const char *name);

void flutterpi_trace_event_end(struct flutterpi *flutterpi, const char *name);

#endif