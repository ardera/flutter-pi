#include "drmdev.h"

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <libudev.h>

#include "pixel_format.h"
#include "resources.h"
#include "util/bitset.h"
#include "util/list.h"
#include "util/lock_ops.h"
#include "util/logging.h"
#include "util/macros.h"
#include "util/refcounting.h"
#include "util/khash.h"

struct pageflip_callbacks {
    uint32_t crtc_id;

    int index;
    struct {
        drmdev_scanout_cb_t scanout_callback;
        void *scanout_callback_userdata;

        void_callback_t void_callback;
        void *void_callback_userdata;
    } callbacks[2];
};

KHASH_MAP_INIT_INT(pageflip_callbacks, struct pageflip_callbacks)

struct drm_fb {
    struct list_head entry;

    uint32_t id;

    uint32_t width, height;

    enum pixfmt format;

    bool has_modifier;
    uint64_t modifier;

    uint32_t flags;

    uint32_t handles[4];
    uint32_t pitches[4];
    uint32_t offsets[4];
};

struct drmdev {
    int fd;
    void *fd_metadata;

    refcount_t n_refs;
    pthread_mutex_t mutex;

    bool supports_atomic_modesetting;
    bool supports_dumb_buffers;

    struct {
        drmdev_scanout_cb_t scanout_callback;
        void *userdata;
        void_callback_t destroy_callback;

        struct kms_req *last_flipped;
    } per_crtc_state[32];

    struct gbm_device *gbm_device;

    struct drmdev_file_interface interface;
    void *interface_userdata;

    struct list_head fbs;
    khash_t(pageflip_callbacks) *pageflip_callbacks;

    struct udev *udev;
    struct udev_device *kms_udev;
    const char *sysnum;
};

/**
 * @brief Check if the given file descriptor is a DRM master.
 */
static bool is_drm_master(int fd) {
    return drmAuthMagic(fd, 0) != -EACCES;
}

/**
 * @brief Check if the given path is a path to a KMS device.
 */
static bool is_kms_device(const char *path, const struct drmdev_file_interface *interface, void *userdata) {
    void *fd_metadata;

    int fd = interface->open(path, O_RDWR, &fd_metadata, userdata);
    if (fd < 0) {
        return false;
    }

    if (!drmIsKMS(fd)) {
        interface->close(fd, fd_metadata, userdata);
        return false;
    }

    interface->close(fd, fd_metadata, userdata);
    return true;
}

