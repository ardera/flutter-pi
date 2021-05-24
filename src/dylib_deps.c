#include <stdlib.h>
#include <stdio.h>
#include <dlfcn.h>
#include <errno.h>
#include <string.h>
#include <flutter-pi.h>

#include <dylib_deps.h>


static void *try_open_dylib(const char *file, int mode) {
	void *handle = dlopen(file, mode);
	if (handle == NULL) {
		fprintf(stderr, "[flutter-pi] Could not open dynamic library \"%s\": %s\n", file, dlerror());
		return NULL;
	}

	return handle;
}


struct libflutter_engine *libflutter_engine_load(char *name) {
	struct libflutter_engine *lib;
	
	lib = malloc(sizeof *lib);
	if (lib == NULL) {
		return NULL;
	}

	lib->handle = try_open_dylib(name, RTLD_NOW | RTLD_LOCAL);
	if (lib->handle == NULL) {
		free(lib);
		return NULL;
	}

#	define LOAD_LIBFLUTTER_ENGINE_PROC(name) \
		do { \
			char *__errorstr; \
			dlerror(); \
			lib->name = dlsym(lib->handle, #name); \
			if ((__errorstr = dlerror()) != NULL) { \
				fprintf(stderr, "[flutter-pi] Could not resolve libflutter_engine procedure \"%s\". dlsym: %s\n", #name, __errorstr); \
				dlclose(lib->handle); \
				free(lib); \
				return NULL; \
			} \
		} while (false)

	LOAD_LIBFLUTTER_ENGINE_PROC(FlutterEngineCreateAOTData);
	LOAD_LIBFLUTTER_ENGINE_PROC(FlutterEngineCollectAOTData);
	LOAD_LIBFLUTTER_ENGINE_PROC(FlutterEngineRun);
	LOAD_LIBFLUTTER_ENGINE_PROC(FlutterEngineShutdown);
	LOAD_LIBFLUTTER_ENGINE_PROC(FlutterEngineInitialize);
	LOAD_LIBFLUTTER_ENGINE_PROC(FlutterEngineDeinitialize);
	LOAD_LIBFLUTTER_ENGINE_PROC(FlutterEngineRunInitialized);
	LOAD_LIBFLUTTER_ENGINE_PROC(FlutterEngineSendWindowMetricsEvent);
	LOAD_LIBFLUTTER_ENGINE_PROC(FlutterEngineSendPointerEvent);
	LOAD_LIBFLUTTER_ENGINE_PROC(FlutterEngineSendPlatformMessage);
	LOAD_LIBFLUTTER_ENGINE_PROC(FlutterPlatformMessageCreateResponseHandle);
	LOAD_LIBFLUTTER_ENGINE_PROC(FlutterPlatformMessageReleaseResponseHandle);
	LOAD_LIBFLUTTER_ENGINE_PROC(FlutterEngineSendPlatformMessageResponse);
	LOAD_LIBFLUTTER_ENGINE_PROC(__FlutterEngineFlushPendingTasksNow);
	LOAD_LIBFLUTTER_ENGINE_PROC(FlutterEngineRegisterExternalTexture);
	LOAD_LIBFLUTTER_ENGINE_PROC(FlutterEngineUnregisterExternalTexture);
	LOAD_LIBFLUTTER_ENGINE_PROC(FlutterEngineMarkExternalTextureFrameAvailable);
	LOAD_LIBFLUTTER_ENGINE_PROC(FlutterEngineUpdateSemanticsEnabled);
	LOAD_LIBFLUTTER_ENGINE_PROC(FlutterEngineUpdateAccessibilityFeatures);
	LOAD_LIBFLUTTER_ENGINE_PROC(FlutterEngineDispatchSemanticsAction);
	LOAD_LIBFLUTTER_ENGINE_PROC(FlutterEngineOnVsync);
	LOAD_LIBFLUTTER_ENGINE_PROC(FlutterEngineReloadSystemFonts);
	LOAD_LIBFLUTTER_ENGINE_PROC(FlutterEngineTraceEventDurationBegin);
	LOAD_LIBFLUTTER_ENGINE_PROC(FlutterEngineTraceEventDurationEnd);
	LOAD_LIBFLUTTER_ENGINE_PROC(FlutterEngineTraceEventInstant);
	LOAD_LIBFLUTTER_ENGINE_PROC(FlutterEnginePostRenderThreadTask);
	LOAD_LIBFLUTTER_ENGINE_PROC(FlutterEngineGetCurrentTime);
	LOAD_LIBFLUTTER_ENGINE_PROC(FlutterEngineRunTask);
	LOAD_LIBFLUTTER_ENGINE_PROC(FlutterEngineUpdateLocales);
	LOAD_LIBFLUTTER_ENGINE_PROC(FlutterEngineRunsAOTCompiledDartCode);
	LOAD_LIBFLUTTER_ENGINE_PROC(FlutterEnginePostDartObject);
	LOAD_LIBFLUTTER_ENGINE_PROC(FlutterEngineNotifyLowMemoryWarning);
	LOAD_LIBFLUTTER_ENGINE_PROC(FlutterEnginePostCallbackOnAllNativeThreads);

#	undef LOAD_LIBFLUTTER_ENGINE_PROC

	return lib;
}

struct libflutter_engine *libflutter_engine_load_for_runtime_mode(enum flutter_runtime_mode runtime_mode) {
	struct libflutter_engine *engine_lib;
	
	if (runtime_mode == kRelease) {
		engine_lib = libflutter_engine_load("libflutter_engine.so.release");
	} else if (runtime_mode == kDebug) {
		engine_lib = libflutter_engine_load("libflutter_engine.so.debug");
	}

	if (engine_lib == NULL) {
		engine_lib = libflutter_engine_load("libflutter_engine.so");
	}
	
	return engine_lib;
}

void libflutter_engine_unload(struct libflutter_engine *lib) {
	dlclose(lib);
	free(lib);
}


struct libegl *libegl_load(void) {
	struct libegl *lib;

	lib = malloc(sizeof *lib);
	if (lib == NULL) {
		return NULL;
	}

	/*
	lib->handle = try_open_dylib("libegl.so", RTLD_NOW | RTLD_GLOBAL);
	if (lib->handle == NULL) {
		free(lib);
		return NULL;
	}
	*/
	
#	define LOAD_LIBEGL_PROC(name) \
		do { \
			char *__errorstr; \
			dlerror(); \
			lib->name = dlsym(lib->handle, #name); \
			if ((__errorstr = dlerror()) != NULL) { \
				fprintf(stderr, "[flutter-pi] Could not resolve libegl procedure \"%s\". dlsym: %s\n", #name, __errorstr); \
				dlclose(lib->handle); \
				free(lib); \
				return NULL; \
			} \
		} while (false)

#	define LOAD_LIBEGL_PROC_OPT(name) lib->name = dlsym(NULL, #name)
#	define LOAD_LIBEGL_EXT_PROC_OPT(name) lib->name = (void*) eglGetProcAddress(#name)

	// 1.0
	/*
	LOAD_LIBEGL_PROC(eglChooseConfig);
	LOAD_LIBEGL_PROC(eglCopyBuffers);
	LOAD_LIBEGL_PROC(eglCreateContext);
	LOAD_LIBEGL_PROC(eglCreatePbufferSurface);
	LOAD_LIBEGL_PROC(eglCreatePixmapSurface);
	LOAD_LIBEGL_PROC(eglCreateWindowSurface);
	LOAD_LIBEGL_PROC(eglDestroyContext);
	LOAD_LIBEGL_PROC(eglDestroySurface);
	LOAD_LIBEGL_PROC(eglGetConfigAttrib);
	LOAD_LIBEGL_PROC(eglGetConfigs);
	LOAD_LIBEGL_PROC(eglGetCurrentDisplay);
	LOAD_LIBEGL_PROC(eglGetCurrentSurface);
	LOAD_LIBEGL_PROC(eglGetDisplay);
	LOAD_LIBEGL_PROC(eglGetError);
	LOAD_LIBEGL_PROC(eglGetProcAddress);
	LOAD_LIBEGL_PROC(eglInitialize);
	LOAD_LIBEGL_PROC(eglMakeCurrent);
	LOAD_LIBEGL_PROC(eglQueryContext);
	LOAD_LIBEGL_PROC(eglQueryString);
	LOAD_LIBEGL_PROC(eglQuerySurface);
	LOAD_LIBEGL_PROC(eglSwapBuffers);
	LOAD_LIBEGL_PROC(eglTerminate);
	LOAD_LIBEGL_PROC(eglWaitGL);
	LOAD_LIBEGL_PROC(eglWaitNative);
	*/
	LOAD_LIBEGL_PROC_OPT(eglBindTexImage);
	LOAD_LIBEGL_PROC_OPT(eglReleaseTexImage);
	LOAD_LIBEGL_PROC_OPT(eglSurfaceAttrib);
	LOAD_LIBEGL_PROC_OPT(eglSwapInterval);
	LOAD_LIBEGL_PROC_OPT(eglBindAPI);
	LOAD_LIBEGL_PROC_OPT(eglQueryAPI);
	LOAD_LIBEGL_PROC_OPT(eglCreatePbufferFromClientBuffer);
	LOAD_LIBEGL_PROC_OPT(eglReleaseThread);
	LOAD_LIBEGL_PROC_OPT(eglWaitClient);
	LOAD_LIBEGL_PROC_OPT(eglGetCurrentContext);
	LOAD_LIBEGL_PROC_OPT(eglCreateSync);
	LOAD_LIBEGL_PROC_OPT(eglDestroySync);
	LOAD_LIBEGL_PROC_OPT(eglClientWaitSync);
	LOAD_LIBEGL_PROC_OPT(eglGetSyncAttrib);
	LOAD_LIBEGL_PROC_OPT(eglCreateImage);
	LOAD_LIBEGL_PROC_OPT(eglDestroyImage);
	LOAD_LIBEGL_PROC_OPT(eglGetPlatformDisplay);
	LOAD_LIBEGL_PROC_OPT(eglCreatePlatformWindowSurface);
	LOAD_LIBEGL_PROC_OPT(eglCreatePlatformPixmapSurface);
	LOAD_LIBEGL_PROC_OPT(eglWaitSync);
	LOAD_LIBEGL_EXT_PROC_OPT(eglCreateSync64KHR);
	LOAD_LIBEGL_EXT_PROC_OPT(eglDebugMessageControlKHR);
	LOAD_LIBEGL_EXT_PROC_OPT(eglQueryDebugKHR);
	LOAD_LIBEGL_EXT_PROC_OPT(eglLabelObjectKHR);
	LOAD_LIBEGL_EXT_PROC_OPT(eglQueryDisplayAttribKHR);
	LOAD_LIBEGL_EXT_PROC_OPT(eglCreateSyncKHR);
	LOAD_LIBEGL_EXT_PROC_OPT(eglDestroySyncKHR);
	LOAD_LIBEGL_EXT_PROC_OPT(eglClientWaitSyncKHR);
	LOAD_LIBEGL_EXT_PROC_OPT(eglGetSyncAttribKHR);
	LOAD_LIBEGL_EXT_PROC_OPT(eglCreateImageKHR);
	LOAD_LIBEGL_EXT_PROC_OPT(eglDestroyImageKHR);
	LOAD_LIBEGL_EXT_PROC_OPT(eglLockSurfaceKHR);
	LOAD_LIBEGL_EXT_PROC_OPT(eglUnlockSurfaceKHR);
	LOAD_LIBEGL_EXT_PROC_OPT(eglQuerySurface64KHR);
	LOAD_LIBEGL_EXT_PROC_OPT(eglSetDamageRegionKHR);
	LOAD_LIBEGL_EXT_PROC_OPT(eglSignalSyncKHR);
	LOAD_LIBEGL_EXT_PROC_OPT(eglCreateStreamKHR);
	LOAD_LIBEGL_EXT_PROC_OPT(eglDestroyStreamKHR);
	LOAD_LIBEGL_EXT_PROC_OPT(eglStreamAttribKHR);
	LOAD_LIBEGL_EXT_PROC_OPT(eglQueryStreamKHR);
	LOAD_LIBEGL_EXT_PROC_OPT(eglQueryStreamu64KHR);
	LOAD_LIBEGL_EXT_PROC_OPT(eglCreateStreamAttribKHR);
	LOAD_LIBEGL_EXT_PROC_OPT(eglSetStreamAttribKHR);
	LOAD_LIBEGL_EXT_PROC_OPT(eglQueryStreamAttribKHR);
	LOAD_LIBEGL_EXT_PROC_OPT(eglStreamConsumerAcquireAttribKHR);
	LOAD_LIBEGL_EXT_PROC_OPT(eglStreamConsumerReleaseAttribKHR);
	LOAD_LIBEGL_EXT_PROC_OPT(eglStreamConsumerGLTextureExternalKHR);
	LOAD_LIBEGL_EXT_PROC_OPT(eglStreamConsumerAcquireKHR);
	LOAD_LIBEGL_EXT_PROC_OPT(eglStreamConsumerReleaseKHR);
	LOAD_LIBEGL_EXT_PROC_OPT(eglGetStreamFileDescriptorKHR);
	LOAD_LIBEGL_EXT_PROC_OPT(eglCreateStreamFromFileDescriptorKHR);
	LOAD_LIBEGL_EXT_PROC_OPT(eglQueryStreamTimeKHR);
	LOAD_LIBEGL_EXT_PROC_OPT(eglCreateStreamProducerSurfaceKHR);
	LOAD_LIBEGL_EXT_PROC_OPT(eglSwapBuffersWithDamageKHR);
	LOAD_LIBEGL_EXT_PROC_OPT(eglWaitSyncKHR);
	LOAD_LIBEGL_EXT_PROC_OPT(eglSetBlobCacheFuncsANDROID);
	LOAD_LIBEGL_EXT_PROC_OPT(eglCreateNativeClientBufferANDROID);
	LOAD_LIBEGL_EXT_PROC_OPT(eglGetCompositorTimingSupportedANDROID);
	LOAD_LIBEGL_EXT_PROC_OPT(eglGetCompositorTimingANDROID);
	LOAD_LIBEGL_EXT_PROC_OPT(eglGetNextFrameIdANDROID);
	LOAD_LIBEGL_EXT_PROC_OPT(eglGetFrameTimestampSupportedANDROID);
	LOAD_LIBEGL_EXT_PROC_OPT(eglGetFrameTimestampsANDROID);
	LOAD_LIBEGL_EXT_PROC_OPT(eglGetNativeClientBufferANDROID);
	LOAD_LIBEGL_EXT_PROC_OPT(eglDupNativeFenceFDANDROID);
	LOAD_LIBEGL_EXT_PROC_OPT(eglPresentationTimeANDROID);
	LOAD_LIBEGL_EXT_PROC_OPT(eglQuerySurfacePointerANGLE);
	LOAD_LIBEGL_EXT_PROC_OPT(eglClientSignalSyncEXT);
	LOAD_LIBEGL_EXT_PROC_OPT(eglCompositorSetContextListEXT);
	LOAD_LIBEGL_EXT_PROC_OPT(eglCompositorSetContextAttributesEXT);
	LOAD_LIBEGL_EXT_PROC_OPT(eglCompositorSetWindowListEXT);
	LOAD_LIBEGL_EXT_PROC_OPT(eglCompositorSetWindowAttributesEXT);
	LOAD_LIBEGL_EXT_PROC_OPT(eglCompositorBindTexWindowEXT);
	LOAD_LIBEGL_EXT_PROC_OPT(eglCompositorSetSizeEXT);
	LOAD_LIBEGL_EXT_PROC_OPT(eglCompositorSwapPolicyEXT);
	LOAD_LIBEGL_EXT_PROC_OPT(eglQueryDeviceAttribEXT);
	LOAD_LIBEGL_EXT_PROC_OPT(eglQueryDeviceStringEXT);
	LOAD_LIBEGL_EXT_PROC_OPT(eglQueryDevicesEXT);
	LOAD_LIBEGL_EXT_PROC_OPT(eglQueryDisplayAttribEXT);
	LOAD_LIBEGL_EXT_PROC_OPT(eglQueryDmaBufFormatsEXT);
	LOAD_LIBEGL_EXT_PROC_OPT(eglQueryDmaBufModifiersEXT);
	LOAD_LIBEGL_EXT_PROC_OPT(eglGetOutputLayersEXT);
	LOAD_LIBEGL_EXT_PROC_OPT(eglGetOutputPortsEXT);
	LOAD_LIBEGL_EXT_PROC_OPT(eglOutputLayerAttribEXT);
	LOAD_LIBEGL_EXT_PROC_OPT(eglQueryOutputLayerAttribEXT);
	LOAD_LIBEGL_EXT_PROC_OPT(eglQueryOutputLayerStringEXT);
	LOAD_LIBEGL_EXT_PROC_OPT(eglOutputPortAttribEXT);
	LOAD_LIBEGL_EXT_PROC_OPT(eglQueryOutputPortAttribEXT);
	LOAD_LIBEGL_EXT_PROC_OPT(eglQueryOutputPortStringEXT);
	LOAD_LIBEGL_EXT_PROC_OPT(eglGetPlatformDisplayEXT);
	LOAD_LIBEGL_EXT_PROC_OPT(eglCreatePlatformWindowSurfaceEXT);
	LOAD_LIBEGL_EXT_PROC_OPT(eglCreatePlatformPixmapSurfaceEXT);
	LOAD_LIBEGL_EXT_PROC_OPT(eglStreamConsumerOutputEXT);
	LOAD_LIBEGL_EXT_PROC_OPT(eglSwapBuffersWithDamageEXT);
	LOAD_LIBEGL_EXT_PROC_OPT(eglUnsignalSyncEXT);
	LOAD_LIBEGL_EXT_PROC_OPT(eglCreatePixmapSurfaceHI);
	LOAD_LIBEGL_EXT_PROC_OPT(eglCreateDRMImageMESA);
	LOAD_LIBEGL_EXT_PROC_OPT(eglExportDRMImageMESA);
	LOAD_LIBEGL_EXT_PROC_OPT(eglExportDMABUFImageQueryMESA);
	LOAD_LIBEGL_EXT_PROC_OPT(eglExportDMABUFImageMESA);
	LOAD_LIBEGL_EXT_PROC_OPT(eglGetDisplayDriverConfig);
	LOAD_LIBEGL_EXT_PROC_OPT(eglGetDisplayDriverName);
	LOAD_LIBEGL_EXT_PROC_OPT(eglSwapBuffersRegionNOK);
	LOAD_LIBEGL_EXT_PROC_OPT(eglSwapBuffersRegion2NOK);
	LOAD_LIBEGL_EXT_PROC_OPT(eglQueryNativeDisplayNV);
	LOAD_LIBEGL_EXT_PROC_OPT(eglQueryNativeWindowNV);
	LOAD_LIBEGL_EXT_PROC_OPT(eglQueryNativePixmapNV);
	LOAD_LIBEGL_EXT_PROC_OPT(eglPostSubBufferNV);
	LOAD_LIBEGL_EXT_PROC_OPT(eglStreamConsumerGLTextureExternalAttribsNV);
	LOAD_LIBEGL_EXT_PROC_OPT(eglStreamFlushNV);
	LOAD_LIBEGL_EXT_PROC_OPT(eglQueryDisplayAttribNV);
	LOAD_LIBEGL_EXT_PROC_OPT(eglSetStreamMetadataNV);
	LOAD_LIBEGL_EXT_PROC_OPT(eglQueryStreamMetadataNV);
	LOAD_LIBEGL_EXT_PROC_OPT(eglResetStreamNV);
	LOAD_LIBEGL_EXT_PROC_OPT(eglCreateStreamSyncNV);
	LOAD_LIBEGL_EXT_PROC_OPT(eglCreateFenceSyncNV);
	LOAD_LIBEGL_EXT_PROC_OPT(eglDestroySyncNV);
	LOAD_LIBEGL_EXT_PROC_OPT(eglFenceNV);
	LOAD_LIBEGL_EXT_PROC_OPT(eglClientWaitSyncNV);
	LOAD_LIBEGL_EXT_PROC_OPT(eglSignalSyncNV);
	LOAD_LIBEGL_EXT_PROC_OPT(eglGetSyncAttribNV);
	LOAD_LIBEGL_EXT_PROC_OPT(eglGetSystemTimeFrequencyNV);
	LOAD_LIBEGL_EXT_PROC_OPT(eglGetSystemTimeNV);
	LOAD_LIBEGL_EXT_PROC_OPT(eglBindWaylandDisplayWL);
	LOAD_LIBEGL_EXT_PROC_OPT(eglUnbindWaylandDisplayWL);
	LOAD_LIBEGL_EXT_PROC_OPT(eglQueryWaylandBufferWL);
	LOAD_LIBEGL_EXT_PROC_OPT(eglCreateWaylandBufferFromImageWL);

#	undef LOAD_LIBEGL_EXT_PROC_OPT
#	undef LOAD_LIBEGL_PROC_OPT
#	undef LOAD_LIBEGL_PROC

	return lib;
}

void libegl_unload(struct libegl *lib) {
	free(lib);
}


struct egl_client_info *egl_client_info_new(struct libegl *lib) {
	struct egl_client_info *info;

	info = malloc(sizeof *info);
	if (info == NULL) {
		return NULL;
	}

	info->client_extensions = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
	if (info->client_extensions == NULL) {
		free(info);
		return NULL;
	}

#	define HAS_EXTENSION(name) \
		(strstr(info->client_extensions, #name) != NULL)

	info->supports_khr_cl_event = HAS_EXTENSION(KHR_cl_event);
	
	info->supports_khr_cl_event2 = HAS_EXTENSION(KHR_cl_event2) &&
		lib->eglCreateSync64KHR;

	info->supports_khr_client_get_all_proc_addresses = HAS_EXTENSION(KHR_client_get_all_proc_addresses);

	info->supports_khr_config_attribs = HAS_EXTENSION(KHR_config_attribs);

	info->supports_khr_context_flush_control = HAS_EXTENSION(KHR_context_flush_control);

	info->supports_khr_create_context = HAS_EXTENSION(KHR_create_context);

	info->supports_khr_create_context_no_error = HAS_EXTENSION(KHR_create_context_no_error);

	info->supports_khr_debug = HAS_EXTENSION(KHR_debug) &&
		lib->eglDebugMessageControlKHR &&
		lib->eglQueryDebugKHR &&
		lib->eglLabelObjectKHR;

	info->supports_khr_display_reference = HAS_EXTENSION(KHR_display_reference) &&
		lib->eglQueryDisplayAttribKHR;

	info->supports_khr_fence_sync = HAS_EXTENSION(KHR_fence_sync) &&
		lib->eglCreateSyncKHR &&
		lib->eglDestroySyncKHR &&
		lib->eglClientWaitSyncKHR &&
		lib->eglGetSyncAttribKHR;

	info->supports_khr_get_all_proc_addresses = HAS_EXTENSION(KHR_get_all_proc_addresses);

	info->supports_khr_gl_colorspace = HAS_EXTENSION(KHR_gl_colorspace);

	info->supports_khr_gl_renderbuffer_image = HAS_EXTENSION(KHR_gl_renderbuffer_image);

	info->supports_khr_gl_texture_2d_image = HAS_EXTENSION(KHR_gl_texture_2d_image);

	info->supports_khr_gl_texture_3d_image = HAS_EXTENSION(KHR_gl_texture_3d_image);

	info->supports_khr_gl_texture_cubemap_image = HAS_EXTENSION(KHR_gl_texture_cubemap_image);

	info->supports_khr_image = HAS_EXTENSION(KHR_image) &&
		lib->eglCreateImageKHR &&
		lib->eglDestroyImageKHR;

	info->supports_khr_image_base = HAS_EXTENSION(KHR_image_base);

	info->supports_khr_image_pixmap = HAS_EXTENSION(KHR_image_pixmap);

	info->supports_khr_lock_surface = HAS_EXTENSION(KHR_lock_surface) &&
		lib->eglLockSurfaceKHR &&
		lib->eglUnlockSurfaceKHR;

	info->supports_khr_lock_surface2 = HAS_EXTENSION(KHR_lock_surface2);

	info->supports_khr_lock_surface3 = HAS_EXTENSION(KHR_lock_surface3) &&
		lib->eglQuerySurface64KHR;

	info->supports_khr_mutable_render_buffer = HAS_EXTENSION(KHR_mutable_render_buffer);

	info->supports_khr_no_config_context = HAS_EXTENSION(KHR_no_config_context);

	info->supports_khr_partial_update = HAS_EXTENSION(KHR_partial_update) &&
		lib->eglSetDamageRegionKHR;

	info->supports_khr_platform_android = HAS_EXTENSION(KHR_platform_android);

	info->supports_khr_platform_gbm = HAS_EXTENSION(KHR_platform_gbm);

	info->supports_khr_platform_wayland = HAS_EXTENSION(KHR_platform_wayland);

	info->supports_khr_platform_x11 = HAS_EXTENSION(KHR_platform_x11);

	info->supports_khr_reusable_sync = HAS_EXTENSION(KHR_reusable_sync) &&
		lib->eglSignalSyncKHR;

	info->supports_khr_stream = HAS_EXTENSION(KHR_stream) &&
		lib->eglCreateStreamKHR &&
		lib->eglDestroyStreamKHR &&
		lib->eglStreamAttribKHR &&
		lib->eglQueryStreamKHR &&
		lib->eglQueryStreamu64KHR;

	info->supports_khr_stream_attrib = HAS_EXTENSION(KHR_stream_attrib) &&
		lib->eglCreateStreamAttribKHR &&
		lib->eglSetStreamAttribKHR &&
		lib->eglQueryStreamAttribKHR &&
		lib->eglStreamConsumerAcquireAttribKHR &&
		lib->eglStreamConsumerReleaseAttribKHR;

	info->supports_khr_stream_consumer_gltexture = HAS_EXTENSION(KHR_stream_consumer_gltexture) &&
		lib->eglStreamConsumerGLTextureExternalKHR &&
		lib->eglStreamConsumerAcquireKHR &&
		lib->eglStreamConsumerReleaseKHR;

	info->supports_khr_stream_cross_process_fd = HAS_EXTENSION(KHR_stream_cross_process_fd) &&
		lib->eglGetStreamFileDescriptorKHR &&
		lib->eglCreateStreamFromFileDescriptorKHR;

	info->supports_khr_stream_fifo = HAS_EXTENSION(KHR_stream_fifo) &&
		lib->eglQueryStreamTimeKHR;

	info->supports_khr_stream_producer_aldatalocator = HAS_EXTENSION(KHR_stream_producer_aldatalocator);

	info->supports_khr_stream_producer_eglsurface = HAS_EXTENSION(KHR_stream_producer_eglsurface) &&
		lib->eglCreateStreamProducerSurfaceKHR;

	info->supports_khr_surfaceless_context = HAS_EXTENSION(KHR_surfaceless_context);

	info->supports_khr_swap_buffers_with_damage = HAS_EXTENSION(KHR_swap_buffers_with_damage) &&
		lib->eglSwapBuffersWithDamageKHR;

	info->supports_khr_vg_parent_image = HAS_EXTENSION(KHR_vg_parent_image);

	info->supports_khr_wait_sync = HAS_EXTENSION(KHR_wait_sync) &&
		lib->eglWaitSyncKHR;

	info->supports_android_gles_layers = HAS_EXTENSION(ANDroid_gles_layers);

	info->supports_android_blob_cache = HAS_EXTENSION(ANDroid_blob_cache) &&
		lib->eglSetBlobCacheFuncsANDROID;

	info->supports_android_create_native_client_buffer = HAS_EXTENSION(ANDroid_create_native_client_buffer) &&
		lib->eglCreateNativeClientBufferANDROID;

	info->supports_android_framebuffer_target = HAS_EXTENSION(ANDroid_framebuffer_target);

	info->supports_android_front_buffer_auto_refresh = HAS_EXTENSION(ANDroid_front_buffer_auto_refresh);

	info->supports_android_get_frame_timestamps = HAS_EXTENSION(ANDroid_get_frame_timestamps) &&
		lib->eglGetCompositorTimingSupportedANDROID &&
		lib->eglGetCompositorTimingANDROID &&
		lib->eglGetNextFrameIdANDROID &&
		lib->eglGetFrameTimestampSupportedANDROID &&
		lib->eglGetFrameTimestampsANDROID;
	
	info->supports_android_get_native_client_buffer = HAS_EXTENSION(ANDroid_get_native_client_buffer) &&
		lib->eglGetNativeClientBufferANDROID;

	info->supports_android_image_native_buffer = HAS_EXTENSION(ANDroid_image_native_buffer);

	info->supports_android_native_fence_sync = HAS_EXTENSION(ANDroid_native_fence_sync) &&
		lib->eglDupNativeFenceFDANDROID;

	info->supports_android_presentation_time = HAS_EXTENSION(ANDroid_presentation_time) &&
		lib->eglPresentationTimeANDROID;

	info->supports_android_recordable = HAS_EXTENSION(ANDroid_recordable);

	info->supports_angle_d3d_share_handle_client_buffer = HAS_EXTENSION(ANGle_d3d_share_handle_client_buffer);

	info->supports_angle_device_d3d = HAS_EXTENSION(ANGle_device_d3d);

	info->supports_angle_query_surface_pointer = HAS_EXTENSION(ANGle_query_surface_pointer) &&
		lib->eglQuerySurfacePointerANGLE;

	info->supports_angle_surface_d3d_texture_2d_share_handle = HAS_EXTENSION(ANGle_surface_d3d_texture_2d_share_handle);

	info->supports_angle_window_fixed_size = HAS_EXTENSION(ANGle_window_fixed_size);

	info->supports_arm_image_format = HAS_EXTENSION(ARM_image_format);

	info->supports_arm_implicit_external_sync = HAS_EXTENSION(ARM_implicit_external_sync);

	info->supports_arm_pixmap_multisample_discard = HAS_EXTENSION(ARM_pixmap_multisample_discard);

	info->supports_ext_bind_to_front = HAS_EXTENSION(EXT_bind_to_front);

	info->supports_ext_buffer_age = HAS_EXTENSION(EXT_buffer_age);

	info->supports_ext_client_extensions = HAS_EXTENSION(EXT_client_extensions);

	info->supports_ext_client_sync = HAS_EXTENSION(EXT_client_sync) &&
		lib->eglClientSignalSyncEXT;

	info->supports_ext_compositor = HAS_EXTENSION(EXT_compositor) &&
		lib->eglCompositorSetContextListEXT &&
		lib->eglCompositorSetContextAttributesEXT &&
		lib->eglCompositorSetWindowListEXT &&
		lib->eglCompositorSetWindowAttributesEXT &&
		lib->eglCompositorBindTexWindowEXT &&
		lib->eglCompositorSetSizeEXT &&
		lib->eglCompositorSwapPolicyEXT;

	info->supports_ext_create_context_robustness = HAS_EXTENSION(EXT_create_context_robustness);

	info->supports_ext_device_base = HAS_EXTENSION(EXT_device_base) &&
		lib->eglQueryDeviceAttribEXT &&
		lib->eglQueryDeviceStringEXT &&
		lib->eglQueryDevicesEXT &&
		lib->eglQueryDisplayAttribEXT;

	info->supports_ext_device_drm = HAS_EXTENSION(EXT_device_drm);

	info->supports_ext_device_enumeration = HAS_EXTENSION(EXT_device_enumeration);

	info->supports_ext_device_openwf = HAS_EXTENSION(EXT_device_openwf);

	info->supports_ext_device_query = HAS_EXTENSION(EXT_device_query);

	info->supports_ext_gl_colorspace_bt2020_linear = HAS_EXTENSION(EXT_gl_colorspace_bt2020_linear);

	info->supports_ext_gl_colorspace_bt2020_pq = HAS_EXTENSION(EXT_gl_colorspace_bt2020_pq);

	info->supports_ext_gl_colorspace_display_p3 = HAS_EXTENSION(EXT_gl_colorspace_display_p3);

	info->supports_ext_gl_colorspace_display_p3_linear = HAS_EXTENSION(EXT_gl_colorspace_display_p3_linear);

	info->supports_ext_gl_colorspace_display_p3_passthrough = HAS_EXTENSION(EXT_gl_colorspace_display_p3_passthrough);

	info->supports_ext_gl_colorspace_scrgb = HAS_EXTENSION(EXT_gl_colorspace_scrgb);

	info->supports_ext_gl_colorspace_scrgb_linear = HAS_EXTENSION(EXT_gl_colorspace_scrgb_linear);

	info->supports_ext_image_dma_buf_import = HAS_EXTENSION(EXT_image_dma_buf_import);

	info->supports_ext_image_dma_buf_import_modifiers = HAS_EXTENSION(EXT_image_dma_buf_import_modifiers) &&
		lib->eglQueryDmaBufFormatsEXT &&
		lib->eglQueryDmaBufModifiersEXT;

	info->supports_ext_image_gl_colorspace = HAS_EXTENSION(EXT_image_gl_colorspace);

	info->supports_ext_image_implicit_sync_control = HAS_EXTENSION(EXT_image_implicit_sync_control);

	info->supports_ext_multiview_window = HAS_EXTENSION(EXT_multiview_window);

	info->supports_ext_output_base = HAS_EXTENSION(EXT_output_base) &&
		lib->eglGetOutputLayersEXT &&
		lib->eglGetOutputPortsEXT &&
		lib->eglOutputLayerAttribEXT &&
		lib->eglQueryOutputLayerAttribEXT &&
		lib->eglQueryOutputLayerStringEXT &&
		lib->eglOutputPortAttribEXT &&
		lib->eglQueryOutputPortAttribEXT &&
		lib->eglQueryOutputPortStringEXT;

	info->supports_ext_output_drm = HAS_EXTENSION(EXT_output_drm);

	info->supports_ext_output_openwf = HAS_EXTENSION(EXT_output_openwf);

	info->supports_ext_pixel_format_float = HAS_EXTENSION(EXT_pixel_format_float);

	info->supports_ext_platform_base = HAS_EXTENSION(EXT_platform_base) &&
		lib->eglGetPlatformDisplayEXT &&
		lib->eglCreatePlatformWindowSurfaceEXT &&
		lib->eglCreatePlatformPixmapSurfaceEXT;

	info->supports_ext_platform_device = HAS_EXTENSION(EXT_platform_device);

	info->supports_ext_platform_wayland = HAS_EXTENSION(EXT_platform_wayland);

	info->supports_ext_platform_x11 = HAS_EXTENSION(EXT_platform_x11);

	info->supports_mesa_platform_xcb = HAS_EXTENSION(MESa_platform_xcb);

	info->supports_ext_protected_content = HAS_EXTENSION(EXT_protected_content);

	info->supports_ext_protected_surface = HAS_EXTENSION(EXT_protected_surface);

	info->supports_ext_stream_consumer_egloutput = HAS_EXTENSION(EXT_stream_consumer_egloutput) &&
		lib->eglStreamConsumerOutputEXT;

	info->supports_ext_surface_cta861_3_metadata = HAS_EXTENSION(EXT_surface_cta861_3_metadata);

	info->supports_ext_surface_smpte2086_metadata = HAS_EXTENSION(EXT_surface_smpte2086_metadata);

	info->supports_ext_swap_buffers_with_damage = HAS_EXTENSION(EXT_swap_buffers_with_damage) &&
		lib->eglSwapBuffersWithDamageEXT;

	info->supports_ext_sync_reuse = HAS_EXTENSION(EXT_sync_reuse) &&
		lib->eglUnsignalSyncEXT;

	info->supports_ext_yuv_surface = HAS_EXTENSION(EXT_yuv_surface);

	info->supports_hi_clientpixmap = HAS_EXTENSION(HI_clientpixmap) &&
		lib->eglCreatePixmapSurfaceHI;

	info->supports_hi_colorformats = HAS_EXTENSION(HI_colorformats);

	info->supports_img_context_priority = HAS_EXTENSION(IMG_context_priority);

	info->supports_img_image_plane_attribs = HAS_EXTENSION(IMG_image_plane_attribs);

	info->supports_mesa_drm_image = HAS_EXTENSION(MESA_drm_image) &&
		lib->eglCreateDRMImageMESA &&
		lib->eglExportDRMImageMESA;

	info->supports_mesa_image_dma_buf_export = HAS_EXTENSION(MESA_image_dma_buf_export) &&
		lib->eglExportDMABUFImageQueryMESA &&
		lib->eglExportDMABUFImageMESA;

	info->supports_mesa_platform_gbm = HAS_EXTENSION(MESA_platform_gbm);

	info->supports_mesa_platform_surfaceless = HAS_EXTENSION(MESA_platform_surfaceless);

	info->supports_mesa_query_driver = HAS_EXTENSION(MESA_query_driver) &&
		lib->eglGetDisplayDriverConfig &&
		lib->eglGetDisplayDriverName;

	info->supports_nok_swap_region = HAS_EXTENSION(NOK_swap_region) &&
		lib->eglSwapBuffersRegionNOK;

	info->supports_nok_swap_region2 = HAS_EXTENSION(NOK_swap_region2) &&
		lib->eglSwapBuffersRegion2NOK;

	info->supports_nok_texture_from_pixmap = HAS_EXTENSION(NOK_texture_from_pixmap);

	info->supports_nv_3dvision_surface = HAS_EXTENSION(NV_3dvision_surface);

	info->supports_nv_context_priority_realtime = HAS_EXTENSION(NV_context_priority_realtime);

	info->supports_nv_coverage_sample = HAS_EXTENSION(NV_coverage_sample);

	info->supports_nv_coverage_sample_resolve = HAS_EXTENSION(NV_coverage_sample_resolve);

	info->supports_nv_cuda_event = HAS_EXTENSION(NV_cuda_event);

	info->supports_nv_depth_nonlinear = HAS_EXTENSION(NV_depth_nonlinear);

	info->supports_nv_device_cuda = HAS_EXTENSION(NV_device_cuda);

	info->supports_nv_native_query = HAS_EXTENSION(NV_native_query) &&
		lib->eglQueryNativeDisplayNV &&
		lib->eglQueryNativeWindowNV &&
		lib->eglQueryNativePixmapNV;

	info->supports_nv_post_convert_rounding = HAS_EXTENSION(NV_post_convert_rounding);

	info->supports_nv_post_sub_buffer = HAS_EXTENSION(NV_post_sub_buffer) &&
		lib->eglPostSubBufferNV;

	info->supports_nv_quadruple_buffer = HAS_EXTENSION(NV_quadruple_buffer);

	info->supports_nv_robustness_video_memory_purge = HAS_EXTENSION(NV_robustness_video_memory_purge);

	info->supports_nv_stream_consumer_gltexture_yuv = HAS_EXTENSION(NV_stream_consumer_gltexture_yuv) &&
		lib->eglStreamConsumerGLTextureExternalAttribsNV;

	info->supports_nv_stream_cross_display = HAS_EXTENSION(NV_stream_cross_display);

	info->supports_nv_stream_cross_object = HAS_EXTENSION(NV_stream_cross_object);

	info->supports_nv_stream_cross_partition = HAS_EXTENSION(NV_stream_cross_partition);

	info->supports_nv_stream_cross_process = HAS_EXTENSION(NV_stream_cross_process);

	info->supports_nv_stream_cross_system = HAS_EXTENSION(NV_stream_cross_system);

	info->supports_nv_stream_dma = HAS_EXTENSION(NV_stream_dma);

	info->supports_nv_stream_fifo_next = HAS_EXTENSION(NV_stream_fifo_next);

	info->supports_nv_stream_fifo_synchronous = HAS_EXTENSION(NV_stream_fifo_synchronous);

	info->supports_nv_stream_flush = HAS_EXTENSION(NV_stream_flush) &&
		lib->eglStreamFlushNV;

	info->supports_nv_stream_frame_limits = HAS_EXTENSION(NV_stream_frame_limits);

	info->supports_nv_stream_metadata = HAS_EXTENSION(NV_stream_metadata) &&
		lib->eglQueryDisplayAttribNV &&
		lib->eglSetStreamMetadataNV &&
		lib->eglQueryStreamMetadataNV;

	info->supports_nv_stream_origin = HAS_EXTENSION(NV_stream_origin);

	info->supports_nv_stream_remote = HAS_EXTENSION(NV_stream_remote);

	info->supports_nv_stream_reset = HAS_EXTENSION(NV_stream_reset) &&
		lib->eglResetStreamNV;

	info->supports_nv_stream_socket = HAS_EXTENSION(NV_stream_socket);

	info->supports_nv_stream_socket_inet = HAS_EXTENSION(NV_stream_socket_inet);

	info->supports_nv_stream_socket_unix = HAS_EXTENSION(NV_stream_socket_unix);

	info->supports_nv_stream_sync = HAS_EXTENSION(NV_stream_sync) &&
		lib->eglCreateStreamSyncNV;

	info->supports_nv_sync = HAS_EXTENSION(NV_sync) &&
		lib->eglCreateFenceSyncNV &&
		lib->eglDestroySyncNV &&
		lib->eglFenceNV &&
		lib->eglClientWaitSyncNV &&
		lib->eglSignalSyncNV &&
		lib->eglGetSyncAttribNV;

	info->supports_nv_system_time = HAS_EXTENSION(NV_system_time) &&
		lib->eglGetSystemTimeFrequencyNV &&
		lib->eglGetSystemTimeNV;

	info->supports_nv_triple_buffer = HAS_EXTENSION(NV_triple_buffer);

	info->supports_tizen_image_native_buffer = HAS_EXTENSION(TIZen_image_native_buffer);

	info->supports_tizen_image_native_surface = HAS_EXTENSION(TIZen_image_native_surface);

	info->supports_wl_bind_wayland_display = HAS_EXTENSION(WL_bind_wayland_display) &&
		lib->eglBindWaylandDisplayWL &&
		lib->eglUnbindWaylandDisplayWL &&
		lib->eglQueryWaylandBufferWL;

	info->supports_wl_create_wayland_buffer_from_image = HAS_EXTENSION(WL_create_wayland_buffer_from_image) &&
		lib->eglCreateWaylandBufferFromImageWL;
	
#	undef HAS_EXTENSION

	return info;
}

void egl_client_info_destroy(struct egl_client_info *client_info) {
	free(client_info);
}


struct egl_display_info *egl_display_info_new(struct libegl *lib, EGLint major, EGLint minor, EGLDisplay display) {
	struct egl_display_info *info;

	info = malloc(sizeof *info);
	if (info == NULL) {
		return NULL;
	}

	info->major = major;
	info->minor = minor;

	info->client_extensions = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
	if (info->client_extensions == NULL) {
		free(info);
		return NULL;
	}

	info->display_extensions = eglQueryString(display, EGL_EXTENSIONS);
	if (info->display_extensions == NULL) {
		free(info);
		return NULL;
	}

#	define HAS_EXTENSION(name) \
		((strstr(info->client_extensions, #name) != NULL) || (strstr(info->display_extensions, #name) != NULL))

	info->supports_11 = ((major == 1 && minor >= 1) || (major > 1)) &&
		lib->eglBindTexImage &&
		lib->eglReleaseTexImage &&
		lib->eglSurfaceAttrib &&
		lib->eglSwapInterval;

	info->supports_12 = ((major == 1 && minor >= 2) || (major > 1)) &&
		lib->eglBindAPI &&
		lib->eglQueryAPI &&
		lib->eglCreatePbufferFromClientBuffer &&
		lib->eglReleaseThread &&
		lib->eglWaitClient;

	info->supports_13 = (major == 1 && minor >= 3) || (major > 1);

	info->supports_14 = ((major == 1 && minor >= 4) || (major > 1)) &&
		lib->eglGetCurrentContext;

	info->supports_15 = ((major == 1 && minor >= 5) || (major > 1)) &&
		lib->eglCreateSync &&
		lib->eglDestroySync &&
		lib->eglClientWaitSync &&
		lib->eglGetSyncAttrib &&
		lib->eglCreateImage &&
		lib->eglDestroyImage &&
		lib->eglGetPlatformDisplay &&
		lib->eglCreatePlatformWindowSurface &&
		lib->eglCreatePlatformPixmapSurface &&
		lib->eglWaitSync;

	info->supports_khr_cl_event = HAS_EXTENSION(KHR_cl_event);
	
	info->supports_khr_cl_event2 = HAS_EXTENSION(KHR_cl_event2) &&
		lib->eglCreateSync64KHR;

	info->supports_khr_client_get_all_proc_addresses = HAS_EXTENSION(KHR_client_get_all_proc_addresses);

	info->supports_khr_config_attribs = HAS_EXTENSION(KHR_config_attribs);

	info->supports_khr_context_flush_control = HAS_EXTENSION(KHR_context_flush_control);

	info->supports_khr_create_context = HAS_EXTENSION(KHR_create_context);

	info->supports_khr_create_context_no_error = HAS_EXTENSION(KHR_create_context_no_error);

	info->supports_khr_debug = HAS_EXTENSION(KHR_debug) &&
		lib->eglDebugMessageControlKHR &&
		lib->eglQueryDebugKHR &&
		lib->eglLabelObjectKHR;

	info->supports_khr_display_reference = HAS_EXTENSION(KHR_display_reference) &&
		lib->eglQueryDisplayAttribKHR;

	info->supports_khr_fence_sync = HAS_EXTENSION(KHR_fence_sync) &&
		lib->eglCreateSyncKHR &&
		lib->eglDestroySyncKHR &&
		lib->eglClientWaitSyncKHR &&
		lib->eglGetSyncAttribKHR;

	info->supports_khr_get_all_proc_addresses = HAS_EXTENSION(KHR_get_all_proc_addresses);

	info->supports_khr_gl_colorspace = HAS_EXTENSION(KHR_gl_colorspace);

	info->supports_khr_gl_renderbuffer_image = HAS_EXTENSION(KHR_gl_renderbuffer_image);

	info->supports_khr_gl_texture_2d_image = HAS_EXTENSION(KHR_gl_texture_2d_image);

	info->supports_khr_gl_texture_3d_image = HAS_EXTENSION(KHR_gl_texture_3d_image);

	info->supports_khr_gl_texture_cubemap_image = HAS_EXTENSION(KHR_gl_texture_cubemap_image);

	info->supports_khr_image = HAS_EXTENSION(KHR_image) &&
		lib->eglCreateImageKHR &&
		lib->eglDestroyImageKHR;

	info->supports_khr_image_base = HAS_EXTENSION(KHR_image_base);

	info->supports_khr_image_pixmap = HAS_EXTENSION(KHR_image_pixmap);

	info->supports_khr_lock_surface = HAS_EXTENSION(KHR_lock_surface) &&
		lib->eglLockSurfaceKHR &&
		lib->eglUnlockSurfaceKHR;

	info->supports_khr_lock_surface2 = HAS_EXTENSION(KHR_lock_surface2);

	info->supports_khr_lock_surface3 = HAS_EXTENSION(KHR_lock_surface3) &&
		lib->eglQuerySurface64KHR;

	info->supports_khr_mutable_render_buffer = HAS_EXTENSION(KHR_mutable_render_buffer);

	info->supports_khr_no_config_context = HAS_EXTENSION(KHR_no_config_context);

	info->supports_khr_partial_update = HAS_EXTENSION(KHR_partial_update) &&
		lib->eglSetDamageRegionKHR;

	info->supports_khr_platform_android = HAS_EXTENSION(KHR_platform_android);

	info->supports_khr_platform_gbm = HAS_EXTENSION(KHR_platform_gbm);

	info->supports_khr_platform_wayland = HAS_EXTENSION(KHR_platform_wayland);

	info->supports_khr_platform_x11 = HAS_EXTENSION(KHR_platform_x11);

	info->supports_khr_reusable_sync = HAS_EXTENSION(KHR_reusable_sync) &&
		lib->eglSignalSyncKHR;

	info->supports_khr_stream = HAS_EXTENSION(KHR_stream) &&
		lib->eglCreateStreamKHR &&
		lib->eglDestroyStreamKHR &&
		lib->eglStreamAttribKHR &&
		lib->eglQueryStreamKHR &&
		lib->eglQueryStreamu64KHR;

	info->supports_khr_stream_attrib = HAS_EXTENSION(KHR_stream_attrib) &&
		lib->eglCreateStreamAttribKHR &&
		lib->eglSetStreamAttribKHR &&
		lib->eglQueryStreamAttribKHR &&
		lib->eglStreamConsumerAcquireAttribKHR &&
		lib->eglStreamConsumerReleaseAttribKHR;

	info->supports_khr_stream_consumer_gltexture = HAS_EXTENSION(KHR_stream_consumer_gltexture) &&
		lib->eglStreamConsumerGLTextureExternalKHR &&
		lib->eglStreamConsumerAcquireKHR &&
		lib->eglStreamConsumerReleaseKHR;

	info->supports_khr_stream_cross_process_fd = HAS_EXTENSION(KHR_stream_cross_process_fd) &&
		lib->eglGetStreamFileDescriptorKHR &&
		lib->eglCreateStreamFromFileDescriptorKHR;

	info->supports_khr_stream_fifo = HAS_EXTENSION(KHR_stream_fifo) &&
		lib->eglQueryStreamTimeKHR;

	info->supports_khr_stream_producer_aldatalocator = HAS_EXTENSION(KHR_stream_producer_aldatalocator);

	info->supports_khr_stream_producer_eglsurface = HAS_EXTENSION(KHR_stream_producer_eglsurface) &&
		lib->eglCreateStreamProducerSurfaceKHR;

	info->supports_khr_surfaceless_context = HAS_EXTENSION(KHR_surfaceless_context);

	info->supports_khr_swap_buffers_with_damage = HAS_EXTENSION(KHR_swap_buffers_with_damage) &&
		lib->eglSwapBuffersWithDamageKHR;

	info->supports_khr_vg_parent_image = HAS_EXTENSION(KHR_vg_parent_image);

	info->supports_khr_wait_sync = HAS_EXTENSION(KHR_wait_sync) &&
		lib->eglWaitSyncKHR;

	info->supports_android_gles_layers = HAS_EXTENSION(ANDroid_gles_layers);

	info->supports_android_blob_cache = HAS_EXTENSION(ANDroid_blob_cache) &&
		lib->eglSetBlobCacheFuncsANDROID;

	info->supports_android_create_native_client_buffer = HAS_EXTENSION(ANDroid_create_native_client_buffer) &&
		lib->eglCreateNativeClientBufferANDROID;

	info->supports_android_framebuffer_target = HAS_EXTENSION(ANDroid_framebuffer_target);

	info->supports_android_front_buffer_auto_refresh = HAS_EXTENSION(ANDroid_front_buffer_auto_refresh);

	info->supports_android_get_frame_timestamps = HAS_EXTENSION(ANDroid_get_frame_timestamps) &&
		lib->eglGetCompositorTimingSupportedANDROID &&
		lib->eglGetCompositorTimingANDROID &&
		lib->eglGetNextFrameIdANDROID &&
		lib->eglGetFrameTimestampSupportedANDROID &&
		lib->eglGetFrameTimestampsANDROID;
	
	info->supports_android_get_native_client_buffer = HAS_EXTENSION(ANDroid_get_native_client_buffer) &&
		lib->eglGetNativeClientBufferANDROID;

	info->supports_android_image_native_buffer = HAS_EXTENSION(ANDroid_image_native_buffer);

	info->supports_android_native_fence_sync = HAS_EXTENSION(ANDroid_native_fence_sync) &&
		lib->eglDupNativeFenceFDANDROID;

	info->supports_android_presentation_time = HAS_EXTENSION(ANDroid_presentation_time) &&
		lib->eglPresentationTimeANDROID;

	info->supports_android_recordable = HAS_EXTENSION(ANDroid_recordable);

	info->supports_angle_d3d_share_handle_client_buffer = HAS_EXTENSION(ANGle_d3d_share_handle_client_buffer);

	info->supports_angle_device_d3d = HAS_EXTENSION(ANGle_device_d3d);

	info->supports_angle_query_surface_pointer = HAS_EXTENSION(ANGle_query_surface_pointer) &&
		lib->eglQuerySurfacePointerANGLE;

	info->supports_angle_surface_d3d_texture_2d_share_handle = HAS_EXTENSION(ANGle_surface_d3d_texture_2d_share_handle);

	info->supports_angle_window_fixed_size = HAS_EXTENSION(ANGle_window_fixed_size);

	info->supports_arm_image_format = HAS_EXTENSION(ARM_image_format);

	info->supports_arm_implicit_external_sync = HAS_EXTENSION(ARM_implicit_external_sync);

	info->supports_arm_pixmap_multisample_discard = HAS_EXTENSION(ARM_pixmap_multisample_discard);

	info->supports_ext_bind_to_front = HAS_EXTENSION(EXT_bind_to_front);

	info->supports_ext_buffer_age = HAS_EXTENSION(EXT_buffer_age);

	info->supports_ext_client_extensions = HAS_EXTENSION(EXT_client_extensions);

	info->supports_ext_client_sync = HAS_EXTENSION(EXT_client_sync) &&
		lib->eglClientSignalSyncEXT;

	info->supports_ext_compositor = HAS_EXTENSION(EXT_compositor) &&
		lib->eglCompositorSetContextListEXT &&
		lib->eglCompositorSetContextAttributesEXT &&
		lib->eglCompositorSetWindowListEXT &&
		lib->eglCompositorSetWindowAttributesEXT &&
		lib->eglCompositorBindTexWindowEXT &&
		lib->eglCompositorSetSizeEXT &&
		lib->eglCompositorSwapPolicyEXT;

	info->supports_ext_create_context_robustness = HAS_EXTENSION(EXT_create_context_robustness);

	info->supports_ext_device_base = HAS_EXTENSION(EXT_device_base) &&
		lib->eglQueryDeviceAttribEXT &&
		lib->eglQueryDeviceStringEXT &&
		lib->eglQueryDevicesEXT &&
		lib->eglQueryDisplayAttribEXT;

	info->supports_ext_device_drm = HAS_EXTENSION(EXT_device_drm);

	info->supports_ext_device_enumeration = HAS_EXTENSION(EXT_device_enumeration);

	info->supports_ext_device_openwf = HAS_EXTENSION(EXT_device_openwf);

	info->supports_ext_device_query = HAS_EXTENSION(EXT_device_query);

	info->supports_ext_gl_colorspace_bt2020_linear = HAS_EXTENSION(EXT_gl_colorspace_bt2020_linear);

	info->supports_ext_gl_colorspace_bt2020_pq = HAS_EXTENSION(EXT_gl_colorspace_bt2020_pq);

	info->supports_ext_gl_colorspace_display_p3 = HAS_EXTENSION(EXT_gl_colorspace_display_p3);

	info->supports_ext_gl_colorspace_display_p3_linear = HAS_EXTENSION(EXT_gl_colorspace_display_p3_linear);

	info->supports_ext_gl_colorspace_display_p3_passthrough = HAS_EXTENSION(EXT_gl_colorspace_display_p3_passthrough);

	info->supports_ext_gl_colorspace_scrgb = HAS_EXTENSION(EXT_gl_colorspace_scrgb);

	info->supports_ext_gl_colorspace_scrgb_linear = HAS_EXTENSION(EXT_gl_colorspace_scrgb_linear);

	info->supports_ext_image_dma_buf_import = HAS_EXTENSION(EXT_image_dma_buf_import);

	info->supports_ext_image_dma_buf_import_modifiers = HAS_EXTENSION(EXT_image_dma_buf_import_modifiers) &&
		lib->eglQueryDmaBufFormatsEXT &&
		lib->eglQueryDmaBufModifiersEXT;

	info->supports_ext_image_gl_colorspace = HAS_EXTENSION(EXT_image_gl_colorspace);

	info->supports_ext_image_implicit_sync_control = HAS_EXTENSION(EXT_image_implicit_sync_control);

	info->supports_ext_multiview_window = HAS_EXTENSION(EXT_multiview_window);

	info->supports_ext_output_base = HAS_EXTENSION(EXT_output_base) &&
		lib->eglGetOutputLayersEXT &&
		lib->eglGetOutputPortsEXT &&
		lib->eglOutputLayerAttribEXT &&
		lib->eglQueryOutputLayerAttribEXT &&
		lib->eglQueryOutputLayerStringEXT &&
		lib->eglOutputPortAttribEXT &&
		lib->eglQueryOutputPortAttribEXT &&
		lib->eglQueryOutputPortStringEXT;

	info->supports_ext_output_drm = HAS_EXTENSION(EXT_output_drm);

	info->supports_ext_output_openwf = HAS_EXTENSION(EXT_output_openwf);

	info->supports_ext_pixel_format_float = HAS_EXTENSION(EXT_pixel_format_float);

	info->supports_ext_platform_base = HAS_EXTENSION(EXT_platform_base) &&
		lib->eglGetPlatformDisplayEXT &&
		lib->eglCreatePlatformWindowSurfaceEXT &&
		lib->eglCreatePlatformPixmapSurfaceEXT;

	info->supports_ext_platform_device = HAS_EXTENSION(EXT_platform_device);

	info->supports_ext_platform_wayland = HAS_EXTENSION(EXT_platform_wayland);

	info->supports_ext_platform_x11 = HAS_EXTENSION(EXT_platform_x11);

	info->supports_mesa_platform_xcb = HAS_EXTENSION(MESa_platform_xcb);

	info->supports_ext_protected_content = HAS_EXTENSION(EXT_protected_content);

	info->supports_ext_protected_surface = HAS_EXTENSION(EXT_protected_surface);

	info->supports_ext_stream_consumer_egloutput = HAS_EXTENSION(EXT_stream_consumer_egloutput) &&
		lib->eglStreamConsumerOutputEXT;

	info->supports_ext_surface_cta861_3_metadata = HAS_EXTENSION(EXT_surface_cta861_3_metadata);

	info->supports_ext_surface_smpte2086_metadata = HAS_EXTENSION(EXT_surface_smpte2086_metadata);

	info->supports_ext_swap_buffers_with_damage = HAS_EXTENSION(EXT_swap_buffers_with_damage) &&
		lib->eglSwapBuffersWithDamageEXT;

	info->supports_ext_sync_reuse = HAS_EXTENSION(EXT_sync_reuse) &&
		lib->eglUnsignalSyncEXT;

	info->supports_ext_yuv_surface = HAS_EXTENSION(EXT_yuv_surface);

	info->supports_hi_clientpixmap = HAS_EXTENSION(HI_clientpixmap) &&
		lib->eglCreatePixmapSurfaceHI;

	info->supports_hi_colorformats = HAS_EXTENSION(HI_colorformats);

	info->supports_img_context_priority = HAS_EXTENSION(IMG_context_priority);

	info->supports_img_image_plane_attribs = HAS_EXTENSION(IMG_image_plane_attribs);

	info->supports_mesa_drm_image = HAS_EXTENSION(MESA_drm_image) &&
		lib->eglCreateDRMImageMESA &&
		lib->eglExportDRMImageMESA;

	info->supports_mesa_image_dma_buf_export = HAS_EXTENSION(MESA_image_dma_buf_export) &&
		lib->eglExportDMABUFImageQueryMESA &&
		lib->eglExportDMABUFImageMESA;

	info->supports_mesa_platform_gbm = HAS_EXTENSION(MESA_platform_gbm);

	info->supports_mesa_platform_surfaceless = HAS_EXTENSION(MESA_platform_surfaceless);

	info->supports_mesa_query_driver = HAS_EXTENSION(MESA_query_driver) &&
		lib->eglGetDisplayDriverConfig &&
		lib->eglGetDisplayDriverName;

	info->supports_nok_swap_region = HAS_EXTENSION(NOK_swap_region) &&
		lib->eglSwapBuffersRegionNOK;

	info->supports_nok_swap_region2 = HAS_EXTENSION(NOK_swap_region2) &&
		lib->eglSwapBuffersRegion2NOK;

	info->supports_nok_texture_from_pixmap = HAS_EXTENSION(NOK_texture_from_pixmap);

	info->supports_nv_3dvision_surface = HAS_EXTENSION(NV_3dvision_surface);

	info->supports_nv_context_priority_realtime = HAS_EXTENSION(NV_context_priority_realtime);

	info->supports_nv_coverage_sample = HAS_EXTENSION(NV_coverage_sample);

	info->supports_nv_coverage_sample_resolve = HAS_EXTENSION(NV_coverage_sample_resolve);

	info->supports_nv_cuda_event = HAS_EXTENSION(NV_cuda_event);

	info->supports_nv_depth_nonlinear = HAS_EXTENSION(NV_depth_nonlinear);

	info->supports_nv_device_cuda = HAS_EXTENSION(NV_device_cuda);

	info->supports_nv_native_query = HAS_EXTENSION(NV_native_query) &&
		lib->eglQueryNativeDisplayNV &&
		lib->eglQueryNativeWindowNV &&
		lib->eglQueryNativePixmapNV;

	info->supports_nv_post_convert_rounding = HAS_EXTENSION(NV_post_convert_rounding);

	info->supports_nv_post_sub_buffer = HAS_EXTENSION(NV_post_sub_buffer) &&
		lib->eglPostSubBufferNV;

	info->supports_nv_quadruple_buffer = HAS_EXTENSION(NV_quadruple_buffer);

	info->supports_nv_robustness_video_memory_purge = HAS_EXTENSION(NV_robustness_video_memory_purge);

	info->supports_nv_stream_consumer_gltexture_yuv = HAS_EXTENSION(NV_stream_consumer_gltexture_yuv) &&
		lib->eglStreamConsumerGLTextureExternalAttribsNV;

	info->supports_nv_stream_cross_display = HAS_EXTENSION(NV_stream_cross_display);

	info->supports_nv_stream_cross_object = HAS_EXTENSION(NV_stream_cross_object);

	info->supports_nv_stream_cross_partition = HAS_EXTENSION(NV_stream_cross_partition);

	info->supports_nv_stream_cross_process = HAS_EXTENSION(NV_stream_cross_process);

	info->supports_nv_stream_cross_system = HAS_EXTENSION(NV_stream_cross_system);

	info->supports_nv_stream_dma = HAS_EXTENSION(NV_stream_dma);

	info->supports_nv_stream_fifo_next = HAS_EXTENSION(NV_stream_fifo_next);

	info->supports_nv_stream_fifo_synchronous = HAS_EXTENSION(NV_stream_fifo_synchronous);

	info->supports_nv_stream_flush = HAS_EXTENSION(NV_stream_flush) &&
		lib->eglStreamFlushNV;

	info->supports_nv_stream_frame_limits = HAS_EXTENSION(NV_stream_frame_limits);

	info->supports_nv_stream_metadata = HAS_EXTENSION(NV_stream_metadata) &&
		lib->eglQueryDisplayAttribNV &&
		lib->eglSetStreamMetadataNV &&
		lib->eglQueryStreamMetadataNV;

	info->supports_nv_stream_origin = HAS_EXTENSION(NV_stream_origin);

	info->supports_nv_stream_remote = HAS_EXTENSION(NV_stream_remote);

	info->supports_nv_stream_reset = HAS_EXTENSION(NV_stream_reset) &&
		lib->eglResetStreamNV;

	info->supports_nv_stream_socket = HAS_EXTENSION(NV_stream_socket);

	info->supports_nv_stream_socket_inet = HAS_EXTENSION(NV_stream_socket_inet);

	info->supports_nv_stream_socket_unix = HAS_EXTENSION(NV_stream_socket_unix);

	info->supports_nv_stream_sync = HAS_EXTENSION(NV_stream_sync) &&
		lib->eglCreateStreamSyncNV;

	info->supports_nv_sync = HAS_EXTENSION(NV_sync) &&
		lib->eglCreateFenceSyncNV &&
		lib->eglDestroySyncNV &&
		lib->eglFenceNV &&
		lib->eglClientWaitSyncNV &&
		lib->eglSignalSyncNV &&
		lib->eglGetSyncAttribNV;

	info->supports_nv_system_time = HAS_EXTENSION(NV_system_time) &&
		lib->eglGetSystemTimeFrequencyNV &&
		lib->eglGetSystemTimeNV;

	info->supports_nv_triple_buffer = HAS_EXTENSION(NV_triple_buffer);

	info->supports_tizen_image_native_buffer = HAS_EXTENSION(TIZen_image_native_buffer);

	info->supports_tizen_image_native_surface = HAS_EXTENSION(TIZen_image_native_surface);

	info->supports_wl_bind_wayland_display = HAS_EXTENSION(WL_bind_wayland_display) &&
		lib->eglBindWaylandDisplayWL &&
		lib->eglUnbindWaylandDisplayWL &&
		lib->eglQueryWaylandBufferWL;

	info->supports_wl_create_wayland_buffer_from_image = HAS_EXTENSION(WL_create_wayland_buffer_from_image) &&
		lib->eglCreateWaylandBufferFromImageWL;
	
#	undef HAS_EXTENSION

	return info;
}

void egl_display_info_destroy(struct egl_display_info *display_info) {
	free(display_info);
}


struct libgl *libgl_load(gl_proc_resolver_t proc_resolver) {
	struct libgl *libgl;

	libgl = malloc(sizeof *libgl);
	if (libgl == NULL) {
		return NULL;
	}

	libgl->EGLImageTargetRenderbufferStorageOES = proc_resolver("glEGLImageTargetRenderbufferStorageOES");
	DEBUG_ASSERT(libgl->EGLImageTargetRenderbufferStorageOES != NULL);

	return libgl;
}

void libgl_unload(struct libgl *lib) {
	free(lib);
}