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
    struct list_head entry;

    const struct flutterpi_plugin_v2 *plugin;
    void *userdata;
    bool initialized;
};

struct platch_obj_cb_data {
    struct list_head entry;

    char *channel;
    enum platch_codec codec;
    platch_obj_recv_callback callback;
    platform_message_callback_v2_t callback_v2;
    void *userdata;
};

struct plugin_registry {
    pthread_mutex_t lock;
    struct flutterpi *flutterpi;
    struct list_head plugins;
    struct list_head callbacks;
};

DEFINE_STATIC_LOCK_OPS(plugin_registry, lock)

struct static_plugin_list_entry {
    struct list_head entry;
    const struct flutterpi_plugin_v2 *plugin;
};

static pthread_once_t static_plugins_init_flag = PTHREAD_ONCE_INIT;
static pthread_mutex_t static_plugins_lock;
static struct list_head static_plugins;

static struct plugin_instance *get_plugin_by_name_locked(struct plugin_registry *registry, const char *plugin_name) {
    list_for_each_entry(struct plugin_instance, instance, &registry->plugins, entry) {
        if (streq(instance->plugin->name, plugin_name)) {
            return instance;
        }
    }

    return NULL;
}

static struct plugin_instance *get_plugin_by_name(struct plugin_registry *registry, const char *plugin_name) {
    struct plugin_instance *instance;

    plugin_registry_lock(registry);

    instance = get_plugin_by_name_locked(registry, plugin_name);

    plugin_registry_unlock(registry);

    return instance;
}

static struct platch_obj_cb_data *get_cb_data_by_channel_locked(struct plugin_registry *registry, const char *channel) {
    list_for_each_entry(struct platch_obj_cb_data, data, &registry->callbacks, entry) {
        if (streq(data->channel, channel)) {
            return data;
        }
    }

    return NULL;
}

struct plugin_registry *plugin_registry_new(struct flutterpi *flutterpi) {
    struct plugin_registry *reg;
    ASSERTED int ok;

    reg = malloc(sizeof *reg);
    if (reg == NULL) {
        return NULL;
    }

    ok = pthread_mutex_init(&reg->lock, get_default_mutex_attrs());
    ASSERT_ZERO(ok);

    list_inithead(&reg->plugins);
    list_inithead(&reg->callbacks);

    reg->flutterpi = flutterpi;
    return reg;

    return NULL;
}

void plugin_registry_destroy(struct plugin_registry *registry) {
    plugin_registry_ensure_plugins_deinitialized(registry);

    // remove all plugins
    list_for_each_entry_safe(struct plugin_instance, instance, &registry->plugins, entry) {
        assert(instance->initialized == false);
        list_del(&instance->entry);
        free(instance);
    }

    assert(list_is_empty(&registry->plugins));
    assert(list_is_empty(&registry->callbacks));
    free(registry);
}

int plugin_registry_on_platform_message(struct plugin_registry *registry, const FlutterPlatformMessage *message) {
    struct platch_obj_cb_data *data;
    platch_obj_recv_callback callback;
    platform_message_callback_v2_t callback_v2;
    struct platch_obj object;
    enum platch_codec codec;
    void *userdata;
    int ok;

    plugin_registry_lock(registry);

    data = get_cb_data_by_channel_locked(registry, message->channel);
    if (data == NULL || (data->callback == NULL && data->callback_v2 == NULL)) {
        ok = platch_respond_not_implemented((FlutterPlatformMessageResponseHandle *) message->response_handle);
        goto fail_unlock;
    }

    codec = data->codec;
    callback = data->callback;
    callback_v2 = data->callback_v2;
    userdata = data->userdata;

    plugin_registry_unlock(registry);

    if (callback_v2 != NULL) {
        callback_v2(userdata, message);
    } else {
        ok = platch_decode((uint8_t *) message->message, message->message_size, codec, &object);
        if (ok != 0) {
            platch_respond_not_implemented((FlutterPlatformMessageResponseHandle *) message->response_handle);
            goto fail_return_ok;
        }

        ok = callback(
            (char *) message->channel,
            &object,
            (FlutterPlatformMessageResponseHandle *) message->response_handle
        );  //, userdata);
        if (ok != 0) {
            goto fail_free_object;
        }

        platch_free_obj(&object);
    }

    return 0;

fail_free_object:
    platch_free_obj(&object);

fail_unlock:
    plugin_registry_unlock(registry);

fail_return_ok:
    return ok;
}