static void assert_rotations_work() {
    assert(PLANE_TRANSFORM_ROTATE_0.rotate_0 == true);
    assert(PLANE_TRANSFORM_ROTATE_0.rotate_90 == false);
    assert(PLANE_TRANSFORM_ROTATE_0.rotate_180 == false);
    assert(PLANE_TRANSFORM_ROTATE_0.rotate_270 == false);
    assert(PLANE_TRANSFORM_ROTATE_0.reflect_x == false);
    assert(PLANE_TRANSFORM_ROTATE_0.reflect_y == false);

    assert(PLANE_TRANSFORM_ROTATE_90.rotate_0 == false);
    assert(PLANE_TRANSFORM_ROTATE_90.rotate_90 == true);
    assert(PLANE_TRANSFORM_ROTATE_90.rotate_180 == false);
    assert(PLANE_TRANSFORM_ROTATE_90.rotate_270 == false);
    assert(PLANE_TRANSFORM_ROTATE_90.reflect_x == false);
    assert(PLANE_TRANSFORM_ROTATE_90.reflect_y == false);

    assert(PLANE_TRANSFORM_ROTATE_180.rotate_0 == false);
    assert(PLANE_TRANSFORM_ROTATE_180.rotate_90 == false);
    assert(PLANE_TRANSFORM_ROTATE_180.rotate_180 == true);
    assert(PLANE_TRANSFORM_ROTATE_180.rotate_270 == false);
    assert(PLANE_TRANSFORM_ROTATE_180.reflect_x == false);
    assert(PLANE_TRANSFORM_ROTATE_180.reflect_y == false);

    assert(PLANE_TRANSFORM_ROTATE_270.rotate_0 == false);
    assert(PLANE_TRANSFORM_ROTATE_270.rotate_90 == false);
    assert(PLANE_TRANSFORM_ROTATE_270.rotate_180 == false);
    assert(PLANE_TRANSFORM_ROTATE_270.rotate_270 == true);
    assert(PLANE_TRANSFORM_ROTATE_270.reflect_x == false);
    assert(PLANE_TRANSFORM_ROTATE_270.reflect_y == false);

    assert(PLANE_TRANSFORM_REFLECT_X.rotate_0 == false);
    assert(PLANE_TRANSFORM_REFLECT_X.rotate_90 == false);
    assert(PLANE_TRANSFORM_REFLECT_X.rotate_180 == false);
    assert(PLANE_TRANSFORM_REFLECT_X.rotate_270 == false);
    assert(PLANE_TRANSFORM_REFLECT_X.reflect_x == true);
    assert(PLANE_TRANSFORM_REFLECT_X.reflect_y == false);

    assert(PLANE_TRANSFORM_REFLECT_Y.rotate_0 == false);
    assert(PLANE_TRANSFORM_REFLECT_Y.rotate_90 == false);
    assert(PLANE_TRANSFORM_REFLECT_Y.rotate_180 == false);
    assert(PLANE_TRANSFORM_REFLECT_Y.rotate_270 == false);
    assert(PLANE_TRANSFORM_REFLECT_Y.reflect_x == false);
    assert(PLANE_TRANSFORM_REFLECT_Y.reflect_y == true);

    drm_plane_transform_t r = PLANE_TRANSFORM_NONE;

    r.rotate_0 = true;
    r.reflect_x = true;
    assert(r.u32 == (DRM_MODE_ROTATE_0 | DRM_MODE_REFLECT_X));

    r.u32 = DRM_MODE_ROTATE_90 | DRM_MODE_REFLECT_Y;
    assert(r.rotate_0 == false);
    assert(r.rotate_90 == true);
    assert(r.rotate_180 == false);
    assert(r.rotate_270 == false);
    assert(r.reflect_x == false);
    assert(r.reflect_y == true);
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

#ifdef USE_LEGACY_KMS
    *supports_atomic_modesetting = false;
#else
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
#endif

    return 0;
}


static struct udev_device *find_udev_kms_device(struct udev *udev, const char *seat, const struct drmdev_file_interface *interface, void *interface_userdata) {
    struct udev_enumerate *enumerator;
    
    enumerator = udev_enumerate_new(udev);
    udev_enumerate_add_match_subsystem(enumerator, "drm");
    udev_enumerate_add_match_sysname(enumerator, "card[0-9]*");

    udev_enumerate_scan_devices(enumerator);

    struct udev_list_entry *entry;
    udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(enumerator)) {
        const char *syspath = udev_list_entry_get_name(entry);

        struct udev_device *udev_device = udev_device_new_from_syspath(udev, syspath);
        if (udev_device == NULL) {
            LOG_ERROR("Could not create udev device from syspath. udev_device_new_from_syspath: %s\n", strerror(errno));
            continue;
        }
        
        // Find out if the drm card is connected to our seat.
        // This could also be part of the enumerator filter, e.g.:
        //
        //     udev_enumerate_add_match_property(enumerator, "ID_SEAT", seat),
        //
        // if we didn't have to handle a NULL value for ID_SEAT.
        const char *device_seat = udev_device_get_property_value(udev_device, "ID_SEAT");
        if (device_seat == NULL) {
            device_seat = "seat0";
        }
        if (!streq(device_seat, seat)) {
            udev_device_unref(udev_device);
            continue;
        }

        // devnode is the path to the /dev/dri/cardX device.
        const char *devnode = udev_device_get_devnode(udev_device);
        if (devnode == NULL) {
            // likely a connector, not a card.
            udev_device_unref(udev_device);
            continue;
        }

        if (access(devnode, R_OK | W_OK) != 0) {
            LOG_ERROR("Insufficient permissions to open KMS device \"%s\" for display output. access: %s\n", devnode, strerror(errno));
            udev_device_unref(udev_device);
            continue;
        }

        if (!is_kms_device(devnode, interface, interface_userdata)) {
            udev_device_unref(udev_device);
            continue;
        }

        udev_enumerate_unref(enumerator);
        return udev_device;
    }

    udev_enumerate_unref(enumerator);
    return NULL;
}

