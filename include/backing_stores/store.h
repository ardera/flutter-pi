// SPDX-License-Identifier: MIT
/*
 * Backing stores.
 *
 * - are rendering targets for the current rendering method (gl / software)
 * - but also framebuffers the display can scanout
 * - for example, an EGLSurface backed by a GBM surface could be used as a backing store
 * - or a KMS dumb buffer could be a backing store for software rendering
 *
 * Copyright (c) 2021, Hannes Winkler <hanneswinkler2000@web.de>
 */


#ifndef _FLUTTERPI_INCLUDE_BACKING_STORES_STORE_H
#define _FLUTTERPI_INCLUDE_BACKING_STORES_STORE_H

#include <flutter_embedder.h>

#include <collection.h>

struct backing_store;


void backing_store_fill_fl_backing_store(struct backing_store *store, FlutterBackingStore *fl_store);

void backing_store_destroy(struct backing_store *store);

#endif // _FLUTTERPI_INCLUDE_BACKING_STORES_STORE_H
