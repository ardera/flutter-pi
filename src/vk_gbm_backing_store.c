// SPDX-License-Identifier: MIT
/*
 * Vulkan GBM Backing Store
 *
 * - a surface that can be used for filling flutter vulkan backing stores
 * - and for scanout using KMS
 *
 * Copyright (c) 2022, Hannes Winkler <hanneswinkler2000@web.de>
 */

#include <stdlib.h>

#include <collection.h>
#include <stdatomic.h>

#include <tracer.h>
#include <surface.h>
#include <surface_private.h>
#include <backing_store.h>
#include <backing_store_private.h>

#include <vk_renderer.h>
#include <vulkan.h>

#include <vk_gbm_backing_store.h>

FILE_DESCR("vulkan gbm backing store")

struct vk_gbm_backing_store;
struct vk_renderer;

struct fb {
    struct gbm_bo *bo;
    VkDeviceMemory memory;
    VkImage image;
    FlutterVulkanImage fl_image;
};

struct locked_fb {
    struct vk_gbm_backing_store *store;
    atomic_flag is_locked;
    refcount_t n_refs;
    struct fb *fb;
};

struct vk_gbm_backing_store {
    union {
        struct surface surface;
        struct backing_store backing_store;
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
     * @brief The framebuffer that was last queued to be presented using @ref vk_gbm_backing_store_queue_present_vulkan.
     * 
     * This framebuffer is still locked so we can present it again any time, without worrying about it being acquired by
     * flutter using @ref vk_gbm_backing_store_fill_vulkan. Even when @ref vk_gbm_backing_store_present_kms is called,
     * we don't set this to NULL.
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
    struct vk_gbm_backing_store *store;

