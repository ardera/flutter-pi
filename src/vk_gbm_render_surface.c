// SPDX-License-Identifier: MIT
/*
 * Vulkan GBM Backing Store
 *
 * - a render surface that can be used for filling flutter vulkan backing stores
 * - and for scanout using KMS
 *
 * Copyright (c) 2022, Hannes Winkler <hanneswinkler2000@web.de>
 */

#include <stdlib.h>
#include <unistd.h>

#include <collection.h>
#include <stdatomic.h>

#include <tracer.h>
#include <surface.h>
#include <surface_private.h>
#include <render_surface.h>
#include <render_surface_private.h>

#include <vk_renderer.h>
#include <vulkan.h>

#include <vk_gbm_render_surface.h>

FILE_DESCR("gbm/vulkan render surface")

struct vk_gbm_render_surface;
struct vk_renderer;

struct fb {
    struct gbm_bo *bo;
    VkDeviceMemory memory;
    VkImage image;
    FlutterVulkanImage fl_image;
};

struct locked_fb {
    struct vk_gbm_render_surface *surface;
    atomic_flag is_locked;
    refcount_t n_refs;
    struct fb *fb;
};

struct vk_gbm_render_surface {
    union {
        struct surface surface;
        struct render_surface render_surface;
    };

    uuid_t uuid;

    /**
     * @brief The vulkan renderer we use for talking to vulkan.
     *
     */
    struct vk_renderer *renderer;

    /**
     * @brief Just some vulkan images that are compatible with GBM/DRM/KMS.
     *
     * 4 framebuffers is good enough for most use-cases.
     */
    struct fb fbs[4];

    /**
     * @brief This is just some locking wrapper around the simple fbs above.
     *
     * Any locked_fb for which is_locked is false can be locked and then freely used for anything.
     * Everything that needs that locked_fb for something should keep a reference on it.
     * Once the reference count drops to zero, is_locked will be set to false and the fb is ready to be reused again.
     *
     */
    struct locked_fb locked_fbs[4];

    /**
     * @brief The framebuffer that was last queued to be presented using @ref vk_gbm_render_surface_queue_present_vulkan.
     *
     * This framebuffer is still locked so we can present it again any time, without worrying about it being acquired by
     * flutter using @ref vk_gbm_render_surface_fill_vulkan. Even when @ref vk_gbm_render_surface_present_kms is called,
     * we don't set this to NULL.
     *
     * This is the framebuffer that will be presented on screen when @ref vk_gbm_render_surface_present_kms or
     * @ref vk_gbm_render_surface_present_fbdev is called.
     *
     */
    struct locked_fb *front_fb;

    /**
     * @brief The pixel format to use for all framebuffers.
     *
     */
    enum pixfmt pixel_format;

#ifdef DEBUG
    /**
     * @brief The number of framebuffers that are currently locked.
     *
     */
    atomic_int n_locked_fbs;
#endif
};

static void locked_fb_destroy(struct locked_fb *fb) {
    struct vk_gbm_render_surface *surface;

    surface = fb->surface;
    fb->surface = NULL;

#ifdef DEBUG
    atomic_fetch_sub(&surface->n_locked_fbs, 1);
#endif
    atomic_flag_clear(&fb->is_locked);
    surface_unref(CAST_SURFACE(surface));
}

DEFINE_STATIC_REF_OPS(locked_fb, n_refs)

MAYBE_UNUSED static bool atomic_flag_test(atomic_flag *flag) {
    bool before = atomic_flag_test_and_set(flag);
    if (before == false) {
        atomic_flag_clear(flag);
    }
    return before;
}

