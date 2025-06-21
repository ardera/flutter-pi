// SPDX-License-Identifier: MIT
/*
 * KMS Modesetting
 *
 * - implements the interface to linux kernel modesetting
 * - allows querying connected screens, crtcs, planes, etc
 * - allows setting video modes, showing things on screen
 *
 * Copyright (c) 2022, Hannes Winkler <hanneswinkler2000@web.de>
 */

#ifndef _FLUTTERPI_SRC_MODESETTING_H
#define _FLUTTERPI_SRC_MODESETTING_H

#include <stdbool.h>

#include <pthread.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#include "pixel_format.h"
#include "util/collection.h"
#include "util/geometry.h"
#include "util/refcounting.h"

/**
 * @brief Interface that will be used to open and close files.
 */
struct file_interface {
    int (*open)(const char *path, int flags, void **fd_metadata_out, void *userdata);
    void (*close)(int fd, void *fd_metadata, void *userdata);
};

struct drmdev;

typedef void (*drmdev_scanout_cb_t)(uint64_t vblank_ns, void *userdata);

struct drmdev;
struct udev;

struct drmdev *drmdev_new_from_interface_fd(int fd, void *fd_metadata, const struct file_interface *interface, void *userdata);

struct drmdev *drmdev_new_from_path(const char *path, const struct file_interface *interface, void *userdata);

struct drmdev *drmdev_new_from_udev_primary(struct udev *udev, const char *seat, const struct file_interface *interface, void *interface_userdata);

DECLARE_REF_OPS(drmdev)

/**
 * @brief Get the drm_resources for this drmdev, taking a reference on it.
 * 
 * @param drmdev The drmdev.
 * @returns The drm_resources for this drmdev.
 */
struct drm_resources *drmdev_query_resources(struct drmdev *drmdev);

/**
 * @brief Get the file descriptor for the modesetting-capable /dev/dri/cardX device.
 * 
 * @param drmdev The drmdev.
 * @returns The file descriptor for the device.
 */
int drmdev_get_modesetting_fd(struct drmdev *drmdev);

/**
 * @brief Notify the drmdev that the modesetting fd has available data.
 */
void drmdev_dispatch_modesetting(struct drmdev *drmdev);

bool drmdev_supports_dumb_buffers(struct drmdev *drmdev);
int drmdev_create_dumb_buffer(
    struct drmdev *drmdev,
    int width,
    int height,
    int bpp,
    uint32_t *gem_handle_out,
    uint32_t *pitch_out,
    size_t *size_out
);
void drmdev_destroy_dumb_buffer(struct drmdev *drmdev, uint32_t gem_handle);
void *drmdev_map_dumb_buffer(struct drmdev *drmdev, uint32_t gem_handle, size_t size);
void drmdev_unmap_dumb_buffer(struct drmdev *drmdev, void *map, size_t size);

struct gbm_device *drmdev_get_gbm_device(struct drmdev *drmdev);

uint32_t drmdev_add_fb(
    struct drmdev *drmdev,
    uint32_t width,
    uint32_t height,
    enum pixfmt pixel_format,
    uint32_t bo_handle,
    uint32_t pitch,
    uint32_t offset,
    bool has_modifier,
    uint64_t modifier
);

uint32_t drmdev_add_fb_multiplanar(
    struct drmdev *drmdev,
    uint32_t width,
    uint32_t height,
    enum pixfmt pixel_format,
    const uint32_t bo_handles[4],
    const uint32_t pitches[4],
    const uint32_t offsets[4],
    bool has_modifiers,
    const uint64_t modifiers[4]
);

uint32_t drmdev_add_fb_from_dmabuf(
    struct drmdev *drmdev,
    uint32_t width,
    uint32_t height,
    enum pixfmt pixel_format,
    int prime_fd,
    uint32_t pitch,
    uint32_t offset,
    bool has_modifier,
    uint64_t modifier
);

uint32_t drmdev_add_fb_from_dmabuf_multiplanar(
    struct drmdev *drmdev,
    uint32_t width,
    uint32_t height,
    enum pixfmt pixel_format,
    const int prime_fds[4],
    const uint32_t pitches[4],
    const uint32_t offsets[4],
    bool has_modifiers,
    const uint64_t modifiers[4]
);

uint32_t drmdev_add_fb_from_gbm_bo(struct drmdev *drmdev, struct gbm_bo *bo, bool cast_opaque);

int drmdev_rm_fb_locked(struct drmdev *drmdev, uint32_t fb_id);

int drmdev_rm_fb(struct drmdev *drmdev, uint32_t fb_id);

int drmdev_get_last_vblank(struct drmdev *drmdev, uint32_t crtc_id, uint64_t *last_vblank_ns_out);

bool drmdev_can_commit(struct drmdev *drmdev);

void drmdev_suspend(struct drmdev *drmdev);

int drmdev_resume(struct drmdev *drmdev);

int drmdev_move_cursor(struct drmdev *drmdev, uint32_t crtc_id, struct vec2i pos);

int drmdev_commit_atomic_sync(
    struct drmdev *drmdev,
    drmModeAtomicReq *req,
    bool allow_modeset,
    uint32_t crtc_id,
    void_callback_t on_release,
    void *userdata,
    uint64_t *vblank_ns_out
);

int drmdev_commit_atomic_async(
    struct drmdev *drmdev,
    drmModeAtomicReq *req,
    bool allow_modeset,
    uint32_t crtc_id,
    drmdev_scanout_cb_t on_scanout,
    void_callback_t on_release,
    void *userdata
);

#endif  // _FLUTTERPI_SRC_MODESETTING_H