    store = fb->store;
    fb->store = NULL;
#ifdef DEBUG
    atomic_fetch_sub(&store->n_locked_fbs, 1);
#endif
    atomic_flag_clear(&fb->is_locked);
    surface_unref(CAST_SURFACE(store));
}

DEFINE_STATIC_REF_OPS(locked_fb, n_refs)

COMPILE_ASSERT(offsetof(struct vk_gbm_backing_store, surface) == 0);
COMPILE_ASSERT(offsetof(struct vk_gbm_backing_store, backing_store.surface) == 0);

static const uuid_t uuid = CONST_UUID(0x26, 0xfe, 0x91, 0x53, 0x75, 0xf2, 0x41, 0x90, 0xa1, 0xf5, 0xba, 0xe1, 0x1b, 0x28, 0xd5, 0xe5);

#define CAST_THIS(ptr) CAST_VK_GBM_BACKING_STORE(ptr)
#define CAST_THIS_UNCHECKED(ptr) CAST_VK_GBM_BACKING_STORE_UNCHECKED(ptr)

#ifdef DEBUG
ATTR_PURE struct vk_gbm_backing_store *__checked_cast_vk_gbm_backing_store(void *ptr) {
    struct vk_gbm_backing_store *store;
    
    store = CAST_VK_GBM_BACKING_STORE_UNCHECKED(ptr);
    DEBUG_ASSERT(uuid_equals(store->uuid, uuid));
    return store;
}
#endif

void vk_gbm_backing_store_deinit(struct surface *s);
static int vk_gbm_backing_store_present_kms(struct surface *s, const struct fl_layer_props *props, struct kms_req_builder *builder);
static int vk_gbm_backing_store_present_fbdev(struct surface *s, const struct fl_layer_props *props, struct fbdev_commit_builder *builder);
static int vk_gbm_backing_store_fill(struct backing_store *store, FlutterBackingStore *fl_store);
static int vk_gbm_backing_store_queue_present(struct backing_store *store, const FlutterBackingStore *fl_store);

static int fb_init(struct fb *fb, struct gbm_device *gbm_device, struct vk_renderer *renderer, int width, int height, enum pixfmt pixel_format, uint64_t drm_modifier) {
    PFN_vkGetMemoryFdPropertiesKHR get_memory_fd_props;
    VkSubresourceLayout layout;
    VkDeviceMemory img_device_memory;
    struct gbm_bo *bo;
    VkDevice device;
    VkResult ok;
    VkImage vkimg;
    int fd;

    DEBUG_ASSERT_MSG(get_pixfmt_info(pixel_format)->vk_format != VK_FORMAT_UNDEFINED, "Given pixel format is not compatible with any vulkan sRGB format.");

    device = vk_renderer_get_device(renderer);

    ok = vkCreateImage(
        device,
        &(VkImageCreateInfo){
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .flags = 0,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = get_pixfmt_info(pixel_format)->vk_format,
            .extent = { .width = width, .height = height, .depth = 1 },
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
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

    if (gbm_bo_get_offset(bo, 0) != layout.offset) {
        LOG_ERROR("GBM BO layout doesn't match image layout. This is probably a driver / kernel bug.\n");
        goto fail_destroy_bo;
    }

    if (gbm_bo_get_stride_for_plane(bo, 0) != layout.rowPitch) {
        LOG_ERROR("GBM BO layout doesn't match image layout. This is probably a driver / kernel bug.\n");
        goto fail_destroy_bo;
    }

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
        goto fail_destroy_bo;
    }

    ok = get_memory_fd_props(
        device,
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        fd,
        &fd_memory_props
    );
    if (ok != VK_SUCCESS) {
        LOG_VK_ERROR(ok, "Couldn't get dmabuf memory properties. vkGetMemoryFdPropertiesKHR");
        goto fail_destroy_bo;
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
        goto fail_destroy_bo;
    }

    // now, create a VkDeviceMemory instance from our dmabuf.
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
        goto fail_destroy_bo;
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
        .format = get_pixfmt_info(pixel_format)->vk_format,
    };

    return 0;
    
    
    fail_free_device_memory:
    vkFreeMemory(device, img_device_memory, NULL);

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

int vk_gbm_backing_store_init(
    struct vk_gbm_backing_store *store,
    struct tracer *tracer,
    struct point size,
    struct gbm_device *gbm_device,
    struct vk_renderer *renderer,
    enum pixfmt pixel_format
) {
    int ok;

    ok = backing_store_init(CAST_BACKING_STORE_UNCHECKED(store), tracer, size);
    if (ok != 0) {
        return EIO;
    }

    for (int i = 0; i < ARRAY_SIZE(store->fbs); i++) {
        ok = fb_init(store->fbs + i, gbm_device, renderer, (int) size.x, (int) size.y, pixel_format, DRM_FORMAT_MOD_LINEAR);
        if (ok != 0) {
            LOG_ERROR("Could not initialize vulkan GBM framebuffer.\n");
            goto fail_deinit_previous_fbs;
        }

        continue;


        fail_deinit_previous_fbs:
        for (int j = 0; j < i; j++) {
            fb_deinit(store->fbs + j, vk_renderer_get_device(renderer));
        }
        goto fail_deinit_backing_store;
    }

    for (int i = 0; i < ARRAY_SIZE(store->locked_fbs); i++) {
        store->locked_fbs[i].store = NULL;
        store->locked_fbs[i].is_locked = (atomic_flag) ATOMIC_FLAG_INIT;
        store->locked_fbs[i].n_refs = REFCOUNT_INIT_0;
        store->locked_fbs[i].fb = store->fbs + i;
    }

    COMPILE_ASSERT(ARRAY_SIZE(store->fbs) == ARRAY_SIZE(store->locked_fbs));

    store->surface.present_kms = vk_gbm_backing_store_present_kms;
    store->surface.present_fbdev = vk_gbm_backing_store_present_fbdev;
    store->surface.deinit = vk_gbm_backing_store_deinit;
    store->backing_store.fill = vk_gbm_backing_store_fill;
    store->backing_store.queue_present = vk_gbm_backing_store_queue_present;
    
    uuid_copy(&store->uuid, uuid);
    store->renderer = vk_renderer_ref(renderer);
    store->front_fb = NULL;
    store->pixel_format = pixel_format;
#ifdef DEBUG
    store->n_locked_fbs = ATOMIC_VAR_INIT(0);
#endif
    return 0;


    fail_deinit_backing_store:
    backing_store_deinit(CAST_SURFACE_UNCHECKED(store));
    return EIO;
}

ATTR_MALLOC struct vk_gbm_backing_store *vk_gbm_backing_store_new(
    struct tracer *tracer,
    struct point size,
    struct gbm_device *device,
    struct vk_renderer *renderer,
    enum pixfmt pixel_format
) {
    struct vk_gbm_backing_store *store;
    int ok;
    
    store = malloc(sizeof *store);
    if (store == NULL) {
        goto fail_return_null;
    }

    ok = vk_gbm_backing_store_init(store, tracer, size, device, renderer, pixel_format);
    if (ok != 0) {
        goto fail_free_store;
    }

    return store;


    fail_free_store:
    free(store);

    fail_return_null:
    return NULL;
}

void vk_gbm_backing_store_deinit(struct surface *s) {
    struct vk_gbm_backing_store *store;

    store = CAST_THIS(s);
    (void) store;

    for (int i = 0; i < ARRAY_SIZE(store->fbs); i++) {
        fb_deinit(store->fbs + i, vk_renderer_get_device(store->renderer));
    }

    backing_store_deinit(s);
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
    struct locked_fb *fb;

    DEBUG_ASSERT_NOT_NULL(userdata);

    fb = userdata;
    locked_fb_unref(fb);
}

static int vk_gbm_backing_store_present_kms(struct surface *s, const struct fl_layer_props *props, struct kms_req_builder *builder) {
    struct vk_gbm_backing_store *store;
    struct gbm_bo_meta *meta;
    struct drmdev *drmdev;
    struct gbm_bo *bo;
    enum pixfmt pixel_format, opaque_pixel_format;
    uint32_t fb_id, opaque_fb_id;
    int ok;

    store = CAST_THIS(s);
    (void) store;
    (void) props;
    (void) builder;

    /// TODO: Implement non axis-aligned fl_layer_props
    DEBUG_ASSERT_MSG(props->is_aa_rect, "only axis aligned view geometry is supported right now");

    surface_lock(s);

    DEBUG_ASSERT_NOT_NULL_MSG(store->front_fb, "There's no framebuffer available for scanout right now. Make sure you called backing_store_swap_buffers() before presenting.");

    bo = store->front_fb->fb->bo;
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

        TRACER_BEGIN(store->surface.tracer, "drmdev_add_fb (non-opaque)");
        fb_id = drmdev_add_fb(
            drmdev,
            gbm_bo_get_width(bo),
            gbm_bo_get_height(bo),
            store->pixel_format,
            gbm_bo_get_handle(bo).u32,
            gbm_bo_get_stride(bo),
            gbm_bo_get_offset(bo, 0),
            true, gbm_bo_get_modifier(bo),
            0
        );
        TRACER_END(store->surface.tracer, "drmdev_add_fb (non-opaque)");

        if (fb_id == 0) {
            ok = EIO;
            LOG_ERROR("Couldn't add GBM buffer as DRM framebuffer.\n");
            goto fail_free_meta;
        }

        if (get_pixfmt_info(store->pixel_format)->is_opaque == false) {
            has_opaque_fb = false;
            opaque_pixel_format = pixfmt_opaque(store->pixel_format);
            if (get_pixfmt_info(opaque_pixel_format)->is_opaque) {
                
                TRACER_BEGIN(store->surface.tracer, "drmdev_add_fb (opaque)");
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
                TRACER_END(store->surface.tracer, "drmdev_add_fb (opaque)");

                if (opaque_fb_id != 0) {
                    has_opaque_fb = true;
                }
            }
        } else {
            has_opaque_fb = true;
            opaque_fb_id = fb_id;
            opaque_pixel_format = store->pixel_format;
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
        "vk_gbm_backing_store_present_kms:\n"
        "    src_x, src_y, src_w, src_h: %f %f %f %f\n"
        "    dst_x, dst_y, dst_w, dst_h: %f %f %f %f\n",
        0.0, 0.0,
        store->backing_store.size.x,
        store->backing_store.size.y,
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
            pixel_format = store->pixel_format;
        }
    } else {
        fb_id = meta->fb_id;
        pixel_format = store->pixel_format;
    }

    LOG_DEBUG("presenting fb %d\n", store->front_fb - store->locked_fbs);

    TRACER_BEGIN(store->surface.tracer, "kms_req_builder_push_fb_layer");
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
            .src_w = DOUBLE_TO_FP1616_ROUNDED(store->backing_store.size.x),
            .src_h = DOUBLE_TO_FP1616_ROUNDED(store->backing_store.size.y),
            
            .has_rotation = false,
            .rotation = PLANE_TRANSFORM_ROTATE_0,
            
            .has_in_fence_fd = false,
            .in_fence_fd = 0
        },
        on_release_layer,
        locked_fb_ref(store->front_fb)
    );
    TRACER_END(store->surface.tracer, "kms_req_builder_push_fb_layer");
    if (ok != 0) {
        goto fail_unref_locked_fb;
    }


    surface_unlock(s);
    return ok;


    fail_unref_locked_fb:
    locked_fb_unref(store->front_fb);
    goto fail_unlock;

    fail_free_meta:
    free(meta);

    fail_unlock:
    surface_unlock(s);
    return ok;
}