static void log_locked_fbs(struct vk_gbm_render_surface *surface, const char *note) {
#ifdef VK_LOG_LOCKED_FBS
    LOG_DEBUG(
        "locked: %c, %c, %c, %c",
        atomic_flag_test(&surface->locked_fbs[0].is_locked) ? 'y' : 'n',
        atomic_flag_test(&surface->locked_fbs[1].is_locked) ? 'y' : 'n',
        atomic_flag_test(&surface->locked_fbs[2].is_locked) ? 'y' : 'n',
        atomic_flag_test(&surface->locked_fbs[3].is_locked) ? 'y' : 'n'
    );

    if (note != NULL) {
        LOG_DEBUG_UNPREFIXED(" (%s)\n", note);
    } else {
        LOG_DEBUG_UNPREFIXED("\n");
    }
#else
    (void) surface;
    (void) note;
#endif
}

COMPILE_ASSERT(offsetof(struct vk_gbm_render_surface, surface) == 0);
COMPILE_ASSERT(offsetof(struct vk_gbm_render_surface, render_surface.surface) == 0);

static const uuid_t uuid = CONST_UUID(0x26, 0xfe, 0x91, 0x53, 0x75, 0xf2, 0x41, 0x90, 0xa1, 0xf5, 0xba, 0xe1, 0x1b, 0x28, 0xd5, 0xe5);

#define CAST_THIS(ptr) CAST_VK_GBM_RENDER_SURFACE(ptr)
#define CAST_THIS_UNCHECKED(ptr) CAST_VK_GBM_RENDER_SURFACE_UNCHECKED(ptr)

#ifdef DEBUG
ATTR_PURE struct vk_gbm_render_surface *__checked_cast_vk_gbm_render_surface(void *ptr) {
    struct vk_gbm_render_surface *surface;

    surface = CAST_VK_GBM_RENDER_SURFACE_UNCHECKED(ptr);
    DEBUG_ASSERT(uuid_equals(surface->uuid, uuid));
    return surface;
}
#endif

void vk_gbm_render_surface_deinit(struct surface *s);
static int vk_gbm_render_surface_present_kms(struct surface *s, const struct fl_layer_props *props, struct kms_req_builder *builder);
static int vk_gbm_render_surface_present_fbdev(struct surface *s, const struct fl_layer_props *props, struct fbdev_commit_builder *builder);
static int vk_gbm_render_surface_fill(struct render_surface *surface, FlutterBackingStore *fl_store);
static int vk_gbm_render_surface_queue_present(struct render_surface *surface, const FlutterBackingStore *fl_store);

static bool is_srgb_format(VkFormat vk_format) {
    return vk_format == VK_FORMAT_R8G8B8A8_SRGB || vk_format == VK_FORMAT_B8G8R8A8_SRGB;
}

static VkFormat srgb_to_unorm_format(VkFormat vk_format) {
    DEBUG_ASSERT(is_srgb_format(vk_format));
    if (vk_format == VK_FORMAT_R8G8B8A8_SRGB) {
        return VK_FORMAT_R8G8B8A8_UNORM;
    } else if (vk_format == VK_FORMAT_B8G8R8A8_SRGB) {
        return VK_FORMAT_B8G8R8A8_UNORM;
    } else {
        UNREACHABLE();
    }
}

