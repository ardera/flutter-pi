#include "resources.h"

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

#include "pixel_format.h"
#include "util/bitset.h"
#include "util/list.h"
#include "util/lock_ops.h"
#include "util/logging.h"
#include "util/macros.h"
#include "util/refcounting.h"

struct _drmModeFB2;

struct drm_mode_fb2 {
    uint32_t fb_id;
    uint32_t width, height;
    uint32_t pixel_format; /* fourcc code from drm_fourcc.h */
    uint64_t modifier; /* applies to all buffers */
    uint32_t flags;

    /* per-plane GEM handle; may be duplicate entries for multiple planes */
    uint32_t handles[4];
    uint32_t pitches[4]; /* bytes */
    uint32_t offsets[4]; /* bytes */
};

#ifdef HAVE_FUNC_ATTRIBUTE_WEAK
    extern struct _drmModeFB2 *drmModeGetFB2(int fd, uint32_t bufferId) __attribute__((weak));
    extern void drmModeFreeFB2(struct _drmModeFB2 *ptr) __attribute__((weak));
    #define HAVE_WEAK_DRM_MODE_GET_FB2
#endif

#ifdef HAVE_WEAK_DRM_MODE_GET_FB2
static bool drm_fb_get_format(int drm_fd, uint32_t fb_id, enum pixfmt *format_out) {
    struct drm_mode_fb2 *fb;

    // drmModeGetFB2 might not be present.
    // If __attribute__((weak)) is supported by the compiler, we redefine it as
    // weak above.
    // If we don't have weak, we can't check for it here.
    if (drmModeGetFB2 && drmModeFreeFB2) {
        fb = (struct drm_mode_fb2*) drmModeGetFB2(drm_fd, fb_id);
        if (fb == NULL) {
            return false;
        }

        for (int i = 0; i < PIXFMT_COUNT; i++) {
            if (get_pixfmt_info(i)->drm_format == fb->pixel_format) {
                *format_out = i;
                drmModeFreeFB2((struct _drmModeFB2 *) fb);
                return true;
            }
        }

        drmModeFreeFB2((struct _drmModeFB2 *) fb);
        return false;
    } else {
        return false;
    }
}
#else
static bool drm_fb_get_format(int drm_fd, uint32_t fb_id, enum pixfmt *format_out) {
    (void) drm_fd;
    (void) fb_id;
    (void) format_out;
    return false;
}
#endif

static size_t sizeof_drm_format_modifier_blob(struct drm_format_modifier_blob *blob) {
    return MAX3(
        sizeof(struct drm_format_modifier_blob),
        blob->formats_offset + sizeof(uint32_t) * blob->count_formats,
        blob->modifiers_offset + sizeof(struct drm_format_modifier) * blob->count_modifiers
    );
}


static int drm_connector_init(int drm_fd, uint32_t connector_id, struct drm_connector *out) {
    memset(out, 0, sizeof(*out));

    drm_connector_prop_ids_init(&out->ids);

    {
        drmModeConnector *connector = drmModeGetConnector(drm_fd, connector_id);
        if (connector == NULL) {
            return ENOMEM;
        }

        out->id = connector->connector_id;
        out->type = connector->connector_type;
        out->type_id = connector->connector_type_id;
        out->n_encoders = connector->count_encoders;

        assert(connector->count_encoders <= 32);
        memcpy(out->encoders, connector->encoders, connector->count_encoders * sizeof(uint32_t));

        out->variable_state.connection_state = (enum drm_connection_state) connector->connection;
        out->variable_state.subpixel_layout = (enum drm_subpixel_layout) connector->subpixel;
        out->variable_state.width_mm = connector->mmWidth;
        out->variable_state.height_mm = connector->mmHeight;

        assert((connector->modes == NULL) == (connector->count_modes == 0));
        if (connector->modes != NULL) {
            out->variable_state.n_modes = connector->count_modes;
            out->variable_state.modes = memdup(connector->modes, connector->count_modes * sizeof(*connector->modes));
            if (out->variable_state.modes == NULL) {
                drmModeFreeConnector(connector);
                return ENOMEM;
            }
        }

        out->committed_state.encoder_id = connector->encoder_id;

        drmModeFreeConnector(connector);
    }

    {
        drmModeObjectProperties *props = drmModeObjectGetProperties(drm_fd, connector_id, DRM_MODE_OBJECT_CONNECTOR);
        if (props == NULL) {
            return ENOMEM;
        }

        out->committed_state.crtc_id = DRM_ID_NONE;
        for (uint32_t i = 0; i < props->count_props; i++) {
            uint32_t id = props->props[i];

            drmModePropertyRes *prop_info = drmModeGetProperty(drm_fd, id);
            if (prop_info == NULL) {
                drmModeFreeObjectProperties(props);
                return ENOMEM;
            }

            #define CHECK_ASSIGN_PROPERTY_ID(_name_str, _name)                     \
                if (strncmp(prop_info->name, _name_str, DRM_PROP_NAME_LEN) == 0) { \
                    out->ids._name = prop_info->prop_id;                                \
                } else

            DRM_CONNECTOR_PROPERTIES(CHECK_ASSIGN_PROPERTY_ID) {
                // this is the trailing else case
                LOG_DEBUG("Unknown DRM connector property: %s\n", prop_info->name);
            }

            #undef CHECK_ASSIGN_PROPERTY_ID

            if (id == out->ids.crtc_id) {
                out->committed_state.crtc_id = props->prop_values[i];
            }

            drmModeFreeProperty(prop_info);
        }

        drmModeFreeObjectProperties(props);
    }

    return 0;
}

static void drm_connector_fini(struct drm_connector *connector) {
    free(connector->variable_state.modes);
}

