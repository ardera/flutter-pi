#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <unistd.h>

#include <sys/epoll.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <modesetting.h>
#include <pixel_format.h>

FILE_DESCR("modesetting")

struct kms_req_layer {
    struct kms_fb_layer layer;

    uint32_t plane_id;

    bool set_zpos;
    int64_t zpos;

    bool set_rotation;
    drm_plane_transform_t rotation;

    kms_fb_release_cb_t release_callback;
    kms_deferred_fb_release_cb_t deferred_release_callback;
    void *release_callback_userdata;
};

struct kms_req_builder {
    refcount_t n_refs;

    struct drmdev *drmdev;
    bool use_legacy;
    bool supports_atomic;

    struct drm_connector *connector;
    struct drm_crtc *crtc;

    BMAP_DECLARATION(planes, 32);
    drmModeAtomicReq *req;
    int64_t next_zpos;

    int n_layers;
    struct kms_req_layer layers[32];

    bool unset_mode;
    bool has_mode;
    drmModeModeInfo mode;
};

struct drmdev {
    int fd;

    refcount_t n_refs;
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

    struct gbm_device *gbm_device;

    int event_fd;

    struct {
        kms_scanout_cb_t scanout_callback;
        void *userdata;
        void_callback_t destroy_callback;

        struct kms_req *last_flipped;
    } per_crtc_state[32];

    int master_fd;
    void *master_fd_metadata;

    struct drmdev_interface interface;
    void *userdata;
};

struct drmdev_atomic_req {
    struct drmdev *drmdev;
    drmModeAtomicReq *atomic_req;

    void *available_planes_storage[32];
    struct pointer_set available_planes;
};

static bool is_drm_master(int fd) {
    return drmAuthMagic(fd, 0) != -EACCES;
}

static struct drm_mode_blob *drm_mode_blob_new(int drm_fd, const drmModeModeInfo *mode) {
    struct drm_mode_blob *blob;
    uint32_t blob_id;
    int ok;

    blob = malloc(sizeof *blob);
    if (blob == NULL) {
        return NULL;
    }

    ok = drmModeCreatePropertyBlob(drm_fd, mode, sizeof *mode, &blob_id);
    if (ok != 0) {
        ok = errno;
        LOG_ERROR("Couldn't upload mode to kernel. drmModeCreatePropertyBlob: %s\n", strerror(ok));
        free(blob);
        return NULL;
    }

    blob->drm_fd = dup(drm_fd);
    blob->blob_id = blob_id;
    blob->mode = *mode;
    return blob;
}

void drm_mode_blob_destroy(struct drm_mode_blob *blob) {
    int ok;

    DEBUG_ASSERT_NOT_NULL(blob);

    ok = drmModeDestroyPropertyBlob(blob->drm_fd, blob->blob_id);
    if (ok != 0) {
        ok = errno;
        LOG_ERROR("Couldn't destroy mode property blob. drmModeDestroyPropertyBlob: %s\n", strerror(ok));
    }
    // we dup()-ed it in drm_mode_blob_new.
    close(blob->drm_fd);
    free(blob);
}

DEFINE_STATIC_LOCK_OPS(drmdev, mutex)

static int fetch_connector(int drm_fd, uint32_t connector_id, struct drm_connector *connector_out) {
    struct drm_connector_prop_ids ids;
    drmModeObjectProperties *props;
    drmModePropertyRes *prop_info;
    drmModeConnector *connector;
    drmModeModeInfo *modes;
    uint32_t crtc_id;
    int ok;

    drm_connector_prop_ids_init(&ids);

    connector = drmModeGetConnector(drm_fd, connector_id);
    if (connector == NULL) {
        ok = errno;
        LOG_ERROR("Could not get DRM device connector. drmModeGetConnector");
        return ok;
    }

    props = drmModeObjectGetProperties(drm_fd, connector_id, DRM_MODE_OBJECT_CONNECTOR);
    if (props == NULL) {
        ok = errno;
        perror("[modesetting] Could not get DRM device connectors properties. drmModeObjectGetProperties");
        goto fail_free_connector;
    }

    crtc_id = DRM_ID_NONE;
    for (int i = 0; i < props->count_props; i++) {       
        prop_info = drmModeGetProperty(drm_fd, props->props[i]);
        if (prop_info == NULL) {
            ok = errno;
            LOG_ERROR("Could not get DRM device connector properties' info. drmModeGetProperty: %s\n", strerror(ok));
            goto fail_free_props;
        }

#define CHECK_ASSIGN_PROPERTY_ID(_name_str, _name)                         \
    if (strncmp(prop_info->name, _name_str, DRM_PROP_NAME_LEN) == 0) { \
        ids._name = prop_info->prop_id;                                \
    } else

        DRM_CONNECTOR_PROPERTIES(CHECK_ASSIGN_PROPERTY_ID) {
            // this is the trailing else case
            LOG_DEBUG("Unknown DRM connector property: %s\n", prop_info->name);
        }

#undef CHECK_ASSIGN_PROPERTY_ID

        if (strncmp(prop_info->name, "CRTC_ID", DRM_PROP_NAME_LEN) == 0) {
            crtc_id = props->prop_values[i];
        }

        drmModeFreeProperty(prop_info);
        prop_info = NULL;
    }

    DEBUG_ASSERT((connector->modes == NULL) == (connector->count_modes == 0));

    if (connector->modes != NULL) {
        modes = memdup(connector->modes, connector->count_modes * sizeof(*connector->modes));
        if (modes == NULL) {
            ok = ENOMEM;
            goto fail_free_props;
        }
    } else {
        modes = NULL;
    }

    connector_out->id = connector->connector_id;
    connector_out->type = connector->connector_type;
    connector_out->type_id = connector->connector_type_id;
    connector_out->ids = ids;
    connector_out->n_encoders = connector->count_encoders;
    DEBUG_ASSERT(connector->count_encoders <= 32);
    memcpy(connector_out->encoders, connector->encoders, connector->count_encoders * sizeof(uint32_t)); 
    connector_out->variable_state.connection_state = (enum drm_connection_state) connector->connection;
    connector_out->variable_state.subpixel_layout = (enum drm_subpixel_layout) connector->subpixel;
    connector_out->variable_state.width_mm = connector->mmWidth;
    connector_out->variable_state.height_mm = connector->mmHeight;
    connector_out->variable_state.n_modes = connector->count_modes;
    connector_out->variable_state.modes = modes;
    connector_out->committed_state.crtc_id = crtc_id;
    connector_out->committed_state.encoder_id = connector->encoder_id;
    drmModeFreeObjectProperties(props);
    drmModeFreeConnector(connector);
    return 0;


fail_free_props:
    drmModeFreeObjectProperties(props);

fail_free_connector:
    drmModeFreeConnector(connector);
    return ok;
}

static void free_connector(struct drm_connector *connector) {
    free(connector->variable_state.modes);
}

static int fetch_connectors(struct drmdev *drmdev, struct drm_connector **connectors_out, size_t *n_connectors_out) {
    struct drm_connector *connectors;
    int ok;

    connectors = calloc(drmdev->res->count_connectors, sizeof *connectors);
    if (connectors == NULL) {
        *connectors_out = NULL;
        return ENOMEM;
    }

    for (int i = 0; i < drmdev->res->count_connectors; i++) {
        ok = fetch_connector(drmdev->fd, drmdev->res->connectors[i], connectors + i);
        if (ok != 0) {
            for (int j = 0; j < i; j++)
                free_connector(connectors + j);
            goto fail_free_connectors;
        }
    }

    *connectors_out = connectors;
    *n_connectors_out = drmdev->res->count_connectors;
    return 0;

fail_free_connectors:
    free(connectors);
    *connectors_out = NULL;
    *n_connectors_out = 0;
    return ok;
}

static int free_connectors(struct drm_connector *connectors, size_t n_connectors) {
    for (int i = 0; i < n_connectors; i++) {
        free_connector(connectors + i);
    }
    free(connectors);
    return 0;
}

static int fetch_encoder(int drm_fd, uint32_t encoder_id, struct drm_encoder *encoder_out) {
    drmModeEncoder *encoder;
    int ok;

    encoder = drmModeGetEncoder(drm_fd, encoder_id);
    if (encoder == NULL) {
        ok = errno;
        perror("[modesetting] Could not get DRM device encoder. drmModeGetEncoder");
        return ok;
    }

    encoder_out->encoder = encoder;
    return 0;
}

static void free_encoder(struct drm_encoder *encoder) {
    drmModeFreeEncoder(encoder->encoder);
}

static int fetch_encoders(struct drmdev *drmdev, struct drm_encoder **encoders_out, size_t *n_encoders_out) {
    struct drm_encoder *encoders;
    int n_allocated_encoders;
    int ok;

    encoders = calloc(drmdev->res->count_encoders, sizeof *encoders);
    if (encoders == NULL) {
        *encoders_out = NULL;
        *n_encoders_out = 0;
        return ENOMEM;
    }

    n_allocated_encoders = 0;
    for (int i = 0; i < drmdev->res->count_encoders; i++, n_allocated_encoders++) {
        ok = fetch_encoder(drmdev->fd, drmdev->res->encoders[i], encoders + i);
        if (ok != 0) {
            goto fail_free_encoders;
        }
    }

    *encoders_out = encoders;
    *n_encoders_out = drmdev->res->count_encoders;
    return 0;

fail_free_encoders:
    for (int i = 0; i < n_allocated_encoders; i++) {
        drmModeFreeEncoder(encoders[i].encoder);
    }

    free(encoders);

    *encoders_out = NULL;
    *n_encoders_out = 0;
    return ok;
}

