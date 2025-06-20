#include "req_builder.h"

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <libudev.h>

#include "drmdev.h"
#include "resources.h"
#include "pixel_format.h"
#include "util/bitset.h"
#include "util/list.h"
#include "util/lock_ops.h"
#include "util/logging.h"
#include "util/macros.h"
#include "util/refcounting.h"

#ifdef DEBUG_DRM_PLANE_ALLOCATIONS
    #define LOG_DRM_PLANE_ALLOCATION_DEBUG LOG_DEBUG
#else
    #define LOG_DRM_PLANE_ALLOCATION_DEBUG(...)
#endif

struct kms_req_layer {
    struct kms_fb_layer layer;

    uint32_t plane_id;
    struct drm_plane *plane;

    bool set_zpos;
    int64_t zpos;

    bool set_rotation;
    drm_plane_transform_t rotation;

    void_callback_t release_callback;
    kmsreq_syncfile_cb_t deferred_release_callback;
    void *release_callback_userdata;
};

struct kms_req_builder {
    refcount_t n_refs;

    struct drmdev *drmdev;
    struct drm_resources *res;
    struct drm_connector *connector;
    struct drm_crtc *crtc;
    uint32_t available_planes;
    
    bool use_atomic;
    drmModeAtomicReq *req;
    
    int64_t next_zpos;
    bool unset_mode;
    bool has_mode;
    drmModeModeInfo mode;
    
    int n_layers;
    struct kms_req_layer layers[32];

    kmsreq_scanout_cb_t scanout_cb;
    void *scanout_cb_userdata;

    void_callback_t release_cb;
    void *release_cb_userdata;
};

COMPILE_ASSERT(BITSET_SIZE(((struct kms_req_builder *) 0)->available_planes) == 32);

static bool plane_qualifies(
    // clang-format off
    struct drm_plane *plane,
    bool allow_primary,
    bool allow_overlay,
    bool allow_cursor,
    enum pixfmt format,
    bool has_modifier, uint64_t modifier,
    bool has_zpos, int64_t zpos_lower_limit, int64_t zpos_upper_limit,
    bool has_rotation, drm_plane_transform_t rotation,
    bool has_id_range, uint32_t id_lower_limit
    // clang-format on
) {
    LOG_DRM_PLANE_ALLOCATION_DEBUG("  checking if plane with id %" PRIu32 " qualifies...\n", plane->id);

    if (plane->type == DRM_PRIMARY_PLANE) {
        if (!allow_primary) {
            LOG_DRM_PLANE_ALLOCATION_DEBUG("    does not qualify: plane type is primary but allow_primary is false\n");
            return false;
        }
    } else if (plane->type == DRM_OVERLAY_PLANE) {
        if (!allow_overlay) {
            LOG_DRM_PLANE_ALLOCATION_DEBUG("    does not qualify: plane type is overlay but allow_overlay is false\n");
            return false;
        }
    } else if (plane->type == DRM_CURSOR_PLANE) {
        if (!allow_cursor) {
            LOG_DRM_PLANE_ALLOCATION_DEBUG("    does not qualify: plane type is cursor but allow_cursor is false\n");
            return false;
        }
    } else {
        ASSERT(false);
    }

    if (has_modifier) {
        if (drm_plane_supports_modified_formats(plane)) {
            // return false if we want a modified format but the plane doesn't support modified formats
            LOG_DRM_PLANE_ALLOCATION_DEBUG(
                "    does not qualify: framebuffer has modifier %" PRIu64 " but plane does not support modified formats\n",
                modifier
            );
            return false;
        }

        if (!drm_plane_supports_modified_format(plane, format, modifier)) {
            LOG_DRM_PLANE_ALLOCATION_DEBUG(
                "    does not qualify: plane does not support the modified format %s, %" PRIu64 ".\n",
                get_pixfmt_info(format)->name,
                modifier
            );

            // not found in the supported modified format list
            return false;
        }
    } else {
        // we don't want a modified format, return false if the format is not in the list
        // of supported (unmodified) formats
        if (!plane->supported_formats[format]) {
            LOG_DRM_PLANE_ALLOCATION_DEBUG(
                "    does not qualify: plane does not support the (unmodified) format %s.\n",
                get_pixfmt_info(format)->name
            );
            return false;
        }
    }

    if (has_zpos) {
        if (!plane->has_zpos) {
            // return false if we want a zpos but the plane doesn't support one
            LOG_DRM_PLANE_ALLOCATION_DEBUG("    does not qualify: zpos constraints specified but plane doesn't have a zpos property.\n");
            return false;
        } else if (zpos_lower_limit > plane->max_zpos || zpos_upper_limit < plane->min_zpos) {
            // return false if the zpos we want is outside the supported range of the plane
            LOG_DRM_PLANE_ALLOCATION_DEBUG("    does not qualify: plane limits cannot satisfy the specified zpos constraints.\n");
            LOG_DRM_PLANE_ALLOCATION_DEBUG(
                "      plane zpos range: %" PRIi64 " <= zpos <= %" PRIi64 ", given zpos constraints: %" PRIi64 " <= zpos <= %" PRIi64 ".\n",
                plane->min_zpos,
                plane->max_zpos,
                zpos_lower_limit,
                zpos_upper_limit
            );
            return false;
        }
    }
    if (has_id_range && plane->id < id_lower_limit) {
        LOG_DRM_PLANE_ALLOCATION_DEBUG("    does not qualify: plane id does not satisfy the given plane id constrains.\n");
        LOG_DRM_PLANE_ALLOCATION_DEBUG("      plane id: %" PRIu32 ", plane id lower limit: %" PRIu32 "\n", plane->id, id_lower_limit);
        return false;
    }
    if (has_rotation) {
        if (!plane->has_rotation) {
            // return false if the plane doesn't support rotation
            LOG_DRM_PLANE_ALLOCATION_DEBUG("    does not qualify: explicit rotation requested but plane has no rotation property.\n");
            return false;
        } else if (plane->has_hardcoded_rotation && plane->hardcoded_rotation.u32 != rotation.u32) {
            // return false if the plane has a hardcoded rotation and the rotation we want
            // is not the hardcoded one
            LOG_DRM_PLANE_ALLOCATION_DEBUG("    does not qualify: plane has hardcoded rotation that doesn't match the requested rotation.\n"
            );
            return false;
        } else if (rotation.u32 & ~plane->supported_rotations.u32) {
            // return false if we can't construct the rotation using the rotation
            // bits that are supported by the plane
            LOG_DRM_PLANE_ALLOCATION_DEBUG("    does not qualify: requested rotation is not supported by the plane.\n");
            return false;
        }
    }

    LOG_DRM_PLANE_ALLOCATION_DEBUG("    does qualify.\n");
    return true;
}