static int drm_connector_copy(struct drm_connector *dst, const struct drm_connector *src) {
    *dst = *src;

    if (src->variable_state.modes != NULL) {
        dst->variable_state.modes = memdup(src->variable_state.modes, src->variable_state.n_modes * sizeof(*src->variable_state.modes));
        if (dst->variable_state.modes == NULL) {
            return ENOMEM;
        }
    }

    return 0;
}


static int drm_encoder_init(int drm_fd, uint32_t encoder_id, struct drm_encoder *out) {
    drmModeEncoder *encoder = drmModeGetEncoder(drm_fd, encoder_id);
    if (encoder == NULL) {
        int ok = errno;
        if (ok == 0) ok = ENOMEM;
        return ok;
    }

    out->id = encoder->encoder_id;
    out->type = encoder->encoder_type;
    if (out->type > DRM_ENCODER_TYPE_MAX) {
        out->type = DRM_ENCODER_TYPE_NONE;
    }

    out->possible_crtcs = encoder->possible_crtcs;
    out->possible_clones = encoder->possible_clones;

    out->variable_state.crtc_id = encoder->crtc_id;

    drmModeFreeEncoder(encoder);
    return 0;
}

static int drm_encoder_copy(struct drm_encoder *dst, const struct drm_encoder *src) {
    *dst = *src;
    return 0;
}

static void drm_encoder_fini(struct drm_encoder *encoder) {
    (void) encoder;
}


static int drm_crtc_init(int drm_fd, int crtc_index, uint32_t crtc_id, struct drm_crtc *out) {
    memset(out, 0, sizeof(*out));

    drm_crtc_prop_ids_init(&out->ids);
    
    {
        drmModeCrtc *crtc = drmModeGetCrtc(drm_fd, crtc_id);
        if (crtc == NULL) {
            int ok = errno;
            if (ok == 0) ok = ENOMEM;
            return ok;
        }

        out->id = crtc->crtc_id;
        out->index = crtc_index;
        out->bitmask = 1u << crtc_index;
        out->committed_state.has_mode = crtc->mode_valid;
        out->committed_state.mode = crtc->mode;
        out->committed_state.mode_blob = NULL;

        drmModeFreeCrtc(crtc);
    }

    {
        drmModeObjectProperties *props = drmModeObjectGetProperties(drm_fd, crtc_id, DRM_MODE_OBJECT_CRTC);
        if (props == NULL) {
            int ok = errno;
            if (ok == 0) ok = ENOMEM;
            return ok;
        }

        for (int i = 0; i < props->count_props; i++) {
            drmModePropertyRes *prop_info = drmModeGetProperty(drm_fd, props->props[i]);
            if (prop_info == NULL) {
                drmModeFreeObjectProperties(props);
                int ok = errno;
                if (ok == 0) ok = ENOMEM;
                return ok;
            }

    #define CHECK_ASSIGN_PROPERTY_ID(_name_str, _name)                               \
        if (strncmp(prop_info->name, _name_str, ARRAY_SIZE(prop_info->name)) == 0) { \
            out->ids._name = prop_info->prop_id;                                          \
        } else

            DRM_CRTC_PROPERTIES(CHECK_ASSIGN_PROPERTY_ID) {
                // this is the trailing else case
                LOG_DEBUG("Unknown DRM crtc property: %s\n", prop_info->name);
            }

    #undef CHECK_ASSIGN_PROPERTY_ID


            drmModeFreeProperty(prop_info);
        }

        drmModeFreeObjectProperties(props);
    }

    return 0;
}

static int drm_crtc_copy(struct drm_crtc *dst, const struct drm_crtc *src) {
    *dst = *src;
    return 0;
}

static void drm_crtc_fini(struct drm_crtc *crtc) {
    (void) crtc;
}

bool drm_resources_any_crtc_plane_supports_format(struct drm_resources *r, uint32_t crtc_id, enum pixfmt pixel_format) {
    struct drm_crtc *crtc = drm_resources_get_crtc(r, crtc_id);
    if (crtc == NULL) {
        return false;
    }

    drm_resources_for_each_plane(r, plane) {
        if (!(plane->possible_crtcs & crtc->bitmask)) {
            // Only query planes that are possible to connect to the CRTC we're using.
            continue;
        }

        if (plane->type != DRM_PRIMARY_PLANE && plane->type != DRM_OVERLAY_PLANE) {
            // We explicitly only look for primary and overlay planes.
            continue;
        }

        if (drm_plane_supports_unmodified_format(plane, pixel_format)) {
            return true;
        }
    }

    return false;
}


static void drm_plane_init_rotation(drmModePropertyRes *info, uint64_t value, struct drm_plane *out) {
    assert(out->has_rotation == false);
    out->has_rotation = true;

    out->supported_rotations = PLANE_TRANSFORM_NONE;
    assert(info->flags & DRM_MODE_PROP_BITMASK);

    for (int k = 0; k < info->count_enums; k++) {
        out->supported_rotations.u32 |= 1 << info->enums[k].value;
    }

    assert(PLANE_TRANSFORM_IS_VALID(out->supported_rotations));

    if (info->flags & DRM_MODE_PROP_IMMUTABLE) {
        out->has_hardcoded_rotation = true;
        out->hardcoded_rotation.u64 = value;
    }

    out->committed_state.rotation.u64 = value;
}