static void free_encoders(struct drm_encoder *encoders, size_t n_encoders) {
    for (int i = 0; i < n_encoders; i++) {
        free_encoder(encoders + i);
    }
    free(encoders);
}

static int fetch_crtc(int drm_fd, int crtc_index, uint32_t crtc_id, struct drm_crtc *crtc_out) {
    struct drm_crtc_prop_ids ids;
    drmModeObjectProperties *props;
    drmModePropertyRes *prop_info;
    drmModeCrtc *crtc;
    int ok;

    drm_crtc_prop_ids_init(&ids);

    crtc = drmModeGetCrtc(drm_fd, crtc_id);
    if (crtc == NULL) {
        ok = errno;
        perror("[modesetting] Could not get DRM device CRTC. drmModeGetCrtc");
        return ok;
    }

    props = drmModeObjectGetProperties(drm_fd, crtc_id, DRM_MODE_OBJECT_CRTC);
    if (props == NULL) {
        ok = errno;
        perror("[modesetting] Could not get DRM device CRTCs properties. drmModeObjectGetProperties");
        goto fail_free_crtc;
    }

    for (int i = 0; i < props->count_props; i++) {
        prop_info = drmModeGetProperty(drm_fd, props->props[i]);
        if (prop_info == NULL) {
            ok = errno;
            perror("[modesetting] Could not get DRM device CRTCs properties' info. drmModeGetProperty");
            goto fail_free_props;
        }

#define CHECK_ASSIGN_PROPERTY_ID(_name_str, _name)                                       \
    if (strncmp(prop_info->name, _name_str, ARRAY_SIZE(prop_info->name)) == 0) { \
        ids._name = prop_info->prop_id;                                              \
    } else

        DRM_CRTC_PROPERTIES(CHECK_ASSIGN_PROPERTY_ID) {
            // this is the trailing else case
            LOG_DEBUG("Unknown DRM crtc property: %s\n", prop_info->name);
        }

#undef CHECK_ASSIGN_PROPERTY_ID

        drmModeFreeProperty(prop_info);
        prop_info = NULL;
    }

    crtc_out->id = crtc->crtc_id;
    crtc_out->index = crtc_index;
    crtc_out->bitmask = 1u << crtc_index;
    crtc_out->ids = ids;
    crtc_out->committed_state.has_mode = crtc->mode_valid;
    crtc_out->committed_state.mode = crtc->mode;
    crtc_out->committed_state.mode_blob = NULL;
    drmModeFreeObjectProperties(props);
    drmModeFreeCrtc(crtc);
    return 0;


fail_free_props:
    drmModeFreeObjectProperties(props);

fail_free_crtc:
    drmModeFreeCrtc(crtc);
    return ok;
}

static void free_crtc(struct drm_crtc *crtc) {
    /// TODO: Implement
    (void) crtc;
}

static int fetch_crtcs(struct drmdev *drmdev, struct drm_crtc **crtcs_out, size_t *n_crtcs_out) {
    struct drm_crtc *crtcs;
    int ok;

    crtcs = calloc(drmdev->res->count_crtcs, sizeof *crtcs);
    if (crtcs == NULL) {
        *crtcs_out = NULL;
        *n_crtcs_out = 0;
        return ENOMEM;
    }

    for (int i = 0; i < drmdev->res->count_crtcs; i++) {
        ok = fetch_crtc(drmdev->fd, i, drmdev->res->crtcs[i], crtcs + i);
        if (ok != 0) {
            for (int j = 0; j < i; j++)
                free_crtc(crtcs + i);
            goto fail_free_crtcs;
        }
    }

    *crtcs_out = crtcs;
    *n_crtcs_out = drmdev->res->count_crtcs;
    return 0;

fail_free_crtcs:
    free(crtcs);
    *crtcs_out = NULL;
    *n_crtcs_out = 0;
    return ok;
}

static int free_crtcs(struct drm_crtc *crtcs, size_t n_crtcs) {
    for (int i = 0; i < n_crtcs; i++) {
        free_crtc(crtcs + i);
    }
    free(crtcs);
    return 0;
}

static int get_supported_modified_formats(
    struct drm_format_modifier_blob *blob,
    int max_formats_out,
    int *n_formats_out,
    struct modified_format *formats_out
) {
    struct drm_format_modifier *modifiers;
    uint32_t *formats;

    DEBUG_ASSERT_NOT_NULL(blob);
    DEBUG_ASSERT_NOT_NULL(n_formats_out);
    DEBUG_ASSERT(blob->version == FORMAT_BLOB_CURRENT);

    modifiers = (void *) (((char *) blob) + blob->modifiers_offset);
    formats = (void *) (((char *) blob) + blob->formats_offset);

    int index = 0;
    for (int i = 0; i < blob->count_modifiers; i++) {
        for (int j = modifiers[i].offset; (j < blob->count_formats) && (j < modifiers[i].offset + 64); j++) {
            bool is_format_bit_set = (modifiers[i].formats & (1ull << (j % 64))) != 0;
            if (!is_format_bit_set) {
                continue;
            }

            for (int k = 0; k < kCount_PixFmt; k++) {
                if (get_pixfmt_info(k)->drm_format == formats[j]) {
                    if ((index >= max_formats_out) && formats_out) {
                        return ENOMEM;
                    } else if (formats_out) {
                        formats_out[index].format = k;
                        formats_out[index].modifier = modifiers[i].modifier;
                    }
                    index++;
                }
            }
        }
    }

    *n_formats_out = index;
    return 0;
}

