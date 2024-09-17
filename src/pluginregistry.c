#ifdef __STDC_ALLOC_LIB__
    #define __STDC_WANT_LIB_EXT2__ 1
#else
    #define _POSIX_C_SOURCE 200809L
#endif

#include "pluginregistry.h"

#include <stdatomic.h>
#include <string.h>

#include <sys/select.h>
#include <unistd.h>

#include <alloca.h>
#include <klib/khash.h>
#include <klib/kvec.h>

#include "flutter-pi.h"
#include "platformchannel.h"
#include "util/collection.h"
#include "util/list.h"
#include "util/lock_ops.h"
#include "util/logging.h"

/**
 * @brief details of a plugin for flutter-pi.
 *
 * All plugins are initialized (by calling their "init" callback)
 * when @ref plugin_registry_ensure_plugins_initialized is called by flutter-pi.
 *
 * In the init callback, you probably want to do stuff like
 * register callbacks for some method channels your plugin uses
 * or dynamically allocate memory for your plugin if you need to.
 */
struct plugin_instance {
    struct flutterpi_plugin_v2 *plugin;
    void *userdata;
    bool initialized;
};

struct platch_obj_cb_data {
    char *channel;
    enum platch_codec codec;
    platch_obj_recv_callback callback;
    platform_message_callback_v2_t callback_v2;
    void *userdata;
};

KHASH_MAP_INIT_STR(static_plugins, struct flutterpi_plugin_v2 *)
KHASH_MAP_INIT_STR(plugins, struct plugin_instance)
KHASH_MAP_INIT_STR(callbacks, struct platch_obj_cb_data)

struct plugin_registry {
    struct flutterpi *flutterpi;

    mutex_t plugins_lock;
    khash_t(plugins) * plugins;

    mutex_t callbacks_lock;
    khash_t(callbacks) * callbacks;
};

static khash_t(static_plugins) * static_plugins;
static pthread_once_t static_plugins_init_flag = PTHREAD_ONCE_INIT;
static mutex_t static_plugins_lock;

struct plugin_registry *plugin_registry_new(struct flutterpi *flutterpi) {
    struct plugin_registry *reg;

    reg = malloc(sizeof *reg);
    if (reg == NULL) {
        return NULL;
    }

    mutex_init(&reg->plugins_lock);
    reg->plugins = kh_init(plugins);
    mutex_init(&reg->callbacks_lock);
    reg->callbacks = kh_init(callbacks);
    reg->flutterpi = flutterpi;
    return reg;
}

void plugin_registry_destroy(struct plugin_registry *registry) {
    plugin_registry_ensure_plugins_deinitialized(registry);

    {
        struct plugin_instance instance;
        kh_foreach_value(registry->plugins, instance, { flutterpi_plugin_v2_unrefp(&instance.plugin); });
    }

    free(registry);
}

int plugin_registry_on_platform_message(struct plugin_registry *registry, const FlutterPlatformMessage *message) {
    struct platch_obj_cb_data data;
    int ok;

    mutex_lock(&registry->callbacks_lock);

    khiter_t entry = kh_get(callbacks, registry->callbacks, message->channel);
    if (entry == kh_end(registry->callbacks)) {
        ok = platch_respond_not_implemented((FlutterPlatformMessageResponseHandle *) message->response_handle);
        mutex_unlock(&registry->callbacks_lock);
        return ok;
    }

    data = kh_value(registry->callbacks, entry);

    mutex_unlock(&registry->callbacks_lock);

    if (data.callback_v2 != NULL) {
        data.callback_v2(data.userdata, message);
    } else {
        struct platch_obj object;

        ok = platch_decode((uint8_t *) message->message, message->message_size, data.codec, &object);
        if (ok != 0) {
            return platch_respond_not_implemented((FlutterPlatformMessageResponseHandle *) message->response_handle);
        }

        ok = data.callback((char *) message->channel, &object, (FlutterPlatformMessageResponseHandle *) message->response_handle);

        platch_free_obj(&object);
        return ok;
    }

    return 0;
}

void plugin_registry_add_plugin_locked(struct plugin_registry *registry, struct flutterpi_plugin_v2 *plugin) {
    int ok;

    khiter_t entry = kh_put(plugins, registry->plugins, plugin->name, &ok);
    if (ok == -1) {
        return;
    }

    kh_value(registry->plugins, entry).plugin = flutterpi_plugin_v2_ref(plugin);
    kh_value(registry->plugins, entry).initialized = false;
    kh_value(registry->plugins, entry).userdata = NULL;
}