static void drm_plane_init_zpos(drmModePropertyRes *info, uint64_t value, struct drm_plane *out) {
    assert(out->has_zpos == false);
    out->has_zpos = true;

    if (info->flags & DRM_MODE_PROP_SIGNED_RANGE) {
        out->min_zpos = uint64_to_int64(info->values[0]);
        out->max_zpos = uint64_to_int64(info->values[1]);
        out->committed_state.zpos = uint64_to_int64(value);
        assert(out->min_zpos <= out->max_zpos);
        assert(out->min_zpos <= out->committed_state.zpos);
        assert(out->committed_state.zpos <= out->max_zpos);
    } else if (info->flags & DRM_MODE_PROP_RANGE) {
        assert(info->values[0] <= INT64_MAX);
        assert(info->values[1] <= INT64_MAX);

        out->min_zpos = info->values[0];
        out->max_zpos = info->values[1];
        out->committed_state.zpos = value;
        assert(out->min_zpos <= out->max_zpos);
    } else {
        ASSERT_MSG(false, "Invalid property type for zpos property.");
    }

    if (info->flags & DRM_MODE_PROP_IMMUTABLE) {
        out->has_hardcoded_zpos = true;
        assert(value <= INT64_MAX);
        
        out->hardcoded_zpos = value;
        if (out->min_zpos != out->max_zpos) {
            LOG_DEBUG(
                "DRM plane minimum supported zpos does not equal maximum supported zpos, even though zpos is "
                "immutable.\n"
            );
            out->min_zpos = out->max_zpos = out->hardcoded_zpos;
        }
    }
}

static int drm_plane_init_in_formats(int drm_fd, drmModePropertyRes *info, uint64_t value, struct drm_plane *out) {
    drmModePropertyBlobRes *blob;
    
    (void) info;

    blob = drmModeGetPropertyBlob(drm_fd, value);
    if (blob == NULL) {
        int ok = errno;
        if (ok == 0) ok = ENOMEM;
        return ok;
    }

    out->supports_modifiers = true;
    out->supported_modified_formats_blob = memdup(blob->data, blob->length);
    if (out->supported_modified_formats_blob == NULL) {
        drmModeFreePropertyBlob(blob);
        return ENOMEM;
    }

    drmModeFreePropertyBlob(blob);
    return 0;
}

static void drm_plane_init_alpha(drmModePropertyRes *info, uint64_t value, struct drm_plane *out) {
    out->has_alpha = true;
    assert(info->flags == DRM_MODE_PROP_RANGE);
    assert(info->values[0] == 0);
    assert(info->values[1] == 0xFFFF);
    assert(value <= 0xFFFF);

    out->committed_state.alpha = (uint16_t) value;
}

static void drm_plane_init_blend_mode(drmModePropertyRes *info, uint64_t value, struct drm_plane *out) {
    out->has_blend_mode = true;
    assert(info->flags == DRM_MODE_PROP_ENUM);

    for (int i = 0; i < info->count_enums; i++) {
        if (streq(info->enums[i].name, "None")) {
            ASSERT_EQUALS(info->enums[i].value, DRM_BLEND_MODE_NONE);
            out->supported_blend_modes[DRM_BLEND_MODE_NONE] = true;
        } else if (streq(info->enums[i].name, "Pre-multiplied")) {
            ASSERT_EQUALS(info->enums[i].value, DRM_BLEND_MODE_PREMULTIPLIED);
            out->supported_blend_modes[DRM_BLEND_MODE_PREMULTIPLIED] = true;
        } else if (streq(info->enums[i].name, "Coverage")) {
            ASSERT_EQUALS(info->enums[i].value, DRM_BLEND_MODE_COVERAGE);
            out->supported_blend_modes[DRM_BLEND_MODE_COVERAGE] = true;
        } else {
            LOG_DEBUG(
                "Unknown KMS pixel blend mode: %s (value: %" PRIu64 ")\n",
                info->enums[i].name,
                (uint64_t) info->enums[i].value
            );
        }
    }

    out->committed_state.blend_mode = value;
    assert(out->committed_state.blend_mode >= 0 && out->committed_state.blend_mode <= DRM_BLEND_MODE_MAX);
    assert(out->supported_blend_modes[out->committed_state.blend_mode]);
}

static int drm_plane_init(int drm_fd, uint32_t plane_id, struct drm_plane *out) {
    bool has_type;
    int ok;

    memset(out, 0, sizeof(*out));

    drm_plane_prop_ids_init(&out->ids);

    {
        drmModePlane *plane = drmModeGetPlane(drm_fd, plane_id);
        if (plane == NULL) {
            ok = errno;
            if (ok == 0) ok = ENOMEM;
            return ok;
        }

        out->id = plane->plane_id;
        out->possible_crtcs = plane->possible_crtcs;
        out->committed_state.fb_id = plane->fb_id;
        out->committed_state.crtc_id = plane->crtc_id;

        for (int i = 0; i < plane->count_formats; i++) {
            for (int j = 0; j < PIXFMT_COUNT; j++) {
                if (get_pixfmt_info(j)->drm_format == plane->formats[i]) {
                    out->supported_formats[j] = true;
                    break;
                }
            }
        }

        drmModeFreePlane(plane);
    }

    drmModeObjectProperties *props = drmModeObjectGetProperties(drm_fd, plane_id, DRM_MODE_OBJECT_PLANE);
    if (props == NULL) {
        ok = errno;
        if (ok == 0) ok = ENOMEM;
        return ok;
    }

    has_type = false;
    for (int j = 0; j < props->count_props; j++) {
        uint32_t id = props->props[j];
        uint64_t value = props->prop_values[j];
        
        drmModePropertyRes *info = drmModeGetProperty(drm_fd, id);
        if (info == NULL) {
            ok = errno;
            if (ok == 0) ok = ENOMEM;
            goto fail_maybe_free_supported_modified_formats_blob;
        }

#define CHECK_ASSIGN_PROPERTY_ID(_name_str, _name)                     \
    if (strncmp(info->name, _name_str, ARRAY_SIZE(info->name)) == 0) { \
        out->ids._name = info->prop_id;                                     \
    } else

        DRM_PLANE_PROPERTIES(CHECK_ASSIGN_PROPERTY_ID) {
            // do nothing
        }

#undef CHECK_ASSIGN_PROPERTY_ID


        if (id == out->ids.type) {
            assert(has_type == false);
            has_type = true;

            out->type = value;
        } else if (id == out->ids.rotation) {
            drm_plane_init_rotation(info, value, out);
        } else if (id == out->ids.zpos) {
            drm_plane_init_zpos(info, value, out);
        } else if (id == out->ids.src_x) {
            out->committed_state.src_x = value;
        } else if (id == out->ids.src_y) {
            out->committed_state.src_y = value;
        } else if (id == out->ids.src_w) {
            out->committed_state.src_w = value;
        } else if (id == out->ids.src_h) {
            out->committed_state.src_h = value;
        } else if (id == out->ids.crtc_x) {
            out->committed_state.crtc_x = value;
        } else if (id == out->ids.crtc_y) {
            out->committed_state.crtc_y = value;
        } else if (id == out->ids.crtc_w) {
            out->committed_state.crtc_w = value;
        } else if (id == out->ids.crtc_h) {
            out->committed_state.crtc_h = value;
        } else if (id == out->ids.in_formats) {
            ok = drm_plane_init_in_formats(drm_fd, info, value, out);
            if (ok != 0) {
                drmModeFreeProperty(info);
                goto fail_maybe_free_supported_modified_formats_blob;
            }
        } else if (id == out->ids.alpha) {
            drm_plane_init_alpha(info, value, out);
        } else if (id == out->ids.pixel_blend_mode) {
            drm_plane_init_blend_mode(info, value, out);
        }


        drmModeFreeProperty(info);
    }

    drmModeFreeObjectProperties(props);

    assert(has_type);
    (void) has_type;

    out->committed_state.has_format = drm_fb_get_format(drm_fd, out->committed_state.fb_id, &out->committed_state.format);
    return 0;

fail_maybe_free_supported_modified_formats_blob:
    if (out->supported_modified_formats_blob != NULL)
        free(out->supported_modified_formats_blob);

    drmModeFreeObjectProperties(props);
    return ok;
}