static int fb_init(struct fb *fb, struct gbm_device *gbm_device, struct vk_renderer *renderer, int width, int height, enum pixfmt pixel_format, uint64_t drm_modifier) {
    PFN_vkGetMemoryFdPropertiesKHR get_memory_fd_props;
    VkSubresourceLayout layout;
    VkDeviceMemory img_device_memory;
    struct gbm_bo *bo;
    VkFormat vk_format;
    VkDevice device;
    VkResult ok;
    VkImage vkimg;
    int fd;

    DEBUG_ASSERT_MSG(get_pixfmt_info(pixel_format)->vk_format != VK_FORMAT_UNDEFINED, "Given pixel format is not compatible with any vulkan sRGB format.");

    device = vk_renderer_get_device(renderer);

    /// FIXME: Right now, using any _SRGB format (for example VK_FORMAT_B8G8R8A8_SRGB) will not work because
    /// that'll break some assertions inside flutter / skia. (VK_FORMAT_B8G8R8A8_SRGB maps to GrColorType::kRGBA_8888_SRGB,
    /// but some other part of flutter will use GrColorType::kRGBA_8888 so you'll get a mismatch at some point)
    /// We're just converting the _SRGB to a _UNORM here, but I'm not really sure that's guaranteed to work.
    /// (_UNORM can mean anything basically)

    vk_format = get_pixfmt_info(pixel_format)->vk_format;
    if (is_srgb_format(vk_format)) {
        vk_format = srgb_to_unorm_format(vk_format);
    }

    ok = vkCreateImage(
        device,
        &(VkImageCreateInfo){
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .flags = 0,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = vk_format,
            .extent = { .width = width, .height = height, .depth = 1 },
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            // Tell vulkan that the tiling we want to use is determined by a DRM format modifier
            // (in our case DRM_FORMAT_MOD_LINEAR, but could be something else as well, using a device-supported
            // modifier is probably faster if that's possible)
            .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
            // These are the usage flags flutter will use too internally
            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = 0,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .pNext =
                &(VkExternalMemoryImageCreateInfo){
                    .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
                    .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                    .pNext =
                        &(VkImageDrmFormatModifierExplicitCreateInfoEXT){
                            .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
                            .drmFormatModifierPlaneCount = 1,
                            .drmFormatModifier = drm_modifier,
                            .pPlaneLayouts =
                                (VkSubresourceLayout[1]){
                                    {
                                        /// These are just dummy values, but they need to be there AFAIK
                                        .offset = 0,
                                        .size = 0,
                                        .rowPitch = 0,
                                        .arrayPitch = 0,
                                        .depthPitch = 0,
                                    },
                                },
                        },
                },
        },
        NULL,
        &vkimg
    );
    if (ok != VK_SUCCESS) {
        LOG_VK_ERROR(ok, "Could not create Vulkan image. vkCreateImage");
        return EIO;
    }

    // We _should_ only have one plane in the linear case.
    // Query the layout of that plane to check if it matches the GBM BOs layout.
    vkGetImageSubresourceLayout(
        device,
        vkimg,
        &(VkImageSubresource){
            .aspectMask = VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT,  // For v3dv, this doesn't really matter
            .mipLevel = 0,
            .arrayLayer = 0,
        },
        &layout
    );

    // Create a GBM BO with the actual memory we're going to use
    bo = gbm_bo_create_with_modifiers(
        gbm_device,
        width,
        height,
        get_pixfmt_info(pixel_format)->gbm_format,
        &drm_modifier,
        1
    );
    if (bo == NULL) {
        LOG_ERROR("Could not create GBM BO. gbm_bo_create_with_modifiers: %s\n", strerror(errno));
        goto fail_destroy_image;
    }

    // Just some paranoid checks that the layout matches (had some issues with that initially)
    if (gbm_bo_get_offset(bo, 0) != layout.offset) {
        LOG_ERROR("GBM BO layout doesn't match image layout. This is probably a driver / kernel bug.\n");
        goto fail_destroy_bo;
    }

    if (gbm_bo_get_stride_for_plane(bo, 0) != layout.rowPitch) {
        LOG_ERROR("GBM BO layout doesn't match image layout. This is probably a driver / kernel bug.\n");
        goto fail_destroy_bo;
    }

    // gbm_bo_get_fd will dup us a new dmabuf fd.
    // So if we don't use it, we need to close it.
    fd = gbm_bo_get_fd(bo);
    if (fd < 0) {
        LOG_ERROR("Couldn't get dmabuf fd for GBM buffer. gbm_bo_get_fd: %s\n", strerror(errno));
        goto fail_destroy_bo;
    }

    // find out as which memory types we can import our dmabuf fd
    VkMemoryFdPropertiesKHR fd_memory_props = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
        .pNext = NULL,
        .memoryTypeBits = 0,
    };

    get_memory_fd_props = (PFN_vkGetMemoryFdPropertiesKHR) vkGetDeviceProcAddr(device, "vkGetMemoryFdPropertiesKHR");
    if (get_memory_fd_props == NULL) {
        LOG_ERROR("Couldn't resolve vkGetMemoryFdPropertiesKHR.\n");
        goto fail_close_fd;
    }

    ok = get_memory_fd_props(
        device,
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        fd,
        &fd_memory_props
    );
    if (ok != VK_SUCCESS) {
        LOG_VK_ERROR(ok, "Couldn't get dmabuf memory properties. vkGetMemoryFdPropertiesKHR");
        goto fail_close_fd;
    }

    // Find out the memory requirements for our image (the supported memory types for import)
    VkMemoryRequirements2 image_memory_reqs = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
        .memoryRequirements = { 0 },
        .pNext = NULL
    };

    vkGetImageMemoryRequirements2(
        device,
        &(VkImageMemoryRequirementsInfo2) {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
            .image = vkimg,
            .pNext = NULL,
        },
        &image_memory_reqs
    );

    // Find a memory type that fits both to the dmabuf and the image
    int mem = vk_renderer_find_mem_type(
        renderer,
        0,
        image_memory_reqs.memoryRequirements.memoryTypeBits & fd_memory_props.memoryTypeBits
    );
    if (mem < 0) {
        LOG_ERROR("Couldn't find a memory type that's both supported by the image and the dmabuffer.\n");
        goto fail_close_fd;
    }

    // now, create a VkDeviceMemory instance from our dmabuf.
    // after successful import, the fd is owned by the device memory object
    // and we don't need to close it.
    ok = vkAllocateMemory(
        device,
        &(VkMemoryAllocateInfo) {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = layout.size,
            .memoryTypeIndex = mem,
            .pNext = &(VkImportMemoryFdInfoKHR) {
                .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
                .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                .fd = fd,
                .pNext = &(VkMemoryDedicatedAllocateInfo) {
                    .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
                    .image = vkimg,
                    .buffer = VK_NULL_HANDLE,
                    .pNext = NULL,
                },
            },
        },
        NULL,
        &img_device_memory
    );
    if (ok != VK_SUCCESS) {
        LOG_VK_ERROR(ok, "Couldn't import dmabuf as vulkan device memory. vkAllocateMemory");
        goto fail_close_fd;
    }

    ok = vkBindImageMemory2(
        device,
        1,
        &(VkBindImageMemoryInfo){
            .sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO,
            .image = vkimg,
            .memory = img_device_memory,
            .memoryOffset = 0,
            .pNext = NULL,
        }
    );
    if (ok != VK_SUCCESS) {
        LOG_VK_ERROR(ok, "Couldn't bind dmabuf-backed vulkan device memory to vulkan image. vkBindImageMemory2");
        goto fail_free_device_memory;
    }

    fb->bo = bo;
    fb->memory = img_device_memory;
    fb->image = vkimg;

    COMPILE_ASSERT(sizeof(FlutterVulkanImage) == 24);
    fb->fl_image = (FlutterVulkanImage) {
        .struct_size = sizeof(FlutterVulkanImage),
        .image = fb->image,
        .format = vk_format,
    };

    return 0;


    fail_free_device_memory:
    vkFreeMemory(device, img_device_memory, NULL);
    goto fail_destroy_bo;

    fail_close_fd:
    close(fd);

    fail_destroy_bo:
    gbm_bo_destroy(bo);

    fail_destroy_image:
    vkDestroyImage(device, vkimg, NULL);
    return EIO;
}