static void drmdev_on_page_flip(
    struct drmdev *drmdev,
    uint32_t crtc_id,
    uint64_t vblank_ns
) {
    ASSERT_NOT_NULL(drmdev);
    struct pageflip_callbacks cbs_copy;

    {
        ASSERTED int ok;
        ok = pthread_mutex_lock(&drmdev->mutex);
        ASSERT_ZERO(ok);

        khint_t cbs_bucket = kh_get(pageflip_callbacks, drmdev->pageflip_callbacks, crtc_id);
        if (cbs_bucket == kh_end(drmdev->pageflip_callbacks)) {
            // No callbacks for this CRTC.
            ok = pthread_mutex_unlock(&drmdev->mutex);
            ASSERT_ZERO(ok);
            return;
        }

        struct pageflip_callbacks *cbs = &kh_value(drmdev->pageflip_callbacks, cbs_bucket);
        memcpy(&cbs_copy, cbs, sizeof *cbs);

        cbs->callbacks[cbs->index].scanout_callback = NULL;
        cbs->callbacks[cbs->index].void_callback = NULL;
        cbs->callbacks[cbs->index].scanout_callback_userdata = NULL;
        cbs->callbacks[cbs->index].void_callback_userdata = NULL;
        cbs->index = cbs->index ^ 1;

        ok = pthread_mutex_unlock(&drmdev->mutex);
        ASSERT_ZERO(ok);
    }

    if (cbs_copy.callbacks[cbs_copy.index].void_callback != NULL) {
        cbs_copy.callbacks[cbs_copy.index].void_callback(cbs_copy.callbacks[cbs_copy.index].void_callback_userdata);
    }

    if (cbs_copy.callbacks[cbs_copy.index].scanout_callback != NULL) {
        cbs_copy.callbacks[cbs_copy.index].scanout_callback(vblank_ns, cbs_copy.callbacks[cbs_copy.index].scanout_callback_userdata);
    }
}

static void on_page_flip(
    int fd,
    unsigned int sequence,
    unsigned int tv_sec, unsigned int tv_usec,
    unsigned int crtc_id,
    void *userdata
) {
    struct drmdev *drmdev;

    ASSERT_NOT_NULL(userdata);
    drmdev = userdata;

    (void) fd;
    (void) sequence;

    uint64_t vblank_ns = tv_sec * 1000000000ull + tv_usec * 1000ull;
    drmdev_on_page_flip(drmdev, crtc_id, vblank_ns);

    drmdev_unref(drmdev);
}

/**
 * @brief Should be called when the drmdev modesetting fd is ready.
 */
void drmdev_dispatch_modesetting(struct drmdev *drmdev) {
    int ok;

    static drmEventContext ctx = {
        .version = DRM_EVENT_CONTEXT_VERSION,
        .vblank_handler = NULL,
        .page_flip_handler = NULL,
        .page_flip_handler2 = on_page_flip,
        .sequence_handler = NULL,
    };

    ok = drmHandleEvent(drmdev->fd, &ctx);
    if (ok != 0) {
        LOG_ERROR("Could not handle DRM event. drmHandleEvent: %s\n", strerror(errno));
    }
}

/**
 * @brief Create a new drmdev from the primary drm device for the given udev & seat. 
 */
struct drmdev *drmdev_new_from_udev_primary(struct udev *udev, const char *seat, const struct drmdev_file_interface *interface, void *interface_userdata) {
    struct drmdev *d;
    uint64_t cap;
    int ok;

    assert_rotations_work();

    d = malloc(sizeof *d);
    if (d == NULL) {
        return NULL;
    }

    d->n_refs = REFCOUNT_INIT_1;
    pthread_mutex_init(&d->mutex, get_default_mutex_attrs());
    d->interface = *interface;
    d->interface_userdata = interface_userdata;

    // find a KMS device for the given seat.
    d->kms_udev = find_udev_kms_device(udev, seat, interface, interface_userdata);
    if (d->kms_udev == NULL) {
        LOG_ERROR("Could not find a KMS device for seat %s.\n", seat);
        goto fail_free_dev;
    }