static int drm_plane_copy(struct drm_plane *dst, const struct drm_plane *src) {
    *dst = *src;

    if (src->supported_modified_formats_blob != NULL) {
        /// TODO: Implement
        dst->supported_modified_formats_blob = memdup(src->supported_modified_formats_blob, sizeof_drm_format_modifier_blob(src->supported_modified_formats_blob));
        if (dst->supported_modified_formats_blob == NULL) {
            return ENOMEM;
        }
    }

    return 0;
}

static void drm_plane_fini(struct drm_plane *plane) {
    if (plane->supported_modified_formats_blob != NULL) {
        free(plane->supported_modified_formats_blob);
    }
}

void drm_plane_for_each_modified_format(struct drm_plane *plane, drm_plane_modified_format_callback_t callback, void *userdata) {
    struct drm_format_modifier_blob *blob;
    struct drm_format_modifier *modifiers;
    uint32_t *formats;

    ASSERT_NOT_NULL(plane);
    ASSERT_NOT_NULL(callback);
    ASSERT(plane->supports_modifiers);
    ASSERT_EQUALS(plane->supported_modified_formats_blob->version, FORMAT_BLOB_CURRENT);

    blob = plane->supported_modified_formats_blob;

    modifiers = (void *) (((char *) blob) + blob->modifiers_offset);
    formats = (void *) (((char *) blob) + blob->formats_offset);

    int index = 0;
    for (int i = 0; i < blob->count_modifiers; i++) {
        for (int j = modifiers[i].offset; (j < blob->count_formats) && (j < modifiers[i].offset + 64); j++) {
            bool is_format_bit_set = (modifiers[i].formats & (1ull << (j % 64))) != 0;
            if (!is_format_bit_set) {
                continue;
            }

            if (has_pixfmt_for_drm_format(formats[j])) {
                enum pixfmt format = get_pixfmt_for_drm_format(formats[j]);

                bool should_continue = callback(plane, index, format, modifiers[i].modifier, userdata);
                if (!should_continue) {
                    goto exit;
                }

                index++;
            }
        }
    }

exit:
    return;
}

static bool
check_modified_format_supported(UNUSED struct drm_plane *plane, UNUSED int index, enum pixfmt format, uint64_t modifier, void *userdata) {
    struct {
        enum pixfmt format;
        uint64_t modifier;
        bool found;
    } *context = userdata;

    if (format == context->format && modifier == context->modifier) {
        context->found = true;
        return false;
    } else {
        return true;
    }
}

bool drm_plane_supports_modified_formats(struct drm_plane *plane) {
    return plane->supports_modifiers;
}

bool drm_plane_supports_modified_format(struct drm_plane *plane, enum pixfmt format, uint64_t modifier) {
    if (!plane->supported_modified_formats_blob) {
        // return false if we want a modified format but the plane doesn't support modified formats
        return false;
    }

    struct {
        enum pixfmt format;
        uint64_t modifier;
        bool found;
    } context = {
        .format = format,
        .modifier = modifier,
        .found = false,
    };

    // Check if the requested format & modifier is supported.
    drm_plane_for_each_modified_format(plane, check_modified_format_supported, &context);

    return context.found;
}

bool drm_plane_supports_unmodified_format(struct drm_plane *plane, enum pixfmt format) {
    // we don't want a modified format, return false if the format is not in the list
    // of supported (unmodified) formats
    return plane->supported_formats[format];
}


struct drm_resources *drm_resources_new(int drm_fd) {
    struct drm_resources *r;
    int ok;

    r = calloc(1, sizeof *r);
    if (r == NULL) {
        return NULL;
    }

    r->n_refs = REFCOUNT_INIT_1;
    
    r->have_filter = false;

    drmModeRes *res = drmModeGetResources(drm_fd);
    if (res == NULL) {
        ok = errno;
        if (ok == 0) ok = EINVAL;
        LOG_ERROR("Could not get DRM device resources. drmModeGetResources: %s\n", strerror(ok));
        return NULL;
    }