void plugin_registry_add_plugin(struct plugin_registry *registry, struct flutterpi_plugin_v2 *plugin) {
    mutex_lock(&registry->plugins_lock);
    plugin_registry_add_plugin_locked(registry, plugin);
    mutex_unlock(&registry->plugins_lock);
}

static void static_plugin_registry_ensure_initialized(void);

int plugin_registry_add_plugins_from_static_registry(struct plugin_registry *registry) {
    static_plugin_registry_ensure_initialized();

    mutex_lock(&registry->plugins_lock);
    mutex_lock(&static_plugins_lock);

    {
        struct flutterpi_plugin_v2 *plugin;
        kh_foreach_value(static_plugins, plugin, { plugin_registry_add_plugin_locked(registry, plugin); })
    }

    mutex_unlock(&static_plugins_lock);
    mutex_unlock(&registry->plugins_lock);

    return 0;
}

int plugin_registry_ensure_plugins_initialized(struct plugin_registry *registry) {
    enum plugin_init_result result;

    mutex_lock(&registry->plugins_lock);

    {
        const char *plugin_name;
        struct plugin_instance instance;
        kh_foreach(registry->plugins, plugin_name, instance, {
            if (instance.initialized == false) {
                result = instance.plugin->init(registry->flutterpi, &instance.userdata);
                if (result == PLUGIN_INIT_RESULT_ERROR) {
                    LOG_ERROR("Error initializing plugin \"%s\".\n", plugin_name);
                    goto fail_deinit_all_initialized;
                } else if (result == PLUGIN_INIT_RESULT_NOT_APPLICABLE) {
                    // This is not an error.
                    LOG_DEBUG("INFO: Plugin \"%s\" is not available in this flutter-pi instance.\n", plugin_name);
                    continue;
                }

                ASSERT(result == PLUGIN_INIT_RESULT_INITIALIZED);
                instance.initialized = true;
            }
        });
    }

    LOG_DEBUG("Initialized plugins: ");
    {
        const char *plugin_name;
        struct plugin_instance instance;
        kh_foreach(registry->plugins, plugin_name, instance, {
            if (instance.initialized) {
                LOG_DEBUG_UNPREFIXED("%s, ", plugin_name);
            }
        });
    }
    LOG_DEBUG_UNPREFIXED("\n");

    mutex_unlock(&registry->plugins_lock);
    return 0;

fail_deinit_all_initialized: {
    struct plugin_instance instance;
    kh_foreach_value(registry->plugins, instance, {
        if (instance.initialized) {
            instance.plugin->deinit(registry->flutterpi, instance.userdata);
            instance.userdata = NULL;
            instance.initialized = false;
        }
    });
}
    mutex_unlock(&registry->plugins_lock);
    return EINVAL;
}

void plugin_registry_ensure_plugins_deinitialized(struct plugin_registry *registry) {
    mutex_lock(&registry->plugins_lock);

    struct plugin_instance instance;
    kh_foreach_value(registry->plugins, instance, {
        if (instance.initialized == true) {
            instance.plugin->deinit(registry->flutterpi, instance.userdata);
            instance.initialized = false;
        }
    });

    mutex_unlock(&registry->plugins_lock);
}

/// TODO: Move this into a separate flutter messenger API
static int set_receiver_locked(
    struct plugin_registry *registry,
    const char *channel,
    enum platch_codec codec,
    platch_obj_recv_callback callback,
    platform_message_callback_v2_t callback_v2,
    void *userdata
) {
    char *channel_dup;

    ASSERT_MSG((!!callback) != (!!callback_v2), "Exactly one of callback or callback_v2 must be non-NULL.");
    assert_mutex_locked(&registry->callbacks_lock);

    channel_dup = strdup(channel);
    if (channel_dup == NULL) {
        return ENOMEM;
    }

    int ok;
    khiter_t entry = kh_put(callbacks, registry->callbacks, channel_dup, &ok);
    if (ok == -1) {
        free(channel_dup);
        return ENOMEM;
    } else if (ok == 0) {
        // key was already present,
        // free the channel string.
        free(channel_dup);
    } else {
        kh_value(registry->callbacks, entry).channel = channel_dup;
    }

    kh_value(registry->callbacks, entry).codec = codec;
    kh_value(registry->callbacks, entry).callback = callback;
    kh_value(registry->callbacks, entry).callback_v2 = callback_v2;
    kh_value(registry->callbacks, entry).userdata = userdata;
    return 0;
}

