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

struct flutterpi;
struct plugin_registry;

typedef enum plugin_init_result (*plugin_init_t)(struct flutterpi *flutterpi, void **userdata_out);

typedef void (*plugin_deinit_t)(struct flutterpi *flutterpi, void *userdata);

struct flutterpi_plugin_v2 {
    const char *name;
    plugin_init_t init;
    plugin_deinit_t deinit;
};

/// The return value of a plugin initializer function.
enum plugin_init_result {
    PLUGIN_INIT_RESULT_INITIALIZED,  ///< The plugin was successfully initialized.
    PLUGIN_INIT_RESULT_NOT_APPLICABLE,  ///< The plugin couldn't be initialized, because it's not compatible with the flutter-pi instance.
    ///  For example, the plugin requires OpenGL but flutter-pi is using software rendering.
    ///  This is not an error, and flutter-pi will continue initializing the other plugins.
    PLUGIN_INIT_RESULT_ERROR  ///< The plugin couldn't be initialized because an unexpected error ocurred.
    ///  Flutter-pi may decide to abort the startup phase of the whole flutter-pi instance at that point.
};

struct _FlutterPlatformMessageResponseHandle;
typedef struct _FlutterPlatformMessageResponseHandle FlutterPlatformMessageResponseHandle;

/// A Callback that gets called when a platform message
/// arrives on a channel you registered it with.
/// channel is the method channel that received a platform message,
/// object is the object that is the result of automatically decoding
/// the platform message using the codec given to plugin_registry_set_receiver.
/// BE AWARE that object->type can be kNotImplemented, REGARDLESS of the codec
///   passed to plugin_registry_set_receiver.
typedef int (*platch_obj_recv_callback)(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *responsehandle);

typedef void (*platform_message_callback_v2_t)(void *userdata, const FlutterPlatformMessage *message);

/**
 * @brief Create a new plugin registry instance and add the hardcoded plugins, but don't initialize them yet.
 */
struct plugin_registry *plugin_registry_new(struct flutterpi *flutterpi);

void plugin_registry_destroy(struct plugin_registry *registry);

void plugin_registry_add_plugin(struct plugin_registry *registry, const struct flutterpi_plugin_v2 *plugin);

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
int plugin_registry_set_receiver_v2_locked(
    struct plugin_registry *registry,
    const char *channel,
    platform_message_callback_v2_t callback,
    void *userdata
);

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
 */
int plugin_registry_set_receiver_locked(const char *channel, enum platch_codec codec, platch_obj_recv_callback callback);

/**
 * @brief Sets the callback that should be called when a platform message arrives on channel `channel`.
 *
 * The platform message will be automatically decoded using the codec `codec`.
 */
int plugin_registry_set_receiver(const char *channel, enum platch_codec codec, platch_obj_recv_callback callback);

/**
 * @brief Removes the callback for platform channel `channel`.
 *
 */
int plugin_registry_remove_receiver_v2_locked(struct plugin_registry *registry, const char *channel);

/**
 * @brief Removes the callback for platform channel `channel`.
 *
 */
int plugin_registry_remove_receiver_v2(struct plugin_registry *registry, const char *channel);

/**
 * @brief Removes the callback for platform channel `channel`.
 *
 */
int plugin_registry_remove_receiver_locked(const char *channel);

/**
 * @brief Removes the callback for platform channel `channel`.
 *
 */
int plugin_registry_remove_receiver(const char *channel);

void *plugin_registry_get_plugin_userdata(struct plugin_registry *registry, const char *plugin_name);

void *plugin_registry_get_plugin_userdata_locked(struct plugin_registry *registry, const char *plugin_name);

/**
 * @brief Returns true @ref registry has a plugin with name @ref plugin_name.
 */
bool plugin_registry_is_plugin_present(struct plugin_registry *registry, const char *plugin_name);

/**
 * @brief Returns true @ref registry has a plugin with name @ref plugin_name.
 */
bool plugin_registry_is_plugin_present_locked(struct plugin_registry *registry, const char *plugin_name);

int plugin_registry_deinit(void);

void static_plugin_registry_add_plugin(const struct flutterpi_plugin_v2 *plugin);

void static_plugin_registry_remove_plugin(const char *plugin_name);

#define FLUTTERPI_PLUGIN(_name, _identifier_name, _init, _deinit)                \
    __attribute__((constructor)) static void __reg_plugin_##_identifier_name() { \
        static struct flutterpi_plugin_v2 plugin = {                             \
            .name = (_name),                                                     \
            .init = (_init),                                                     \
            .deinit = (_deinit),                                                 \
        };                                                                       \
        static_plugin_registry_add_plugin(&plugin);                              \
    }                                                                            \
                                                                                 \
    __attribute__((destructor)) static void __unreg_plugin_##_identifier_name() { static_plugin_registry_remove_plugin(_name); }

#endif  // _FLUTTERPI_SRC_PLUGINREGISTRY_H
