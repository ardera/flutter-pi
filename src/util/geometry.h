// SPDX-License-Identifier: MIT
/*
 * Geometry - structs and functions for working with vectors & rectangles
 *
 * Copyright (c) 2023, Hannes Winkler <hanneswinkler2000@web.de>
 */

#ifndef _FLUTTERPI_SRC_UTIL_GEOMETRY_H
#define _FLUTTERPI_SRC_UTIL_GEOMETRY_H

#include <math.h>
#include <stdbool.h>

#include "macros.h"

/**
 * @brief A 2-dimensional vector with 2 float coordinates.
 *
 */
struct vec2f {
    double x, y;
};

#define VEC2F(_x, _y) ((struct vec2f){ .x = (_x), .y = (_y) })

ATTR_CONST static inline struct vec2f vec2f_add(struct vec2f a, struct vec2f b) {
    return VEC2F(a.x + b.x, a.y + b.y);
}

ATTR_CONST static inline struct vec2f vec2f_sub(struct vec2f a, struct vec2f b) {
    return VEC2F(a.x - b.x, a.y - b.y);
}

ATTR_CONST static inline bool vec2f_equals(struct vec2f a, struct vec2f b) {
    return a.x == b.x && a.y == b.y;
}

ATTR_CONST static inline struct vec2f vec2f_round(struct vec2f a) {
    return VEC2F(round(a.x), round(a.y));
}

struct vec2i {
    int x, y;
};

#define VEC2I(_x, _y) ((struct vec2i){ .x = (_x), .y = (_y) })

ATTR_CONST static inline struct vec2i vec2f_round_to_integer(struct vec2f a) {
    return VEC2I((int) round(a.x), (int) round(a.y));
}

ATTR_CONST static inline struct vec2i vec2i_add(struct vec2i a, struct vec2i b) {
    return VEC2I(a.x + b.x, a.y + b.y);
}

ATTR_CONST static inline struct vec2i vec2i_sub(struct vec2i a, struct vec2i b) {
    return VEC2I(a.x - b.x, a.y - b.y);
}

ATTR_CONST static inline struct vec2i vec2i_swap_xy(const struct vec2i point) {
    return VEC2I(point.y, point.x);
}

/**
 * @brief A quadrilateral with 4 2-dimensional float coordinates.
 *
 */
struct quad {
    struct vec2f top_left, top_right, bottom_left, bottom_right;
};

#define QUAD(_top_left, _top_right, _bottom_left, _bottom_right) \
    ((struct quad){                                              \
        .top_left = _top_left,                                   \
        .top_right = _top_right,                                 \
        .bottom_left = _bottom_left,                             \
        .bottom_right = _bottom_right,                           \
    })

#define QUAD_FROM_COORDS(_x1, _y1, _x2, _y2, _x3, _y3, _x4, _y4) QUAD(VEC2F(_x1, _y1), VEC2F(_x2, _y2), VEC2F(_x3, _y3), VEC2F(_x4, _y4))

struct aa_rect {
    struct vec2f offset, size;
};

#define AA_RECT(_offset, _size) \
    ((struct aa_rect){          \
        .offset = offset,       \
        .size = size,           \
    })

#define AA_RECT_FROM_COORDS(offset_x, offset_y, width, height) \
    ((struct aa_rect){                                         \
        .offset = VEC2F(offset_x, offset_y),                   \
        .size = VEC2F(width, height),                          \
    })

ATTR_CONST static inline struct aa_rect quad_get_aa_bounding_rect(const struct quad _rect) {
    double l = MIN4(_rect.top_left.x, _rect.top_right.x, _rect.bottom_left.x, _rect.bottom_right.x);
    double r = MAX4(_rect.top_left.x, _rect.top_right.x, _rect.bottom_left.x, _rect.bottom_right.x);
    double t = MIN4(_rect.top_left.y, _rect.top_right.y, _rect.bottom_left.y, _rect.bottom_right.y);
    double b = MAX4(_rect.top_left.y, _rect.top_right.y, _rect.bottom_left.y, _rect.bottom_right.y);
    return AA_RECT_FROM_COORDS(l, t, r - l, b - t);
}

ATTR_CONST static inline struct vec2f aa_rect_top_left(const struct aa_rect rect) {
    return rect.offset;
}

ATTR_CONST static inline struct vec2f aa_rect_top_right(const struct aa_rect rect) {
    return VEC2F(rect.offset.x + rect.size.x, rect.offset.y);
}