static int fetch_plane(int drm_fd, uint32_t plane_id, struct drm_plane *plane_out) {
    struct drm_plane_prop_ids ids;
    drmModeObjectProperties *props;
    struct modified_format *supported_modified_formats;
    drm_plane_transform_t hardcoded_rotation, supported_rotations, committed_rotation;
    enum drm_blend_mode committed_blend_mode;
    enum drm_plane_type type;
    drmModePropertyRes *info;
    drmModePlane *plane;
    uint32_t comitted_crtc_x, comitted_crtc_y, comitted_crtc_w, comitted_crtc_h;
    uint32_t comitted_src_x, comitted_src_y, comitted_src_w, comitted_src_h;
    uint16_t committed_alpha;
    int64_t min_zpos, max_zpos, hardcoded_zpos, committed_zpos;
    bool supported_blend_modes[kCount_DrmBlendMode] = { 0 };
    bool supported_formats[kCount_PixFmt] = { 0 };
    bool has_type, has_rotation, has_zpos, has_hardcoded_zpos, has_hardcoded_rotation, supports_modifiers, has_alpha,
        has_blend_mode;
    int ok, n_supported_modified_formats;

    drm_plane_prop_ids_init(&ids);

    plane = drmModeGetPlane(drm_fd, plane_id);
    if (plane == NULL) {
        ok = errno;
        perror("[modesetting] Could not get DRM device plane. drmModeGetPlane");
        return ok;
    }

    props = drmModeObjectGetProperties(drm_fd, plane_id, DRM_MODE_OBJECT_PLANE);
    if (props == NULL) {
        ok = errno;
        perror("[modesetting] Could not get DRM device planes' properties. drmModeObjectGetProperties");
        goto fail_free_plane;
    }

    has_type = false;
    has_rotation = false;
    has_hardcoded_rotation = false;
    has_zpos = false;
    has_hardcoded_zpos = false;
    supports_modifiers = false;
    has_alpha = false;
    has_blend_mode = false;
    n_supported_modified_formats = 0;
    supported_modified_formats = NULL;
    comitted_crtc_x = comitted_crtc_y = comitted_crtc_w = comitted_crtc_h = 0;
    comitted_src_x = comitted_src_y = comitted_src_w = comitted_src_h = 0;
    for (int j = 0; j < props->count_props; j++) {
        info = drmModeGetProperty(drm_fd, props->props[j]);
        if (info == NULL) {
            ok = errno;
            perror("[modesetting] Could not get DRM device planes' properties' info. drmModeGetProperty");
            goto fail_maybe_free_supported_formats;
        }

        if (strcmp(info->name, "type") == 0) {
            DEBUG_ASSERT(has_type == false);
            has_type = true;

            type = props->prop_values[j];
        } else if (strcmp(info->name, "rotation") == 0) {
            DEBUG_ASSERT(has_rotation == false);
            has_rotation = true;

            supported_rotations = PLANE_TRANSFORM_NONE;
            DEBUG_ASSERT(info->flags & DRM_MODE_PROP_BITMASK);

            for (int k = 0; k < info->count_enums; k++) {
                supported_rotations.u32 |= 1 << info->enums[k].value;
            }

            DEBUG_ASSERT(PLANE_TRANSFORM_IS_VALID(supported_rotations));

            if (info->flags & DRM_MODE_PROP_IMMUTABLE) {
                has_hardcoded_rotation = true;
                hardcoded_rotation.u64 = props->prop_values[j];
            }

            committed_rotation.u64 = props->prop_values[j];
        } else if (strcmp(info->name, "zpos") == 0) {
            DEBUG_ASSERT(has_zpos == false);
            has_zpos = true;

            if (info->flags & DRM_MODE_PROP_SIGNED_RANGE) {
                min_zpos = *(int64_t *) (info->values + 0);
                max_zpos = *(int64_t *) (info->values + 1);
                committed_zpos = *(int64_t *) (props->prop_values + j);
                DEBUG_ASSERT(min_zpos <= max_zpos);
                DEBUG_ASSERT(min_zpos <= committed_zpos);
                DEBUG_ASSERT(committed_zpos <= max_zpos);
            } else if (info->flags & DRM_MODE_PROP_RANGE) {
                DEBUG_ASSERT(info->values[0] < (uint64_t) INT64_MAX);
                DEBUG_ASSERT(info->values[1] < (uint64_t) INT64_MAX);
                min_zpos = info->values[0];
                max_zpos = info->values[1];
                committed_zpos = props->prop_values[j];
                DEBUG_ASSERT(min_zpos <= max_zpos);
            } else {
                DEBUG_ASSERT_MSG(info->flags && false, "Invalid property type for zpos property.");
            }

            if (info->flags & DRM_MODE_PROP_IMMUTABLE) {
                has_hardcoded_zpos = true;
                DEBUG_ASSERT(props->prop_values[j] < (uint64_t) INT64_MAX);
                hardcoded_zpos = committed_zpos;
                if (min_zpos != max_zpos) {
                    LOG_DEBUG(
                        "DRM plane minimum supported zpos does not equal maximum supported zpos, even though zpos is "
                        "immutable.\n"
                    );
                    min_zpos = max_zpos = hardcoded_zpos;
                }
            }
        } else if (strcmp(info->name, "SRC_X") == 0) {
            comitted_src_x = props->prop_values[j];
        } else if (strcmp(info->name, "SRC_Y") == 0) {
            comitted_src_y = props->prop_values[j];
        } else if (strcmp(info->name, "SRC_W") == 0) {
            comitted_src_w = props->prop_values[j];
        } else if (strcmp(info->name, "SRC_H") == 0) {
            comitted_src_h = props->prop_values[j];
        } else if (strcmp(info->name, "CRTC_X") == 0) {
            comitted_crtc_x = props->prop_values[j];
        } else if (strcmp(info->name, "CRTC_Y") == 0) {
            comitted_crtc_y = props->prop_values[j];
        } else if (strcmp(info->name, "CRTC_W") == 0) {
            comitted_crtc_w = props->prop_values[j];
        } else if (strcmp(info->name, "CRTC_H") == 0) {
            comitted_crtc_h = props->prop_values[j];
        } else if (strcmp(info->name, "IN_FORMATS") == 0) {
            drmModePropertyBlobRes *blob;

            blob = drmModeGetPropertyBlob(drm_fd, props->prop_values[j]);
            if (blob == NULL) {
                ok = errno;
                LOG_ERROR(
                    "Couldn't get list of supported format modifiers for plane %u. drmModeGetPropertyBlob: %s\n",
                    plane_id,
                    strerror(ok)
                );
                drmModeFreeProperty(info);
                goto fail_free_props;
            }

            supports_modifiers = true;
            n_supported_modified_formats = 0;

            get_supported_modified_formats(blob->data, 0, &n_supported_modified_formats, NULL);

            supported_modified_formats = calloc(sizeof *supported_modified_formats, n_supported_modified_formats);
            if (supported_modified_formats == NULL) {
                ok = ENOMEM;
                drmModeFreePropertyBlob(blob);
                drmModeFreeProperty(info);
                goto fail_free_props;
            }

            ok = get_supported_modified_formats(
                blob->data,
                n_supported_modified_formats,
                &n_supported_modified_formats,
                supported_modified_formats
            );
            if (ok != 0) {
                free(supported_modified_formats);
                drmModeFreePropertyBlob(blob);
                drmModeFreeProperty(info);
                goto fail_free_props;
            }

            drmModeFreePropertyBlob(blob);
        } else if (strcmp(info->name, "alpha") == 0) {
            has_alpha = true;
            DEBUG_ASSERT(info->flags == DRM_MODE_PROP_RANGE);
            DEBUG_ASSERT(info->values[0] == 0);
            DEBUG_ASSERT(info->values[1] == 0xFFFF);
            DEBUG_ASSERT(props->prop_values[j] <= 0xFFFF);

            committed_alpha = (uint16_t) props->prop_values[j];
        } else if (strcmp(info->name, "pixel blend mode") == 0) {
            has_blend_mode = true;
            DEBUG_ASSERT(info->flags == DRM_MODE_PROP_ENUM);

            for (int i = 0; i < info->count_enums; i++) {
                if (strcmp(info->enums[i].name, "None") == 0) {
                    DEBUG_ASSERT_EQUALS(info->enums[i].value, kNone_DrmBlendMode);
                    supported_blend_modes[kNone_DrmBlendMode] = true;
                } else if (strcmp(info->enums[i].name, "Pre-multiplied") == 0) {
                    DEBUG_ASSERT_EQUALS(info->enums[i].value, kPremultiplied_DrmBlendMode);
                    supported_blend_modes[kPremultiplied_DrmBlendMode] = true;
                } else if (strcmp(info->enums[i].name, "Coverage") == 0) {
                    DEBUG_ASSERT_EQUALS(info->enums[i].value, kCoverage_DrmBlendMode);
                    supported_blend_modes[kCoverage_DrmBlendMode] = true;
                } else {
                    LOG_DEBUG(
                        "Unknown KMS pixel blend mode: %s (value: %" PRIu64 ")\n",
                        info->enums[i].name,
                        (uint64_t) info->enums[i].value
                    );
                }
            }

            committed_blend_mode = props->prop_values[j];
            DEBUG_ASSERT(committed_blend_mode >= 0 && committed_blend_mode <= kMax_DrmBlendMode);
            DEBUG_ASSERT(supported_blend_modes[committed_blend_mode]);
        }

#define CHECK_ASSIGN_PROPERTY_ID(_name_str, _name) \
        else if (strncmp(info->name, _name_str, ARRAY_SIZE(info->name)) == 0) { \
            ids._name = info->prop_id; \
        }

        DRM_PLANE_PROPERTIES(CHECK_ASSIGN_PROPERTY_ID)

#undef CHECK_ASSIGN_PROPERTY_ID

        drmModeFreeProperty(info);
    }

    DEBUG_ASSERT(has_type);
    (void) has_type;

    for (int i = 0; i < plane->count_formats; i++) {
        for (int j = 0; j < kCount_PixFmt; j++) {
            if (get_pixfmt_info(j)->drm_format == plane->formats[i]) {
                supported_formats[j] = true;
                break;
            }
        }
    }

    plane_out->id = plane->plane_id;
    plane_out->possible_crtcs = plane->possible_crtcs;
    plane_out->ids = ids;
    plane_out->type = type;
    plane_out->has_zpos = has_zpos;
    plane_out->min_zpos = min_zpos;
    plane_out->max_zpos = max_zpos;
    plane_out->has_hardcoded_zpos = has_hardcoded_zpos;
    plane_out->hardcoded_zpos = hardcoded_zpos;
    plane_out->has_rotation = has_rotation;
    plane_out->supported_rotations = supported_rotations;
    plane_out->has_hardcoded_rotation = has_hardcoded_rotation;
    plane_out->hardcoded_rotation = hardcoded_rotation;
    memcpy(plane_out->supported_formats, supported_formats, sizeof supported_formats);
    plane_out->supports_modifiers = supports_modifiers;
    plane_out->n_supported_modified_formats = n_supported_modified_formats;
    plane_out->supported_modified_formats = supported_modified_formats;
    plane_out->has_alpha = has_alpha;
    plane_out->has_blend_mode = has_blend_mode;
    memcpy(plane_out->supported_blend_modes, supported_blend_modes, sizeof supported_blend_modes);
    plane_out->committed_state.crtc_id = plane->crtc_id;
    plane_out->committed_state.fb_id = plane->fb_id;
    plane_out->committed_state.src_x = comitted_src_x;
    plane_out->committed_state.src_y = comitted_src_y;
    plane_out->committed_state.src_w = comitted_src_w;
    plane_out->committed_state.src_h = comitted_src_h;
    plane_out->committed_state.crtc_x = comitted_crtc_x;
    plane_out->committed_state.crtc_y = comitted_crtc_y;
    plane_out->committed_state.crtc_w = comitted_crtc_w;
    plane_out->committed_state.crtc_h = comitted_crtc_h;
    plane_out->committed_state.zpos = committed_zpos;
    plane_out->committed_state.rotation = committed_rotation;
    plane_out->committed_state.alpha = committed_alpha;
    plane_out->committed_state.blend_mode = committed_blend_mode;
    drmModeFreeObjectProperties(props);
    drmModeFreePlane(plane);
    return 0;

fail_maybe_free_supported_formats:
    if (supported_modified_formats != NULL)
        free(supported_modified_formats);

fail_free_props:
    drmModeFreeObjectProperties(props);

fail_free_plane:
    drmModeFreePlane(plane);
    return ok;
}

static void free_plane(struct drm_plane *plane) {
    if (plane->supported_modified_formats != NULL) {
        free(plane->supported_modified_formats);
    }
}

