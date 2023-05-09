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
#include "util/dynarray.h"

FILE_DESCR("plugin registry")

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
    const struct flutterpi_plugin_v2 *plugin;
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

struct plugin_registry {
    pthread_mutex_t lock;
    struct flutterpi *flutterpi;
    struct util_dynarray plugins;
    struct util_dynarray callbacks;
};

DEFINE_STATIC_LOCK_OPS(plugin_registry, lock)

static pthread_mutex_t static_plugins_lock = PTHREAD_MUTEX_INITIALIZER;
static struct util_dynarray static_plugins = { 0 };

static struct plugin_instance *get_plugin_by_name_locked(struct plugin_registry *registry, const char *plugin_name) {
    util_dynarray_foreach(&registry->plugins, struct plugin_instance, instance) {
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
    util_dynarray_foreach(&registry->callbacks, struct platch_obj_cb_data, data) {
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

    util_dynarray_init(&reg->plugins);
    util_dynarray_init(&reg->callbacks);

    reg->flutterpi = flutterpi;
    return reg;

    return NULL;
}

void plugin_registry_destroy(struct plugin_registry *registry) {
    plugin_registry_ensure_plugins_deinitialized(registry);
    util_dynarray_fini(&registry->plugins);
    util_dynarray_fini(&registry->callbacks);
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
    if (data == NULL || data->callback == NULL) {
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
    struct plugin_instance instance = {
        .plugin = plugin,
        .initialized = false,
        .userdata = NULL,
    };

    util_dynarray_append(&registry->plugins, struct plugin_instance, instance);
}

void plugin_registry_add_plugin(struct plugin_registry *registry, const struct flutterpi_plugin_v2 *plugin) {
    plugin_registry_lock(registry);
    plugin_registry_add_plugin_locked(registry, plugin);
    plugin_registry_unlock(registry);
}

int plugin_registry_add_plugins_from_static_registry(struct plugin_registry *registry) {
    ASSERTED int ok;

    ok = pthread_mutex_lock(&static_plugins_lock);
    ASSERT_ZERO(ok);

    util_dynarray_foreach(&static_plugins, struct flutterpi_plugin_v2, plugin) {
        plugin_registry_add_plugin(registry, plugin);
    }

    ok = pthread_mutex_unlock(&static_plugins_lock);
    ASSERT_ZERO(ok);

    return 0;
}

int plugin_registry_ensure_plugins_initialized(struct plugin_registry *registry) {
    enum plugin_init_result result;

    plugin_registry_lock(registry);

    util_dynarray_foreach(&registry->plugins, struct plugin_instance, instance) {
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
    util_dynarray_foreach(&registry->plugins, struct plugin_instance, instance) {
        if (instance->initialized) {
            LOG_DEBUG_UNPREFIXED("%s, ", instance->plugin->name);
        }
    }
    LOG_DEBUG_UNPREFIXED("\n");

    plugin_registry_unlock(registry);
    return 0;

fail_deinit_all_initialized:
    util_dynarray_foreach(&registry->plugins, struct plugin_instance, instance) {
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

    util_dynarray_foreach(&registry->plugins, struct plugin_instance, instance) {
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

        struct platch_obj_cb_data data = {
            .channel = channel_dup,
            .codec = codec,
            .callback = callback,
            .callback_v2 = callback_v2,
            .userdata = userdata,
        };

        util_dynarray_append(&registry->callbacks, struct platch_obj_cb_data, data);
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

static bool cb_data_channel_equals(struct platch_obj_cb_data cbdata, const char *channel) {
    return streq(cbdata.channel, channel);
}

int plugin_registry_remove_receiver_v2_locked(struct plugin_registry *registry, const char *channel) {
    struct platch_obj_cb_data *data;

    data = get_cb_data_by_channel_locked(registry, channel);
    if (data == NULL) {
        return EINVAL;
    }

    free(data->channel);

    util_dynarray_delete_single_where_unordered(&registry->callbacks, struct platch_obj_cb_data, cb_data_channel_equals, channel);

    return 0;
}

int plugin_registry_remove_receiver_v2(struct plugin_registry *registry, const char *channel) {
    int ok;

    plugin_registry_lock(registry);
    ok = plugin_registry_remove_receiver_v2_locked(registry, channel);
    plugin_registry_unlock(registry);

    return ok;
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

void static_plugin_registry_add_plugin(const struct flutterpi_plugin_v2 *plugin) {
    ASSERTED int ok;

    ok = pthread_mutex_lock(&static_plugins_lock);
    ASSERT_ZERO(ok);

    util_dynarray_append(&static_plugins, struct flutterpi_plugin_v2, *plugin);

    ok = pthread_mutex_unlock(&static_plugins_lock);
    ASSERT_ZERO(ok);
}

static bool static_plugin_name_equals(const struct flutterpi_plugin_v2 plugin, const char *plugin_name) {
    return streq(plugin.name, plugin_name);
}

void static_plugin_registry_remove_plugin(const char *plugin_name) {
    ASSERTED int ok;

    ok = pthread_mutex_lock(&static_plugins_lock);
    ASSERT_ZERO(ok);

    util_dynarray_delete_single_where_unordered(&static_plugins, struct flutterpi_plugin_v2, static_plugin_name_equals, plugin_name);

    ok = pthread_mutex_unlock(&static_plugins_lock);
    ASSERT_ZERO(ok);
}
