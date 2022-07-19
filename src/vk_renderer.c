// SPDX-License-Identifier: MIT
/*
 * Vulkan Renderer Implementation
 *
 * Copyright (c) 2022, Hannes Winkler <hanneswinkler2000@web.de>
 */


#include <stdlib.h>
#include <alloca.h>

#include <collection.h>
#include <vulkan.h>
#include <vk_renderer.h>

#define VALIDATION_LAYER_NAME "VK_LAYER_KHRONOS_validation"

FILE_DESCR("vulkan renderer")

MAYBE_UNUSED static VkLayerProperties *get_layer_props(int n_layers, VkLayerProperties *layers, const char *layer_name) {
    for (int i = 0; i < n_layers; i++) {
        if (strcmp(layers[i].layerName, layer_name) == 0) {
            return layers + i;
        }
    }
    return NULL;
}

MAYBE_UNUSED static bool supports_layer(int n_layers, VkLayerProperties *layers, const char *layer_name) {
    return get_layer_props(n_layers, layers, layer_name) != NULL;
}

static VkExtensionProperties *get_extension_props(int n_extensions, VkExtensionProperties *extensions, const char *extension_name) {
    for (int i = 0; i < n_extensions; i++) {
        if (strcmp(extensions[i].extensionName, extension_name) == 0) {
            return extensions + i;
        }
    }
    return NULL;
}

MAYBE_UNUSED static bool supports_extension(int n_extensions, VkExtensionProperties *extensions, const char *extension_name) {
    return get_extension_props(n_extensions, extensions, extension_name) != NULL;
}

static VkBool32 on_debug_utils_message(
    VkDebugUtilsMessageSeverityFlagBitsEXT           severity,
    MAYBE_UNUSED VkDebugUtilsMessageTypeFlagsEXT     types,
    const VkDebugUtilsMessengerCallbackDataEXT*      data,
    MAYBE_UNUSED void*                               userdata
) {
    LOG_DEBUG(
        "[%s] (%d, %s) %s (queues: %d, cmdbufs: %d, objects: %d)\n",
        severity == VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT ? "VERBOSE" :
            severity == VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT ? "INFO" :
            severity == VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT ? "WARNING" :
            severity == VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "ERROR" : "unknown severity",
        data->messageIdNumber,
        data->pMessageIdName,
        data->pMessage,
        data->queueLabelCount,
        data->cmdBufLabelCount,
        data->objectCount
    );
    return VK_TRUE;
}

static int get_graphics_queue_family_index(VkPhysicalDevice device) {
    uint32_t n_queue_families;

    vkGetPhysicalDeviceQueueFamilyProperties(device, &n_queue_families, NULL);

    VkQueueFamilyProperties queue_families[n_queue_families];
    vkGetPhysicalDeviceQueueFamilyProperties(device, &n_queue_families, queue_families);

    for (unsigned i = 0; i < n_queue_families; i++) {
        if (queue_families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            return i;
        }
    }

    return -1;
}

static int score_physical_device(VkPhysicalDevice device, const char **required_device_extensions) {
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceFeatures features;
    VkResult ok;
    uint32_t n_available_extensions;
    int graphics_queue_fam_index;
    int score = 1;

    vkGetPhysicalDeviceProperties(device, &props);
    vkGetPhysicalDeviceFeatures(device, &features);

    if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
        score += 15;
    } else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
        score += 10;
    }

    graphics_queue_fam_index = get_graphics_queue_family_index(device);
    if (graphics_queue_fam_index == -1) {
        LOG_ERROR("Physical device does not support a graphics queue.\n");
        return 0;
    }

    ok = vkEnumerateDeviceExtensionProperties(device, NULL, &n_available_extensions, NULL);
    if (ok != 0) {
        LOG_VK_ERROR(ok, "Could not query available physical device extensions. vkEnumerateDeviceExtensionProperties");
        return 0;
    }

    VkExtensionProperties available_extensions[n_available_extensions];
    ok = vkEnumerateDeviceExtensionProperties(device, NULL, &n_available_extensions, available_extensions);
    if (ok != 0) {
        LOG_VK_ERROR(ok, "Could not query available physical device extensions. vkEnumerateDeviceExtensionProperties");
        return 0;
    }

    for (const char **cursor = required_device_extensions; *cursor != NULL; cursor++) {
        for (unsigned i = 0; i < n_available_extensions; i++) {
            if (strcmp(available_extensions[i].extensionName, *cursor) == 0) {
                goto found;
            }
        }
        LOG_ERROR("Required extension %s is not supported by vulkan device.\n", *cursor);
        return 0;

        found:
        continue;
    }

    return score;
}


