#ifndef _MODESETTING_H
#define _MODESETTING_H

#include <stdbool.h>
#include <pthread.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#include <collection.h>

struct drm_connector {
    drmModeConnector *connector;
	drmModeObjectProperties *props;
    drmModePropertyRes **props_info;
};

struct drm_encoder {
    drmModeEncoder *encoder;
};

struct drm_crtc {
    drmModeCrtc *crtc;
    drmModeObjectProperties *props;
    drmModePropertyRes **props_info;
    uint32_t bitmask;
    uint8_t index;
};

struct drm_plane {
    int type;
    drmModePlane *plane;
    drmModeObjectProperties *props;
    drmModePropertyRes **props_info;
};

struct drmdev {
    int fd;

    pthread_mutex_t mutex;
    bool supports_atomic_modesetting;

    size_t n_connectors;
    struct drm_connector *connectors;

    size_t n_encoders;
    struct drm_encoder *encoders;

    size_t n_crtcs;
    struct drm_crtc *crtcs;

    size_t n_planes;
    struct drm_plane *planes;

    drmModeRes *res;
    drmModePlaneRes *plane_res;

    bool is_configured;
    const struct drm_connector *selected_connector;
    const struct drm_encoder *selected_encoder;
    const struct drm_crtc *selected_crtc;
    const drmModeModeInfo *selected_mode;
    uint32_t selected_mode_blob_id;
};

struct drmdev_atomic_req {
    struct drmdev *drmdev;
    drmModeAtomicReq *atomic_req;

    void *available_planes_storage[32];
    struct pointer_set available_planes;
};

int drmdev_new_from_fd(
    struct drmdev **drmdev_out,
    int fd
);

int drmdev_new_from_path(
    struct drmdev **drmdev_out,
    const char *path
);

int drmdev_configure(
    struct drmdev *drmdev,
    uint32_t connector_id,
    uint32_t encoder_id,
    uint32_t crtc_id,
    const drmModeModeInfo *mode
);

int drmdev_plane_get_type(
    struct drmdev *drmdev,
    uint32_t plane_id
);

int drmdev_plane_supports_setting_rotation_value(
    struct drmdev *drmdev,
    uint32_t plane_id,
    int drm_rotation,
    bool *result
);

int drmdev_plane_get_min_zpos_value(
    struct drmdev *drmdev,
    uint32_t plane_id,
    int64_t *min_zpos_out
);

int drmdev_plane_get_max_zpos_value(
    struct drmdev *drmdev,
    uint32_t plane_id,
    int64_t *max_zpos_out
);

int drmdev_plane_supports_setting_zpos(
    struct drmdev *drmdev,
    uint32_t plane_id,
    bool *result
);

int drmdev_plane_supports_setting_zpos_value(
    struct drmdev *drmdev,
    uint32_t plane_id,
    int64_t zpos,
    bool *result
);

int drmdev_new_atomic_req(
    struct drmdev *drmdev,
    struct drmdev_atomic_req **req_out
);

void drmdev_destroy_atomic_req(
    struct drmdev_atomic_req *req
);

int drmdev_atomic_req_put_connector_property(
    struct drmdev_atomic_req *req,
    const char *name,
    uint64_t value
);

int drmdev_atomic_req_put_crtc_property(
    struct drmdev_atomic_req *req,
    const char *name,
    uint64_t value
);

int drmdev_atomic_req_put_plane_property(
    struct drmdev_atomic_req *req,
    uint32_t plane_id,
    const char *name,
    uint64_t value
);

int drmdev_atomic_req_put_modeset_props(
    struct drmdev_atomic_req *req,
    uint32_t *flags
);

inline static int drmdev_atomic_req_reserve_plane(
    struct drmdev_atomic_req *req,
    struct drm_plane *plane
) {
    return pset_remove(&req->available_planes, plane);
}

int drmdev_atomic_req_commit(
    struct drmdev_atomic_req *req,
    uint32_t flags,
    void *userdata
);

int drmdev_legacy_set_mode_and_fb(
    struct drmdev *drmdev,
    uint32_t fb_id
);

/**
 * @brief Do a nonblocking, vblank-synced framebuffer swap.
 */
