#ifndef DYLIB_DEPS_H
#define DYLIB_DEPS_H

#include <sys/types.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <flutter_embedder.h>

#include <flutter-pi.h>

typedef uint64_t (*flutter_engine_get_current_time_t)();

struct libflutter_engine {
	void *handle;
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
};

struct wl_display;
struct wl_resource;
struct libegl {
	EGLBoolean (EGLAPIENTRYP eglBindTexImage)(EGLDisplay dpy, EGLSurface surface, EGLint buffer);
	EGLBoolean (EGLAPIENTRYP eglReleaseTexImage)(EGLDisplay dpy, EGLSurface surface, EGLint buffer);
	EGLBoolean (EGLAPIENTRYP eglSurfaceAttrib)(EGLDisplay dpy, EGLSurface surface, EGLint attribute, EGLint value);
	EGLBoolean (EGLAPIENTRYP eglSwapInterval)(EGLDisplay dpy, EGLint interval);
	EGLBoolean (EGLAPIENTRYP eglBindAPI)(EGLenum api);
	EGLenum (EGLAPIENTRYP eglQueryAPI)(void);
	EGLSurface (EGLAPIENTRYP eglCreatePbufferFromClientBuffer)(EGLDisplay dpy, EGLenum buftype, EGLClientBuffer buffer, EGLConfig config, const EGLint *attrib_list);
	EGLBoolean (EGLAPIENTRYP eglReleaseThread)(void);
	EGLBoolean (EGLAPIENTRYP eglWaitClient)(void);
	EGLContext (EGLAPIENTRYP eglGetCurrentContext)(void);
	EGLSync (EGLAPIENTRYP eglCreateSync)(EGLDisplay dpy, EGLenum type, const EGLAttrib *attrib_list);
	EGLBoolean (EGLAPIENTRYP eglDestroySync)(EGLDisplay dpy, EGLSync sync);
	EGLint (EGLAPIENTRYP eglClientWaitSync)(EGLDisplay dpy, EGLSync sync, EGLint flags, EGLTime timeout);
	EGLBoolean (EGLAPIENTRYP eglGetSyncAttrib)(EGLDisplay dpy, EGLSync sync, EGLint attribute, EGLAttrib *value);
	EGLImage (EGLAPIENTRYP eglCreateImage)(EGLDisplay dpy, EGLContext ctx, EGLenum target, EGLClientBuffer buffer, const EGLAttrib *attrib_list);
	EGLBoolean (EGLAPIENTRYP eglDestroyImage)(EGLDisplay dpy, EGLImage image);
	EGLDisplay (EGLAPIENTRYP eglGetPlatformDisplay)(EGLenum platform, void *native_display, const EGLAttrib *attrib_list);
	EGLSurface (EGLAPIENTRYP eglCreatePlatformWindowSurface)(EGLDisplay dpy, EGLConfig config, void *native_window, const EGLAttrib *attrib_list);
	EGLSurface (EGLAPIENTRYP eglCreatePlatformPixmapSurface)(EGLDisplay dpy, EGLConfig config, void *native_pixmap, const EGLAttrib *attrib_list);
	EGLBoolean (EGLAPIENTRYP eglWaitSync)(EGLDisplay dpy, EGLSync sync, EGLint flags);
	PFNEGLCREATESYNC64KHRPROC eglCreateSync64KHR;
	EGLint (EGLAPIENTRYP eglDebugMessageControlKHR)(EGLDEBUGPROCKHR callback, const EGLAttrib *attrib_list);
	EGLBoolean (EGLAPIENTRYP eglQueryDebugKHR)(EGLint attribute, EGLAttrib *value);
	EGLint (EGLAPIENTRYP eglLabelObjectKHR)(EGLDisplay display, EGLenum objectType, EGLObjectKHR object, EGLLabelKHR label);
	EGLBoolean (EGLAPIENTRYP eglQueryDisplayAttribKHR)(EGLDisplay dpy, EGLint name, EGLAttrib *value);
	EGLSyncKHR (EGLAPIENTRYP eglCreateSyncKHR)(EGLDisplay dpy, EGLenum type, const EGLint *attrib_list);
	EGLBoolean (EGLAPIENTRYP eglDestroySyncKHR)(EGLDisplay dpy, EGLSyncKHR sync);
	EGLint (EGLAPIENTRYP eglClientWaitSyncKHR)(EGLDisplay dpy, EGLSyncKHR sync, EGLint flags, EGLTimeKHR timeout);
	EGLBoolean (EGLAPIENTRYP eglGetSyncAttribKHR)(EGLDisplay dpy, EGLSyncKHR sync, EGLint attribute, EGLint *value);
	EGLImageKHR (EGLAPIENTRYP eglCreateImageKHR)(EGLDisplay dpy, EGLContext ctx, EGLenum target, EGLClientBuffer buffer, const EGLint *attrib_list);
	EGLBoolean (EGLAPIENTRYP eglDestroyImageKHR)(EGLDisplay dpy, EGLImageKHR image);
	EGLBoolean (EGLAPIENTRYP eglLockSurfaceKHR)(EGLDisplay dpy, EGLSurface surface, const EGLint *attrib_list);
	EGLBoolean (EGLAPIENTRYP eglUnlockSurfaceKHR)(EGLDisplay dpy, EGLSurface surface);
	EGLBoolean (EGLAPIENTRYP eglQuerySurface64KHR)(EGLDisplay dpy, EGLSurface surface, EGLint attribute, EGLAttribKHR *value);
	EGLBoolean (EGLAPIENTRYP eglSetDamageRegionKHR)(EGLDisplay dpy, EGLSurface surface, EGLint *rects, EGLint n_rects);
	EGLBoolean (EGLAPIENTRYP eglSignalSyncKHR)(EGLDisplay dpy, EGLSyncKHR sync, EGLenum mode);
	EGLStreamKHR (EGLAPIENTRYP eglCreateStreamKHR)(EGLDisplay dpy, const EGLint *attrib_list);
	EGLBoolean (EGLAPIENTRYP eglDestroyStreamKHR)(EGLDisplay dpy, EGLStreamKHR stream);
	EGLBoolean (EGLAPIENTRYP eglStreamAttribKHR)(EGLDisplay dpy, EGLStreamKHR stream, EGLenum attribute, EGLint value);
	EGLBoolean (EGLAPIENTRYP eglQueryStreamKHR)(EGLDisplay dpy, EGLStreamKHR stream, EGLenum attribute, EGLint *value);
	EGLBoolean (EGLAPIENTRYP eglQueryStreamu64KHR)(EGLDisplay dpy, EGLStreamKHR stream, EGLenum attribute, EGLuint64KHR *value);
	EGLStreamKHR (EGLAPIENTRYP eglCreateStreamAttribKHR)(EGLDisplay dpy, const EGLAttrib *attrib_list);
	EGLBoolean (EGLAPIENTRYP eglSetStreamAttribKHR)(EGLDisplay dpy, EGLStreamKHR stream, EGLenum attribute, EGLAttrib value);
	EGLBoolean (EGLAPIENTRYP eglQueryStreamAttribKHR)(EGLDisplay dpy, EGLStreamKHR stream, EGLenum attribute, EGLAttrib *value);
	EGLBoolean (EGLAPIENTRYP eglStreamConsumerAcquireAttribKHR)(EGLDisplay dpy, EGLStreamKHR stream, const EGLAttrib *attrib_list);
	EGLBoolean (EGLAPIENTRYP eglStreamConsumerReleaseAttribKHR)(EGLDisplay dpy, EGLStreamKHR stream, const EGLAttrib *attrib_list);
	EGLBoolean (EGLAPIENTRYP eglStreamConsumerGLTextureExternalKHR)(EGLDisplay dpy, EGLStreamKHR stream);
	EGLBoolean (EGLAPIENTRYP eglStreamConsumerAcquireKHR)(EGLDisplay dpy, EGLStreamKHR stream);
	EGLBoolean (EGLAPIENTRYP eglStreamConsumerReleaseKHR)(EGLDisplay dpy, EGLStreamKHR stream);
	EGLNativeFileDescriptorKHR (EGLAPIENTRYP eglGetStreamFileDescriptorKHR)(EGLDisplay dpy, EGLStreamKHR stream);
	EGLStreamKHR (EGLAPIENTRYP eglCreateStreamFromFileDescriptorKHR)(EGLDisplay dpy, EGLNativeFileDescriptorKHR file_descriptor);
	EGLBoolean (EGLAPIENTRYP eglQueryStreamTimeKHR)(EGLDisplay dpy, EGLStreamKHR stream, EGLenum attribute, EGLTimeKHR *value);
	EGLSurface (EGLAPIENTRYP eglCreateStreamProducerSurfaceKHR)(EGLDisplay dpy, EGLConfig config, EGLStreamKHR stream, const EGLint *attrib_list);
	EGLBoolean (EGLAPIENTRYP eglSwapBuffersWithDamageKHR)(EGLDisplay dpy, EGLSurface surface, const EGLint *rects, EGLint n_rects);
	EGLint (EGLAPIENTRYP eglWaitSyncKHR)(EGLDisplay dpy, EGLSyncKHR sync, EGLint flags);
	void (EGLAPIENTRYP eglSetBlobCacheFuncsANDROID)(EGLDisplay dpy, EGLSetBlobFuncANDROID set, EGLGetBlobFuncANDROID get);
	EGLClientBuffer (EGLAPIENTRYP eglCreateNativeClientBufferANDROID)(const EGLint *attrib_list);
	EGLBoolean (EGLAPIENTRYP eglGetCompositorTimingSupportedANDROID)(EGLDisplay dpy, EGLSurface surface, EGLint name);
	EGLBoolean (EGLAPIENTRYP eglGetCompositorTimingANDROID)(EGLDisplay dpy, EGLSurface surface, EGLint numTimestamps,  const EGLint *names, EGLnsecsANDROID *values);
	EGLBoolean (EGLAPIENTRYP eglGetNextFrameIdANDROID)(EGLDisplay dpy, EGLSurface surface, EGLuint64KHR *frameId);
	EGLBoolean (EGLAPIENTRYP eglGetFrameTimestampSupportedANDROID)(EGLDisplay dpy, EGLSurface surface, EGLint timestamp);
	EGLBoolean (EGLAPIENTRYP eglGetFrameTimestampsANDROID)(EGLDisplay dpy, EGLSurface surface, EGLuint64KHR frameId, EGLint numTimestamps,  const EGLint *timestamps, EGLnsecsANDROID *values);
	EGLClientBuffer (EGLAPIENTRYP eglGetNativeClientBufferANDROID)(const struct AHardwareBuffer *buffer);
	EGLint (EGLAPIENTRYP eglDupNativeFenceFDANDROID)(EGLDisplay dpy, EGLSyncKHR sync);
	EGLBoolean (EGLAPIENTRYP eglPresentationTimeANDROID)(EGLDisplay dpy, EGLSurface surface, EGLnsecsANDROID time);
	EGLBoolean (EGLAPIENTRYP eglQuerySurfacePointerANGLE)(EGLDisplay dpy, EGLSurface surface, EGLint attribute, void **value);
	EGLBoolean (EGLAPIENTRYP eglClientSignalSyncEXT)(EGLDisplay dpy, EGLSync sync, const EGLAttrib *attrib_list);
	EGLBoolean (EGLAPIENTRYP eglCompositorSetContextListEXT)(const EGLint *external_ref_ids, EGLint num_entries);
	EGLBoolean (EGLAPIENTRYP eglCompositorSetContextAttributesEXT)(EGLint external_ref_id, const EGLint *context_attributes, EGLint num_entries);
	EGLBoolean (EGLAPIENTRYP eglCompositorSetWindowListEXT)(EGLint external_ref_id, const EGLint *external_win_ids, EGLint num_entries);
	EGLBoolean (EGLAPIENTRYP eglCompositorSetWindowAttributesEXT)(EGLint external_win_id, const EGLint *window_attributes, EGLint num_entries);
	EGLBoolean (EGLAPIENTRYP eglCompositorBindTexWindowEXT)(EGLint external_win_id);
	EGLBoolean (EGLAPIENTRYP eglCompositorSetSizeEXT)(EGLint external_win_id, EGLint width, EGLint height);
	EGLBoolean (EGLAPIENTRYP eglCompositorSwapPolicyEXT)(EGLint external_win_id, EGLint policy);
	EGLBoolean (EGLAPIENTRYP eglQueryDeviceAttribEXT)(EGLDeviceEXT device, EGLint attribute, EGLAttrib *value);
	const char *(EGLAPIENTRYP eglQueryDeviceStringEXT)(EGLDeviceEXT device, EGLint name);
	EGLBoolean (EGLAPIENTRYP eglQueryDevicesEXT)(EGLint max_devices, EGLDeviceEXT *devices, EGLint *num_devices);
	EGLBoolean (EGLAPIENTRYP eglQueryDisplayAttribEXT)(EGLDisplay dpy, EGLint attribute, EGLAttrib *value);
	EGLBoolean (EGLAPIENTRYP eglQueryDmaBufFormatsEXT)(EGLDisplay dpy, EGLint max_formats, EGLint *formats, EGLint *num_formats);
	EGLBoolean (EGLAPIENTRYP eglQueryDmaBufModifiersEXT)(EGLDisplay dpy, EGLint format, EGLint max_modifiers, EGLuint64KHR *modifiers, EGLBoolean *external_only, EGLint *num_modifiers);
	EGLBoolean (EGLAPIENTRYP eglGetOutputLayersEXT)(EGLDisplay dpy, const EGLAttrib *attrib_list, EGLOutputLayerEXT *layers, EGLint max_layers, EGLint *num_layers);
	EGLBoolean (EGLAPIENTRYP eglGetOutputPortsEXT)(EGLDisplay dpy, const EGLAttrib *attrib_list, EGLOutputPortEXT *ports, EGLint max_ports, EGLint *num_ports);
	EGLBoolean (EGLAPIENTRYP eglOutputLayerAttribEXT)(EGLDisplay dpy, EGLOutputLayerEXT layer, EGLint attribute, EGLAttrib value);
	EGLBoolean (EGLAPIENTRYP eglQueryOutputLayerAttribEXT)(EGLDisplay dpy, EGLOutputLayerEXT layer, EGLint attribute, EGLAttrib *value);
	const char *(EGLAPIENTRYP eglQueryOutputLayerStringEXT)(EGLDisplay dpy, EGLOutputLayerEXT layer, EGLint name);
	EGLBoolean (EGLAPIENTRYP eglOutputPortAttribEXT)(EGLDisplay dpy, EGLOutputPortEXT port, EGLint attribute, EGLAttrib value);
	EGLBoolean (EGLAPIENTRYP eglQueryOutputPortAttribEXT)(EGLDisplay dpy, EGLOutputPortEXT port, EGLint attribute, EGLAttrib *value);
	const char *(EGLAPIENTRYP eglQueryOutputPortStringEXT)(EGLDisplay dpy, EGLOutputPortEXT port, EGLint name);
	EGLDisplay (EGLAPIENTRYP eglGetPlatformDisplayEXT)(EGLenum platform, void *native_display, const EGLint *attrib_list);
	EGLSurface (EGLAPIENTRYP eglCreatePlatformWindowSurfaceEXT)(EGLDisplay dpy, EGLConfig config, void *native_window, const EGLint *attrib_list);
	EGLSurface (EGLAPIENTRYP eglCreatePlatformPixmapSurfaceEXT)(EGLDisplay dpy, EGLConfig config, void *native_pixmap, const EGLint *attrib_list);
	EGLBoolean (EGLAPIENTRYP eglStreamConsumerOutputEXT)(EGLDisplay dpy, EGLStreamKHR stream, EGLOutputLayerEXT layer);
	EGLBoolean (EGLAPIENTRYP eglSwapBuffersWithDamageEXT)(EGLDisplay dpy, EGLSurface surface, const EGLint *rects, EGLint n_rects);
	EGLBoolean (EGLAPIENTRYP eglUnsignalSyncEXT)(EGLDisplay dpy, EGLSync sync, const EGLAttrib *attrib_list);
	EGLSurface (EGLAPIENTRYP eglCreatePixmapSurfaceHI)(EGLDisplay dpy, EGLConfig config, struct EGLClientPixmapHI *pixmap);
	EGLImageKHR (EGLAPIENTRYP eglCreateDRMImageMESA)(EGLDisplay dpy, const EGLint *attrib_list);
	EGLBoolean (EGLAPIENTRYP eglExportDRMImageMESA)(EGLDisplay dpy, EGLImageKHR image, EGLint *name, EGLint *handle, EGLint *stride);
	EGLBoolean (EGLAPIENTRYP eglExportDMABUFImageQueryMESA)(EGLDisplay dpy, EGLImageKHR image, int *fourcc, int *num_planes, EGLuint64KHR *modifiers);
	EGLBoolean (EGLAPIENTRYP eglExportDMABUFImageMESA)(EGLDisplay dpy, EGLImageKHR image, int *fds, EGLint *strides, EGLint *offsets);
	char *(EGLAPIENTRYP eglGetDisplayDriverConfig)(EGLDisplay dpy);
	const char *(EGLAPIENTRYP eglGetDisplayDriverName)(EGLDisplay dpy);
	EGLBoolean (EGLAPIENTRYP eglSwapBuffersRegionNOK)(EGLDisplay dpy, EGLSurface surface, EGLint numRects, const EGLint *rects);
	EGLBoolean (EGLAPIENTRYP eglSwapBuffersRegion2NOK)(EGLDisplay dpy, EGLSurface surface, EGLint numRects, const EGLint *rects);
	EGLBoolean (EGLAPIENTRYP eglQueryNativeDisplayNV)(EGLDisplay dpy, EGLNativeDisplayType *display_id);
	EGLBoolean (EGLAPIENTRYP eglQueryNativeWindowNV)(EGLDisplay dpy, EGLSurface surf, EGLNativeWindowType *window);
	EGLBoolean (EGLAPIENTRYP eglQueryNativePixmapNV)(EGLDisplay dpy, EGLSurface surf, EGLNativePixmapType *pixmap);
	EGLBoolean (EGLAPIENTRYP eglPostSubBufferNV)(EGLDisplay dpy, EGLSurface surface, EGLint x, EGLint y, EGLint width, EGLint height);
	EGLBoolean (EGLAPIENTRYP eglStreamConsumerGLTextureExternalAttribsNV)(EGLDisplay dpy, EGLStreamKHR stream, const EGLAttrib *attrib_list);
	EGLBoolean (EGLAPIENTRYP eglStreamFlushNV)(EGLDisplay dpy, EGLStreamKHR stream);
	EGLBoolean (EGLAPIENTRYP eglQueryDisplayAttribNV)(EGLDisplay dpy, EGLint attribute, EGLAttrib *value);
	EGLBoolean (EGLAPIENTRYP eglSetStreamMetadataNV)(EGLDisplay dpy, EGLStreamKHR stream, EGLint n, EGLint offset, EGLint size, const void *data);
	EGLBoolean (EGLAPIENTRYP eglQueryStreamMetadataNV)(EGLDisplay dpy, EGLStreamKHR stream, EGLenum name, EGLint n, EGLint offset, EGLint size, void *data);
	EGLBoolean (EGLAPIENTRYP eglResetStreamNV)(EGLDisplay dpy, EGLStreamKHR stream);
	EGLSyncKHR (EGLAPIENTRYP eglCreateStreamSyncNV)(EGLDisplay dpy, EGLStreamKHR stream, EGLenum type, const EGLint *attrib_list);
	EGLSyncNV (EGLAPIENTRYP eglCreateFenceSyncNV)(EGLDisplay dpy, EGLenum condition, const EGLint *attrib_list);
	EGLBoolean (EGLAPIENTRYP eglDestroySyncNV)(EGLSyncNV sync);
	EGLBoolean (EGLAPIENTRYP eglFenceNV)(EGLSyncNV sync);
	EGLint (EGLAPIENTRYP eglClientWaitSyncNV)(EGLSyncNV sync, EGLint flags, EGLTimeNV timeout);
	EGLBoolean (EGLAPIENTRYP eglSignalSyncNV)(EGLSyncNV sync, EGLenum mode);
	EGLBoolean (EGLAPIENTRYP eglGetSyncAttribNV)(EGLSyncNV sync, EGLint attribute, EGLint *value);
	EGLuint64NV (EGLAPIENTRYP eglGetSystemTimeFrequencyNV)(void);
	EGLuint64NV (EGLAPIENTRYP eglGetSystemTimeNV)(void);
	EGLBoolean (EGLAPIENTRYP eglBindWaylandDisplayWL)(EGLDisplay dpy, struct wl_display *display);
	EGLBoolean (EGLAPIENTRYP eglUnbindWaylandDisplayWL)(EGLDisplay dpy, struct wl_display *display);
	EGLBoolean (EGLAPIENTRYP eglQueryWaylandBufferWL)(EGLDisplay dpy, struct wl_resource *buffer, EGLint attribute, EGLint *value);
	struct wl_buffer *(EGLAPIENTRYP eglCreateWaylandBufferFromImageWL)(EGLDisplay dpy, EGLImageKHR image);
};