UNUSED static struct drm_plane *allocate_plane(
    // clang-format off
    struct kms_req_builder *builder,
    bool allow_primary,
    bool allow_overlay,
    bool allow_cursor,
    enum pixfmt format,
    bool has_modifier, uint64_t modifier,
    bool has_zpos, int64_t zpos_lower_limit, int64_t zpos_upper_limit,
    bool has_rotation, drm_plane_transform_t rotation,
    bool has_id_range, uint32_t id_lower_limit
    // clang-format on
) {
    for (unsigned int i = 0; i < builder->res->n_planes; i++) {
        struct drm_plane *plane = builder->res->planes + i;

        if (builder->available_planes & (1 << i)) {
            // find out if the plane matches our criteria
            bool qualifies = plane_qualifies(
                plane,
                allow_primary,
                allow_overlay,
                allow_cursor,
                format,
                has_modifier,
                modifier,
                has_zpos,
                zpos_lower_limit,
                zpos_upper_limit,
                has_rotation,
                rotation,
                has_id_range,
                id_lower_limit
            );

            // if it doesn't, look for the next one
            if (!qualifies) {
                continue;
            }

            // we found one, mark it as used and return it
            builder->available_planes &= ~(1 << i);
            return plane;
        }
    }

    // we didn't find an available plane matching our criteria
    return NULL;
}

UNUSED static void release_plane(struct kms_req_builder *builder, uint32_t plane_id) {
    unsigned int index = drm_resources_get_plane_index(builder->res, plane_id);
    if (index == UINT_MAX) {
        LOG_ERROR("Could not find plane with id %" PRIu32 ".\n", plane_id);
        return;
    }

    assert(!(builder->available_planes & (1 << index)));
    builder->available_planes |= (1 << index);
}

struct kms_req_builder *kms_req_builder_new_atomic(struct drmdev *drmdev, struct drm_resources *resources, uint32_t crtc_id) {
    struct kms_req_builder *builder;

    ASSERT_NOT_NULL(resources);
    assert(crtc_id != 0 && crtc_id != 0xFFFFFFFF);