static int fetch_planes(struct drmdev *drmdev, struct drm_plane **planes_out, size_t *n_planes_out) {
    struct drm_plane *planes;
    int ok;

    planes = calloc(drmdev->plane_res->count_planes, sizeof *planes);
    if (planes == NULL) {
        *planes_out = NULL;
        return ENOMEM;
    }

    for (int i = 0; i < drmdev->plane_res->count_planes; i++) {
        ok = fetch_plane(drmdev->fd, drmdev->plane_res->planes[i], planes + i);
        if (ok != 0) {
            for (int j = 0; j < i; j++) {
                free_plane(planes + i);
            }
            free(planes);
            return ENOMEM;
        }

        DEBUG_ASSERT_MSG(
            planes[0].has_zpos == planes[i].has_zpos,
            "If one plane has a zpos property, all planes need to have one."
        );
    }

    *planes_out = planes;
    *n_planes_out = drmdev->plane_res->count_planes;

    return 0;
}

static void free_planes(struct drm_plane *planes, size_t n_planes) {
    for (int i = 0; i < n_planes; i++) {
        free_plane(planes + i);
    }
    free(planes);
}

static void assert_rotations_work() {
    DEBUG_ASSERT(PLANE_TRANSFORM_ROTATE_0.rotate_0 == true);
    DEBUG_ASSERT(PLANE_TRANSFORM_ROTATE_0.rotate_90 == false);
    DEBUG_ASSERT(PLANE_TRANSFORM_ROTATE_0.rotate_180 == false);
    DEBUG_ASSERT(PLANE_TRANSFORM_ROTATE_0.rotate_270 == false);
    DEBUG_ASSERT(PLANE_TRANSFORM_ROTATE_0.reflect_x == false);
    DEBUG_ASSERT(PLANE_TRANSFORM_ROTATE_0.reflect_y == false);

    DEBUG_ASSERT(PLANE_TRANSFORM_ROTATE_90.rotate_0 == false);
    DEBUG_ASSERT(PLANE_TRANSFORM_ROTATE_90.rotate_90 == true);
    DEBUG_ASSERT(PLANE_TRANSFORM_ROTATE_90.rotate_180 == false);
    DEBUG_ASSERT(PLANE_TRANSFORM_ROTATE_90.rotate_270 == false);
    DEBUG_ASSERT(PLANE_TRANSFORM_ROTATE_90.reflect_x == false);
    DEBUG_ASSERT(PLANE_TRANSFORM_ROTATE_90.reflect_y == false);

    DEBUG_ASSERT(PLANE_TRANSFORM_ROTATE_180.rotate_0 == false);
    DEBUG_ASSERT(PLANE_TRANSFORM_ROTATE_180.rotate_90 == false);
    DEBUG_ASSERT(PLANE_TRANSFORM_ROTATE_180.rotate_180 == true);
    DEBUG_ASSERT(PLANE_TRANSFORM_ROTATE_180.rotate_270 == false);
    DEBUG_ASSERT(PLANE_TRANSFORM_ROTATE_180.reflect_x == false);
    DEBUG_ASSERT(PLANE_TRANSFORM_ROTATE_180.reflect_y == false);

    DEBUG_ASSERT(PLANE_TRANSFORM_ROTATE_270.rotate_0 == false);
    DEBUG_ASSERT(PLANE_TRANSFORM_ROTATE_270.rotate_90 == false);
    DEBUG_ASSERT(PLANE_TRANSFORM_ROTATE_270.rotate_180 == false);
    DEBUG_ASSERT(PLANE_TRANSFORM_ROTATE_270.rotate_270 == true);
    DEBUG_ASSERT(PLANE_TRANSFORM_ROTATE_270.reflect_x == false);
    DEBUG_ASSERT(PLANE_TRANSFORM_ROTATE_270.reflect_y == false);

    DEBUG_ASSERT(PLANE_TRANSFORM_REFLECT_X.rotate_0 == false);
    DEBUG_ASSERT(PLANE_TRANSFORM_REFLECT_X.rotate_90 == false);
    DEBUG_ASSERT(PLANE_TRANSFORM_REFLECT_X.rotate_180 == false);
    DEBUG_ASSERT(PLANE_TRANSFORM_REFLECT_X.rotate_270 == false);
    DEBUG_ASSERT(PLANE_TRANSFORM_REFLECT_X.reflect_x == true);
    DEBUG_ASSERT(PLANE_TRANSFORM_REFLECT_X.reflect_y == false);

    DEBUG_ASSERT(PLANE_TRANSFORM_REFLECT_Y.rotate_0 == false);
    DEBUG_ASSERT(PLANE_TRANSFORM_REFLECT_Y.rotate_90 == false);
    DEBUG_ASSERT(PLANE_TRANSFORM_REFLECT_Y.rotate_180 == false);
    DEBUG_ASSERT(PLANE_TRANSFORM_REFLECT_Y.rotate_270 == false);
    DEBUG_ASSERT(PLANE_TRANSFORM_REFLECT_Y.reflect_x == false);
    DEBUG_ASSERT(PLANE_TRANSFORM_REFLECT_Y.reflect_y == true);

    drm_plane_transform_t r = PLANE_TRANSFORM_NONE;

    r.rotate_0 = true;
    r.reflect_x = true;
    DEBUG_ASSERT(r.u32 == (DRM_MODE_ROTATE_0 | DRM_MODE_REFLECT_X));

    r.u32 = DRM_MODE_ROTATE_90 | DRM_MODE_REFLECT_Y;
    DEBUG_ASSERT(r.rotate_0 == false);
    DEBUG_ASSERT(r.rotate_90 == true);
    DEBUG_ASSERT(r.rotate_180 == false);
    DEBUG_ASSERT(r.rotate_270 == false);
    DEBUG_ASSERT(r.reflect_x == false);
    DEBUG_ASSERT(r.reflect_y == true);
    (void) r;
}

static int set_drm_client_caps(int fd, bool *supports_atomic_modesetting) {
    int ok;

    ok = drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    if (ok < 0) {
        ok = errno;
        LOG_ERROR("Could not set DRM client universal planes capable. drmSetClientCap: %s\n", strerror(ok));
        return ok;
    }

    ok = drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);
    if ((ok < 0) && (errno == EOPNOTSUPP)) {
        if (supports_atomic_modesetting != NULL) {
            *supports_atomic_modesetting = false;
        }
    } else if (ok < 0) {
        ok = errno;
        LOG_ERROR("Could not set DRM client atomic capable. drmSetClientCap: %s\n", strerror(ok));
        return ok;
    } else {
        if (supports_atomic_modesetting != NULL) {
            *supports_atomic_modesetting = true;
        }
    }

    return 0;
}

struct drmdev *drmdev_new_from_fd(int fd, const struct drmdev_interface *interface, void *userdata) {
    struct gbm_device *gbm_device;
    struct drmdev *drmdev;
    bool supports_atomic_modesetting;
    int ok, master_fd, event_fd;

    assert_rotations_work();

    drmdev = malloc(sizeof *drmdev);
    if (drmdev == NULL) {
        return NULL;
    }

    master_fd = fd;

    ok = set_drm_client_caps(fd, &supports_atomic_modesetting);
    if (ok != 0) {
        goto fail_close_master_fd;
    }

    if (master_fd > 0) {
        bool master_fd_supports_atomic_modesetting;

        ok = set_drm_client_caps(master_fd, &master_fd_supports_atomic_modesetting);
        if (ok != 0) {
            goto fail_close_master_fd;
        }

        DEBUG_ASSERT_EQUALS(supports_atomic_modesetting, master_fd_supports_atomic_modesetting);
    }

    drmdev->res = drmModeGetResources(fd);
    if (drmdev->res == NULL) {
        ok = errno;
        LOG_ERROR("Could not get DRM device resources. drmModeGetResources: %s\n", strerror(ok));
        goto fail_free_drmdev;
    }

    drmdev->plane_res = drmModeGetPlaneResources(fd);
    if (drmdev->plane_res == NULL) {
        ok = errno;
        LOG_ERROR("Could not get DRM device planes resources. drmModeGetPlaneResources: %s\n", strerror(ok));
        goto fail_free_resources;
    }

    drmdev->fd = fd;

    ok = fetch_connectors(drmdev, &drmdev->connectors, &drmdev->n_connectors);
    if (ok != 0) {
        goto fail_free_plane_resources;
    }

    ok = fetch_encoders(drmdev, &drmdev->encoders, &drmdev->n_encoders);
    if (ok != 0) {
        goto fail_free_connectors;
    }

    ok = fetch_crtcs(drmdev, &drmdev->crtcs, &drmdev->n_crtcs);
    if (ok != 0) {
        goto fail_free_encoders;
    }

    ok = fetch_planes(drmdev, &drmdev->planes, &drmdev->n_planes);
    if (ok != 0) {
        goto fail_free_crtcs;
    }

    gbm_device = gbm_create_device(drmdev->fd);
    if (gbm_device == NULL) {
        LOG_ERROR("Could not create GBM device.\n");
        goto fail_free_planes;
    }

    event_fd = epoll_create1(EPOLL_CLOEXEC);
    if (event_fd < 0) {
        LOG_ERROR("Could not create modesetting epoll instance.\n");
        goto fail_destroy_gbm_device;
    }

    ok =
        epoll_ctl(event_fd, EPOLL_CTL_ADD, fd, &(struct epoll_event){ .events = EPOLLIN | EPOLLPRI, .data.ptr = NULL });
    if (ok != 0) {
        LOG_ERROR("Could not add DRM file descriptor to epoll instance.\n");
        goto fail_close_event_fd;
    }