int drmdev_legacy_primary_plane_pageflip(
    struct drmdev *drmdev,
    uint32_t fb_id,
    void *userdata
);

/**
 * @brief Do a blocking, vblank-synced framebuffer swap.
 * Using this in combination with @ref drmdev_legacy_primary_plane_pageflip
 * is not a good idea, since it will block until the primary plane pageflip is complete,
 * and then block even longer till the overlay plane pageflip completes the vblank after.
 */
int drmdev_legacy_overlay_plane_pageflip(
    struct drmdev *drmdev,
    uint32_t plane_id,
    uint32_t fb_id,
    int32_t crtc_x,
    int32_t crtc_y,
    int32_t crtc_w,
    int32_t crtc_h,
    uint32_t src_x,
    uint32_t src_y,
    uint32_t src_w,
    uint32_t src_h
);

int drmdev_legacy_set_connector_property(
    struct drmdev *drmdev,
    const char *name,
    uint64_t value
);

int drmdev_legacy_set_crtc_property(
    struct drmdev *drmdev,
    const char *name,
    uint64_t value
);

int drmdev_legacy_set_plane_property(
    struct drmdev *drmdev,
    uint32_t plane_id,
    const char *name,
    uint64_t value
);

float mode_get_vrefresh(const drmModeModeInfo *mode);

inline static struct drm_connector *__next_connector(const struct drmdev *drmdev, const struct drm_connector *connector) {
    bool found = connector == NULL;
    for (size_t i = 0; i < drmdev->n_connectors; i++) {
        if (drmdev->connectors + i == connector) {
            found = true;
        } else if (found) {
            return drmdev->connectors + i;
        }
    }

    return NULL;
}

inline static struct drm_encoder *__next_encoder(const struct drmdev *drmdev, const struct drm_encoder *encoder) {
    bool found = encoder == NULL;
    for (size_t i = 0; i < drmdev->n_encoders; i++) {
        if (drmdev->encoders + i == encoder) {
            found = true;
        } else if (found) {
            return drmdev->encoders + i;
        }
    }

    return NULL;
}

inline static struct drm_crtc *__next_crtc(const struct drmdev *drmdev, const struct drm_crtc *crtc) {
    bool found = crtc == NULL;
    for (size_t i = 0; i < drmdev->n_crtcs; i++) {
        if (drmdev->crtcs + i == crtc) {
            found = true;
        } else if (found) {
            return drmdev->crtcs + i;
        }
    }

    return NULL;
}

inline static struct drm_plane *__next_plane(const struct drmdev *drmdev, const struct drm_plane *plane) {
    bool found = plane == NULL;
    for (size_t i = 0; i < drmdev->n_planes; i++) {
        if (drmdev->planes + i == plane) {
            found = true;
        } else if (found) {
            return drmdev->planes + i;
        }
    }

    return NULL;
}

inline static drmModeModeInfo *__next_mode(const struct drm_connector *connector, const drmModeModeInfo *mode) {
    bool found = mode == NULL;
    for (int i = 0; i < connector->connector->count_modes; i++) {
        if (connector->connector->modes + i == mode) {
            found = true;
        } else if (found) {
            return connector->connector->modes + i;
        }
    }

    return NULL;
}

#define for_each_connector_in_drmdev(drmdev, connector) for (connector = __next_connector(drmdev, NULL); connector != NULL; connector = __next_connector(drmdev, connector))

#define for_each_encoder_in_drmdev(drmdev, encoder) for (encoder = __next_encoder(drmdev, NULL); encoder != NULL; encoder = __next_encoder(drmdev, encoder))

#define for_each_crtc_in_drmdev(drmdev, crtc) for (crtc = __next_crtc(drmdev, NULL); crtc != NULL; crtc = __next_crtc(drmdev, crtc))

#define for_each_plane_in_drmdev(drmdev, plane) for (plane = __next_plane(drmdev, NULL); plane != NULL; plane = __next_plane(drmdev, plane))

#define for_each_mode_in_connector(connector, mode) for (mode = __next_mode(connector, NULL); mode != NULL; mode = __next_mode(connector, mode))

#define for_each_unreserved_plane_in_atomic_req(atomic_req, plane) for_each_pointer_in_pset(&(atomic_req)->available_planes, plane)

#endif