static void fb_deinit(struct fb *fb, VkDevice device) {
    DEBUG_ASSERT_NOT_NULL(fb);

    vkFreeMemory(device, fb->memory, NULL);
    gbm_bo_destroy(fb->bo);
    vkDestroyImage(device, fb->image, NULL);
}

int vk_gbm_render_surface_init(
    struct vk_gbm_render_surface *surface,
    struct tracer *tracer,
    struct point size,
    struct gbm_device *gbm_device,
    struct vk_renderer *renderer,
    enum pixfmt pixel_format
) {
    int ok;

    ok = render_surface_init(CAST_RENDER_SURFACE_UNCHECKED(surface), tracer, size);
    if (ok != 0) {
        return EIO;
    }

    for (int i = 0; i < ARRAY_SIZE(surface->fbs); i++) {
        ok = fb_init(surface->fbs + i, gbm_device, renderer, (int) size.x, (int) size.y, pixel_format, DRM_FORMAT_MOD_LINEAR);
        if (ok != 0) {
            LOG_ERROR("Could not initialize vulkan GBM framebuffer.\n");
            goto fail_deinit_previous_fbs;
        }

        continue;


        fail_deinit_previous_fbs:
        for (int j = 0; j < i; j++) {
            fb_deinit(surface->fbs + j, vk_renderer_get_device(renderer));
        }
        goto fail_deinit_render_surface;
    }

    for (int i = 0; i < ARRAY_SIZE(surface->locked_fbs); i++) {
        surface->locked_fbs[i].surface = NULL;
        surface->locked_fbs[i].is_locked = (atomic_flag) ATOMIC_FLAG_INIT;
        surface->locked_fbs[i].n_refs = REFCOUNT_INIT_0;
        surface->locked_fbs[i].fb = surface->fbs + i;
    }

    COMPILE_ASSERT(ARRAY_SIZE(surface->fbs) == ARRAY_SIZE(surface->locked_fbs));

    surface->surface.present_kms = vk_gbm_render_surface_present_kms;
    surface->surface.present_fbdev = vk_gbm_render_surface_present_fbdev;
    surface->surface.deinit = vk_gbm_render_surface_deinit;
    surface->render_surface.fill = vk_gbm_render_surface_fill;
    surface->render_surface.queue_present = vk_gbm_render_surface_queue_present;

    uuid_copy(&surface->uuid, uuid);
    surface->renderer = vk_renderer_ref(renderer);
    surface->front_fb = NULL;
    surface->pixel_format = pixel_format;
#ifdef DEBUG
    surface->n_locked_fbs = ATOMIC_VAR_INIT(0);
#endif
    return 0;


    fail_deinit_render_surface:
    render_surface_deinit(CAST_SURFACE_UNCHECKED(surface));
    return EIO;
}