    d->sysnum = udev_device_get_sysnum(d->kms_udev);

    d->fd = interface->open(udev_device_get_devnode(d->kms_udev), O_RDWR, &d->fd_metadata, interface_userdata);
    if (d->fd < 0) {
        LOG_ERROR("Could not open KMS device. interface->open: %s\n", strerror(errno));
        goto fail_unref_kms_udev;
    }

    set_drm_client_caps(d->fd, &d->supports_atomic_modesetting);

    cap = 0;
    ok = drmGetCap(d->fd, DRM_CAP_DUMB_BUFFER, &cap);
    if (ok < 0) {
        d->supports_dumb_buffers = false;
    } else {
        d->supports_dumb_buffers = !!cap;
    }

    d->gbm_device = gbm_create_device(d->fd);
    if (d->gbm_device == NULL) {
        LOG_ERROR("Could not create GBM device.\n");
        goto fail_close_fd;
    }

    list_inithead(&d->fbs);
    d->pageflip_callbacks = kh_init(pageflip_callbacks);

    return d;


fail_close_fd:
    interface->close(d->fd, d->fd_metadata, interface_userdata);

fail_unref_kms_udev:
    udev_device_unref(d->kms_udev);

fail_free_dev:
    free(d);
    return NULL;
}

static void drmdev_destroy(struct drmdev *drmdev) {
    assert(refcount_is_zero(&drmdev->n_refs));

    gbm_device_destroy(drmdev->gbm_device);
    drmdev->interface.close(drmdev->fd, drmdev->fd_metadata, drmdev->interface_userdata);
    free(drmdev);
}

DEFINE_REF_OPS(drmdev, n_refs)

struct drm_resources *drmdev_query_resources(struct drmdev *drmdev) {
    ASSERT_NOT_NULL(drmdev);
    return drm_resources_new(drmdev->fd);
}

int drmdev_get_modesetting_fd(struct drmdev *drmdev) {
    ASSERT_NOT_NULL(drmdev);
    return drmdev->fd;
}

bool drmdev_supports_dumb_buffers(struct drmdev *drmdev) {
    return drmdev->supports_dumb_buffers;
}

int drmdev_create_dumb_buffer(
    struct drmdev *drmdev,
    int width,
    int height,
    int bpp,
    uint32_t *gem_handle_out,
    uint32_t *pitch_out,
    size_t *size_out
) {
    struct drm_mode_create_dumb create_req;
    int ok;

    ASSERT_NOT_NULL(drmdev);
    ASSERT_NOT_NULL(gem_handle_out);
    ASSERT_NOT_NULL(pitch_out);
    ASSERT_NOT_NULL(size_out);

    memset(&create_req, 0, sizeof create_req);
    create_req.width = width;
    create_req.height = height;
    create_req.bpp = bpp;
    create_req.flags = 0;

    ok = ioctl(drmdev->fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_req);
    if (ok < 0) {
        ok = errno;
        LOG_ERROR("Could not create dumb buffer. ioctl: %s\n", strerror(errno));
        goto fail_return_ok;
    }

    *gem_handle_out = create_req.handle;
    *pitch_out = create_req.pitch;
    *size_out = create_req.size;
    return 0;

fail_return_ok:
    return ok;
}

void drmdev_destroy_dumb_buffer(struct drmdev *drmdev, uint32_t gem_handle) {
    struct drm_mode_destroy_dumb destroy_req;
    int ok;

    ASSERT_NOT_NULL(drmdev);

    memset(&destroy_req, 0, sizeof destroy_req);
    destroy_req.handle = gem_handle;

    ok = ioctl(drmdev->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_req);
    if (ok < 0) {
        LOG_ERROR("Could not destroy dumb buffer. ioctl: %s\n", strerror(errno));
    }
}