    pthread_mutex_init(&drmdev->mutex, NULL);
    drmdev->n_refs = REFCOUNT_INIT_1;
    drmdev->fd = fd;
    drmdev->supports_atomic_modesetting = supports_atomic_modesetting;
    drmdev->gbm_device = gbm_device;
    drmdev->event_fd = event_fd;
    memset(drmdev->per_crtc_state, 0, sizeof(drmdev->per_crtc_state));
    drmdev->master_fd = master_fd;
    drmdev->interface = *interface;
    drmdev->userdata = userdata;
    return drmdev;

fail_close_event_fd:
    close(event_fd);

fail_destroy_gbm_device:
    gbm_device_destroy(gbm_device);

fail_free_planes:
    free_planes(drmdev->planes, drmdev->n_planes);

fail_free_crtcs:
    free_crtcs(drmdev->crtcs, drmdev->n_crtcs);

fail_free_encoders:
    free_encoders(drmdev->encoders, drmdev->n_encoders);

fail_free_connectors:
    free_connectors(drmdev->connectors, drmdev->n_connectors);

fail_free_plane_resources:
    drmModeFreePlaneResources(drmdev->plane_res);

fail_free_resources:
    drmModeFreeResources(drmdev->res);

fail_close_master_fd:
    interface->close(master_fd, NULL, userdata);

fail_free_drmdev:
    free(drmdev);

    return NULL;
}

struct drmdev *drmdev_new_from_path(const char *path, const struct drmdev_interface *interface, void *userdata) {
    struct drmdev *drmdev;
    int fd;

    DEBUG_ASSERT_NOT_NULL(path);
    DEBUG_ASSERT_NOT_NULL(interface);

    fd = open(path, O_RDWR);
    if (fd < 0) {
        LOG_ERROR("Could not open DRM device. open: %s\n", strerror(errno));
        return NULL;
    }

    drmdev = drmdev_new_from_fd(fd, interface, userdata);
    if (drmdev == NULL) {
        close(fd);
        return NULL;
    }

    return drmdev;
}

void drmdev_destroy(struct drmdev *drmdev) {
    DEBUG_ASSERT(refcount_is_zero(&drmdev->n_refs));
    
    close(drmdev->event_fd);
    gbm_device_destroy(drmdev->gbm_device);
    free_planes(drmdev->planes, drmdev->n_planes);
    free_crtcs(drmdev->crtcs, drmdev->n_crtcs);
    free_encoders(drmdev->encoders, drmdev->n_encoders);
    free_connectors(drmdev->connectors, drmdev->n_connectors);
    drmModeFreePlaneResources(drmdev->plane_res);
    drmModeFreeResources(drmdev->res);
    free(drmdev);
}

DEFINE_REF_OPS(drmdev, n_refs)

int drmdev_get_fd(struct drmdev *drmdev) {
    DEBUG_ASSERT_NOT_NULL(drmdev);
    return drmdev->master_fd;
}

int drmdev_get_event_fd(struct drmdev *drmdev) {
    DEBUG_ASSERT_NOT_NULL(drmdev);
    return drmdev->master_fd;
}

static void drmdev_on_page_flip_locked(
    int fd,
    unsigned int sequence,
    unsigned int tv_sec,
    unsigned int tv_usec,
    unsigned int crtc_id,
    void *userdata
) {
    struct kms_req_builder *builder;
    struct drm_crtc *crtc;
    struct kms_req **last_flipped;
    struct kms_req *req;
    struct drmdev *drmdev;

    DEBUG_ASSERT_NOT_NULL(userdata);
    builder = userdata;
    req = userdata;

    (void) fd;
    (void) sequence;
    (void) crtc_id;

    drmdev = builder->drmdev;
    for_each_crtc_in_drmdev(drmdev, crtc) {
        if (crtc->id == crtc_id) {
            break;
        }
    }

    DEBUG_ASSERT_NOT_NULL_MSG(crtc, "Invalid CRTC id");

    if (drmdev->per_crtc_state[crtc->index].scanout_callback != NULL) {
        uint64_t vblank_ns = tv_sec * 1000000000ull + tv_usec * 1000ull;
        drmdev->per_crtc_state[crtc->index].scanout_callback(drmdev, vblank_ns, drmdev->per_crtc_state[crtc->index].userdata);

        // clear the scanout callback
        drmdev->per_crtc_state[crtc->index].scanout_callback = NULL;
        drmdev->per_crtc_state[crtc->index].destroy_callback = NULL;
        drmdev->per_crtc_state[crtc->index].userdata = NULL;
    }

    last_flipped = &drmdev->per_crtc_state[crtc->index].last_flipped;
    if (*last_flipped != NULL) {
        /// TODO: Remove this if we ever cache KMS reqs.
        /// FIXME: This will fail if we're using blocking commits.
        // DEBUG_ASSERT(refcount_is_one(&((struct kms_req_builder*) *last_flipped)->n_refs));
    }

    kms_req_swap_ptrs(last_flipped, req);
    kms_req_unref(req);
}

static int drmdev_on_modesetting_fd_ready_locked(struct drmdev *drmdev) {
    int ok;

    static drmEventContext ctx = {
        .version = DRM_EVENT_CONTEXT_VERSION,
        .vblank_handler = NULL,
        .page_flip_handler = NULL,
        .page_flip_handler2 = drmdev_on_page_flip_locked,
        .sequence_handler = NULL,
    };

    ok = drmHandleEvent(drmdev->master_fd, &ctx);
    if (ok != 0) {
        return EIO;
    }

    return 0;
}

int drmdev_on_event_fd_ready(struct drmdev *drmdev) {
    struct epoll_event events[16];
    int ok, n_events;

    DEBUG_ASSERT_NOT_NULL(drmdev);

    drmdev_lock(drmdev);

    while (1) {
        ok = epoll_wait(drmdev->event_fd, events, ARRAY_SIZE(events), 0);
        if ((ok < 0) && (errno == EINTR)) {
            // retry
            continue;
        } else if (ok < 0) {
            ok = errno;
            LOG_ERROR("Could read kernel modesetting events. epoll_wait: %s\n", strerror(ok));
            goto fail_unlock;
        } else {
            break;
        }
    }

    n_events = ok;
    for (int i = 0; i < n_events; i++) {
        // currently this could only be the root drmdev fd.
        DEBUG_ASSERT_EQUALS(events[i].data.ptr, NULL);
        ok = drmdev_on_modesetting_fd_ready_locked(drmdev);
        if (ok != 0) {
            goto fail_unlock;
        }
    }

    drmdev_unlock(drmdev);

    return 0;

fail_unlock:
    drmdev_unlock(drmdev);
    return ok;
}

struct gbm_device *drmdev_get_gbm_device(struct drmdev *drmdev) {
    DEBUG_ASSERT_NOT_NULL(drmdev);
    return drmdev->gbm_device;
}

int drmdev_get_last_vblank(struct drmdev *drmdev, uint32_t crtc_id, uint64_t *last_vblank_ns_out) {
    int ok;

    DEBUG_ASSERT_NOT_NULL(drmdev);
    DEBUG_ASSERT_NOT_NULL(last_vblank_ns_out);

    ok = drmCrtcGetSequence(drmdev->fd, crtc_id, NULL, last_vblank_ns_out);
    if (ok < 0) {
        ok = errno;
        LOG_ERROR("Could not get next vblank timestamp. drmCrtcGetSequence: %s\n", strerror(ok));
        return ok;
    }

    return 0;
}

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
) {
    uint32_t fb_id;
    int ok;

    DEBUG_ASSERT_NOT_NULL(drmdev);
    DEBUG_ASSERT(width > 0 && height > 0);
    DEBUG_ASSERT(bo_handle != 0);
    DEBUG_ASSERT(pitch != 0);

    fb_id = 0;
    if (has_modifier) {
        LOG_DEBUG("adding fb with pixel format %"PRIu32" and modifier %"PRIu64"\n", get_pixfmt_info(pixel_format)->drm_format, modifier);
        ok = drmModeAddFB2WithModifiers(
            drmdev->fd,
            width,
            height,
            get_pixfmt_info(pixel_format)->drm_format,
            (uint32_t[4]){ bo_handle, 0 },
            (uint32_t[4]){ pitch, 0 },
            (uint32_t[4]){ offset, 0 },
            (const uint64_t[4]){ modifier, 0 },
            &fb_id,
            DRM_MODE_FB_MODIFIERS
        );
        if (ok < 0) {
            LOG_ERROR("Couldn't add buffer as DRM fb. drmModeAddFB2WithModifiers: %s\n", strerror(-ok));
            return 0;
        }
    } else {
        ok = drmModeAddFB2(
            drmdev->fd,
            width,
            height,
            get_pixfmt_info(pixel_format)->drm_format,
            (uint32_t[4]){ bo_handle, 0 },
            (uint32_t[4]){ pitch, 0 },
            (uint32_t[4]){ offset, 0 },
            &fb_id,
            0
        );
        if (ok < 0) {
            LOG_ERROR("Couldn't add buffer as DRM fb. drmModeAddFB2: %s\n", strerror(-ok));
            return 0;
        }
    }

    DEBUG_ASSERT(fb_id != 0);
    return fb_id;
}