    r->min_width = res->min_width;
    r->max_width = res->max_width;
    r->min_height = res->min_height;
    r->max_height = res->max_height;

    r->connectors = calloc(res->count_connectors, sizeof *(r->connectors));
    if (r->connectors == NULL) {
        ok = ENOMEM;
        goto fail_free_res;
    }

    r->n_connectors = 0;
    for (int i = 0; i < res->count_connectors; i++) {
        ok = drm_connector_init(drm_fd, res->connectors[i], r->connectors + i);
        if (ok != 0) {
            goto fail_free_connectors;
        }

        r->n_connectors++;
    }

    r->encoders = calloc(res->count_encoders, sizeof *(r->encoders));
    if (r->encoders == NULL) {
        ok = ENOMEM;
        goto fail_free_connectors;
    }

    r->n_encoders = 0;
    for (int i = 0; i < res->count_encoders; i++) {
        ok = drm_encoder_init(drm_fd, res->encoders[i], r->encoders + i);
        if (ok != 0) {
            goto fail_free_encoders;
        }

        r->n_encoders++;
    }

    r->crtcs = calloc(res->count_crtcs, sizeof *(r->crtcs));
    if (r->crtcs == NULL) {
        ok = ENOMEM;
        goto fail_free_encoders;
    }

    r->n_crtcs = 0;
    for (int i = 0; i < res->count_crtcs; i++) {
        ok = drm_crtc_init(drm_fd, i, res->crtcs[i], r->crtcs + i);
        if (ok != 0) {
            goto fail_free_crtcs;
        }

        r->n_crtcs++;
    }

    drmModeFreeResources(res);
    res = NULL;

    drmModePlaneRes *plane_res = drmModeGetPlaneResources(drm_fd);
    if (plane_res == NULL) {
        ok = errno;
        if (ok == 0) ok = EINVAL;
        LOG_ERROR("Could not get DRM device planes resources. drmModeGetPlaneResources: %s\n", strerror(ok));
        goto fail_free_crtcs;
    }

    r->planes = calloc(plane_res->count_planes, sizeof *(r->planes));
    if (r->planes == NULL) {
        ok = ENOMEM;
        goto fail_free_plane_res;
    }

    r->n_planes = 0;
    for (int i = 0; i < plane_res->count_planes; i++) {
        ok = drm_plane_init(drm_fd, plane_res->planes[i], r->planes + i);
        if (ok != 0) {
            goto fail_free_planes;
        }

        r->n_planes++;
    }

    drmModeFreePlaneResources(plane_res);
    return r;

fail_free_planes:
    for (int i = 0; i < r->n_planes; i++)
        drm_plane_fini(r->planes + i);
    free(r->planes);

fail_free_plane_res:
    if (plane_res != NULL) {
        drmModeFreePlaneResources(plane_res);
    }

fail_free_crtcs:
    for (int i = 0; i < r->n_crtcs; i++)
        drm_crtc_fini(r->crtcs + i);
    free(r->crtcs);

fail_free_encoders:
    for (int i = 0; i < r->n_encoders; i++)
        drm_encoder_fini(r->encoders + i);
    free(r->encoders);

fail_free_connectors:
    for (int i = 0; i < r->n_connectors; i++)
        drm_connector_fini(r->connectors + i);
    free(r->connectors);

fail_free_res:
    if (res != NULL) {
        drmModeFreeResources(res);
    }

    return NULL;
}

struct drm_resources *drm_resources_new_filtered(int drm_fd, uint32_t connector_id, uint32_t encoder_id, uint32_t crtc_id, size_t n_planes, const uint32_t *plane_ids) {
    struct drm_resources *r;
    int ok;

    r = calloc(1, sizeof *r);
    if (r == NULL) {
        return NULL;
    }

    r->n_refs = REFCOUNT_INIT_1;
    r->have_filter = true;
    r->filter.connector_id = connector_id;
    r->filter.encoder_id = encoder_id;
    r->filter.crtc_id = crtc_id;
    r->filter.n_planes = n_planes;
    memcpy(r->filter.plane_ids, plane_ids, n_planes * sizeof *plane_ids);

    {
        drmModeRes *res = drmModeGetResources(drm_fd);
        if (res == NULL) {
            ok = errno;
            if (ok == 0) ok = EINVAL;
            LOG_ERROR("Could not get DRM device resources. drmModeGetResources: %s\n", strerror(ok));
            goto fail_free_r;
        }

        r->min_width = res->min_width;
        r->max_width = res->max_width;
        r->min_height = res->min_height;
        r->max_height = res->max_height;

        drmModeFreeResources(res);
    }

    r->connectors = calloc(1, sizeof *(r->connectors));
    if (r->connectors == NULL) {
        ok = ENOMEM;
        goto fail_free_r;
    }

    ok = drm_connector_init(drm_fd, r->filter.connector_id, r->connectors + 0);
    if (ok == 0) {
        r->n_connectors = 1;
    } else {
        r->n_connectors = 0;
    }

    if (r->n_connectors == 0) {
        free(r->connectors);
        r->connectors = NULL;
    }

    r->encoders = calloc(1, sizeof *(r->encoders));
    if (r->encoders == NULL) {
        ok = ENOMEM;
        goto fail_free_connectors;
    }

    ok = drm_encoder_init(drm_fd, r->filter.encoder_id, r->encoders + 0);
    if (ok == 0) {
        r->n_encoders = 1;
    } else {
        r->n_encoders = 0;
    }

    if (r->n_encoders == 0) {
        free(r->encoders);
        r->encoders = NULL;
    }

    r->crtcs = calloc(1, sizeof *(r->crtcs));
    if (r->crtcs == NULL) {
        ok = ENOMEM;
        goto fail_free_encoders;
    }