    builder = calloc(1, sizeof *builder);
    if (builder == NULL) {
        return NULL;
    }

    builder->n_refs = REFCOUNT_INIT_1;
    builder->res = drm_resources_ref(resources);
    builder->use_atomic = true;
    builder->drmdev = drmdev_ref(drmdev);
    builder->connector = NULL;
    builder->n_layers = 0;
    builder->has_mode = false;
    builder->unset_mode = false;

    builder->crtc = drm_resources_get_crtc(resources, crtc_id);
    if (builder->crtc == NULL) {
        LOG_ERROR("Invalid CRTC: %" PRId32 "\n", crtc_id);
        goto fail_unref_drmdev;
    }

    builder->req = drmModeAtomicAlloc();
    if (builder->req == NULL) {
        goto fail_unref_drmdev;
    }

    // set the CRTC to active
    drmModeAtomicAddProperty(builder->req, crtc_id, builder->crtc->ids.active, 1);

    builder->next_zpos = drm_resources_get_min_zpos_for_crtc(resources, crtc_id);
    builder->available_planes = drm_resources_get_possible_planes_for_crtc(resources, crtc_id);
    return builder;

fail_unref_drmdev:
    drmdev_unref(builder->drmdev);
    drm_resources_unref(builder->res);
    free(builder);
    return NULL;
}

struct kms_req_builder *kms_req_builder_new_legacy(struct drmdev *drmdev, struct drm_resources *resources, uint32_t crtc_id) {
    struct kms_req_builder *builder;

    ASSERT_NOT_NULL(resources);
    assert(crtc_id != 0 && crtc_id != 0xFFFFFFFF);

    builder = calloc(1, sizeof *builder);
    if (builder == NULL) {
        return NULL;
    }

    builder->n_refs = REFCOUNT_INIT_1;
    builder->res = drm_resources_ref(resources);
    builder->use_atomic = false;
    builder->drmdev = drmdev_ref(drmdev);
    builder->connector = NULL;
    builder->n_layers = 0;
    builder->has_mode = false;
    builder->unset_mode = false;

    builder->crtc = drm_resources_get_crtc(resources, crtc_id);
    if (builder->crtc == NULL) {
        LOG_ERROR("Invalid CRTC: %" PRId32 "\n", crtc_id);
        goto fail_unref_drmdev;
    }

    builder->req = NULL;
    builder->next_zpos = drm_resources_get_min_zpos_for_crtc(resources, crtc_id);
    builder->available_planes = drm_resources_get_possible_planes_for_crtc(resources, crtc_id);
    return builder;

fail_unref_drmdev:
    drmdev_unref(builder->drmdev);
    drm_resources_unref(builder->res);
    free(builder);
    return NULL;
}

static void kms_req_builder_destroy(struct kms_req_builder *builder) {
    /// TODO: Is this complete?
    for (int i = 0; i < builder->n_layers; i++) {
        if (builder->layers[i].release_callback != NULL) {
            builder->layers[i].release_callback(builder->layers[i].release_callback_userdata);
        }
    }
    if (builder->req != NULL) {
        drmModeAtomicFree(builder->req);
    }
    drm_resources_unref(builder->res);
    drmdev_unref(builder->drmdev);
    free(builder);
}

DEFINE_REF_OPS(kms_req_builder, n_refs)

struct drmdev *kms_req_builder_get_drmdev(struct kms_req_builder *builder) {
    ASSERT_NOT_NULL(builder);
    return drmdev_ref(builder->drmdev);
}

struct drmdev *kms_req_builder_peek_drmdev(struct kms_req_builder *builder) {
    ASSERT_NOT_NULL(builder);
    return builder->drmdev;
}

struct drm_resources *kms_req_builder_get_resources(struct kms_req_builder *builder) {
    ASSERT_NOT_NULL(builder);
    return drm_resources_ref(builder->res);
}

struct drm_resources *kms_req_builder_peek_resources(struct kms_req_builder *builder) {
    ASSERT_NOT_NULL(builder);
    return builder->res;
}

struct drm_crtc *kms_req_builder_get_crtc(struct kms_req_builder *builder) {
    return builder->crtc;
}

bool kms_req_builder_prefer_next_layer_opaque(struct kms_req_builder *builder) {
    ASSERT_NOT_NULL(builder);
    return builder->n_layers == 0;
}