uint32_t drmdev_add_fb_multiplanar(
    struct drmdev *drmdev,
    uint32_t width,
    uint32_t height,
    enum pixfmt pixel_format,
    uint32_t bo_handles[4],
    uint32_t pitches[4],
    uint32_t offsets[4],
    bool has_modifiers,
    uint64_t modifiers[4]
) {
    uint32_t fb_id;
    int ok;

    DEBUG_ASSERT_NOT_NULL(drmdev);
    DEBUG_ASSERT(width > 0 && height > 0);
    DEBUG_ASSERT(bo_handles[0] != 0);
    DEBUG_ASSERT(pitches[0] != 0);

    fb_id = 0;
    if (has_modifiers) {
        ok = drmModeAddFB2WithModifiers(
            drmdev->fd,
            width,
            height,
            get_pixfmt_info(pixel_format)->drm_format,
            bo_handles,
            pitches,
            offsets,
            modifiers,
            &fb_id,
            DRM_MODE_FB_MODIFIERS
        );
        if (ok < 0) {
            LOG_ERROR("Couldn't add buffer as DRM fb. drmModeAddFB2WithModifiers: %s\n", strerror(-ok));
            return 0;
        }
    } else {
        ok = drmModeAddFB2(
            drmdev->fd,
            width,
            height,
            get_pixfmt_info(pixel_format)->drm_format,
            bo_handles,
            pitches,
            offsets,
            &fb_id,
            0
        );
        if (ok < 0) {
            LOG_ERROR("Couldn't add buffer as DRM fb. drmModeAddFB2: %s\n", strerror(-ok));
            return 0;
        }
    }

    DEBUG_ASSERT(fb_id != 0);
    return fb_id;
}

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
) {
    uint32_t bo_handle;
    int ok;

    ok = drmPrimeFDToHandle(drmdev->fd, prime_fd, &bo_handle);
    if (ok < 0) {
        LOG_ERROR("Couldn't import DMA-buffer as GEM buffer. drmPrimeFDToHandle: %s\n", strerror(errno));
        return 0;
    }

    return drmdev_add_fb(drmdev, width, height, pixel_format, prime_fd, pitch, offset, has_modifier, modifier);
}

uint32_t drmdev_add_fb_from_dmabuf_multiplanar(
    struct drmdev *drmdev,
    uint32_t width,
    uint32_t height,
    enum pixfmt pixel_format,
    int prime_fds[4],
    uint32_t pitches[4],
    uint32_t offsets[4],
    bool has_modifiers,
    uint64_t modifiers[4]
) {
    uint32_t bo_handles[4] = { 0 };
    int ok;

    for (int i = 0; (i < 4) && (prime_fds[i] != 0); i++) {
        ok = drmPrimeFDToHandle(drmdev->fd, prime_fds[i], bo_handles + i);
        if (ok < 0) {
            LOG_ERROR("Couldn't import DMA-buffer as GEM buffer. drmPrimeFDToHandle: %s\n", strerror(errno));
            return 0;
        }
    }

    return drmdev_add_fb_multiplanar(
        drmdev,
        width,
        height,
        pixel_format,
        bo_handles,
        pitches,
        offsets,
        has_modifiers,
        modifiers
    );
}

int drmdev_rm_fb(struct drmdev *drmdev, uint32_t fb_id) {
    int ok;

    ok = drmModeRmFB(drmdev->fd, fb_id);
    if (ok < 0) {
        LOG_ERROR("Could not remove DRM framebuffer. drmModeRmFB: %s\n", strerror(-ok));
        return -ok;
    }

    return 0;
}

bool drmdev_can_modeset(struct drmdev *drmdev) {
    bool can_modeset;

    DEBUG_ASSERT_NOT_NULL(drmdev);

    drmdev_lock(drmdev);
    can_modeset = drmdev->master_fd > 0;
    drmdev_unlock(drmdev);

    return can_modeset;
}

void drmdev_suspend(struct drmdev *drmdev) {
    DEBUG_ASSERT_NOT_NULL(drmdev);

    drmdev_lock(drmdev);

    if (drmdev->master_fd <= 0) {
        LOG_ERROR("drmdev_suspend was called, but drmdev is already suspended\n");
        drmdev_unlock(drmdev);
        return;
    }

    drmdev->interface.close(drmdev->master_fd, drmdev->master_fd_metadata, drmdev->userdata);
    drmdev->master_fd = -1;
    drmdev->master_fd_metadata = NULL;

    drmdev_unlock(drmdev);
}

int drmdev_resume(struct drmdev *drmdev) {
    drmDevicePtr device;
    void *fd_metadata;
    int ok, master_fd;

    DEBUG_ASSERT_NOT_NULL(drmdev);

    drmdev_lock(drmdev);
    
    if (drmdev->master_fd > 0) {
        ok = EINVAL;
        LOG_ERROR("drmdev_resume was called, but drmdev is already resumed\n");
        goto fail_unlock;
    }

    ok = drmGetDevice(drmdev->fd, &device);
    if (ok < 0) {
        ok = errno;
        LOG_ERROR("Couldn't query DRM device info. drmGetDevice: %s\n", strerror(ok));
        goto fail_unlock;
    }

    ok = drmdev->interface.open(device->nodes[DRM_NODE_PRIMARY], O_CLOEXEC | O_NONBLOCK, &fd_metadata, drmdev->userdata);
    if (ok < 0) {
        ok = -ok;
        LOG_ERROR("Couldn't open DRM device.\n");
        goto fail_free_device;   
    }

    master_fd = ok;

    drmFreeDevice(&device);

    ok = set_drm_client_caps(master_fd, NULL);
    if (ok != 0) {
        goto fail_close_device;
    }

    drmdev->master_fd = master_fd;
    drmdev->master_fd_metadata = fd_metadata;
    drmdev_unlock(drmdev);
    return 0;

    fail_close_device:
    drmdev->interface.close(master_fd, fd_metadata, drmdev->userdata);
    goto fail_unlock;

    fail_free_device:
    drmFreeDevice(&device);

    fail_unlock:
    drmdev_unlock(drmdev);
    return ok;
}

static void drmdev_set_scanout_callback_locked(
    struct drmdev *drmdev,
    uint32_t crtc_id,
    kms_scanout_cb_t scanout_callback,
    void *userdata,
    void_callback_t destroy_callback
) {
    struct drm_crtc *crtc;

    DEBUG_ASSERT_NOT_NULL(drmdev);

    for_each_crtc_in_drmdev(drmdev, crtc) {
        if (crtc->id == crtc_id) {
            break;
        }
    }

    DEBUG_ASSERT_NOT_NULL_MSG(crtc, "Could not find CRTC with given id.");

    // If there's already a scanout callback configured, this is probably a state machine error.
    // The scanout callback is configured in kms_req_commit and is cleared after it was called.
    // So if this is called again this mean kms_req_commit is called but the previous frame wasn't committed yet.
    DEBUG_ASSERT_EQUALS_MSG(drmdev->per_crtc_state[crtc->index].scanout_callback, NULL, "There's already a scanout callback configured for this CRTC.");
    drmdev->per_crtc_state[crtc->index].scanout_callback = scanout_callback;
    drmdev->per_crtc_state[crtc->index].destroy_callback = destroy_callback;
    drmdev->per_crtc_state[crtc->index].userdata = userdata;
}

MAYBE_UNUSED static struct drm_plane *get_plane_by_id(struct drmdev *drmdev, uint32_t plane_id) {
    struct drm_plane *plane;

    plane = NULL;
    for (int i = 0; i < drmdev->n_planes; i++) {
        if (drmdev->planes[i].id == plane_id) {
            plane = drmdev->planes + i;
            break;
        }
    }

    return plane;
}