struct egl_client_info {
	const char *client_extensions;
	bool supports_khr_cl_event;
	bool supports_khr_cl_event2;
	bool supports_khr_client_get_all_proc_addresses;
	bool supports_khr_config_attribs;
	bool supports_khr_context_flush_control;
	bool supports_khr_create_context;
	bool supports_khr_create_context_no_error;
	bool supports_khr_debug;
	bool supports_khr_display_reference;
	bool supports_khr_fence_sync;
	bool supports_khr_get_all_proc_addresses;
	bool supports_khr_gl_colorspace;
	bool supports_khr_gl_renderbuffer_image;
	bool supports_khr_gl_texture_2d_image;
	bool supports_khr_gl_texture_3d_image;
	bool supports_khr_gl_texture_cubemap_image;
	bool supports_khr_image;
	bool supports_khr_image_base;
	bool supports_khr_image_pixmap;
	bool supports_khr_lock_surface;
	bool supports_khr_lock_surface2;
	bool supports_khr_lock_surface3;
	bool supports_khr_mutable_render_buffer;
	bool supports_khr_no_config_context;
	bool supports_khr_partial_update;
	bool supports_khr_platform_android;
	bool supports_khr_platform_gbm;
	bool supports_khr_platform_wayland;
	bool supports_khr_platform_x11;
	bool supports_khr_reusable_sync;
	bool supports_khr_stream;
	bool supports_khr_stream_attrib;
	bool supports_khr_stream_consumer_gltexture;
	bool supports_khr_stream_cross_process_fd;
	bool supports_khr_stream_fifo;
	bool supports_khr_stream_producer_aldatalocator;
	bool supports_khr_stream_producer_eglsurface;
	bool supports_khr_surfaceless_context;
	bool supports_khr_swap_buffers_with_damage;
	bool supports_khr_vg_parent_image;
	bool supports_khr_wait_sync;
	bool supports_android_gles_layers;
	bool supports_android_blob_cache;
	bool supports_android_create_native_client_buffer;
	bool supports_android_framebuffer_target;
	bool supports_android_front_buffer_auto_refresh;
	bool supports_android_get_frame_timestamps;
	bool supports_android_get_native_client_buffer;
	bool supports_android_image_native_buffer;
	bool supports_android_native_fence_sync;
	bool supports_android_presentation_time;
	bool supports_android_recordable;
	bool supports_angle_d3d_share_handle_client_buffer;
	bool supports_angle_device_d3d;
	bool supports_angle_query_surface_pointer;
	bool supports_angle_surface_d3d_texture_2d_share_handle;
	bool supports_angle_window_fixed_size;
	bool supports_arm_image_format;
	bool supports_arm_implicit_external_sync;
	bool supports_arm_pixmap_multisample_discard;
	bool supports_ext_bind_to_front;
	bool supports_ext_buffer_age;
	bool supports_ext_client_extensions;
	bool supports_ext_client_sync;
	bool supports_ext_compositor;
	bool supports_ext_create_context_robustness;
	bool supports_ext_device_base;
	bool supports_ext_device_drm;
	bool supports_ext_device_enumeration;
	bool supports_ext_device_openwf;
	bool supports_ext_device_query;
	bool supports_ext_gl_colorspace_bt2020_linear;
	bool supports_ext_gl_colorspace_bt2020_pq;
	bool supports_ext_gl_colorspace_display_p3;
	bool supports_ext_gl_colorspace_display_p3_linear;
	bool supports_ext_gl_colorspace_display_p3_passthrough;
	bool supports_ext_gl_colorspace_scrgb;
	bool supports_ext_gl_colorspace_scrgb_linear;
	bool supports_ext_image_dma_buf_import;
	bool supports_ext_image_dma_buf_import_modifiers;
	bool supports_ext_image_gl_colorspace;
	bool supports_ext_image_implicit_sync_control;
	bool supports_ext_multiview_window;
	bool supports_ext_output_base;
	bool supports_ext_output_drm;
	bool supports_ext_output_openwf;
	bool supports_ext_pixel_format_float;
	bool supports_ext_platform_base;
	bool supports_ext_platform_device;
	bool supports_ext_platform_wayland;
	bool supports_ext_platform_x11;
	bool supports_mesa_platform_xcb;
	bool supports_ext_protected_content;
	bool supports_ext_protected_surface;
	bool supports_ext_stream_consumer_egloutput;
	bool supports_ext_surface_cta861_3_metadata;
	bool supports_ext_surface_smpte2086_metadata;
	bool supports_ext_swap_buffers_with_damage;
	bool supports_ext_sync_reuse;
	bool supports_ext_yuv_surface;
	bool supports_hi_clientpixmap;
	bool supports_hi_colorformats;
	bool supports_img_context_priority;
	bool supports_img_image_plane_attribs;
	bool supports_mesa_drm_image;
	bool supports_mesa_image_dma_buf_export;
	bool supports_mesa_platform_gbm;
	bool supports_mesa_platform_surfaceless;
	bool supports_mesa_query_driver;
	bool supports_nok_swap_region;
	bool supports_nok_swap_region2;
	bool supports_nok_texture_from_pixmap;
	bool supports_nv_3dvision_surface;
	bool supports_nv_context_priority_realtime;
	bool supports_nv_coverage_sample;
	bool supports_nv_coverage_sample_resolve;
	bool supports_nv_cuda_event;
	bool supports_nv_depth_nonlinear;
	bool supports_nv_device_cuda;
	bool supports_nv_native_query;
	bool supports_nv_post_convert_rounding;
	bool supports_nv_post_sub_buffer;
	bool supports_nv_quadruple_buffer;
	bool supports_nv_robustness_video_memory_purge;
	bool supports_nv_stream_consumer_gltexture_yuv;
	bool supports_nv_stream_cross_display;
	bool supports_nv_stream_cross_object;
	bool supports_nv_stream_cross_partition;
	bool supports_nv_stream_cross_process;
	bool supports_nv_stream_cross_system;
	bool supports_nv_stream_dma;
	bool supports_nv_stream_fifo_next;
	bool supports_nv_stream_fifo_synchronous;
	bool supports_nv_stream_flush;
	bool supports_nv_stream_frame_limits;
	bool supports_nv_stream_metadata;
	bool supports_nv_stream_origin;
	bool supports_nv_stream_remote;
	bool supports_nv_stream_reset;
	bool supports_nv_stream_socket;
	bool supports_nv_stream_socket_inet;
	bool supports_nv_stream_socket_unix;
	bool supports_nv_stream_sync;
	bool supports_nv_sync;
	bool supports_nv_system_time;
	bool supports_nv_triple_buffer;
	bool supports_tizen_image_native_buffer;
	bool supports_tizen_image_native_surface;
	bool supports_wl_bind_wayland_display;
	bool supports_wl_create_wayland_buffer_from_image;
};