int kms_req_builder_set_mode(struct kms_req_builder *builder, const drmModeModeInfo *mode) {
    ASSERT_NOT_NULL(builder);
    ASSERT_NOT_NULL(mode);
    builder->has_mode = true;
    builder->mode = *mode;
    return 0;
}

int kms_req_builder_unset_mode(struct kms_req_builder *builder) {
    ASSERT_NOT_NULL(builder);
    assert(!builder->has_mode);
    builder->unset_mode = true;
    return 0;
}

int kms_req_builder_set_connector(struct kms_req_builder *builder, uint32_t connector_id) {
    struct drm_connector *conn;

    ASSERT_NOT_NULL(builder);
    assert(DRM_ID_IS_VALID(connector_id));

    conn = drm_resources_get_connector(builder->res, connector_id);
    if (conn == NULL) {
        LOG_ERROR("Could not find connector with id %" PRIu32 "\n", connector_id);
        return EINVAL;
    }

    builder->connector = conn;
    return 0;
}

int kms_req_builder_push_fb_layer(
    struct kms_req_builder *builder,
    const struct kms_fb_layer *layer,
    void_callback_t release_callback,
    kmsreq_syncfile_cb_t deferred_release_callback,
    void *userdata
) {
    struct drm_plane *plane;
    int64_t zpos;
    bool has_zpos;
    bool close_in_fence_fd_after;
    int ok, index;

    ASSERT_NOT_NULL(builder);
    ASSERT_NOT_NULL(layer);
    ASSERT_NOT_NULL(release_callback);
    ASSERT_EQUALS_MSG(deferred_release_callback, NULL, "deferred release callbacks are not supported right now.");

    if (!builder->use_atomic && builder->n_layers > 1) {
        // Multi-plane commits are un-vsynced without atomic modesetting.
        // And when atomic modesetting is supported but we're still using legacy,
        // every individual plane commit is vsynced.
        LOG_DEBUG("Can't do multi-plane commits when using legacy modesetting.\n");
        return EINVAL;
    }

    close_in_fence_fd_after = false;
    if (!builder->use_atomic && layer->has_in_fence_fd) {
        LOG_DEBUG("Explicit fencing is not supported for legacy modesetting. Implicit fencing will be used instead.\n");
        close_in_fence_fd_after = true;
    }

    // Index of our layer.
    index = builder->n_layers;

    // If we should prefer a cursor plane, try to find one first.
    plane = NULL;
    if (layer->prefer_cursor) {
        plane = allocate_plane(
            // clang-format off
            builder,
            /* allow_primary */ false,
            /* allow_overlay */ false,
            /* allow_cursor  */ true,
            /* format */ layer->format,
            /* modifier */ layer->has_modifier, layer->modifier,
            /* zpos */ false, 0, 0,
            /* rotation */ layer->has_rotation, layer->rotation,
            /* id_range */ false, 0
            // clang-format on
        );
        if (plane == NULL) {
            LOG_DEBUG("Couldn't find a fitting cursor plane.\n");
        }
    }

    /// TODO: Not sure we can use crtc_x, crtc_y, etc with primary planes
    if (plane == NULL && index == 0) {
        // if this is the first layer, try using a
        // primary plane for it.

        /// TODO: Use cursor_plane->max_zpos - 1 as the upper zpos limit, instead of INT64_MAX
        plane = allocate_plane(
            // clang-format off
            builder,
            /* allow_primary */ true,
            /* allow_overlay */ false,
            /* allow_cursor */ false,
            /* format */ layer->format,
            /* modifier */ layer->has_modifier, layer->modifier,
            /* zpos */ false, 0, 0,
            /* rotation */ layer->has_rotation, layer->rotation,
            /* id_range */ false, 0
            // clang-format on
        );

        if (plane == NULL && !get_pixfmt_info(layer->format)->is_opaque) {
            // maybe we can find a plane if we use the opaque version of this pixel format?
            plane = allocate_plane(
                // clang-format off
                builder,
                /* allow_primary */ true,
                /* allow_overlay */ false,
                /* allow_cursor */ false,
                /* format */ pixfmt_opaque(layer->format),
                /* modifier */ layer->has_modifier, layer->modifier,
                /* zpos */ false, 0, 0,
                /* rotation */ layer->has_rotation, layer->rotation,
                /* id_range */ false, 0
                // clang-format on
            );
        }
    } else if (plane == NULL) {
        // First try to find an overlay plane with a higher zpos.
        plane = allocate_plane(
            // clang-format off
            builder,
            /* allow_primary */ false,
            /* allow_overlay */ true,
            /* allow_cursor */ false,
            /* format */ layer->format,
            /* modifier */ layer->has_modifier, layer->modifier,
            /* zpos */ true, builder->next_zpos, INT64_MAX,
            /* rotation */ layer->has_rotation, layer->rotation,
            /* id_range */ false, 0
            // clang-format on
        );

        // If we can't find one, find an overlay plane with the next highest plane_id.
        // (According to some comments in the kernel, that's the fallback KMS uses for the
        // occlusion order if no zpos property is supported, i.e. planes with plane id occlude
        // planes with lower id)
        if (plane == NULL) {
            plane = allocate_plane(
                // clang-format off
                builder,
                /* allow_primary */ false,
                /* allow_overlay */ true,
                /* allow_cursor */ false,
                /* format */ layer->format,
                /* modifier */ layer->has_modifier, layer->modifier,
                /* zpos */ false, 0, 0,
                /* rotation */ layer->has_rotation, layer->rotation,
                /* id_range */ true, builder->layers[index - 1].plane_id + 1
                // clang-format on
            );
        }
    }

    if (plane == NULL) {
        LOG_ERROR("Could not find a suitable unused DRM plane for pushing the framebuffer.\n");
        return EIO;
    }

    // Now that we have a plane, use the minimum zpos
    // that's both higher than the last layers zpos and
    // also supported by the plane.
    // This will also work for planes with hardcoded zpos.
    has_zpos = plane->has_zpos;
    if (has_zpos) {
        zpos = builder->next_zpos;
        if (plane->min_zpos > zpos) {
            zpos = plane->min_zpos;
        }
    } else {
        // just to silence an uninitialized use warning below.
        zpos = 0;
    }

    if (!builder->use_atomic) {
    } else {
        uint32_t plane_id = plane->id;

        /// TODO: Error checking
        /// TODO: Maybe add these in the kms_req_builder_commit instead?
        drmModeAtomicAddProperty(builder->req, plane_id, plane->ids.crtc_id, builder->crtc->id);
        drmModeAtomicAddProperty(builder->req, plane_id, plane->ids.fb_id, layer->drm_fb_id);
        drmModeAtomicAddProperty(builder->req, plane_id, plane->ids.crtc_x, layer->dst_x);
        drmModeAtomicAddProperty(builder->req, plane_id, plane->ids.crtc_y, layer->dst_y);
        drmModeAtomicAddProperty(builder->req, plane_id, plane->ids.crtc_w, layer->dst_w);
        drmModeAtomicAddProperty(builder->req, plane_id, plane->ids.crtc_h, layer->dst_h);
        drmModeAtomicAddProperty(builder->req, plane_id, plane->ids.src_x, layer->src_x);
        drmModeAtomicAddProperty(builder->req, plane_id, plane->ids.src_y, layer->src_y);
        drmModeAtomicAddProperty(builder->req, plane_id, plane->ids.src_w, layer->src_w);
        drmModeAtomicAddProperty(builder->req, plane_id, plane->ids.src_h, layer->src_h);

        if (plane->has_zpos && !plane->has_hardcoded_zpos) {
            drmModeAtomicAddProperty(builder->req, plane_id, plane->ids.zpos, zpos);
        }

        if (layer->has_rotation && plane->has_rotation && !plane->has_hardcoded_rotation) {
            drmModeAtomicAddProperty(builder->req, plane_id, plane->ids.rotation, layer->rotation.u64);
        }

        if (index == 0) {
            if (plane->has_alpha) {
                drmModeAtomicAddProperty(builder->req, plane_id, plane->ids.alpha, plane->max_alpha);
            }

            if (plane->has_blend_mode && plane->supported_blend_modes[DRM_BLEND_MODE_NONE]) {
                drmModeAtomicAddProperty(builder->req, plane_id, plane->ids.pixel_blend_mode, DRM_BLEND_MODE_NONE);
            }
        }
    }

    // This should be done when we're sure we're not failing.
    // Because on failure it would be the callers job to close the fd.
    if (close_in_fence_fd_after) {
        ok = close(layer->in_fence_fd);
        if (ok < 0) {
            ok = errno;
            LOG_ERROR("Could not close layer in_fence_fd. close: %s\n", strerror(ok));
            goto fail_release_plane;
        }
    }

    /// TODO: Right now we're adding zpos, rotation to the atomic request unconditionally
    /// when specified in the fb layer. Ideally we would check for updates
    /// on commit and only add to the atomic request when zpos / rotation changed.
    builder->n_layers++;
    if (has_zpos) {
        builder->next_zpos = zpos + 1;
    }
    builder->layers[index].layer = *layer;
    builder->layers[index].plane_id = plane->id;
    builder->layers[index].plane = plane;
    builder->layers[index].set_zpos = has_zpos;
    builder->layers[index].zpos = zpos;
    builder->layers[index].set_rotation = layer->has_rotation;
    builder->layers[index].rotation = layer->rotation;
    builder->layers[index].release_callback = release_callback;
    builder->layers[index].deferred_release_callback = deferred_release_callback;
    builder->layers[index].release_callback_userdata = userdata;
    return 0;

fail_release_plane:
    release_plane(builder, plane->id);
    return ok;
}