struct drm_connector *__next_connector(const struct drmdev *drmdev, const struct drm_connector *connector) {
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

struct drm_encoder *__next_encoder(const struct drmdev *drmdev, const struct drm_encoder *encoder) {
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

struct drm_crtc *__next_crtc(const struct drmdev *drmdev, const struct drm_crtc *crtc) {
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

struct drm_plane *__next_plane(const struct drmdev *drmdev, const struct drm_plane *plane) {
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

drmModeModeInfo *__next_mode(const struct drm_connector *connector, const drmModeModeInfo *mode) {
    bool found = mode == NULL;
    for (int i = 0; i < connector->variable_state.n_modes; i++) {
        if (connector->variable_state.modes + i == mode) {
            found = true;
        } else if (found) {
            return connector->variable_state.modes + i;
        }
    }

    return NULL;
}

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
    if (plane->type == kPrimary_DrmPlaneType) {
        if (!allow_primary) {
            return false;
        }
    } else if (plane->type == kOverlay_DrmPlaneType) {
        if (!allow_overlay) {
            return false;
        }
    } else if (plane->type == kCursor_DrmPlaneType) {
        if (!allow_cursor) {
            return false;
        }
    }
    if (has_modifier) {
        if (!plane->supported_modified_formats) {
            // return false if we want a modified format but the plane doesn't support modified formats
            return false;
        }

        // search for the modified format in the list of supported modified formats
        for (int i = 0; i < plane->n_supported_modified_formats; i++) {
            if (plane->supported_modified_formats[i].format == format &&
                plane->supported_modified_formats[i].modifier == modifier) {
                goto found;
            }
        }

        // not found in the supported modified format list
        return false;
    } else {
        // we don't want a modified format, return false if the format is not in the list
        // of supported (unmodified) formats
        if (!plane->supported_formats[format]) {
            return false;
        }
    }

found:
    if (has_zpos) {
        if (!plane->has_zpos) {
            // return false if we want a zpos but the plane doesn't support one
            return false;
        } else if (zpos_lower_limit > plane->max_zpos || zpos_upper_limit < plane->min_zpos) {
            // return false if the zpos we want is outside the supported range of the plane
            return false;
        }
    }
    if (has_id_range && plane->id < id_lower_limit) {
        return false;
    }
    if (has_rotation) {
        if (!plane->has_rotation) {
            // return false if the plane doesn't support rotation
            return false;
        } else if (plane->has_hardcoded_rotation && plane->hardcoded_rotation.u32 != rotation.u32) {
            // return false if the plane has a hardcoded rotation and the rotation we want
            // is not the hardcoded one
            return false;
        } else if (rotation.u32 & ~plane->hardcoded_rotation.u32) {
            // return false if we can't construct the rotation using the rotation
            // bits that are supported by the plane
            return false;
        }
    }
    return true;
}

static struct drm_plane *allocate_plane(
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
    for (int i = 0; i < BMAP_SIZE(builder->planes); i++) {
        struct drm_plane *plane = builder->drmdev->planes + i;

        if (BMAP_IS_SET(builder->planes, i)) {
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
            BMAP_CLEAR(builder->planes, i);
            return plane;
        }
    }

    // we didn't find an available plane matching our criteria
    return NULL;
}

static void release_plane(struct kms_req_builder *builder, uint32_t plane_id) {
    struct drm_plane *plane;
    int index;

    index = 0;
    for_each_plane_in_drmdev(builder->drmdev, plane) {
        if (plane->id == plane_id) {
            break;
        }
        index++;
    }

    if (plane == NULL) {
        LOG_ERROR("Could not release invalid plane %" PRIu32 ".\n", plane_id);
        return;
    }

    DEBUG_ASSERT(!BMAP_IS_SET(builder->planes, index));
    BMAP_SET(builder->planes, index);
}

struct kms_req_builder *drmdev_create_request_builder(struct drmdev *drmdev, uint32_t crtc_id) {
    struct kms_req_builder *builder;
    drmModeAtomicReq *req;
    struct drm_crtc *crtc;
    int64_t min_zpos;
    bool supports_atomic_modesetting;

    DEBUG_ASSERT_NOT_NULL(drmdev);
    DEBUG_ASSERT(crtc_id != 0 && crtc_id != 0xFFFFFFFF);

    drmdev_lock(drmdev);

    for_each_crtc_in_drmdev(drmdev, crtc) {
        if (crtc->id == crtc_id) {
            break;
        }
    }

    if (crtc == NULL) {
        LOG_ERROR("Invalid CRTC id: %" PRId32 "\n", crtc_id);
        goto fail_unlock;
    }

    builder = malloc(sizeof *builder);
    if (builder == NULL) {
        goto fail_unlock;
    }

    supports_atomic_modesetting = drmdev->supports_atomic_modesetting;

    if (supports_atomic_modesetting) {
        req = drmModeAtomicAlloc();
        if (req == NULL) {
            goto fail_free_builder;
        }
    } else {
        req = NULL;
    }

    min_zpos = INT64_MAX;
    BMAP_ZERO(builder->planes);
    for (int i = 0; i < drmdev->n_planes; i++) {
        struct drm_plane *plane = drmdev->planes + i;

        if (plane->possible_crtcs & crtc->bitmask) {
            BMAP_SET(builder->planes, i);
            if (plane->has_zpos && plane->min_zpos < min_zpos) {
                min_zpos = plane->min_zpos;
            }
        }
    }

    drmdev_unlock(drmdev);

    builder->n_refs = REFCOUNT_INIT_1;
    builder->drmdev = drmdev_ref(drmdev);
    // right now they're the same, but they might not be in the future.
    builder->use_legacy = !supports_atomic_modesetting;
    builder->supports_atomic = supports_atomic_modesetting;
    builder->crtc = crtc;
    builder->req = req;
    builder->next_zpos = min_zpos;
    builder->n_layers = 0;
    builder->has_mode = false;
    builder->unset_mode = false;
    return builder;

    fail_free_builder:
    free(builder);