struct egl_display_info {
	const char *client_extensions;
	const char *display_extensions;
	int major, minor;
	bool supports_11;
	bool supports_12;
	bool supports_13;
	bool supports_14;
	bool supports_15;
	bool supports_khr_cl_event;
	bool supports_khr_cl_event2;
	bool supports_khr_client_get_all_proc_addresses;
	bool supports_khr_config_attribs;
	bool supports_khr_context_flush_control;
	bool supports_khr_create_context;
	bool supports_khr_create_context_no_error;
	bool supports_khr_debug;
	bool supports_khr_display_reference;
	bool supports_khr_fence_sync;
	bool supports_khr_get_all_proc_addresses;
	bool supports_khr_gl_colorspace;
	bool supports_khr_gl_renderbuffer_image;
	bool supports_khr_gl_texture_2d_image;
	bool supports_khr_gl_texture_3d_image;
	bool supports_khr_gl_texture_cubemap_image;
	bool supports_khr_image;
	bool supports_khr_image_base;
	bool supports_khr_image_pixmap;
	bool supports_khr_lock_surface;
	bool supports_khr_lock_surface2;
	bool supports_khr_lock_surface3;
	bool supports_khr_mutable_render_buffer;
	bool supports_khr_no_config_context;
	bool supports_khr_partial_update;
	bool supports_khr_platform_android;
	bool supports_khr_platform_gbm;
	bool supports_khr_platform_wayland;
	bool supports_khr_platform_x11;
	bool supports_khr_reusable_sync;
	bool supports_khr_stream;
	bool supports_khr_stream_attrib;
	bool supports_khr_stream_consumer_gltexture;
	bool supports_khr_stream_cross_process_fd;
	bool supports_khr_stream_fifo;
	bool supports_khr_stream_producer_aldatalocator;
	bool supports_khr_stream_producer_eglsurface;
	bool supports_khr_surfaceless_context;
	bool supports_khr_swap_buffers_with_damage;
	bool supports_khr_vg_parent_image;
	bool supports_khr_wait_sync;
	bool supports_android_gles_layers;
	bool supports_android_blob_cache;
	bool supports_android_create_native_client_buffer;
	bool supports_android_framebuffer_target;
	bool supports_android_front_buffer_auto_refresh;
	bool supports_android_get_frame_timestamps;
	bool supports_android_get_native_client_buffer;
	bool supports_android_image_native_buffer;
	bool supports_android_native_fence_sync;
	bool supports_android_presentation_time;
	bool supports_android_recordable;
	bool supports_angle_d3d_share_handle_client_buffer;
	bool supports_angle_device_d3d;
	bool supports_angle_query_surface_pointer;
	bool supports_angle_surface_d3d_texture_2d_share_handle;
	bool supports_angle_window_fixed_size;
	bool supports_arm_image_format;
	bool supports_arm_implicit_external_sync;
	bool supports_arm_pixmap_multisample_discard;
	bool supports_ext_bind_to_front;
	bool supports_ext_buffer_age;
	bool supports_ext_client_extensions;
	bool supports_ext_client_sync;
	bool supports_ext_compositor;
	bool supports_ext_create_context_robustness;
	bool supports_ext_device_base;
	bool supports_ext_device_drm;
	bool supports_ext_device_enumeration;
	bool supports_ext_device_openwf;
	bool supports_ext_device_query;
	bool supports_ext_gl_colorspace_bt2020_linear;
	bool supports_ext_gl_colorspace_bt2020_pq;
	bool supports_ext_gl_colorspace_display_p3;
	bool supports_ext_gl_colorspace_display_p3_linear;
	bool supports_ext_gl_colorspace_display_p3_passthrough;
	bool supports_ext_gl_colorspace_scrgb;
	bool supports_ext_gl_colorspace_scrgb_linear;
	bool supports_ext_image_dma_buf_import;
	bool supports_ext_image_dma_buf_import_modifiers;
	bool supports_ext_image_gl_colorspace;
	bool supports_ext_image_implicit_sync_control;
	bool supports_ext_multiview_window;
	bool supports_ext_output_base;
	bool supports_ext_output_drm;
	bool supports_ext_output_openwf;
	bool supports_ext_pixel_format_float;
	bool supports_ext_platform_base;
	bool supports_ext_platform_device;
	bool supports_ext_platform_wayland;
	bool supports_ext_platform_x11;
	bool supports_mesa_platform_xcb;
	bool supports_ext_protected_content;
	bool supports_ext_protected_surface;
	bool supports_ext_stream_consumer_egloutput;
	bool supports_ext_surface_cta861_3_metadata;
	bool supports_ext_surface_smpte2086_metadata;
	bool supports_ext_swap_buffers_with_damage;
	bool supports_ext_sync_reuse;
	bool supports_ext_yuv_surface;
	bool supports_hi_clientpixmap;
	bool supports_hi_colorformats;
	bool supports_img_context_priority;
	bool supports_img_image_plane_attribs;
	bool supports_mesa_drm_image;
	bool supports_mesa_image_dma_buf_export;
	bool supports_mesa_platform_gbm;
	bool supports_mesa_platform_surfaceless;
	bool supports_mesa_query_driver;
	bool supports_nok_swap_region;
	bool supports_nok_swap_region2;
	bool supports_nok_texture_from_pixmap;
	bool supports_nv_3dvision_surface;
	bool supports_nv_context_priority_realtime;
	bool supports_nv_coverage_sample;
	bool supports_nv_coverage_sample_resolve;
	bool supports_nv_cuda_event;
	bool supports_nv_depth_nonlinear;
	bool supports_nv_device_cuda;
	bool supports_nv_native_query;
	bool supports_nv_post_convert_rounding;
	bool supports_nv_post_sub_buffer;
	bool supports_nv_quadruple_buffer;
	bool supports_nv_robustness_video_memory_purge;
	bool supports_nv_stream_consumer_gltexture_yuv;
	bool supports_nv_stream_cross_display;
	bool supports_nv_stream_cross_object;
	bool supports_nv_stream_cross_partition;
	bool supports_nv_stream_cross_process;
	bool supports_nv_stream_cross_system;
	bool supports_nv_stream_dma;
	bool supports_nv_stream_fifo_next;
	bool supports_nv_stream_fifo_synchronous;
	bool supports_nv_stream_flush;
	bool supports_nv_stream_frame_limits;
	bool supports_nv_stream_metadata;
	bool supports_nv_stream_origin;
	bool supports_nv_stream_remote;
	bool supports_nv_stream_reset;
	bool supports_nv_stream_socket;
	bool supports_nv_stream_socket_inet;
	bool supports_nv_stream_socket_unix;
	bool supports_nv_stream_sync;
	bool supports_nv_sync;
	bool supports_nv_system_time;
	bool supports_nv_triple_buffer;
	bool supports_tizen_image_native_buffer;
	bool supports_tizen_image_native_surface;
	bool supports_wl_bind_wayland_display;
	bool supports_wl_create_wayland_buffer_from_image;
};