int kms_req_builder_push_zpos_placeholder_layer(struct kms_req_builder *builder, int64_t *zpos_out) {
    ASSERT_NOT_NULL(builder);
    ASSERT_NOT_NULL(zpos_out);
    *zpos_out = builder->next_zpos++;
    return 0;
}

struct kms_req *kms_req_builder_build(struct kms_req_builder *builder) {
    return (struct kms_req *) kms_req_builder_ref(builder);
}

UNUSED struct kms_req *kms_req_ref(struct kms_req *req) {
    return (struct kms_req *) kms_req_builder_ref((struct kms_req_builder *) req);
}

UNUSED void kms_req_unref(struct kms_req *req) {
    kms_req_builder_unref((struct kms_req_builder *) req);
}

UNUSED void kms_req_unrefp(struct kms_req **req) {
    kms_req_builder_unrefp((struct kms_req_builder **) req);
}

UNUSED void kms_req_swap_ptrs(struct kms_req **oldp, struct kms_req *new) {
    kms_req_builder_swap_ptrs((struct kms_req_builder **) oldp, (struct kms_req_builder *) new);
}

UNUSED static bool drm_plane_is_active(struct drm_plane *plane) {
    return plane->committed_state.fb_id != 0 && plane->committed_state.crtc_id != 0;
}