ATTR_MALLOC struct vk_gbm_render_surface *vk_gbm_render_surface_new(
    struct tracer *tracer,
    struct point size,
    struct gbm_device *device,
    struct vk_renderer *renderer,
    enum pixfmt pixel_format
) {
    struct vk_gbm_render_surface *surface;
    int ok;

    surface = malloc(sizeof *surface);
    if (surface == NULL) {
        goto fail_return_null;
    }

    ok = vk_gbm_render_surface_init(surface, tracer, size, device, renderer, pixel_format);
    if (ok != 0) {
        goto fail_free_surface;
    }

    return surface;


    fail_free_surface:
    free(surface);

    fail_return_null:
    return NULL;
}

void vk_gbm_render_surface_deinit(struct surface *s) {
    struct vk_gbm_render_surface *vk_surface;

    vk_surface = CAST_THIS(s);

    for (int i = 0; i < ARRAY_SIZE(vk_surface->fbs); i++) {
        fb_deinit(vk_surface->fbs + i, vk_renderer_get_device(vk_surface->renderer));
    }

    render_surface_deinit(s);
}

struct gbm_bo_meta {
    struct drmdev *drmdev;
    uint32_t fb_id;
    bool has_opaque_fb;
    enum pixfmt opaque_pixel_format;
    uint32_t opaque_fb_id;
};

static void on_destroy_gbm_bo_meta(struct gbm_bo *bo, void *meta_void) {
    struct gbm_bo_meta *meta;
    int ok;

    DEBUG_ASSERT_NOT_NULL(bo);
    DEBUG_ASSERT_NOT_NULL(meta_void);
    meta = meta_void;

    ok = drmdev_rm_fb(meta->drmdev, meta->fb_id);
    if (ok != 0) {
        LOG_ERROR("Couldn't remove DRM framebuffer.\n");
    }

    if (meta->has_opaque_fb && meta->opaque_fb_id != meta->fb_id) {
        ok = drmdev_rm_fb(meta->drmdev, meta->opaque_fb_id);
        if (ok != 0) {
            LOG_ERROR("Couldn't remove DRM framebuffer.\n");
        }
    }

    drmdev_unref(meta->drmdev);
    free(meta);
}

