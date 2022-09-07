// SPDX-License-Identifier: MIT
/*
 * Render surface
 * - are special kinds of surfaces that flutter can render into
 * - usually a render surface will have multiple framebuffers internally
 * - the compositor or window will request a framebuffer for flutter to render into
 *   in form of a framebuffer using render_surface_fill.
 * - Once flutter has rendered into that backing store (whatever it's backed by),
 *   the compositor will call render_surface_queue_present on the render surface,
 *   and the argument backing store is the one that was provided using render_surface_fill.
 * - That framebuffer is the one that should be committed when the compositor/window calls surface_present_...
 *
 * Copyright (c) 2022, Hannes Winkler <hanneswinkler2000@web.de>
 */


#ifndef _FLUTTERPI_INCLUDE_RENDER_SURFACE_H
#define _FLUTTERPI_INCLUDE_RENDER_SURFACE_H

#include <collection.h>
#include <flutter_embedder.h>

struct surface;
struct render_surface;

#define CAST_RENDER_SURFACE_UNCHECKED(ptr) ((struct render_surface*) (ptr))
#ifdef DEBUG
#   define CAST_RENDER_SURFACE(ptr) __checked_cast_render_surface(ptr)
ATTR_PURE struct render_surface *__checked_cast_render_surface(void *ptr);
#else
#   define CAST_RENDER_SURFACE(ptr) CAST_RENDER_SURFACE_UNCHECKED(ptr)
#endif

int render_surface_fill(struct render_surface *store, FlutterBackingStore *fl_store);

int render_surface_queue_present(struct render_surface *store, const FlutterBackingStore *fl_store);

#endif // _FLUTTERPI_INCLUDE_BACKING_STORE_H