void plugin_registry_add_plugin_locked(struct plugin_registry *registry, const struct flutterpi_plugin_v2 *plugin) {
    struct plugin_instance *instance;

    instance = malloc(sizeof *instance);
    ASSERT_NOT_NULL(instance);

    instance->plugin = plugin;
    instance->initialized = false;
    instance->userdata = NULL;

    list_addtail(&instance->entry, &registry->plugins);
}

void plugin_registry_add_plugin(struct plugin_registry *registry, const struct flutterpi_plugin_v2 *plugin) {
    plugin_registry_lock(registry);
    plugin_registry_add_plugin_locked(registry, plugin);
    plugin_registry_unlock(registry);
}

static void static_plugin_registry_ensure_initialized();

int plugin_registry_add_plugins_from_static_registry(struct plugin_registry *registry) {
    ASSERTED int ok;

    static_plugin_registry_ensure_initialized();

    ok = pthread_mutex_lock(&static_plugins_lock);
    ASSERT_ZERO(ok);

    list_for_each_entry(struct static_plugin_list_entry, plugin, &static_plugins, entry) {
        plugin_registry_add_plugin(registry, plugin->plugin);
    }

    ok = pthread_mutex_unlock(&static_plugins_lock);
    ASSERT_ZERO(ok);

    return 0;
}

int plugin_registry_ensure_plugins_initialized(struct plugin_registry *registry) {
    enum plugin_init_result result;

    plugin_registry_lock(registry);

    list_for_each_entry(struct plugin_instance, instance, &registry->plugins, entry) {
        if (instance->initialized == false) {
            result = instance->plugin->init(registry->flutterpi, &instance->userdata);
            if (result == PLUGIN_INIT_RESULT_ERROR) {
                LOG_ERROR("Error initializing plugin \"%s\".\n", instance->plugin->name);
                goto fail_deinit_all_initialized;
            } else if (result == PLUGIN_INIT_RESULT_NOT_APPLICABLE) {
                // This is not an error.
                LOG_DEBUG("INFO: Plugin \"%s\" is not available in this flutter-pi instance.\n", instance->plugin->name);
                continue;
            }

            ASSUME(result == PLUGIN_INIT_RESULT_INITIALIZED);
            instance->initialized = true;
        }
    }

    LOG_DEBUG("Initialized plugins: ");
    list_for_each_entry(struct plugin_instance, instance, &registry->plugins, entry) {
        if (instance->initialized) {
            LOG_DEBUG_UNPREFIXED("%s, ", instance->plugin->name);
        }
    }
    LOG_DEBUG_UNPREFIXED("\n");

    plugin_registry_unlock(registry);
    return 0;

fail_deinit_all_initialized:
    list_for_each_entry(struct plugin_instance, instance, &registry->plugins, entry) {
        if (instance->initialized) {
            instance->plugin->deinit(registry->flutterpi, instance->userdata);
            instance->initialized = false;
        }
    }
    plugin_registry_unlock(registry);
    return EINVAL;
}

