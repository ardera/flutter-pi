// SPDX-License-Identifier: MIT
/*
 * Cursor Images
 *
 * Contains all the mouse cursor images in compressed form,
 * and some utilities for using them.
 *
 * Copyright (c) 2023, Hannes Winkler <hanneswinkler2000@web.de>
 */

#ifndef _FLUTTERPI_SRC_CURSOR_H
#define _FLUTTERPI_SRC_CURSOR_H

#include <stdint.h>

#include "util/geometry.h"

enum pointer_kind {
    POINTER_KIND_NONE,
    POINTER_KIND_BASIC,
    POINTER_KIND_CLICK,
    POINTER_KIND_FORBIDDEN,
    POINTER_KIND_WAIT,
    POINTER_KIND_PROGRESS,
    POINTER_KIND_CONTEXT_MENU,
    POINTER_KIND_HELP,
    POINTER_KIND_TEXT,
    POINTER_KIND_VERTICAL_TEXT,
    POINTER_KIND_CELL,
    POINTER_KIND_PRECISE,
    POINTER_KIND_MOVE,
    POINTER_KIND_GRAB,
    POINTER_KIND_GRABBING,
    POINTER_KIND_NO_DROP,
    POINTER_KIND_ALIAS,
    POINTER_KIND_COPY,
    POINTER_KIND_DISAPPEARING,
    POINTER_KIND_ALL_SCROLL,
    POINTER_KIND_RESIZE_LEFT_RIGHT,
    POINTER_KIND_RESIZE_UP_DOWN,
    POINTER_KIND_RESIZE_UP_LEFT_DOWN_RIGHT,
    POINTER_KIND_RESIZE_UP_RIGHT_DOWN_LEFT,
    POINTER_KIND_RESIZE_UP,
    POINTER_KIND_RESIZE_DOWN,
    POINTER_KIND_RESIZE_LEFT,
    POINTER_KIND_RESIZE_RIGHT,
    POINTER_KIND_RESIZE_UP_LEFT,
    POINTER_KIND_RESIZE_UP_RIGHT,
    POINTER_KIND_RESIZE_DOWN_LEFT,
    POINTER_KIND_RESIZE_DOWN_RIGHT,
    POINTER_KIND_RESIZE_COLUMN,
    POINTER_KIND_RESIZE_ROW,
    POINTER_KIND_ZOOM_IN,
    POINTER_KIND_ZOOM_OUT
};

struct pointer_icon;

const struct pointer_icon *pointer_icon_for_details(enum pointer_kind kind, double pixel_ratio);

enum pointer_kind pointer_icon_get_kind(const struct pointer_icon *icon);

float pointer_icon_get_pixel_ratio(const struct pointer_icon *icon);

struct vec2i pointer_icon_get_size(const struct pointer_icon *icon);

struct vec2i pointer_icon_get_hotspot(const struct pointer_icon *icon);

void *pointer_icon_dup_pixels(const struct pointer_icon *icon);

#endif  // _FLUTTERPI_SRC_CURSOR_H