static void on_release_layer(void *userdata) {
    struct vk_gbm_render_surface *surface;
    struct locked_fb *fb;

    DEBUG_ASSERT_NOT_NULL(userdata);

    fb = userdata;
    surface = CAST_THIS_UNCHECKED(surface_ref(CAST_SURFACE_UNCHECKED(fb->surface)));

    locked_fb_unref(fb);

    log_locked_fbs(surface, "release_layer");
    surface_unref(CAST_SURFACE_UNCHECKED(surface));
}

static int vk_gbm_render_surface_present_kms(struct surface *s, const struct fl_layer_props *props, struct kms_req_builder *builder) {
    struct vk_gbm_render_surface *vk_surface;
    struct gbm_bo_meta *meta;
    struct drmdev *drmdev;
    struct gbm_bo *bo;
    enum pixfmt pixel_format, opaque_pixel_format;
    uint32_t fb_id, opaque_fb_id;
    int ok;

    vk_surface = CAST_THIS(s);
    (void) props;
    (void) builder;

    /// TODO: Implement non axis-aligned fl_layer_props
    DEBUG_ASSERT_MSG(props->is_aa_rect, "only axis aligned view geometry is supported right now");

    surface_lock(s);

    DEBUG_ASSERT_NOT_NULL_MSG(vk_surface->front_fb, "There's no framebuffer available for scanout right now. Make sure you called render_surface_queue_present() before presenting.");

    bo = vk_surface->front_fb->fb->bo;
    meta = gbm_bo_get_user_data(bo);
    if (meta == NULL) {
        bool has_opaque_fb;

        meta = malloc(sizeof *meta);
        if (meta == NULL) {
            ok = ENOMEM;
            goto fail_unlock;
        }

        drmdev = kms_req_builder_get_drmdev(builder);
        DEBUG_ASSERT_NOT_NULL(drmdev);

        TRACER_BEGIN(vk_surface->surface.tracer, "drmdev_add_fb (non-opaque)");
        fb_id = drmdev_add_fb(
            drmdev,
            gbm_bo_get_width(bo),
            gbm_bo_get_height(bo),
            vk_surface->pixel_format,
            gbm_bo_get_handle(bo).u32,
            gbm_bo_get_stride(bo),
            gbm_bo_get_offset(bo, 0),
            true, gbm_bo_get_modifier(bo),
            0
        );
        TRACER_END(vk_surface->surface.tracer, "drmdev_add_fb (non-opaque)");

        if (fb_id == 0) {
            ok = EIO;
            LOG_ERROR("Couldn't add GBM buffer as DRM framebuffer.\n");
            goto fail_free_meta;
        }

        if (get_pixfmt_info(vk_surface->pixel_format)->is_opaque == false) {
            has_opaque_fb = false;
            opaque_pixel_format = pixfmt_opaque(vk_surface->pixel_format);
            if (get_pixfmt_info(opaque_pixel_format)->is_opaque) {

                TRACER_BEGIN(vk_surface->surface.tracer, "drmdev_add_fb (opaque)");
                opaque_fb_id = drmdev_add_fb(
                    drmdev,
                    gbm_bo_get_width(bo),
                    gbm_bo_get_height(bo),
                    opaque_pixel_format,
                    gbm_bo_get_handle(bo).u32,
                    gbm_bo_get_stride(bo),
                    gbm_bo_get_offset(bo, 0),
                    true, gbm_bo_get_modifier(bo),
                    0
                );
                TRACER_END(vk_surface->surface.tracer, "drmdev_add_fb (opaque)");

                if (opaque_fb_id != 0) {
                    has_opaque_fb = true;
                }
            }
        } else {
            has_opaque_fb = true;
            opaque_fb_id = fb_id;
            opaque_pixel_format = vk_surface->pixel_format;
        }

        meta->drmdev = drmdev_ref(drmdev);
        meta->fb_id = fb_id;
        meta->has_opaque_fb = has_opaque_fb;
        meta->opaque_pixel_format = opaque_pixel_format;
        meta->opaque_fb_id = opaque_fb_id;
        gbm_bo_set_user_data(bo, meta, on_destroy_gbm_bo_meta);
    } else {
        // We can only add this GBM BO to a single KMS device as an fb right now.
        DEBUG_ASSERT_EQUALS_MSG(meta->drmdev, kms_req_builder_get_drmdev(builder), "Currently GBM BOs can only be scanned out on a single KMS device for their whole lifetime.");
    }

    /*
    LOG_DEBUG(
        "vk_gbm_render_surface_present_kms:\n"
        "    src_x, src_y, src_w, src_h: %f %f %f %f\n"
        "    dst_x, dst_y, dst_w, dst_h: %f %f %f %f\n",
        0.0, 0.0,
        s->render_surface.size.x,
        s->render_surface.size.y,
        props->aa_rect.offset.x,
        props->aa_rect.offset.y,
        props->aa_rect.size.x,
        props->aa_rect.size.y
    );
    */

    // The bottom-most layer should preferably be an opaque layer.
    // For example, on Pi 4, even though ARGB8888 is listed as supported for the primary plane,
    // rendering is completely off.
    // So we just cast our fb to an XRGB8888 framebuffer and scanout that instead.
    if (kms_req_builder_prefer_next_layer_opaque(builder)) {
        if (meta->has_opaque_fb) {
            fb_id = meta->opaque_fb_id;
            pixel_format = meta->opaque_pixel_format;
        } else {
            LOG_DEBUG("Bottom-most framebuffer layer should be opaque, but an opaque framebuffer couldn't be created.\n");
            LOG_DEBUG("Using non-opaque framebuffer instead, which can result in visual glitches.\n");
            fb_id = meta->fb_id;
            pixel_format = vk_surface->pixel_format;
        }
    } else {
        fb_id = meta->fb_id;
        pixel_format = vk_surface->pixel_format;
    }

    TRACER_BEGIN(vk_surface->surface.tracer, "kms_req_builder_push_fb_layer");
    ok = kms_req_builder_push_fb_layer(
        builder,
        &(const struct kms_fb_layer) {
            .drm_fb_id = fb_id,
            .format = pixel_format,
            .has_modifier = false,
            .modifier = 0,

            .dst_x = (int32_t) props->aa_rect.offset.x,
            .dst_y = (int32_t) props->aa_rect.offset.y,
            .dst_w = (uint32_t) props->aa_rect.size.x,
            .dst_h = (uint32_t) props->aa_rect.size.y,

            .src_x = 0,
            .src_y = 0,
            .src_w = DOUBLE_TO_FP1616_ROUNDED(vk_surface->render_surface.size.x),
            .src_h = DOUBLE_TO_FP1616_ROUNDED(vk_surface->render_surface.size.y),

            .has_rotation = false,
            .rotation = PLANE_TRANSFORM_ROTATE_0,

            .has_in_fence_fd = false,
            .in_fence_fd = 0
        },
        on_release_layer,
        NULL,
        locked_fb_ref(vk_surface->front_fb)
    );
    TRACER_END(vk_surface->surface.tracer, "kms_req_builder_push_fb_layer");
    if (ok != 0) {
        goto fail_unref_locked_fb;
    }


    surface_unlock(s);
    return ok;


    fail_unref_locked_fb:
    locked_fb_unref(vk_surface->front_fb);
    goto fail_unlock;

    fail_free_meta:
    free(meta);

    fail_unlock:
    surface_unlock(s);
    return ok;
}