void *drmdev_map_dumb_buffer(struct drmdev *drmdev, uint32_t gem_handle, size_t size) {
    struct drm_mode_map_dumb map_req;
    void *map;
    int ok;

    ASSERT_NOT_NULL(drmdev);

    memset(&map_req, 0, sizeof map_req);
    map_req.handle = gem_handle;

    ok = ioctl(drmdev->fd, DRM_IOCTL_MODE_MAP_DUMB, &map_req);
    if (ok < 0) {
        LOG_ERROR("Could not prepare dumb buffer mmap. ioctl: %s\n", strerror(errno));
        return NULL;
    }

    map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, drmdev->fd, map_req.offset);
    if (map == MAP_FAILED) {
        LOG_ERROR("Could not mmap dumb buffer. mmap: %s\n", strerror(errno));
        return NULL;
    }

    return map;
}

void drmdev_unmap_dumb_buffer(struct drmdev *drmdev, void *map, size_t size) {
    int ok;

    ASSERT_NOT_NULL(drmdev);
    ASSERT_NOT_NULL(map);
    (void) drmdev;

    ok = munmap(map, size);
    if (ok < 0) {
        LOG_ERROR("Couldn't unmap dumb buffer. munmap: %s\n", strerror(errno));
    }
}

struct gbm_device *drmdev_get_gbm_device(struct drmdev *drmdev) {
    ASSERT_NOT_NULL(drmdev);
    return drmdev->gbm_device;
}

int drmdev_get_last_vblank(struct drmdev *drmdev, uint32_t crtc_id, uint64_t *last_vblank_ns_out) {
    int ok;

    ASSERT_NOT_NULL(drmdev);
    ASSERT_NOT_NULL(last_vblank_ns_out);

    ok = drmCrtcGetSequence(drmdev->fd, crtc_id, NULL, last_vblank_ns_out);
    if (ok < 0) {
        ok = errno;
        LOG_ERROR("Could not get next vblank timestamp. drmCrtcGetSequence: %s\n", strerror(ok));
        return ok;
    }

    return 0;
}

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
) {
    struct drm_fb *fb;
    uint32_t fb_id;
    int ok;

    /// TODO: Code in https://elixir.bootlin.com/linux/latest/source/drivers/gpu/drm/drm_framebuffer.c#L257
    ///  assumes handles, pitches, offsets and modifiers for unused planes are zero. Make sure that's the
    ///  case here.
    ASSERT_NOT_NULL(drmdev);
    assert(width > 0 && height > 0);
    assert(bo_handles[0] != 0);
    assert(pitches[0] != 0);

    fb = malloc(sizeof *fb);
    if (fb == NULL) {
        return 0;
    }

    list_inithead(&fb->entry);
    fb->id = 0;
    fb->width = width;
    fb->height = height;
    fb->format = pixel_format;
    fb->has_modifier = has_modifiers;
    fb->modifier = modifiers[0];
    fb->flags = 0;
    memcpy(fb->handles, bo_handles, sizeof(fb->handles));
    memcpy(fb->pitches, pitches, sizeof(fb->pitches));
    memcpy(fb->offsets, offsets, sizeof(fb->offsets));

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
            goto fail_free_fb;
        }
    } else {
        ok = drmModeAddFB2(drmdev->fd, width, height, get_pixfmt_info(pixel_format)->drm_format, bo_handles, pitches, offsets, &fb_id, 0);
        if (ok < 0) {
            LOG_ERROR("Couldn't add buffer as DRM fb. drmModeAddFB2: %s\n", strerror(-ok));
            goto fail_free_fb;
        }
    }

    fb->id = fb_id;
    
    pthread_mutex_lock(&drmdev->mutex);

    list_add(&fb->entry, &drmdev->fbs);

    pthread_mutex_unlock(&drmdev->mutex);

    assert(fb_id != 0);
    return fb_id;

fail_free_fb:
    free(fb);
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
    return drmdev_add_fb_multiplanar(
        drmdev,
        width,
        height,
        pixel_format,
        (uint32_t[4]){ bo_handle, 0 },
        (uint32_t[4]){ pitch, 0 },
        (uint32_t[4]){ offset, 0 },
        has_modifier,
        (const uint64_t[4]){ modifier, 0 }
    );
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
    const int prime_fds[4],
    const uint32_t pitches[4],
    const uint32_t offsets[4],
    bool has_modifiers,
    const uint64_t modifiers[4]
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

    return drmdev_add_fb_multiplanar(drmdev, width, height, pixel_format, bo_handles, pitches, offsets, has_modifiers, modifiers);
}