static int vk_gbm_backing_store_present_fbdev(struct surface *s, const struct fl_layer_props *props, struct fbdev_commit_builder *builder) {
    struct vk_gbm_backing_store *store;

    /// TODO: Implement by mmapping the current front bo, copy it into the fbdev
    /// TODO: Print a warning here if we're not using explicit linear tiling and use glReadPixels instead of gbm_bo_map in that case

    store = CAST_THIS(s);
    (void) store;
    (void) props;
    (void) builder;

    UNIMPLEMENTED();

    return 0;
}


static int vk_gbm_backing_store_fill(struct backing_store *s, FlutterBackingStore *fl_store) {
    struct vk_gbm_backing_store *store;
    int i, ok;

    store = CAST_THIS(s);

    surface_lock(CAST_SURFACE_UNCHECKED(s));

    // Try to find & lock a locked_fb we can use.
    // Note we use atomics here even though we hold the surfaces' mutex because
    // releasing a locked_fb is possibly done without the mutex.
    for (i = 0; i < ARRAY_SIZE(store->locked_fbs); i++) {
        if (atomic_flag_test_and_set(&store->locked_fbs[i].is_locked) == false) {
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
    int before = atomic_fetch_add(&store->n_locked_fbs, 1);
    LOG_DEBUG("filling with fb %d\n", i);
    LOG_DEBUG("locked fbs: before: %d, now: %d\n", before, before+1);
    //DEBUG_ASSERT_MSG(before + 1 <= 3, "sanity check failed: too many locked fbs for double-buffered vsync");
#endif
    store->locked_fbs[i].store = CAST_VK_GBM_BACKING_STORE(surface_ref(CAST_SURFACE_UNCHECKED(s)));
    store->locked_fbs[i].n_refs = REFCOUNT_INIT_1;
    
    COMPILE_ASSERT(sizeof(FlutterVulkanBackingStore) == 16);
    fl_store->type = kFlutterBackingStoreTypeVulkan;
    fl_store->vulkan = (FlutterVulkanBackingStore) {
        .struct_size = sizeof(FlutterVulkanBackingStore),
        .image = &store->locked_fbs[i].fb->fl_image,
        .user_data = surface_ref(CAST_SURFACE_UNCHECKED(store)),
        .destruction_callback = surface_unref_void,
    };

    surface_unlock(CAST_SURFACE_UNCHECKED(s));

    return 0;


    fail_unlock:
    surface_unlock(CAST_SURFACE_UNCHECKED(s));
    return ok;
}

static int vk_gbm_backing_store_queue_present(struct backing_store *s, const FlutterBackingStore *fl_store) {
    struct vk_gbm_backing_store *store;
    struct locked_fb *fb;

    store = CAST_THIS(s);
    
    DEBUG_ASSERT_EQUALS(fl_store->type, kFlutterBackingStoreTypeVulkan);
    /// TODO: Implement handling if fl_store->did_update == false

    surface_lock(CAST_SURFACE_UNCHECKED(s));

    // find out which fb this image belongs too
    fb = NULL;
    for (int i = 0; i < ARRAY_SIZE(store->locked_fbs); i++) {
        if (store->locked_fbs[i].fb->fl_image.image == fl_store->vulkan.image->image) {
            fb = store->locked_fbs + i;
            LOG_DEBUG("queueing present fb %d\n", i);
            break;
        }
    }

    if (fb == NULL) {
        LOG_ERROR("The vulkan image flutter wants to present is not known to this backing store.\n");
        surface_unlock(CAST_SURFACE_UNCHECKED(s));
        return EINVAL;
    }

    locked_fb_swap_ptrs(&store->front_fb, fb);

    surface_unlock(CAST_SURFACE_UNCHECKED(s));
    return 0;
}