ATTR_CONST static inline struct vec2f aa_rect_bottom_left(const struct aa_rect rect) {
    return VEC2F(rect.offset.x, rect.offset.y + rect.size.y);
}

ATTR_CONST static inline struct vec2f aa_rect_bottom_right(const struct aa_rect rect) {
    return vec2f_add(rect.offset, rect.size);
}

ATTR_CONST static inline struct quad get_quad(const struct aa_rect rect) {
    return (struct quad){
        .top_left = rect.offset,
        .top_right.x = rect.offset.x + rect.size.x,
        .top_right.y = rect.offset.y,
        .bottom_left.x = rect.offset.x,
        .bottom_left.y = rect.offset.y + rect.size.y,
        .bottom_right.x = rect.offset.x + rect.size.x,
        .bottom_right.y = rect.offset.y + rect.size.y,
    };
}

ATTR_CONST static inline bool quad_is_axis_aligned(const struct quad quad) {
    struct aa_rect aa = quad_get_aa_bounding_rect(quad);

    return vec2f_equals(quad.top_left, aa_rect_top_left(aa)) && vec2f_equals(quad.top_right, aa_rect_top_right(aa)) &&
           vec2f_equals(quad.bottom_left, aa_rect_bottom_left(aa)) && vec2f_equals(quad.bottom_right, aa_rect_bottom_right(aa));
}

struct mat3f {
    double scaleX;
    double skewX;
    double transX;
    double skewY;
    double scaleY;
    double transY;
    double pers0;
    double pers1;
    double pers2;
};

#define FLUTTER_TRANSFORM_AS_MAT3F(_t) \
    ((struct mat3f){                   \
        (_t).scaleX,                   \
        (_t).skewX,                    \
        (_t).transX,                   \
        (_t).skewY,                    \
        (_t).scaleY,                   \
        (_t).transY,                   \
        (_t).pers0,                    \
        (_t).pers1,                    \
        (_t).pers2,                    \
    })

#define MAT3F_AS_FLUTTER_TRANSFORM(_t) \
    ((FlutterTransformation){          \
        (_t).scaleX,                   \
        (_t).skewX,                    \
        (_t).transX,                   \
        (_t).skewY,                    \
        (_t).scaleY,                   \
        (_t).transY,                   \
        (_t).pers0,                    \
        (_t).pers1,                    \
        (_t).pers2,                    \
    })

#define MAT3F_TRANSLATION(translate_x, translate_y) \
    ((struct mat3f){                                \
        .scaleX = 1,                                \
        .skewX = 0,                                 \
        .transX = translate_x,                      \
        .skewY = 0,                                 \
        .scaleY = 1,                                \
        .transY = translate_y,                      \
        .pers0 = 0,                                 \
        .pers1 = 0,                                 \
        .pers2 = 1,                                 \
    })

/**
 * @brief A flutter transformation that rotates any coords around the x-axis, counter-clockwise.
 */
#define MAT3F_ROTX(deg)                                  \
    ((struct mat3f){                                     \
        .scaleX = 1,                                     \
        .skewX = 0,                                      \
        .transX = 0,                                     \
        .skewY = 0,                                      \
        .scaleY = cos(((double) (deg)) / 180.0 * M_PI),  \
        .transY = -sin(((double) (deg)) / 180.0 * M_PI), \
        .pers0 = 0,                                      \
        .pers1 = sin(((double) (deg)) / 180.0 * M_PI),   \
        .pers2 = cos(((double) (deg)) / 180.0 * M_PI),   \
    })

/**
 * @brief A flutter transformation that rotates any coords around the y-axis, counter-clockwise.
 */
#define MAT3F_ROTY(deg)                                 \
    ((struct mat3f){                                    \
        .scaleX = cos(((double) (deg)) / 180.0 * M_PI), \
        .skewX = 0,                                     \
        .transX = sin(((double) (deg)) / 180.0 * M_PI), \
        .skewY = 0,                                     \
        .scaleY = 1,                                    \
        .transY = 0,                                    \
        .pers0 = -sin(((double) (deg)) / 180.0 * M_PI), \
        .pers1 = 0,                                     \
        .pers2 = cos(((double) (deg)) / 180.0 * M_PI),  \
    })

/**
 * @brief A flutter transformation that rotates any coords around the z-axis, counter-clockwise.
 */