static int vk_gbm_render_surface_present_fbdev(struct surface *s, const struct fl_layer_props *props, struct fbdev_commit_builder *builder) {
    struct vk_gbm_render_surface *render_surface;

    /// TODO: Implement by mmapping the current front bo, copy it into the fbdev
    /// TODO: Print a warning here if we're not using explicit linear tiling and transition to linear layout with vulkan instead of gbm_bo_map in that case

    render_surface = CAST_THIS(s);
    (void) render_surface;
    (void) props;
    (void) builder;

    UNIMPLEMENTED();

    return 0;
}

static int vk_gbm_render_surface_fill(struct render_surface *s, FlutterBackingStore *fl_store) {
    struct vk_gbm_render_surface *render_surface;
    int i, ok;

    render_surface = CAST_THIS(s);

    surface_lock(CAST_SURFACE_UNCHECKED(s));

    // Try to find & lock a locked_fb we can use.
    // Note we use atomics here even though we hold the surfaces' mutex because
    // releasing a locked_fb is possibly done without the mutex.
    for (i = 0; i < ARRAY_SIZE(render_surface->locked_fbs); i++) {
        if (atomic_flag_test_and_set(&render_surface->locked_fbs[i].is_locked) == false) {
            goto locked;
        }
    }

    // If we reached this point, we couldn't lock one of the 4 locked_fbs.
    // Which shouldn't happen except we have an application bug.
    DEBUG_ASSERT_MSG(false, "Couldn't find a free slot to lock the surfaces front framebuffer.");
    ok = EIO;
    goto fail_unlock;

    locked: ;
    /// TODO: Remove this once we're using triple buffering
#ifdef DEBUG
    atomic_fetch_add(&render_surface->n_locked_fbs, 1);
    log_locked_fbs(CAST_THIS_UNCHECKED(s), "fill");
    //DEBUG_ASSERT_MSG(before + 1 <= 3, "sanity check failed: too many locked fbs for double-buffered vsync");
#endif
    render_surface->locked_fbs[i].surface = CAST_VK_GBM_RENDER_SURFACE(surface_ref(CAST_SURFACE_UNCHECKED(s)));
    render_surface->locked_fbs[i].n_refs = REFCOUNT_INIT_1;

    COMPILE_ASSERT(sizeof(FlutterVulkanBackingStore) == 16);
    fl_store->type = kFlutterBackingStoreTypeVulkan;
    fl_store->vulkan = (FlutterVulkanBackingStore) {
        .struct_size = sizeof(FlutterVulkanBackingStore),
        .image = &render_surface->locked_fbs[i].fb->fl_image,
        .user_data = surface_ref(CAST_SURFACE_UNCHECKED(render_surface)),
        .destruction_callback = surface_unref_void,
    };

    surface_unlock(CAST_SURFACE_UNCHECKED(s));

    return 0;


    fail_unlock:
    surface_unlock(CAST_SURFACE_UNCHECKED(s));
    return ok;
}