typedef void *(*gl_proc_resolver_t)(const char *proc_name);

struct libgl {
	PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC EGLImageTargetRenderbufferStorageOES;
};

/**
 * @brief Load libudev dynamically using @ref dlopen, @ref dlsym. The string passed to @ref dlopen
 * is @ref name and it's loaded with RTLD_NOW | RTLD_LOCAL.
 * 
 * @returns NULL on failure, a heap-allocated @ref libudev on success.
 */
struct libflutter_engine *libflutter_engine_load(char *name);

/// TODO: Document
struct libflutter_engine *libflutter_engine_load_for_runtime_mode(enum flutter_runtime_mode runtime_mode);

/**
 * @brief Unload a previously loaded libflutter_engine by closing it dynamic library handle and freeing
 * the libflutter_engine struct.
 */
void libflutter_engine_unload(struct libflutter_engine *lib);

/**
 * @brief Load libudev dynamically using @ref dlopen, @ref dlsym. The string passed to @ref dlopen
 * is "libudev.so", and it's loaded with RTLD_NOW | RTLD_GLOBAL.
 * 
 * @returns NULL on failure, a heap-allocated @ref libudev on success.
 */
struct libudev *libudev_load(void);

/**
 * @brief Unload a previously loaded libudev by closing its dynamic library handle and freeing
 * the libudev struct.
 */
void libudev_unload(struct libudev *lib);

/**
 * @brief Load a the EGL library
 */
struct libegl *libegl_load(void);

void libegl_unload(struct libegl *lib);


struct egl_client_info *egl_client_info_new(struct libegl *lib);

void egl_client_info_destroy(struct egl_client_info *client_info);


struct egl_display_info *egl_display_info_new(
	struct libegl *lib,
	EGLint major,
	EGLint minor,
	EGLDisplay display
);

void egl_display_info_destroy(struct egl_display_info *display_info);


struct libgl *libgl_load(gl_proc_resolver_t proc_resolver);

void libgl_unload(struct libgl *lib);

#endif