uint32_t drmdev_add_fb_from_gbm_bo(struct drmdev *drmdev, struct gbm_bo *bo, bool cast_opaque) {
    enum pixfmt format;
    uint32_t fourcc;
    int n_planes;

    n_planes = gbm_bo_get_plane_count(bo);
    ASSERT(0 <= n_planes && n_planes <= 4);

    fourcc = gbm_bo_get_format(bo);

    if (!has_pixfmt_for_gbm_format(fourcc)) {
        LOG_ERROR("GBM pixel format is not supported.\n");
        return 0;
    }

    format = get_pixfmt_for_gbm_format(fourcc);

    if (cast_opaque) {
        format = pixfmt_opaque(format);
    }

    uint32_t handles[4];
    uint32_t pitches[4];

    // Returns DRM_FORMAT_MOD_INVALID on failure, or DRM_FORMAT_MOD_LINEAR
    // for dumb buffers.
    uint64_t modifier = gbm_bo_get_modifier(bo);
    bool has_modifiers = modifier != DRM_FORMAT_MOD_INVALID;

    for (int i = 0; i < n_planes; i++) {
        // gbm_bo_get_handle_for_plane will return -1 (in gbm_bo_handle.s32) and
        // set errno on failure.
        errno = 0;
        union gbm_bo_handle handle = gbm_bo_get_handle_for_plane(bo, i);
        if (handle.s32 == -1) {
            LOG_ERROR("Could not get GEM handle for plane %d: %s\n", i, strerror(errno));
            return 0;
        }

        handles[i] = handle.u32;

        // gbm_bo_get_stride_for_plane will return 0 and set errno on failure.
        errno = 0;
        uint32_t pitch = gbm_bo_get_stride_for_plane(bo, i);
        if (pitch == 0 && errno != 0) {
            LOG_ERROR("Could not get framebuffer stride for plane %d: %s\n", i, strerror(errno));
            return 0;
        }

        pitches[i] = pitch;
    }

    for (int i = n_planes; i < 4; i++) {
        handles[i] = 0;
        pitches[i] = 0;
    }

    return drmdev_add_fb_multiplanar(
        drmdev,
        gbm_bo_get_width(bo),
        gbm_bo_get_height(bo),
        format,
        handles,
        pitches,
        (uint32_t[4]){
            n_planes >= 1 ? gbm_bo_get_offset(bo, 0) : 0,
            n_planes >= 2 ? gbm_bo_get_offset(bo, 1) : 0,
            n_planes >= 3 ? gbm_bo_get_offset(bo, 2) : 0,
            n_planes >= 4 ? gbm_bo_get_offset(bo, 3) : 0,
        },
        has_modifiers,
        (uint64_t[4]){
            n_planes >= 1 ? modifier : 0,
            n_planes >= 2 ? modifier : 0,
            n_planes >= 3 ? modifier : 0,
            n_planes >= 4 ? modifier : 0,
        }
    );
}

int drmdev_rm_fb(struct drmdev *drmdev, uint32_t fb_id) {
    int ok;

    pthread_mutex_lock(&drmdev->mutex);

    list_for_each_entry(struct drm_fb, fb, &drmdev->fbs, entry) {
        if (fb->id == fb_id) {
            list_del(&fb->entry);
            free(fb);
            break;
        }
    }

    pthread_mutex_unlock(&drmdev->mutex);

    ok = drmModeRmFB(drmdev->fd, fb_id);
    if (ok < 0) {
        ok = -ok;
        LOG_ERROR("Could not remove DRM framebuffer. drmModeRmFB: %s\n", strerror(ok));
        return ok;
    }

    return 0;
}

int drmdev_move_cursor(struct drmdev *drmdev, uint32_t crtc_id, struct vec2i pos) {
    int ok = drmModeMoveCursor(drmdev->fd, crtc_id, pos.x, pos.y);
    if (ok < 0) {
        LOG_ERROR("Couldn't move mouse cursor. drmModeMoveCursor: %s\n", strerror(-ok));
        return -ok;
    }

    return 0;
}

bool drmdev_can_commit(struct drmdev *drmdev) {
    return is_drm_master(drmdev->fd);
}