    /// TODO: Implement
    UNIMPLEMENTED();

    ok = drm_crtc_init(drm_fd, (TRAP(), 0), crtc_id, r->crtcs);
    if (ok == 0) {
        r->n_crtcs = 1;
    } else {
        r->n_crtcs = 0;
    }

    if (r->n_crtcs == 0) {
        free(r->crtcs);
        r->crtcs = NULL;
    }

    r->planes = calloc(r->filter.n_planes, sizeof *(r->planes));
    if (r->planes == NULL) {
        ok = ENOMEM;
        goto fail_free_crtcs;
    }

    r->n_planes = 0;
    for (int i = 0; i < r->filter.n_planes; i++) {
        assert(r->n_planes <= r->filter.n_planes);

        uint32_t plane_id = r->filter.plane_ids[i];
        struct drm_plane *plane = r->planes + r->n_planes;

        ok = drm_plane_init(drm_fd, plane_id, plane);
        if (ok != 0) {
            continue;
        }

        r->n_planes++;
    }

    if (r->n_planes == 0) {
        free(r->planes);
        r->planes = NULL;
    }

    return 0;


fail_free_crtcs:
    for (int i = 0; i < r->n_crtcs; i++) {
        drm_crtc_fini(r->crtcs + i);
    }

    if (r->crtcs != NULL) {
        free(r->crtcs);
    }

fail_free_encoders:
    for (int i = 0; i < r->n_encoders; i++) {
        drm_encoder_fini(r->encoders + i);
    }
    
    if (r->encoders != NULL) {
        free(r->encoders);
    }
    
fail_free_connectors:
    for (int i = 0; i < r->n_connectors; i++) {
        drm_connector_fini(r->connectors + i);
    }

    if (r->connectors != NULL) {
        free(r->connectors);
    }

fail_free_r:
    free(r);
    return NULL;
}

struct drm_resources *drm_resources_dup_filtered(struct drm_resources *res, uint32_t connector_id, uint32_t encoder_id, uint32_t crtc_id, size_t n_planes, const uint32_t *plane_ids) {
    struct drm_resources *r;
    int ok;

    ASSERT_NOT_NULL(res);

    r = calloc(1, sizeof *r);
    if (r == NULL) {
        return NULL;
    }

    r->n_refs = REFCOUNT_INIT_1;

    r->have_filter = true;
    r->filter.connector_id = connector_id;
    r->filter.encoder_id = encoder_id;
    r->filter.crtc_id = crtc_id;
    r->filter.n_planes = n_planes;
    memcpy(r->filter.plane_ids, plane_ids, n_planes * sizeof *plane_ids);

    r->min_width = res->min_width;
    r->max_width = res->max_width;
    r->min_height = res->min_height;
    r->max_height = res->max_height;

    {
        r->connectors = calloc(1, sizeof(struct drm_connector));
        if (r->connectors == NULL) {
            ok = ENOMEM;
            goto fail_free_r;
        }

        struct drm_connector *conn = drm_resources_get_connector(res, connector_id);
        if (conn != NULL) {
            drm_connector_copy(r->connectors, conn);
            r->n_connectors = 1;
        } else {
            r->n_connectors = 0;
        }

        if (r->n_connectors == 0) {
            free(r->connectors);
            r->connectors = NULL;
        }
    }

    {
        r->encoders = calloc(1, sizeof(struct drm_encoder));
        if (r->encoders == NULL) {
            ok = ENOMEM;
            goto fail_free_connectors;
        }

        struct drm_encoder *enc = drm_resources_get_encoder(res, encoder_id);
        if (enc != NULL) {
            drm_encoder_copy(r->encoders, enc);
            r->n_encoders = 1;
        } else {
            r->n_encoders = 0;
        }

        if (r->n_encoders == 0) {
            free(r->encoders);
            r->encoders = NULL;
        }
    }

    {
        r->crtcs = calloc(1, sizeof(struct drm_crtc));
        if (r->crtcs == NULL) {
            ok = ENOMEM;
            goto fail_free_encoders;
        }

        struct drm_crtc *crtc = drm_resources_get_crtc(res, crtc_id);
        if (crtc != NULL) {
            drm_crtc_copy(r->crtcs, crtc);
            r->n_crtcs = 1;
        } else {
            r->n_crtcs = 0;
        }

        if (r->n_crtcs == 0) {
            free(r->crtcs);
            r->crtcs = NULL;
        }
    }

    r->planes = calloc(r->filter.n_planes, sizeof *(r->planes));
    if (r->planes == NULL) {
        ok = ENOMEM;
        goto fail_free_crtcs;
    }

    r->n_planes = 0;
    for (int i = 0; i < r->filter.n_planes; i++) {
        assert(r->n_planes <= r->filter.n_planes);

        uint32_t plane_id = r->filter.plane_ids[i];
        struct drm_plane *dst_plane = r->planes + r->n_planes;

        struct drm_plane *src_plane = drm_resources_get_plane(res, plane_id);
        if (src_plane == NULL) {
            continue;
        }

        ok = drm_plane_copy(dst_plane, src_plane);
        if (ok != 0) {
            for (int j = 0; j < r->n_planes; j++) {
                drm_plane_fini(r->planes + j);
            }
            free(r->planes);
            goto fail_free_crtcs;
        }

        r->n_planes++;
    }

    if (r->n_planes == 0) {
        free(r->planes);
        r->planes = NULL;
    }

    return 0;


fail_free_crtcs:
    for (int i = 0; i < r->n_crtcs; i++) {
        drm_crtc_fini(r->crtcs + i);
    }

    if (r->crtcs != NULL) {
        free(r->crtcs);
    }

fail_free_encoders:
    for (int i = 0; i < r->n_encoders; i++) {
        drm_encoder_fini(r->encoders + i);
    }
    