static int vk_gbm_render_surface_queue_present(struct render_surface *s, const FlutterBackingStore *fl_store) {
    struct vk_gbm_render_surface *vk_surface;
    struct locked_fb *fb;

    vk_surface = CAST_THIS(s);

    DEBUG_ASSERT_EQUALS(fl_store->type, kFlutterBackingStoreTypeVulkan);
    /// TODO: Implement handling if fl_store->did_update == false

    surface_lock(CAST_SURFACE_UNCHECKED(s));

    // find out which fb this image belongs too
    fb = NULL;
    for (int i = 0; i < ARRAY_SIZE(vk_surface->locked_fbs); i++) {
        if (vk_surface->locked_fbs[i].fb->fl_image.image == fl_store->vulkan.image->image) {
            fb = vk_surface->locked_fbs + i;
            break;
        }
    }

    if (fb == NULL) {
        LOG_ERROR("The vulkan image flutter wants to present is not known to this render surface.\n");
        surface_unlock(CAST_SURFACE_UNCHECKED(s));
        return EINVAL;
    }

    // Replace the front fb with the new one
    // (will unref the old one if not NULL internally)
    locked_fb_swap_ptrs(&vk_surface->front_fb, fb);

    // Since flutter no longer uses this fb for rendering, we need to unref it
    locked_fb_unref(fb);

    log_locked_fbs(vk_surface, "queue_present");

    surface_unlock(CAST_SURFACE_UNCHECKED(s));
    return 0;
}