#define MAT3F_ROTZ(deg)                                 \
    ((struct mat3f){                                    \
        .scaleX = cos(((double) (deg)) / 180.0 * M_PI), \
        .skewX = -sin(((double) (deg)) / 180.0 * M_PI), \
        .transX = 0,                                    \
        .skewY = sin(((double) (deg)) / 180.0 * M_PI),  \
        .scaleY = cos(((double) (deg)) / 180.0 * M_PI), \
        .transY = 0,                                    \
        .pers0 = 0,                                     \
        .pers1 = 0,                                     \
        .pers2 = 1,                                     \
    })

/**
 * @brief Returns a matrix that is the result of matrix-multiplying a with b.
 *
 * @param a The first (lhs) input matrix.
 * @param b The second (rhs) input matrix.
 * @return struct mat3f The product of a x b.
 */
ATTR_CONST static inline struct mat3f multiply_mat3f(const struct mat3f a, const struct mat3f b) {
    return (struct mat3f){
        .scaleX = a.scaleX * b.scaleX + a.skewX * b.skewY + a.transX * b.pers0,
        .skewX = a.scaleX * b.skewX + a.skewX * b.scaleY + a.transX * b.pers1,
        .transX = a.scaleX * b.transX + a.skewX * b.transY + a.transX * b.pers2,
        .skewY = a.skewY * b.scaleX + a.scaleY * b.skewY + a.transY * b.pers0,
        .scaleY = a.skewY * b.skewX + a.scaleY * b.scaleY + a.transY * b.pers1,
        .transY = a.skewY * b.transX + a.scaleY * b.transY + a.transY * b.pers2,
        .pers0 = a.pers0 * b.scaleX + a.pers1 * b.skewY + a.pers2 * b.pers0,
        .pers1 = a.pers0 * b.skewX + a.pers1 * b.scaleY + a.pers2 * b.pers1,
        .pers2 = a.pers0 * b.transX + a.pers1 * b.transY + a.pers2 * b.pers2,
    };
}

/**
 * @brief Returns a matrix that is the result of element-wise addition of a and b.
 *
 * @param a The lhs input matrix.
 * @param b The rhs input matrix.
 * @return struct mat3f The result of a + b. (element-wise)
 */
ATTR_CONST static inline struct mat3f add_mat3f(const struct mat3f a, const struct mat3f b) {
    return (struct mat3f){
        .scaleX = a.scaleX + b.scaleX,
        .skewX = a.skewX + b.skewX,
        .transX = a.transX + b.transX,
        .skewY = a.skewY + b.skewY,
        .scaleY = a.scaleY + b.scaleY,
        .transY = a.transY + b.transY,
        .pers0 = a.pers0 + b.pers0,
        .pers1 = a.pers1 + b.pers1,
        .pers2 = a.pers2 + b.pers2,
    };
}

/**
 * @brief Returns the transponated of a.
 *
 * @param a The input matrix.
 * @return struct mat3f a transponated.
 */
ATTR_CONST static inline struct mat3f transponate_mat3f(const struct mat3f a) {
    return (struct mat3f){
        .scaleX = a.scaleX,
        .skewX = a.skewY,
        .transX = a.pers0,
        .skewY = a.skewX,
        .scaleY = a.scaleY,
        .transY = a.pers1,
        .pers0 = a.transX,
        .pers1 = a.transY,
        .pers2 = a.pers2,
    };
}

ATTR_CONST static inline struct vec2f transform_point(const struct mat3f transform, const struct vec2f point) {
    return VEC2F(
        transform.scaleX * point.x + transform.skewX * point.y + transform.transX,
        transform.skewY * point.x + transform.scaleY * point.y + transform.transY
    );
}

ATTR_CONST static inline struct quad transform_quad(const struct mat3f transform, const struct quad rect) {
    return QUAD(
        transform_point(transform, rect.top_left),
        transform_point(transform, rect.top_right),
        transform_point(transform, rect.bottom_left),
        transform_point(transform, rect.bottom_right)
    );
}

ATTR_CONST static inline struct quad transform_aa_rect(const struct mat3f transform, const struct aa_rect rect) {
    return transform_quad(transform, get_quad(rect));
}

ATTR_CONST static inline struct vec2f vec2f_swap_xy(const struct vec2f point) {
    return VEC2F(point.y, point.x);
}

#endif  // _FLUTTERPI_SRC_UTIL_GEOMETRY_H