static int set_receiver(
    struct plugin_registry *registry,
    const char *channel,
    enum platch_codec codec,
    platch_obj_recv_callback callback,
    platform_message_callback_v2_t callback_v2,
    void *userdata
) {
    int ok;

    mutex_lock(&registry->callbacks_lock);
    ok = set_receiver_locked(registry, channel, codec, callback, callback_v2, userdata);
    mutex_unlock(&registry->callbacks_lock);

    return ok;
}

int plugin_registry_set_receiver_v2_locked(
    struct plugin_registry *registry,
    const char *channel,
    platform_message_callback_v2_t callback,
    void *userdata
) {
    return set_receiver_locked(registry, channel, kBinaryCodec, NULL, callback, userdata);
}

int plugin_registry_set_receiver_v2(
    struct plugin_registry *registry,
    const char *channel,
    platform_message_callback_v2_t callback,
    void *userdata
) {
    return set_receiver(registry, channel, kBinaryCodec, NULL, callback, userdata);
}

/// TODO: Move this into a separate flutter messenger API
int plugin_registry_set_receiver_locked(const char *channel, enum platch_codec codec, platch_obj_recv_callback callback) {
    struct plugin_registry *registry;

    registry = flutterpi_get_plugin_registry(flutterpi);
    ASSUME(registry != NULL);

    return set_receiver_locked(registry, channel, codec, callback, NULL, NULL);
}

int plugin_registry_set_receiver(const char *channel, enum platch_codec codec, platch_obj_recv_callback callback) {
    struct plugin_registry *registry;

    registry = flutterpi_get_plugin_registry(flutterpi);
    ASSUME(registry != NULL);

    return set_receiver(registry, channel, codec, callback, NULL, NULL);
}

int plugin_registry_remove_receiver_v2(struct plugin_registry *registry, const char *channel) {
    mutex_lock(&registry->callbacks_lock);

    khiter_t entry = kh_get(callbacks, registry->callbacks, channel);
    if (entry == kh_end(registry->callbacks)) {
        mutex_unlock(&registry->callbacks_lock);
        return EINVAL;
    }

    char *key = kh_val(registry->callbacks, entry).channel;
    kh_del(callbacks, registry->callbacks, entry);
    free(key);

    mutex_unlock(&registry->callbacks_lock);
    return 0;
}

int plugin_registry_remove_receiver(const char *channel) {
    struct plugin_registry *registry;

    registry = flutterpi_get_plugin_registry(flutterpi);
    ASSUME(registry != NULL);

    return plugin_registry_remove_receiver_v2(registry, channel);
}

bool plugin_registry_is_plugin_present_locked(struct plugin_registry *registry, const char *plugin_name) {
    khiter_t plugin = kh_get(plugins, registry->plugins, plugin_name);
    return plugin != kh_end(registry->plugins);
}

bool plugin_registry_is_plugin_present(struct plugin_registry *registry, const char *plugin_name) {
    mutex_lock(&registry->plugins_lock);

    bool present = plugin_registry_is_plugin_present_locked(registry, plugin_name);

    mutex_unlock(&registry->plugins_lock);

    return present;
}

static void static_plugin_registry_initialize(void) {
    static_plugins = kh_init(static_plugins);
    ASSERT_NOT_NULL(static_plugins);

    mutex_init(&static_plugins_lock);
}

static void static_plugin_registry_ensure_initialized(void) {
    pthread_once(&static_plugins_init_flag, static_plugin_registry_initialize);
}

void static_plugin_registry_add_plugin(struct flutterpi_plugin_v2 *plugin) {
    int ok;

    static_plugin_registry_ensure_initialized();

    mutex_lock(&static_plugins_lock);

    khiter_t entry = kh_put(static_plugins, static_plugins, plugin->name, &ok);
    ASSERT(ok >= 0);

    kh_value(static_plugins, entry) = flutterpi_plugin_v2_ref(plugin);

    mutex_unlock(&static_plugins_lock);
}

void static_plugin_registry_remove_plugin(const char *plugin_name) {
    static_plugin_registry_ensure_initialized();

    mutex_lock(&static_plugins_lock);
    khiter_t entry = kh_get(static_plugins, static_plugins, plugin_name);
    if (entry == kh_end(static_plugins)) {
        mutex_unlock(&static_plugins_lock);
        return;
    }

    flutterpi_plugin_v2_unrefp(&kh_value(static_plugins, entry));
    kh_del(static_plugins, static_plugins, entry);

    mutex_unlock(&static_plugins_lock);
}