struct vk_renderer {
    refcount_t n_refs;

    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkQueue graphics_queue;
    VkDebugUtilsMessengerEXT debug_utils_messenger;
    VkCommandPool graphics_cmd_pool;

    PFN_vkCreateDebugUtilsMessengerEXT create_debug_utils_messenger;
    PFN_vkDestroyDebugUtilsMessengerEXT destroy_debug_utils_messenger;

    int n_enabled_layers;
    const char **enabled_layers;

    int n_enabled_instance_extensions;
    const char **enabled_instance_extensions;

    int n_enabled_device_extensions;
    const char **enabled_device_extensions;
};

ATTR_MALLOC struct vk_renderer *vk_renderer_new() {
    PFN_vkDestroyDebugUtilsMessengerEXT destroy_debug_utils_messenger;
    PFN_vkCreateDebugUtilsMessengerEXT create_debug_utils_messenger;
    VkDebugUtilsMessengerEXT debug_utils_messenger;
    struct vk_renderer *renderer;
    VkPhysicalDevice physical_device;
    VkCommandPool graphics_cmd_pool;
    VkInstance instance;
    VkDevice device;
    VkResult ok;
    VkQueue graphics_queue;
    uint32_t n_available_layers, n_available_instance_extensions, n_available_device_extensions, n_physical_devices;
    const char **enabled_layers, **enabled_instance_extensions, **enabled_device_extensions;
    bool enable_debug_utils_messenger;
    int n_enabled_layers, n_enabled_instance_extensions, n_enabled_device_extensions;
    int graphics_queue_family_index;

    renderer = malloc(sizeof *renderer);
    if (renderer == NULL) {
        return NULL;
    }

    ok = vkEnumerateInstanceLayerProperties(&n_available_layers, NULL);
    if (ok != VK_SUCCESS) {
        LOG_VK_ERROR(ok, "Could not query vulkan instance layers. vkEnumerateInstanceLayerProperties");
        goto fail_free_renderer;
    }

    VkLayerProperties *available_layers = alloca(sizeof(VkLayerProperties) * n_available_layers);
    ok = vkEnumerateInstanceLayerProperties(&n_available_layers, available_layers);
    if (ok != VK_SUCCESS) {
        LOG_VK_ERROR(ok, "Could not query vulkan instance layers. vkEnumerateInstanceLayerProperties");
        goto fail_free_renderer;
    }

    n_enabled_layers = 0;
    enabled_layers = malloc(n_available_layers * sizeof(*enabled_layers));
    if (enabled_layers == NULL) {
        goto fail_free_renderer;
    }

#ifdef VULKAN_DEBUG
    if (supports_layer(n_available_layers, available_layers, VALIDATION_LAYER_NAME)) {
        enabled_layers[n_enabled_layers] = VALIDATION_LAYER_NAME;
        n_enabled_layers++;
    } else {
        LOG_DEBUG("Vulkan validation layer was not found. Validation will not be enabled.\n");
    }
#endif

    ok = vkEnumerateInstanceExtensionProperties(NULL, &n_available_instance_extensions, NULL);
    if (ok != VK_SUCCESS) {
        LOG_VK_ERROR(ok, "Could not query vulkan instance extensions. vkEnumerateInstanceExtensionProperties");
        goto fail_free_enabled_layers;
    }

    VkExtensionProperties *available_instance_extensions = alloca(sizeof(VkExtensionProperties) * n_available_instance_extensions);
    ok = vkEnumerateInstanceExtensionProperties(NULL, &n_available_instance_extensions, available_instance_extensions);
    if (ok != VK_SUCCESS) {
        LOG_VK_ERROR(ok, "Could not query vulkan instance extensions. vkEnumerateInstanceExtensionProperties");
        goto fail_free_enabled_layers;
    }

    n_enabled_instance_extensions = 0;
    enabled_instance_extensions = malloc(n_available_instance_extensions * sizeof *enabled_instance_extensions);
    if (enabled_instance_extensions == NULL) {
        goto fail_free_enabled_layers;
    }

    enable_debug_utils_messenger = false;
#ifdef VULKAN_DEBUG
    if (supports_extension(n_available_instance_extensions, available_instance_extensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
        enabled_instance_extensions[n_enabled_instance_extensions] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        n_enabled_instance_extensions++;
        enable_debug_utils_messenger = true;
    } else {
        LOG_DEBUG("Vulkan debug utils extension was not found. Debug logging will not be enabled.\n");
    }
#endif

    /// TODO: Maybe enable some other useful instance extensions here?

    ok = vkCreateInstance(
        &(VkInstanceCreateInfo) {
            .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
            .flags = 0,
            .pApplicationInfo = &(VkApplicationInfo) {
                .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                .pApplicationName = "flutter-pi",
                .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
                .pEngineName = "flutter-pi",
                .engineVersion = VK_MAKE_VERSION(1, 0, 0),
                .apiVersion = VK_MAKE_VERSION(1, 1, 0),
                .pNext = NULL,
            },
            .enabledLayerCount = n_enabled_layers,
            .ppEnabledLayerNames = enabled_layers,
            .enabledExtensionCount = n_enabled_instance_extensions,
            .ppEnabledExtensionNames = enabled_instance_extensions,
            .pNext = enable_debug_utils_messenger ? &(VkDebugUtilsMessengerCreateInfoEXT) {
                .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
                .flags = 0,
                .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT
                    | VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT
                    | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
                    | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
                .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
                    | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                    | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
                .pfnUserCallback = on_debug_utils_message,
                .pUserData = NULL,
                .pNext = NULL,
            } : NULL
        },
        NULL,
        &instance
    );
    if (ok != VK_SUCCESS) {
        LOG_VK_ERROR(ok, "Could not create instance. vkCreateInstance");
        goto fail_free_enabled_instance_extensions;
    }

    if (enable_debug_utils_messenger) {
        create_debug_utils_messenger = (PFN_vkCreateDebugUtilsMessengerEXT) vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
        if (create_debug_utils_messenger == NULL) {
            LOG_ERROR("Could not resolve vkCreateDebugUtilsMessengerEXT function.\n");
            goto fail_destroy_instance;
        }

        destroy_debug_utils_messenger = (PFN_vkDestroyDebugUtilsMessengerEXT) vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
        if (destroy_debug_utils_messenger == NULL) {
            LOG_ERROR("Could not resolve vkDestroyDebugUtilsMessengerEXT function.\n");
            goto fail_destroy_instance;
        }

        ok = create_debug_utils_messenger(
            instance,
            &(VkDebugUtilsMessengerCreateInfoEXT) {
                .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
                .flags = 0,
                .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT
                    | VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT
                    | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
                    | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
                .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
                    | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                    | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
                .pfnUserCallback = on_debug_utils_message,
                .pUserData = NULL,
                .pNext = NULL,
            },
            NULL,
            &debug_utils_messenger
        );
        if (ok != VK_SUCCESS) {
            LOG_VK_ERROR(ok, "Could not create debug utils messenger. vkCreateDebugUtilsMessengerEXT");
            goto fail_destroy_instance;
        }
    } else {
        debug_utils_messenger = VK_NULL_HANDLE;
    }

    ok = vkEnumeratePhysicalDevices(instance, &n_physical_devices, NULL);
    if (ok != VK_SUCCESS) {
        LOG_VK_ERROR(ok, "Could not enumerate physical devices. vkEnumeratePhysicalDevices");
        goto fail_maybe_destroy_messenger;
    }

    VkPhysicalDevice *physical_devices = alloca(sizeof(VkPhysicalDevice) * n_physical_devices);
    ok = vkEnumeratePhysicalDevices(instance, &n_physical_devices, physical_devices);
    if (ok != VK_SUCCESS) {
        LOG_VK_ERROR(ok, "Could not enumerate physical devices. vkEnumeratePhysicalDevices");
        goto fail_maybe_destroy_messenger;
    }

    static const char *required_device_extensions[] = {
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
        NULL
    };

    physical_device = VK_NULL_HANDLE;
    int score = 0;
    for (unsigned i = 0; i < n_physical_devices; i++) {
        VkPhysicalDevice this = physical_devices[i];
        int this_score = score_physical_device(this, required_device_extensions);
        
        if (this_score > score) {
            physical_device = this;
            score = this_score;
        }
    }

    if (physical_device == VK_NULL_HANDLE) {
        LOG_ERROR("No suitable physical device found.\n");
        goto fail_maybe_destroy_messenger;
    }

    ok = vkEnumerateDeviceExtensionProperties(physical_device, NULL, &n_available_device_extensions, NULL);
    if (ok != VK_SUCCESS) {
        LOG_VK_ERROR(ok, "Could not query device extensions. vkEnumerateDeviceExtensionProperties");
        goto fail_maybe_destroy_messenger;
    }

    VkExtensionProperties *available_device_extensions = alloca(sizeof(VkExtensionProperties) * n_available_device_extensions);
    ok = vkEnumerateDeviceExtensionProperties(physical_device, NULL, &n_available_device_extensions, available_device_extensions);
    if (ok != VK_SUCCESS) {
        LOG_VK_ERROR(ok, "Could not query device extensions. vkEnumerateDeviceExtensionProperties");
        goto fail_maybe_destroy_messenger;
    }

    n_enabled_device_extensions = 0;
    enabled_device_extensions = malloc(n_available_device_extensions * sizeof *enabled_device_extensions);
    if (enabled_device_extensions == NULL) {
        goto fail_maybe_destroy_messenger;
    }

    // add all the required extensions to the list of enabled device extensions
    for (const char **cursor = required_device_extensions; cursor != NULL && *cursor != NULL; cursor++) {
        enabled_device_extensions[n_enabled_device_extensions] = *cursor;
        n_enabled_device_extensions++;
    }

    /// TODO: Maybe enable some other useful device extensions here?

    graphics_queue_family_index = get_graphics_queue_family_index(physical_device);

    ok = vkCreateDevice(
        physical_device,
        &(const VkDeviceCreateInfo) {
            .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .flags = 0,
            .queueCreateInfoCount = 1,
            .pQueueCreateInfos = (const VkDeviceQueueCreateInfo[1]) {
                {
                    .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                    .flags = 0,
                    .queueFamilyIndex = graphics_queue_family_index,
                    .queueCount = 1,
                    .pQueuePriorities = (float[1]) { 1.0f },
                    .pNext = NULL,
                },
            },
            .enabledLayerCount = n_enabled_layers,
            .ppEnabledLayerNames = enabled_layers,
            .enabledExtensionCount = n_enabled_device_extensions,
            .ppEnabledExtensionNames = enabled_device_extensions,
            .pEnabledFeatures = &(const VkPhysicalDeviceFeatures) { 0 },
            .pNext = NULL,
        },
        NULL,
        &device
    );
    if (ok != VK_SUCCESS) {
        LOG_VK_ERROR(ok, "Could not create logical device. vkCreateDevice");
        goto fail_free_enabled_device_extensions;
    }

    vkGetDeviceQueue(device, graphics_queue_family_index, 0, &graphics_queue);

    ok = vkCreateCommandPool(
        device,
        &(const VkCommandPoolCreateInfo) {
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = graphics_queue_family_index,
            .pNext = NULL,
        },
        NULL,
        &graphics_cmd_pool
    );
    if (ok != VK_SUCCESS) {
        LOG_VK_ERROR(ok, "Could not create command pool for allocating graphics command buffers. vkCreateCommandPool");
        goto fail_destroy_device;
    }

    renderer->device = device;
    renderer->physical_device = physical_device;
    renderer->instance = instance;
    renderer->graphics_queue = graphics_queue;
    renderer->debug_utils_messenger = debug_utils_messenger;
    renderer->graphics_cmd_pool = graphics_cmd_pool;
    renderer->create_debug_utils_messenger = create_debug_utils_messenger;
    renderer->destroy_debug_utils_messenger = destroy_debug_utils_messenger;
    renderer->n_enabled_layers = n_enabled_layers;
    renderer->enabled_layers = enabled_layers;
    renderer->n_enabled_instance_extensions = n_enabled_instance_extensions;
    renderer->enabled_instance_extensions = enabled_instance_extensions;
    renderer->n_enabled_device_extensions = n_enabled_device_extensions;
    renderer->enabled_device_extensions = enabled_device_extensions;
    return renderer;


    fail_destroy_device:
    vkDestroyDevice(device, NULL);

    fail_free_enabled_device_extensions:
    free(enabled_device_extensions);

    fail_maybe_destroy_messenger:
    if (debug_utils_messenger != VK_NULL_HANDLE) {
        destroy_debug_utils_messenger(instance, debug_utils_messenger, NULL);
    }

    fail_destroy_instance:
    vkDestroyInstance(instance, NULL);

    fail_free_enabled_instance_extensions:
    free(enabled_instance_extensions);

    fail_free_enabled_layers:
    free(enabled_layers);

    fail_free_renderer:
    free(renderer);
    return NULL;
}

void vk_renderer_destroy(struct vk_renderer *renderer) {
    VkResult ok;

    ok = vkDeviceWaitIdle(renderer->device);
    if (ok != VK_SUCCESS) {
        LOG_VK_ERROR(ok, "Couldn't wait for vulkan device idle to destroy it. vkDeviceWaitIdle");
    }

    vkDestroyCommandPool(renderer->device, renderer->graphics_cmd_pool, NULL);
    vkDestroyDevice(renderer->device, NULL);
    free(renderer->enabled_device_extensions);
    if (renderer->debug_utils_messenger != VK_NULL_HANDLE) {
        renderer->destroy_debug_utils_messenger(renderer->instance, renderer->debug_utils_messenger, NULL);
    }
    vkDestroyInstance(renderer->instance, NULL);
    free(renderer->enabled_instance_extensions);
    free(renderer->enabled_layers);
    free(renderer);
}

DEFINE_REF_OPS(vk_renderer, n_refs)

ATTR_CONST uint32_t vk_renderer_get_vk_version(struct vk_renderer MAYBE_UNUSED *renderer) {
    DEBUG_ASSERT_NOT_NULL(renderer);
    return VK_MAKE_VERSION(1, 1, 0);
}

ATTR_PURE VkInstance vk_renderer_get_instance(struct vk_renderer *renderer) {
    DEBUG_ASSERT_NOT_NULL(renderer);
    return renderer->instance;
}

ATTR_PURE VkPhysicalDevice vk_renderer_get_physical_device(struct vk_renderer *renderer) {
    DEBUG_ASSERT_NOT_NULL(renderer);
    return renderer->physical_device;
}

ATTR_PURE VkDevice vk_renderer_get_device(struct vk_renderer *renderer) {
    DEBUG_ASSERT_NOT_NULL(renderer);
    return renderer->device;
}

ATTR_PURE uint32_t vk_renderer_get_queue_family_index(struct vk_renderer *renderer) {
    DEBUG_ASSERT_NOT_NULL(renderer);
    return (uint32_t) get_graphics_queue_family_index(renderer->physical_device);
}

ATTR_PURE VkQueue vk_renderer_get_queue(struct vk_renderer *renderer) {
    DEBUG_ASSERT_NOT_NULL(renderer);
    return renderer->graphics_queue;
}

ATTR_PURE int vk_renderer_get_enabled_instance_extension_count(struct vk_renderer *renderer) {
    DEBUG_ASSERT_NOT_NULL(renderer);
    return renderer->n_enabled_instance_extensions;
}

ATTR_PURE const char **vk_renderer_get_enabled_instance_extensions(struct vk_renderer *renderer) {
    DEBUG_ASSERT_NOT_NULL(renderer);
    return renderer->enabled_instance_extensions;
}

ATTR_PURE int vk_renderer_get_enabled_device_extension_count(struct vk_renderer *renderer) {
    DEBUG_ASSERT_NOT_NULL(renderer);
    return renderer->n_enabled_device_extensions;
}

ATTR_PURE const char **vk_renderer_get_enabled_device_extensions(struct vk_renderer *renderer) {
    DEBUG_ASSERT_NOT_NULL(renderer);
    return renderer->enabled_device_extensions;
}

ATTR_PURE int vk_renderer_find_mem_type(struct vk_renderer *renderer, VkMemoryPropertyFlags flags, uint32_t req_bits) {
    VkPhysicalDeviceMemoryProperties props;

    vkGetPhysicalDeviceMemoryProperties(renderer->physical_device, &props);

    for (unsigned i = 0u; i < props.memoryTypeCount; ++i) {
        if (req_bits & (1 << i)) {
            if ((props.memoryTypes[i].propertyFlags & flags) == flags) {
                return i;
            }
        }
    }

    return -1;
}
