#ifndef _FLUTTERPI_H
#define _FLUTTERPI_H

#include <stdbool.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <stdint.h>
#include <flutter_embedder.h>

#define EGL_PLATFORM_GBM_KHR	0x31D7

struct FlutterPiTask {
    struct FlutterPiTask* next;
	bool is_vblank_event;
	union {
		FlutterTask task;
		drmVBlankReply vbl;
	};
    uint64_t target_time;
};

struct TouchscreenSlot {
	int id;
	int x;
	int y;
	FlutterPointerPhase phase;
};

struct drm_fb {
	struct gbm_bo *bo;
	uint32_t fb_id;
};

FlutterEngine engine;

#endif