static int commit_atomic_common(
    struct drmdev *drmdev,
    drmModeAtomicReq *req,
    bool sync,
    bool allow_modeset,
    uint32_t crtc_id,
    drmdev_scanout_cb_t on_scanout,
    void *scanout_cb_userdata,
    void_callback_t on_release,
    void *release_cb_userdata
) {
    int bucket_status, ok;

    // If we don't get an event, we need to call the page flip callbacks manually.
    uint64_t flags = 0;
    if (allow_modeset) {
        flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;
    }
    if (!sync) {
        flags |= DRM_MODE_PAGE_FLIP_EVENT;
        flags |= DRM_MODE_ATOMIC_NONBLOCK;
    }

    bool pageflip_event = !sync;

    if (on_scanout != NULL || on_release != NULL) {
        ok = pthread_mutex_lock(&drmdev->mutex);
        ASSERT_ZERO(ok);

        khint_t cbs_it = kh_put(pageflip_callbacks, drmdev->pageflip_callbacks, crtc_id, &bucket_status);
        if (bucket_status == -1) {
            ok = pthread_mutex_unlock(&drmdev->mutex);
            ASSERT_ZERO(ok);
            return ENOMEM;
        }

        ok = drmModeAtomicCommit(drmdev->fd, req, flags, pageflip_event ? drmdev_ref(drmdev) : NULL);
        if (ok != 0) {
            ok = -errno;

            ASSERTED int mutex_ok = pthread_mutex_unlock(&drmdev->mutex);
            ASSERT_ZERO(mutex_ok);

            LOG_ERROR("Could not commit atomic request. drmModeAtomicCommit: %s\n", strerror(-ok));
            return ok;
        }

        struct pageflip_callbacks *cbs = &kh_value(drmdev->pageflip_callbacks, cbs_it);

        // If the entry didn't exist, we clear the memory.
        if (bucket_status != 0) {
            memset(cbs, 0, sizeof *cbs);
        }

        cbs->callbacks[cbs->index].scanout_callback = on_scanout;
        cbs->callbacks[cbs->index].scanout_callback_userdata = scanout_cb_userdata;
        cbs->callbacks[cbs->index ^ 1].void_callback = on_release;
        cbs->callbacks[cbs->index ^ 1].void_callback_userdata = release_cb_userdata;

        ok = pthread_mutex_unlock(&drmdev->mutex);
        ASSERT_ZERO(ok);
    } else {
        ok = drmModeAtomicCommit(drmdev->fd, req, flags, pageflip_event ? drmdev_ref(drmdev) : NULL);
        if (ok != 0) {
            ok = -errno;
            LOG_ERROR("Could not commit atomic request. drmModeAtomicCommit: %s\n", strerror(-ok));
            return ok;
        }
    }
    
    /// TODO: Use a more accurate timestamp, e.g. call drmCrtcGetSequence,
    ///  or queue a pageflip event even for synchronous (blocking) commits
    ///  and handle here.
    if (!pageflip_event) {
        drmdev_on_page_flip(drmdev, crtc_id, get_monotonic_time());
    }

    return 0;
}

void set_vblank_timestamp(uint64_t vblank_ns, void *userdata) {
    uint64_t *vblank_ns_out = userdata;
    *vblank_ns_out = vblank_ns;
}

int drmdev_commit_atomic_sync(
    struct drmdev *drmdev,
    drmModeAtomicReq *req,
    bool allow_modeset,
    uint32_t crtc_id,
    void_callback_t on_release,
    void *userdata,
    uint64_t *vblank_ns_out
) {
    return commit_atomic_common(drmdev, req, true, allow_modeset, crtc_id, vblank_ns_out ? set_vblank_timestamp : NULL, vblank_ns_out, on_release, userdata);
}

int drmdev_commit_atomic_async(
    struct drmdev *drmdev,
    drmModeAtomicReq *req,
    bool allow_modeset,
    uint32_t crtc_id,
    drmdev_scanout_cb_t on_scanout,
    void_callback_t on_release,
    void *userdata
) {
    return commit_atomic_common(drmdev, req, false, allow_modeset, crtc_id, on_scanout, userdata, on_release, userdata);
}