static void on_kms_req_scanout(uint64_t vblank_ns, void *userdata) {
    struct kms_req_builder *b;

    ASSERT_NOT_NULL(userdata);
    b = (struct kms_req_builder *) userdata;

    if (b->scanout_cb != NULL) {
        b->scanout_cb(vblank_ns, b->scanout_cb_userdata);
    }
}

static void on_kms_req_release(void *userdata) {
    struct kms_req_builder *b;

    ASSERT_NOT_NULL(userdata);
    b = (struct kms_req_builder *) userdata;

    if (b->release_cb != NULL) {
        b->release_cb(b->release_cb_userdata);
    }

    kms_req_builder_unref(b);
}


static int kms_req_commit_common(
    struct kms_req *req,
    struct drmdev *drmdev,
    bool blocking,
    kmsreq_scanout_cb_t scanout_cb,
    void *scanout_cb_userdata,
    uint64_t *vblank_ns_out,
    void_callback_t release_cb,
    void *release_cb_userdata
) {
    struct kms_req_builder *builder;
    struct drm_blob *mode_blob;
    bool update_mode;
    int ok;

    update_mode = false;
    mode_blob = NULL;

    ASSERT_NOT_NULL(req);
    builder = (struct kms_req_builder *) req;

    if (!drmdev_can_commit(drmdev)) {
        LOG_ERROR("Commit requested, but drmdev is paused right now.\n");
        return EBUSY;
    }

    // only change the mode if the new mode differs from the old one

    /// TOOD: If this is not a standard mode reported by connector/CRTC,
    /// is there a way to verify if it is valid? (maybe use DRM_MODE_ATOMIC_TEST)

    // this could be a single expression but this way you see a bit better what's going on.
    // We need to upload the new mode blob if:
    //  - we have a new mode
    //  - and: we don't have an old mode
    //  - or: the old mode differs from the new mode
    bool upload_mode = false;
    if (builder->has_mode) {
        if (!builder->crtc->committed_state.has_mode) {
            upload_mode = true;
        } else if (memcmp(&builder->crtc->committed_state.mode, &builder->mode, sizeof(drmModeModeInfo)) != 0) {
            upload_mode = true;
        }
    }

    if (upload_mode) {
        update_mode = true;
        mode_blob = drm_blob_new_mode(drmdev_get_modesetting_fd(drmdev), &builder->mode, true);
        if (mode_blob == NULL) {
            return EIO;
        }
    } else if (builder->unset_mode) {
        update_mode = true;
        mode_blob = NULL;
    }

    if (builder->use_atomic) {
        /// TODO: If we can do explicit fencing, don't use the page flip event.
        /// TODO: Can we set OUT_FENCE_PTR even though we didn't set any IN_FENCE_FDs?

        // For every plane that was previously active with our CRTC, but is not used by us anymore,
        // disable it.
        for (unsigned int i = 0; i < builder->res->n_planes; i++) {
            if (!(builder->available_planes & (1 << i))) {
                continue;
            }

            struct drm_plane *plane = builder->res->planes + i;
            if (drm_plane_is_active(plane) && plane->committed_state.crtc_id == builder->crtc->id) {
                drmModeAtomicAddProperty(builder->req, plane->id, plane->ids.crtc_id, 0);
                drmModeAtomicAddProperty(builder->req, plane->id, plane->ids.fb_id, 0);
            }
        }

        if (builder->connector != NULL) {
            // add the CRTC_ID property if that was explicitly set
            drmModeAtomicAddProperty(builder->req, builder->connector->id, builder->connector->ids.crtc_id, builder->crtc->id);
        }

        if (update_mode) {
            if (mode_blob != NULL) {
                drmModeAtomicAddProperty(builder->req, builder->crtc->id, builder->crtc->ids.mode_id, drm_blob_get_id(mode_blob));
            } else {
                drmModeAtomicAddProperty(builder->req, builder->crtc->id, builder->crtc->ids.mode_id, 0);
            }
        }

        builder->scanout_cb = scanout_cb;
        builder->scanout_cb_userdata = scanout_cb_userdata;
        builder->release_cb = release_cb;
        builder->release_cb_userdata = release_cb_userdata;

        if (blocking) {
            ok = drmdev_commit_atomic_sync(drmdev, builder->req, update_mode, builder->crtc->id, on_kms_req_release, kms_req_ref(req), vblank_ns_out);
        } else {
            ok = drmdev_commit_atomic_async(drmdev, builder->req, update_mode, builder->crtc->id, on_kms_req_scanout, on_kms_req_release, kms_req_ref(req));
        }

        if (ok != 0) {
            ok = errno;
            goto fail_unref_builder;
        }
    } else {
        ASSERT_EQUALS(builder->layers[0].layer.dst_x, 0);
        ASSERT_EQUALS(builder->layers[0].layer.dst_y, 0);
        ASSERT_EQUALS(builder->layers[0].layer.dst_w, builder->mode.hdisplay);
        ASSERT_EQUALS(builder->layers[0].layer.dst_h, builder->mode.vdisplay);

        /// TODO: Do we really need to assert this?
        ASSERT_NOT_NULL(builder->connector);

        bool needs_set_crtc = update_mode;

        // check if the plane pixel format changed.
        // that needs a drmModeSetCrtc for legacy KMS as well.
        // get the current, committed fb for the plane, check if we have info
        // for it (we can't use drmModeGetFB2 since that's not present on debian buster)
        // and if we're not absolutely sure the formats match, set needs_set_crtc
        // too.
        if (!needs_set_crtc) {
            struct kms_req_layer *layer = builder->layers + 0;
            struct drm_plane *plane = layer->plane;
            ASSERT_NOT_NULL(plane);

            if (plane->committed_state.has_format && plane->committed_state.format == layer->layer.format) {
                needs_set_crtc = false;
            } else {
                needs_set_crtc = true;
            }
        }

        /// TODO: Handle {src,dst}_{x,y,w,h} here
        /// TODO: Handle setting other properties as well

        /// TODO: Implement
        UNIMPLEMENTED();

        // if (needs_set_crtc) {
        //     /// TODO: Fetch new connector or current connector here since we seem to need it for drmModeSetCrtc
        //     ok = drmModeSetCrtc(
        //         ,
        //         builder->crtc->id,
        //         builder->layers[0].layer.drm_fb_id,
        //         0,
        //         0,
        //         (uint32_t[1]){ builder->connector->id },
        //         1,
        //         builder->unset_mode ? NULL : &builder->mode
        //     );
        //     if (ok != 0) {
        //         ok = errno;
        //         LOG_ERROR("Could not commit display update. drmModeSetCrtc: %s\n", strerror(ok));
        //         goto fail_maybe_destroy_mode_blob;
        //     }

        //     internally_blocking = true;
        // } else {
        //     ok = drmModePageFlip(
        //         builder->drmdev->master_fd,
        //         builder->crtc->id,
        //         builder->layers[0].layer.drm_fb_id,
        //         DRM_MODE_PAGE_FLIP_EVENT,
        //         kms_req_builder_ref(builder)
        //     );
        //     if (ok != 0) {
        //         ok = errno;
        //         LOG_ERROR("Could not commit display update. drmModePageFlip: %s\n", strerror(ok));
        //         goto fail_unref_builder;
        //     }
        // }

        // This should also be ensured by kms_req_builder_push_fb_layer
        ASSERT_MSG(
            builder->use_atomic || builder->n_layers <= 1,
            "There can be at most one framebuffer layer when using legacy modesetting."
        );

        /// TODO: Call drmModeSetPlane for all other layers
        /// TODO: Assert here
    }

    // update struct drm_plane.committed_state for all planes
    for (int i = 0; i < builder->n_layers; i++) {
        struct drm_plane *plane = builder->layers[i].plane;
        struct kms_req_layer *layer = builder->layers + i;

        plane->committed_state.crtc_id = builder->crtc->id;
        plane->committed_state.fb_id = layer->layer.drm_fb_id;
        plane->committed_state.src_x = layer->layer.src_x;
        plane->committed_state.src_y = layer->layer.src_y;
        plane->committed_state.src_w = layer->layer.src_w;
        plane->committed_state.src_h = layer->layer.src_h;
        plane->committed_state.crtc_x = layer->layer.dst_x;
        plane->committed_state.crtc_y = layer->layer.dst_y;
        plane->committed_state.crtc_w = layer->layer.dst_w;
        plane->committed_state.crtc_h = layer->layer.dst_h;

        if (builder->layers[i].set_zpos) {
            plane->committed_state.zpos = layer->zpos;
        }
        if (builder->layers[i].set_rotation) {
            plane->committed_state.rotation = layer->rotation;
        }

        plane->committed_state.has_format = true;
        plane->committed_state.format = layer->layer.format;

        // builder->layers[i].plane->committed_state.alpha = layer->alpha;
        // builder->layers[i].plane->committed_state.blend_mode = builder->layers[i].layer.blend_mode;
    }

    // update struct drm_crtc.committed_state
    if (update_mode) {
        // destroy the old mode blob
        if (builder->crtc->committed_state.mode_blob != NULL) {
            drm_blob_destroy(builder->crtc->committed_state.mode_blob);
        }

        // store the new mode
        if (mode_blob != NULL) {
            builder->crtc->committed_state.has_mode = true;
            builder->crtc->committed_state.mode = builder->mode;
            builder->crtc->committed_state.mode_blob = mode_blob;
        } else {
            builder->crtc->committed_state.has_mode = false;
            builder->crtc->committed_state.mode_blob = NULL;
        }
    }

    // update struct drm_connector.committed_state
    builder->connector->committed_state.crtc_id = builder->crtc->id;
    // builder->connector->committed_state.encoder_id = 0;

    return 0;

fail_unref_builder:
    kms_req_builder_unref(builder);

// fail_maybe_destroy_mode_blob:
//     if (mode_blob != NULL)
//         drm_blob_destroy(mode_blob);

    return ok;
}

void set_vblank_ns(uint64_t vblank_ns, void *userdata) {
    uint64_t *vblank_ns_out;

    ASSERT_NOT_NULL(userdata);
    vblank_ns_out = userdata;

    *vblank_ns_out = vblank_ns;
}

int kms_req_commit_blocking(struct kms_req *req, struct drmdev *drmdev, uint64_t *vblank_ns_out) {
    int ok;

    ok = kms_req_commit_common(req, drmdev, true, NULL, NULL, vblank_ns_out, NULL, NULL);
    if (ok != 0) {
        return ok;
    }

    return 0;
}

int kms_req_commit_nonblocking(struct kms_req *req, struct drmdev *drmdev, kmsreq_scanout_cb_t scanout_cb, void *userdata, void_callback_t release_cb) {
    return kms_req_commit_common(req, drmdev, false, scanout_cb, userdata, NULL, release_cb, userdata);
}