void plugin_registry_ensure_plugins_deinitialized(struct plugin_registry *registry) {
    plugin_registry_lock(registry);

    list_for_each_entry(struct plugin_instance, instance, &registry->plugins, entry) {
        if (instance->initialized == true) {
            instance->plugin->deinit(registry->flutterpi, instance->userdata);
            instance->initialized = false;
        }
    }

    plugin_registry_unlock(registry);
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
    struct platch_obj_cb_data *data_ptr;
    char *channel_dup;

    ASSERT_MSG((!!callback) != (!!callback_v2), "Exactly one of callback or callback_v2 must be non-NULL.");
    ASSERT_MUTEX_LOCKED(registry->lock);

    data_ptr = get_cb_data_by_channel_locked(registry, channel);
    if (data_ptr == NULL) {
        channel_dup = strdup(channel);
        if (channel_dup == NULL) {
            return ENOMEM;
        }

        struct platch_obj_cb_data *data;

        data = malloc(sizeof *data);
        if (data == NULL) {
            free(channel_dup);
            return ENOMEM;
        }

        data->channel = channel_dup;
        data->codec = codec;
        data->callback = callback;
        data->callback_v2 = callback_v2;
        data->userdata = userdata;

        list_addtail(&data->entry, &registry->callbacks);
    } else {
        data_ptr->codec = codec;
        data_ptr->callback = callback;
        data_ptr->callback_v2 = callback_v2;
        data_ptr->userdata = userdata;
    }

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

    plugin_registry_lock(registry);
    ok = set_receiver_locked(registry, channel, codec, callback, callback_v2, userdata);
    plugin_registry_unlock(registry);

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

int plugin_registry_remove_receiver_v2_locked(struct plugin_registry *registry, const char *channel) {
    struct platch_obj_cb_data *data;

    data = get_cb_data_by_channel_locked(registry, channel);
    if (data == NULL) {
        return EINVAL;
    }

    list_del(&data->entry);
    free(data->channel);
    free(data);

    return 0;
}

int plugin_registry_remove_receiver_v2(struct plugin_registry *registry, const char *channel) {
    int ok;

    plugin_registry_lock(registry);
    ok = plugin_registry_remove_receiver_v2_locked(registry, channel);
    plugin_registry_unlock(registry);

    return ok;
}

int plugin_registry_remove_receiver_locked(const char *channel) {
    struct plugin_registry *registry;

    registry = flutterpi_get_plugin_registry(flutterpi);
    ASSUME(registry != NULL);

    return plugin_registry_remove_receiver_v2_locked(registry, channel);
}

int plugin_registry_remove_receiver(const char *channel) {
    struct plugin_registry *registry;

    registry = flutterpi_get_plugin_registry(flutterpi);
    ASSUME(registry != NULL);

    return plugin_registry_remove_receiver_v2(registry, channel);
}

bool plugin_registry_is_plugin_present_locked(struct plugin_registry *registry, const char *plugin_name) {
    return get_plugin_by_name_locked(registry, plugin_name) != NULL;
}

bool plugin_registry_is_plugin_present(struct plugin_registry *registry, const char *plugin_name) {
    return get_plugin_by_name(registry, plugin_name) != NULL;
}

void *plugin_registry_get_plugin_userdata(struct plugin_registry *registry, const char *plugin_name) {
    struct plugin_instance *instance;

    instance = get_plugin_by_name(registry, plugin_name);

    return instance != NULL ? instance->userdata : NULL;
}

void *plugin_registry_get_plugin_userdata_locked(struct plugin_registry *registry, const char *plugin_name) {
    struct plugin_instance *instance;

    instance = get_plugin_by_name_locked(registry, plugin_name);

    return instance != NULL ? instance->userdata : NULL;
}

static void static_plugin_registry_initialize() {
    ASSERTED int ok;

    list_inithead(&static_plugins);

    ok = pthread_mutex_init(&static_plugins_lock, get_default_mutex_attrs());
    ASSERT_ZERO(ok);
}

static void static_plugin_registry_ensure_initialized() {
    pthread_once(&static_plugins_init_flag, static_plugin_registry_initialize);
}

void static_plugin_registry_add_plugin(const struct flutterpi_plugin_v2 *plugin) {
    struct static_plugin_list_entry *entry;
    ASSERTED int ok;

    static_plugin_registry_ensure_initialized();

    ok = pthread_mutex_lock(&static_plugins_lock);
    ASSERT_ZERO(ok);

    entry = malloc(sizeof *entry);
    ASSERT_NOT_NULL(entry);
    
    entry->plugin = plugin;

    list_addtail(&entry->entry, &static_plugins);

    ok = pthread_mutex_unlock(&static_plugins_lock);
    ASSERT_ZERO(ok);
}

void static_plugin_registry_remove_plugin(const char *plugin_name) {
    ASSERTED int ok;

    static_plugin_registry_ensure_initialized();

    ok = pthread_mutex_lock(&static_plugins_lock);
    ASSERT_ZERO(ok);

    list_for_each_entry(struct static_plugin_list_entry, plugin, &static_plugins, entry) {
        if (streq(plugin->plugin->name, plugin_name)) {
            list_del(&plugin->entry);
            free(plugin);
            break;
        }
    }

    ok = pthread_mutex_unlock(&static_plugins_lock);
    ASSERT_ZERO(ok);
}