    if (r->encoders != NULL) {
        free(r->encoders);
    }
    
fail_free_connectors:
    for (int i = 0; i < r->n_connectors; i++) {
        drm_connector_fini(r->connectors + i);
    }

    if (r->connectors != NULL) {
        free(r->connectors);
    }

fail_free_r:
    free(r);
    return NULL;
}

void drm_resources_destroy(struct drm_resources *r) {
    for (int i = 0; i < r->n_planes; i++) {
        drm_plane_fini(r->planes + i);
    }
    if (r->planes != NULL) {
        free(r->planes);
    }

    for (int i = 0; i < r->n_crtcs; i++) {
        drm_crtc_fini(r->crtcs + i);
    }
    if (r->crtcs != NULL) {
        free(r->crtcs);
    }

    for (int i = 0; i < r->n_encoders; i++) {
        drm_encoder_fini(r->encoders + i);
    }
    if (r->encoders != NULL) {
        free(r->encoders);
    }

    for (int i = 0; i < r->n_connectors; i++) {
        drm_connector_fini(r->connectors + i);
    }
    
    if (r->connectors != NULL) {
        free(r->connectors);
    }

    free(r);
}

DEFINE_REF_OPS(drm_resources, n_refs)


void drm_resources_apply_rockchip_workaround(struct drm_resources *r) {
    // Rockchip driver always wants the N-th primary/cursor plane to be associated with the N-th CRTC.
    // If we don't respect this, commits will work but not actually show anything on screen.
    int primary_plane_index = 0;
    int cursor_plane_index = 0;
    drm_resources_for_each_plane(r, plane) {
        if (plane->type == DRM_PLANE_TYPE_PRIMARY) {
            if ((plane->possible_crtcs & (1 << primary_plane_index)) != 0) {
                plane->possible_crtcs = (1 << primary_plane_index);
            } else {
                LOG_DEBUG("Primary plane %d does not support CRTC %d.\n", primary_plane_index, primary_plane_index);
            }

            primary_plane_index++;
        } else if (plane->type == DRM_PLANE_TYPE_CURSOR) {
            if ((plane->possible_crtcs & (1 << cursor_plane_index)) != 0) {
                plane->possible_crtcs = (1 << cursor_plane_index);
            } else {
                LOG_DEBUG("Cursor plane %d does not support CRTC %d.\n", cursor_plane_index, cursor_plane_index);
            }

            cursor_plane_index++;
        }
    }
}


bool drm_resources_has_connector(struct drm_resources *r, uint32_t connector_id) {
    ASSERT_NOT_NULL(r);

    return drm_resources_get_connector(r, connector_id) != NULL;
}

struct drm_connector *drm_resources_get_connector(struct drm_resources *r, uint32_t connector_id) {
    ASSERT_NOT_NULL(r);

    for (int i = 0; i < r->n_connectors; i++) {
        if (r->connectors[i].id == connector_id) {
            return r->connectors + i;
        }
    }

    return NULL;
}

bool drm_resources_has_encoder(struct drm_resources *r, uint32_t encoder_id) {
    ASSERT_NOT_NULL(r);

    return drm_resources_get_encoder(r, encoder_id) != NULL;
}

struct drm_encoder *drm_resources_get_encoder(struct drm_resources *r, uint32_t encoder_id) {
    ASSERT_NOT_NULL(r);

    for (int i = 0; i < r->n_encoders; i++) {
        if (r->encoders[i].id == encoder_id) {
            return r->encoders + i;
        }
    }

    return NULL;
}

bool drm_resources_has_crtc(struct drm_resources *r, uint32_t crtc_id) {
    ASSERT_NOT_NULL(r);
    
    return drm_resources_get_crtc(r, crtc_id) != NULL;
}

struct drm_crtc *drm_resources_get_crtc(struct drm_resources *r, uint32_t crtc_id) {
    ASSERT_NOT_NULL(r);

    for (int i = 0; i < r->n_crtcs; i++) {
        if (r->crtcs[i].id == crtc_id) {
            return r->crtcs + i;
        }
    }

    return NULL;
}

int64_t drm_resources_get_min_zpos_for_crtc(struct drm_resources *r, uint32_t crtc_id) {
    struct drm_crtc *crtc;
    int64_t min_zpos;

    crtc = drm_resources_get_crtc(r, crtc_id);
    if (crtc == NULL) {
        return INT64_MIN;
    }

    min_zpos = INT64_MAX;
    for (int i = 0; i < r->n_planes; i++) {
        struct drm_plane *plane = r->planes + i;

        if (plane->possible_crtcs & crtc->bitmask) {
            if (plane->has_zpos && plane->min_zpos < min_zpos) {
                min_zpos = plane->min_zpos;
            }
        }
    }

    return min_zpos;
}

uint32_t drm_resources_get_possible_planes_for_crtc(struct drm_resources *r, uint32_t crtc_id) {
    struct drm_crtc *crtc;
    uint32_t possible_planes;

    crtc = drm_resources_get_crtc(r, crtc_id);
    if (crtc == NULL) {
        return 0;
    }

    possible_planes = 0;
    for (int i = 0; i < r->n_planes; i++) {
        struct drm_plane *plane = r->planes + i;

        if (plane->possible_crtcs & crtc->bitmask) {
            possible_planes |= 1u << i;
        }
    }

    return possible_planes;
}

bool drm_resources_has_plane(struct drm_resources *r, uint32_t plane_id) {
    ASSERT_NOT_NULL(r);

    return drm_resources_get_plane(r, plane_id) != NULL;
}

struct drm_plane *drm_resources_get_plane(struct drm_resources *r, uint32_t plane_id) {
    ASSERT_NOT_NULL(r);

