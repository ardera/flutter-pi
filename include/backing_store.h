// SPDX-License-Identifier: MIT
/*
 * Backing Stores
 *
 * - implements flutter backing stores (used for the compositor interface)
 *
 * Copyright (c) 2022, Hannes Winkler <hanneswinkler2000@web.de>
 */


#ifndef _FLUTTERPI_INCLUDE_BACKING_STORE_H
#define _FLUTTERPI_INCLUDE_BACKING_STORE_H

#include <collection.h>
#include <flutter_embedder.h>

struct surface;
struct backing_store;

#define CAST_BACKING_STORE_UNCHECKED(ptr) ((struct backing_store*) (ptr))
#ifdef DEBUG
#   define CAST_BACKING_STORE(ptr) __checked_cast_backing_store(ptr)
ATTR_PURE struct backing_store *__checked_cast_backing_store(void *ptr);
#else
#   define CAST_BACKING_STORE(ptr) CAST_BACKING_STORE_UNCHECKED(ptr)
#endif

int backing_store_fill_opengl(struct backing_store *store, FlutterOpenGLBackingStore *fl_store);

int backing_store_fill_software(struct backing_store *store, FlutterSoftwareBackingStore *fl_store);

int backing_store_fill_metal(struct backing_store *store, FlutterMetalBackingStore *fl_store);

int backing_store_fill_vulkan(struct backing_store *store, void *fl_store);


#endif // _FLUTTERPI_INCLUDE_BACKING_STORE_H
