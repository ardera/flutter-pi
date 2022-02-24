// SPDX-License-Identifier: MIT
/*
 * Platform Views
 *
 * - implements flutter platform views (for the compositor interface)
 *
 * Copyright (c) 2022, Hannes Winkler <hanneswinkler2000@web.de>
 */


#ifndef _FLUTTERPI_INCLUDE_PLATFORM_VIEW_H
#define _FLUTTERPI_INCLUDE_PLATFORM_VIEW_H

#include <collection.h>

struct platform_view;
struct kms_req_builder;
struct fbdev_commit_builder;

struct platform_view *platform_view_new();

void platform_view_destroy(struct platform_view *view);

DECLARE_REF_OPS(platform_view)

int platform_view_register(struct platform_view *view);

void platform_view_unregister(struct platform_view *view);

int platform_view_present_kms(
    struct platform_view *view,
    const struct fl_layer *layer,
    struct kms_req_builder *builder
);

int platform_view_present_fbdev(
    struct platform_view *view,
    const struct fl_layer *layer,
    struct fbdev_commit_builder *builder
);

#endif // _FLUTTERPI_INCLUDE_PLATFORM_VIEW_H