    for (int i = 0; i < r->n_planes; i++) {
        if (r->planes[i].id == plane_id) {
            return r->planes + i;
        }
    }

    return NULL;
}

unsigned int drm_resources_get_plane_index(struct drm_resources *r, uint32_t plane_id) {
    ASSERT_NOT_NULL(r);

    for (unsigned int i = 0; i < r->n_planes; i++) {
        if (r->planes[i].id == plane_id) {
            return i;
        }
    }

    return UINT_MAX;
}


struct drm_connector *drm_resources_connector_first(struct drm_resources *r) {
    ASSERT_NOT_NULL(r);

    return r->n_connectors > 0 ? r->connectors : NULL;
}

struct drm_connector *drm_resources_connector_end(struct drm_resources *r) {
    ASSERT_NOT_NULL(r);

    return r->n_connectors > 0 ? r->connectors + r->n_connectors : NULL;
}

struct drm_connector *drm_resources_connector_next(struct drm_resources *r, struct drm_connector *current) {
    ASSERT_NOT_NULL(r);
    ASSERT_NOT_NULL(current);

    return current + 1;
}


drmModeModeInfo *drm_connector_mode_first(struct drm_connector *c) {
    ASSERT_NOT_NULL(c);

    return c->variable_state.n_modes > 0 ? c->variable_state.modes : NULL;
}

drmModeModeInfo *drm_connector_mode_end(struct drm_connector *c) {
    ASSERT_NOT_NULL(c);

    return c->variable_state.n_modes > 0 ? c->variable_state.modes + c->variable_state.n_modes : NULL;
}

drmModeModeInfo *drm_connector_mode_next(struct drm_connector *c, drmModeModeInfo *current) {
    ASSERT_NOT_NULL(c);
    ASSERT_NOT_NULL(current);

    return current + 1;
}


struct drm_encoder *drm_resources_encoder_first(struct drm_resources *r) {
    ASSERT_NOT_NULL(r);

    return r->n_encoders > 0 ? r->encoders : NULL;
}

struct drm_encoder *drm_resources_encoder_end(struct drm_resources *r) {
    ASSERT_NOT_NULL(r);

    return r->n_encoders > 0 ? r->encoders + r->n_encoders : NULL;
}

struct drm_encoder *drm_resources_encoder_next(struct drm_resources *r, struct drm_encoder *current) {
    ASSERT_NOT_NULL(r);
    ASSERT_NOT_NULL(current);

    return current + 1;
}


struct drm_crtc *drm_resources_crtc_first(struct drm_resources *r) {
    ASSERT_NOT_NULL(r);

    return r->n_crtcs > 0 ? r->crtcs : NULL;
}

struct drm_crtc *drm_resources_crtc_end(struct drm_resources *r) {
    ASSERT_NOT_NULL(r);

    return r->n_crtcs > 0 ? r->crtcs + r->n_crtcs : NULL;
}

struct drm_crtc *drm_resources_crtc_next(struct drm_resources *r, struct drm_crtc *current) {
    ASSERT_NOT_NULL(r);
    ASSERT_NOT_NULL(current);

    return current + 1;
}


struct drm_plane *drm_resources_plane_first(struct drm_resources *r) {
    ASSERT_NOT_NULL(r);

    return r->n_planes > 0 ? r->planes : NULL;
}

struct drm_plane *drm_resources_plane_end(struct drm_resources *r) {
    ASSERT_NOT_NULL(r);

    return r->n_planes > 0 ? r->planes + r->n_planes : NULL;
}

struct drm_plane *drm_resources_plane_next(struct drm_resources *r, struct drm_plane *current) {
    ASSERT_NOT_NULL(r);
    ASSERT_NOT_NULL(current);

    return current + 1;
}


struct drm_blob {
    int drm_fd;
    bool close_fd;

    uint32_t blob_id;
    drmModeModeInfo mode;
};

struct drm_blob *drm_blob_new_mode(int drm_fd, const drmModeModeInfo *mode, bool dup_fd) {
    struct drm_blob *blob;
    uint32_t blob_id;
    int ok;

    blob = malloc(sizeof *blob);
    if (blob == NULL) {
        return NULL;
    }

    if (dup_fd) {
        blob->drm_fd = dup(drm_fd);
        if (blob->drm_fd < 0) {
            LOG_ERROR("Couldn't duplicate DRM fd. dup: %s\n", strerror(errno));
            goto fail_free_blob;
        }

        blob->close_fd = true;
    } else {
        blob->drm_fd = drm_fd;
        blob->close_fd = false;
    }

    ok = drmModeCreatePropertyBlob(drm_fd, mode, sizeof *mode, &blob_id);
    if (ok != 0) {
        ok = errno;
        LOG_ERROR("Couldn't upload mode to kernel. drmModeCreatePropertyBlob: %s\n", strerror(ok));
        goto fail_maybe_close_fd;
    }

    blob->blob_id = blob_id;
    blob->mode = *mode;
    return blob;


fail_maybe_close_fd:
    if (blob->close_fd) {
        close(blob->drm_fd);
    }

fail_free_blob:
    free(blob);
    return NULL;
}

void drm_blob_destroy(struct drm_blob *blob) {
    int ok;

    ASSERT_NOT_NULL(blob);

    ok = drmModeDestroyPropertyBlob(blob->drm_fd, blob->blob_id);
    if (ok != 0) {
        ok = errno;
        LOG_ERROR("Couldn't destroy mode property blob. drmModeDestroyPropertyBlob: %s\n", strerror(ok));
    }

    // we dup()-ed it in drm_blob_new_mode.
    if (blob->close_fd) {
        close(blob->drm_fd);
    }
    
    free(blob);
}

int drm_blob_get_id(struct drm_blob *blob) {
    ASSERT_NOT_NULL(blob);
    return blob->blob_id;
}