    fail_unlock:
    drmdev_unlock(drmdev);
    return NULL;
}

void kms_req_builder_destroy(struct kms_req_builder *builder) {
    /// TODO: Is this complete?
    for (int i = 0; i < builder->n_layers; i++) {
        if (builder->layers[i].release_callback != NULL) {
            builder->layers[i].release_callback(builder->layers[i].release_callback_userdata);
        }
    }
    if (builder->req != NULL) {
        drmModeAtomicFree(builder->req);
    }
    drmdev_unref(builder->drmdev);
    free(builder);
}

DEFINE_REF_OPS(kms_req_builder, n_refs)

struct drmdev *kms_req_builder_get_drmdev(struct kms_req_builder *builder) {
    return builder->drmdev;
}

bool kms_req_builder_prefer_next_layer_opaque(struct kms_req_builder *builder) {
    DEBUG_ASSERT_NOT_NULL(builder);
    return builder->n_layers == 0;
}

int kms_req_builder_set_mode(struct kms_req_builder *builder, const drmModeModeInfo *mode) {
    DEBUG_ASSERT_NOT_NULL(builder);
    DEBUG_ASSERT_NOT_NULL(mode);
    builder->has_mode = true;
    builder->mode = *mode;
    return 0;
}

int kms_req_builder_unset_mode(struct kms_req_builder *builder) {
    DEBUG_ASSERT_NOT_NULL(builder);
    DEBUG_ASSERT(!builder->has_mode);
    builder->unset_mode = true;
    return 0;
}

int kms_req_builder_set_connector(struct kms_req_builder *builder, uint32_t connector_id) {
    struct drm_connector *conn;

    DEBUG_ASSERT_NOT_NULL(builder);
    DEBUG_ASSERT(DRM_ID_IS_VALID(connector_id));

    for_each_connector_in_drmdev(builder->drmdev, conn) {
        if (conn->id == connector_id) {
            break;
        }
    }

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
    kms_fb_release_cb_t release_callback,
    kms_deferred_fb_release_cb_t deferred_release_callback,
    void *userdata
) {
    struct drm_plane *plane;
    int64_t zpos;
    bool close_in_fence_fd_after;
    int ok, index;

    DEBUG_ASSERT_NOT_NULL(builder);
    DEBUG_ASSERT_NOT_NULL(layer);
    DEBUG_ASSERT_NOT_NULL(release_callback);
    DEBUG_ASSERT_EQUALS_MSG(deferred_release_callback, NULL, "deferred release callbacks are not supported right now.");

    if (builder->use_legacy && builder->supports_atomic && builder->n_layers > 1) {
        // if we already have a first layer and we should use legacy modesetting even though the kernel driver
        // supports atomic modesetting, return EINVAL.
        // if the driver supports atomic modesetting, drmModeSetPlane will block for vblank, so we can't use it,
        // and we can't use drmModeAtomicCommit for non-blocking multi-plane commits of course.
        // For the first layer we can use drmModePageFlip though.
        LOG_DEBUG(
            "Can't do multi-plane commits when using legacy modesetting (and driver supports atomic modesetting).\n"
        );
        return EINVAL;
    }

    close_in_fence_fd_after = false;
    if (builder->use_legacy && layer->has_in_fence_fd) {
        LOG_DEBUG("Explicit fencing is not supported for legacy modesetting. Implicit fencing will be used instead.\n");
        close_in_fence_fd_after = true;
    }

    // Index of our layer.
    index = builder->n_layers;

    /// TODO: Not sure we can use crtc_x, crtc_y, etc with primary planes
    if (index == 0) {
        // if this is the first layer, try using a
        // primary plane for it.

        /// TODO: Use cursor_plane->max_zpos - 1 as the upper zpos limit, instead of INT64_MAX
        plane = allocate_plane(
            builder,
            /* allow_primary */ true,
            /* allow_overlay */ false,
            /* allow_cursor */ false,
            layer->format,
            layer->has_modifier,
            layer->modifier,
            false,
            0,
            0,
            layer->has_rotation,
            layer->rotation,
            false,
            0
        );

        if (plane == NULL && !get_pixfmt_info(layer->format)->is_opaque) {
            // maybe we can find a plane if we use the opaque version of this pixel format?
            plane = allocate_plane(
                builder,
                /* allow_primary */ true,
                /* allow_overlay */ false,
                /* allow_cursor */ false,
                pixfmt_opaque(layer->format),
                layer->has_modifier,
                layer->modifier,
                false,
                0,
                0,
                layer->has_rotation,
                layer->rotation,
                false,
                0
            );
        }
    } else {
        // First try to find an overlay plane with a higher zpos.
        plane = allocate_plane(
            builder,
            /* allow_primary */ false,
            /* allow_overlay */ true,
            /* allow_cursor */ false,
            layer->format,
            layer->has_modifier,
            layer->modifier,
            true,
            builder->next_zpos,
            INT64_MAX,
            layer->has_rotation,
            layer->rotation,
            false,
            0
        );

        // If we can't find one, find an overlay plane with the next highest plane_id.
        // (According to some comments in the kernel, that's the fallback KMS uses for the
        // occlusion order if no zpos property is supported, i.e. planes with plane id occlude
        // planes with lower id)
        if (plane == NULL) {
            plane = allocate_plane(
                builder,
                /* allow_primary */ false,
                /* allow_overlay */ true,
                /* allow_cursor */ false,
                layer->format,
                layer->has_modifier,
                layer->modifier,
                false,
                0,
                0,
                layer->has_rotation,
                layer->rotation,
                true,
                builder->layers[index - 1].plane_id + 1
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
    if (plane->has_zpos) {
        zpos = builder->next_zpos;
        if (plane->min_zpos > zpos) {
            zpos = plane->min_zpos;
        }
    }

    if (builder->use_legacy) {
        UNIMPLEMENTED();
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
                drmModeAtomicAddProperty(builder->req, plane_id, plane->ids.alpha, DRM_BLEND_ALPHA_OPAQUE);
            }

            if (plane->has_blend_mode && plane->supported_blend_modes[kNone_DrmBlendMode]) {
                drmModeAtomicAddProperty(builder->req, plane_id, plane->ids.pixel_blend_mode, kNone_DrmBlendMode);
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
    if (plane->has_zpos) {
        builder->next_zpos = zpos + 1;
    }
    builder->layers[index].layer = *layer;
    builder->layers[index].plane_id = plane->id;
    builder->layers[index].set_zpos = plane->has_zpos;
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
    DEBUG_ASSERT_NOT_NULL(builder);
    DEBUG_ASSERT_NOT_NULL(zpos_out);
    *zpos_out = builder->next_zpos++;
    return 0;
}

struct kms_req *kms_req_builder_build(struct kms_req_builder *builder) {
    return (struct kms_req *) kms_req_builder_ref(builder);
}

MAYBE_UNUSED struct kms_req *kms_req_ref(struct kms_req *req) {
    return (struct kms_req*) kms_req_builder_ref((struct kms_req_builder *) req);
}

MAYBE_UNUSED void kms_req_unref(struct kms_req *req) {
    return kms_req_builder_unref((struct kms_req_builder *) req);
}

MAYBE_UNUSED void kms_req_unrefp(struct kms_req **req) {
    return kms_req_builder_unrefp((struct kms_req_builder **) req);
}

MAYBE_UNUSED void kms_req_swap_ptrs(struct kms_req **oldp, struct kms_req *new) {
    return kms_req_builder_swap_ptrs((struct kms_req_builder**) oldp, (struct kms_req_builder*) new);
}

static int kms_req_commit_common(
    struct kms_req *req,
    bool blocking,
    kms_scanout_cb_t scanout_cb,
    void *userdata,
    void_callback_t destroy_cb
) {
    struct kms_req_builder *builder;
    struct drm_mode_blob *mode_blob;
    uint32_t flags;
    bool update_mode;
    int ok;

    update_mode = false;
    mode_blob = NULL;

    DEBUG_ASSERT_NOT_NULL(req);
    builder = (struct kms_req_builder *) req;

    drmdev_lock(builder->drmdev);

    if (builder->drmdev->master_fd < 0) {
        LOG_ERROR("Commit requested, but drmdev doesn't have a DRM master fd right now.\n");
        ok = EBUSY;
        goto fail_unlock;
    }

    if (!is_drm_master(builder->drmdev->master_fd)) {
        LOG_ERROR("Commit requested, but drmdev is paused right now.\n");
        ok = EBUSY;
        goto fail_unlock;
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
        mode_blob = drm_mode_blob_new(builder->drmdev->fd, &builder->mode);
        if (mode_blob == NULL) {
            ok = EIO;
            goto fail_unlock;
        }
    } else if (builder->unset_mode) {
        update_mode = true;
        mode_blob = NULL;
    }

    if (builder->use_legacy) {
        DEBUG_ASSERT_EQUALS(builder->layers[0].layer.dst_x, 0);
        DEBUG_ASSERT_EQUALS(builder->layers[0].layer.dst_y, 0);
        DEBUG_ASSERT_EQUALS(builder->layers[0].layer.dst_w, builder->mode.hdisplay);
        DEBUG_ASSERT_EQUALS(builder->layers[0].layer.dst_h, builder->mode.vdisplay);

        /// TODO: Do we really need to assert this?
        DEBUG_ASSERT(get_pixfmt_info(builder->layers[0].layer.format)->is_opaque);

        if (update_mode) {
            /// TODO: Fetch new connector or current connector here since we seem to need it for drmModeSetCrtc
            UNIMPLEMENTED();

            ok = drmModeSetCrtc(
                builder->drmdev->master_fd,
                builder->crtc->id,
                builder->layers[0].layer.drm_fb_id,
                0,
                0,
                NULL,
                0,
                builder->unset_mode ? NULL : &builder->mode
            );
            if (ok != 0) {
                ok = errno;
                LOG_ERROR("Could not commit display update. drmModeSetCrtc: %s\n", strerror(ok));
                goto fail_maybe_destroy_mode_blob;
            }
        } else {
            ok = drmModePageFlip(
                builder->drmdev->master_fd,
                builder->crtc->id,
                builder->layers[0].layer.drm_fb_id,
                DRM_MODE_PAGE_FLIP_EVENT,
                kms_req_builder_ref(builder)
            );
            if (ok != 0) {
                ok = errno;
                LOG_ERROR("Could not commit display update. drmModePageFlip: %s\n", strerror(ok));
                goto fail_unref_builder;
            }
        }

        // This should also be ensured by kms_req_builder_push_fb_layer
        DEBUG_ASSERT_MSG(
            !(builder->supports_atomic && builder->n_layers > 1),
            "There can be at most one framebuffer layer when the KMS device supports atomic modesetting but we are "
            "using legacy modesetting."
        );

        /// TODO: Call drmModeSetPlane for all other layers
        /// TODO: Assert here

    } else {
        /// TODO: If we can do explicit fencing, don't use the page flip event.
        /// TODO: Can we set OUT_FENCE_PTR even though we didn't set any IN_FENCE_FDs?
        flags = DRM_MODE_PAGE_FLIP_EVENT | (blocking ? 0 : DRM_MODE_ATOMIC_NONBLOCK) | (update_mode ? DRM_MODE_ATOMIC_ALLOW_MODESET : 0);

        /// TODO: If we're on raspberry pi and only have one layer, we can do an async pageflip
        /// on the primary plane to replace the next queued frame. (To do _real_ triple buffering
        /// with fully decoupled framerate, potentially)
        ok = drmModeAtomicCommit(builder->drmdev->master_fd, builder->req, flags, kms_req_builder_ref(builder));
        if (ok != 0) {
            ok = errno;
            LOG_ERROR("Could not commit display update. drmModeAtomicCommit: %s\n", strerror(ok));
            goto fail_unref_builder;
        }
    }

    if (update_mode) {
        // destroy the old mode blob
        if (builder->crtc->committed_state.mode_blob != NULL) {
            /// TODO: Should we defer this to after the pageflip?
            drm_mode_blob_destroy(builder->crtc->committed_state.mode_blob);
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

    drmdev_set_scanout_callback_locked(builder->drmdev, builder->crtc->id, scanout_cb, userdata, destroy_cb);

    if (blocking) {
        // handle the page-flip event here, rather than via the eventfd
        ok = drmdev_on_modesetting_fd_ready_locked(builder->drmdev);
        if (ok != 0) {
            LOG_ERROR("Couldn't synchronously handle pageflip event.\n");
            goto fail_unset_scanout_callback;
        }
    }

    drmdev_unlock(builder->drmdev);

    return 0;

fail_unset_scanout_callback:
    builder->drmdev->per_crtc_state[builder->crtc->index].scanout_callback = NULL;
    builder->drmdev->per_crtc_state[builder->crtc->index].destroy_callback = NULL;
    builder->drmdev->per_crtc_state[builder->crtc->index].userdata = NULL;
    goto fail_unlock;

fail_unref_builder:
    kms_req_builder_unref(builder);

fail_maybe_destroy_mode_blob:
    if (mode_blob != NULL)
        drm_mode_blob_destroy(mode_blob);

fail_unlock:
    drmdev_unlock(builder->drmdev);

    return ok;
}

void set_vblank_ns(struct drmdev *drmdev, uint64_t vblank_ns, void *userdata) {
    uint64_t *vblank_ns_out;

    DEBUG_ASSERT_NOT_NULL(userdata);
    vblank_ns_out = userdata;
    (void) drmdev;
    
    *vblank_ns_out = vblank_ns;
}

int kms_req_commit_blocking(struct kms_req *req, uint64_t *vblank_ns_out) {
    uint64_t vblank_ns;
    int ok;

    vblank_ns = int64_to_uint64(-1);
    ok = kms_req_commit_common(
        req,
        true,
        set_vblank_ns, &vblank_ns, NULL
    );
    if (ok != 0) {
        return ok;
    }

    // make sure the vblank_ns is actually set
    DEBUG_ASSERT(vblank_ns != int64_to_uint64(-1));
    if (vblank_ns_out != NULL) {
        *vblank_ns_out = vblank_ns;
    }

    return 0;
}

int kms_req_commit_nonblocking(struct kms_req *req, kms_scanout_cb_t scanout_cb, void *userdata, void_callback_t destroy_cb) {
    return kms_req_commit_common(
        req,
        false,
        scanout_cb, userdata, destroy_cb
    );
}
