// SPDX-License-Identifier: MIT
/*
 * Plugin Registry
 *
 * Initializes & deinitializes plugins, manages registration of plugins.
 *
 * Copyright (c) 2023, Hannes Winkler <hanneswinkler2000@web.de>
 */

#ifndef _FLUTTERPI_SRC_PLUGINREGISTRY_H
#define _FLUTTERPI_SRC_PLUGINREGISTRY_H

#include <string.h>

#include <sys/select.h>
#include <unistd.h>

#include "platformchannel.h"
#include "util/refcounting.h"

struct flutterpi;
struct plugin_registry;

/**
 * @brief The result of a plugin initialization.
 */
enum plugin_init_result {
    /**
     * @brief The plugin was successfully initialized.
     */
    PLUGIN_INIT_RESULT_INITIALIZED,

    /**
     * @brief The plugin couldn't be initialized, because it's not compatible with
     * the flutter-pi instance.
     * 
     * For example, the plugin requires OpenGL but flutter-pi is using software rendering.
     * This is not an error and flutter-pi will continue initializing the other plugins.
     */
    PLUGIN_INIT_RESULT_NOT_APPLICABLE,

    /**
     * @brief The plugin couldn't be initialized because an unexpected error ocurred.
     * 
     * Flutter-pi may decide to abort the startup phase of the whole flutter-pi instance at that point.
     */
    PLUGIN_INIT_RESULT_ERROR
};

typedef enum plugin_init_result (*plugin_init_t)(struct flutterpi *flutterpi, void **userdata_out);

typedef void (*plugin_deinit_t)(struct flutterpi *flutterpi, void *userdata);

struct flutterpi_plugin_v2 {
    refcount_t n_refs;
    const char *name;
    plugin_init_t init;
    plugin_deinit_t deinit;
};

static inline void flutterpi_plugin_v2_destroy(UNUSED struct flutterpi_plugin_v2 *plugin) {
    // no-op
}

DEFINE_STATIC_INLINE_REF_OPS(flutterpi_plugin_v2, n_refs)

struct _FlutterPlatformMessageResponseHandle;
typedef struct _FlutterPlatformMessageResponseHandle FlutterPlatformMessageResponseHandle;

/**
 * @brief A callback that gets called when a platform message arrives on a channel you registered it with.
 * 
 * @param channel The method channel that received a platform message.
 * @param object The object that is the result of automatically decoding the platform message using the codec given to plugin_registry_set_receiver.
 * @param responsehandle The response handle to respond to the platform message.
 * @return int 0 if the message was handled successfully, positive errno-code if an error occurred.
 */
typedef int (*platch_obj_recv_callback)(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *responsehandle);

typedef void (*platform_message_callback_v2_t)(void *userdata, const FlutterPlatformMessage *message);

/**
 * @brief Create a new plugin registry instance and add the hardcoded plugins, but don't initialize them yet.
 */
struct plugin_registry *plugin_registry_new(struct flutterpi *flutterpi);

void plugin_registry_destroy(struct plugin_registry *registry);

void plugin_registry_add_plugin(struct plugin_registry *registry, struct flutterpi_plugin_v2 *plugin);

int plugin_registry_add_plugins_from_static_registry(struct plugin_registry *registry);

/**
 * @brief Initialize all not-yet initialized plugins.
 */
int plugin_registry_ensure_plugins_initialized(struct plugin_registry *registry);

/**
 * @brief Deinitialize all initialized plugins.
 */
void plugin_registry_ensure_plugins_deinitialized(struct plugin_registry *registry);

/**
 * @brief Called by flutter-pi when a platform message arrives.
 */
int plugin_registry_on_platform_message(struct plugin_registry *registry, const FlutterPlatformMessage *message);

/**
 * @brief Sets the callback that should be called when a platform message arrives on channel `channel`.
 *
 * The platform message will be automatically decoded using the codec `codec`.
 */
int plugin_registry_set_receiver_v2(
    struct plugin_registry *registry,
    const char *channel,
    platform_message_callback_v2_t callback,
    void *userdata
);

/**
 * @brief Sets the callback that should be called when a platform message arrives on channel `channel`.
 *
 * The platform message will be automatically decoded using the codec `codec`.
 * 
 * @attention See @ref plugin_registry_remove_receiver_v2 for the semantics.
 * 
 * @param registry The plugin registry to set the receiver on.
 * @param channel The channel to set the receiver on.
 * @param callback The callback to call when a message arrives.
 */
int plugin_registry_set_receiver(const char *channel, enum platch_codec codec, platch_obj_recv_callback callback);

/**
 * @brief Removes the callback for platform channel `channel`.
 *
 * @attention This function will remove the receiver from the plugin registries
 * internal list of receiver callbacks. It does not wait for a currently running
 * callback to finish (obviously, because if you're calling this function from
 * within the callback, that would be impossible).
 * 
 * If you're not calling this function from the platform thread, so not from within
 * a receiver callback or a plugin init / deinit callback, the receiver callback
 * might still be in progress, or even be called one more time after this function returns.
 * 
 * So don't do this:
 *    void my_video_channel_callback(...) {
 *       struct my_video_data *data = userdata;
 *       // ...
 *    }
 *  
 *    // THREAD X (e.g. gstreamer video thread)   
 *    plugin_registry_remove_receiver_v2(registry, "my_video_channel");
 *    free(my_video_data);
 * 
 * Because the callback might still be called / in progress after you've freed the data.
 * 
 * Instead, post a message to the platform thread to remove the receiver callback:
 *    void remove_receiver_cb(void *userdata) {
 *       struct my_video_data *data = userdata;
 *       plugin_registry_remove_receiver_v2(registry, "my_video_channel");
 *       free(data);
 *    }
 *    
 *    // THREAD X
 *    flutterpi_post_platform_task(flutterpi, remove_receiver_cb, my_video_data);
 * 
 * @param registry The plugin registry to remove the receiver from.
 * @param channel The channel to remove the receiver from.
 */
int plugin_registry_remove_receiver_v2(struct plugin_registry *registry, const char *channel);

/**
 * @brief Removes the callback for platform channel `channel`.
 *
 */
int plugin_registry_remove_receiver(const char *channel);

void static_plugin_registry_add_plugin(struct flutterpi_plugin_v2 *plugin);

void static_plugin_registry_remove_plugin(const char *plugin_name);

#define FLUTTERPI_PLUGIN(_name, _identifier_name, _init, _deinit)                                                                   \
    static struct flutterpi_plugin_v2 _identifier_name##_plugin_struct = {                                                          \
        .n_refs = REFCOUNT_INIT_0,                                                                                                  \
        .name = (_name),                                                                                                            \
        .init = (_init),                                                                                                            \
        .deinit = (_deinit),                                                                                                        \
    };                                                                                                                              \
                                                                                                                                    \
    __attribute__((constructor)) static void __reg_plugin_##_identifier_name(void) {                                                \
        static_plugin_registry_add_plugin(&_identifier_name##_plugin_struct);                                                       \
    }                                                                                                                               \
                                                                                                                                    \
    __attribute__((destructor)) static void __unreg_plugin_##_identifier_name(void) {                                               \
        static_plugin_registry_remove_plugin(_name);                                                                                \
        ASSERT_MSG(refcount_is_zero(&_identifier_name##_plugin_struct.n_refs), "Plugin still in use while destructor is running."); \
    }

#endif  // _FLUTTERPI_SRC_PLUGINREGISTRY_H
