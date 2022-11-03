#ifdef __STDC_ALLOC_LIB__
#define __STDC_WANT_LIB_EXT2__ 1
#else
#define _POSIX_C_SOURCE 200809L
#endif

#include <unistd.h>
#include <string.h>
#include <alloca.h>
#include <sys/select.h>
#include <stdatomic.h>

#include <platformchannel.h>
#include <pluginregistry.h>
#include <collection.h>
#include <flutter-pi.h>

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
	void *userdata;
};

struct plugin_registry {
	struct flutterpi *flutterpi;
	struct concurrent_pointer_set plugins;
	struct concurrent_pointer_set callbacks;
};

static struct concurrent_pointer_set static_plugins = CPSET_INITIALIZER(CPSET_DEFAULT_MAX_SIZE);

static struct plugin_instance *get_plugin_by_name_locked(
	struct plugin_registry *registry,
	const char *plugin_name
) {
	struct plugin_instance *instance;

	for_each_pointer_in_cpset(&registry->plugins, instance) {
		if (strcmp(instance->plugin->name, plugin_name) == 0) {
			break;
		}
	}

	return instance;
}

static struct plugin_instance *get_plugin_by_name(
	struct plugin_registry *registry,
	const char *plugin_name
) {
	struct plugin_instance *instance;

	cpset_lock(&registry->plugins);

	instance = get_plugin_by_name_locked(registry, plugin_name);

	cpset_unlock(&registry->plugins);

	return instance;
}

static struct platch_obj_cb_data *get_cb_data_by_channel_locked(
	struct plugin_registry *registry,
	const char *channel
) {
	struct platch_obj_cb_data *data;

	for_each_pointer_in_cpset(&registry->callbacks, data) {
		if (strcmp(data->channel, channel) == 0) {
			break;
		}
	}

	return data;
}

MAYBE_UNUSED static struct platch_obj_cb_data *get_cb_data_by_channel(
	struct plugin_registry *registry,
	const char *channel
) {
	struct platch_obj_cb_data *data;

	cpset_lock(&registry->callbacks);
	data = get_cb_data_by_channel_locked(registry, channel);
	cpset_unlock(&registry->callbacks);

	return data;
}

struct plugin_registry *plugin_registry_new(struct flutterpi *flutterpi) {
	struct plugin_registry *reg;
	int ok;

	reg = malloc(sizeof *reg);
	if (reg == NULL) {
		return NULL;
	}

	ok = cpset_init(&reg->plugins, CPSET_DEFAULT_MAX_SIZE);
	if (ok != 0) {
		goto fail_free_registry;
	}

	ok = cpset_init(&reg->callbacks, CPSET_DEFAULT_MAX_SIZE);
	if (ok != 0) {
		goto fail_deinit_plugins_cpset;
	}

	reg->flutterpi = flutterpi;
	return reg;


	fail_deinit_plugins_cpset:
	cpset_deinit(&reg->plugins);
	
	fail_free_registry:
	free(reg);

	return NULL;
}

void plugin_registry_destroy(struct plugin_registry *registry) {
	struct plugin_instance *instance;

	plugin_registry_ensure_plugins_deinitialized(registry);
	for_each_pointer_in_cpset(&registry->plugins, instance) {
		cpset_remove_locked(&registry->plugins, instance);
		free(instance);
		instance = NULL;
	}
	cpset_deinit(&registry->callbacks);
	cpset_deinit(&registry->plugins);
	free(registry);
}

int plugin_registry_on_platform_message(struct plugin_registry *registry, FlutterPlatformMessage *message) {
	struct platch_obj_cb_data *data, data_copy;
	struct platch_obj object;
	int ok;

	cpset_lock(&registry->callbacks);

	data = get_cb_data_by_channel_locked(registry, message->channel);
	if (data == NULL || data->callback == NULL) {
		cpset_unlock(&registry->callbacks);
		return platch_respond_not_implemented((FlutterPlatformMessageResponseHandle*) message->response_handle);
	}

	data_copy = *data;
	cpset_unlock(&registry->callbacks);

	ok = platch_decode((uint8_t*) message->message, message->message_size, data_copy.codec, &object);
	if (ok != 0) {
		return ok;
	}

	ok = data_copy.callback((char*) message->channel, &object, (FlutterPlatformMessageResponseHandle*) message->response_handle); //, data->userdata);
	if (ok != 0) {
		platch_free_obj(&object);
		return ok;
	}

	platch_free_obj(&object);

	return 0;
}

int plugin_registry_add_plugin(struct plugin_registry *registry, const struct flutterpi_plugin_v2 *plugin) {
	struct plugin_instance *instance;
	int ok;

	instance = malloc(sizeof *instance);
	if (instance == NULL) {
		return ENOMEM;
	}

	instance->plugin = plugin;
	instance->initialized = false;
	instance->userdata = NULL;

	ok = cpset_put(&registry->plugins, instance);
	if (ok != 0) {
		free(instance);
		return ENOMEM;
	}

	return 0;
}

int plugin_registry_add_plugins_from_static_registry(struct plugin_registry *registry) {
	const struct flutterpi_plugin_v2 *plugin;
	int ok;

	cpset_lock(&static_plugins);
	for_each_pointer_in_cpset(&static_plugins, plugin) {
		ok = plugin_registry_add_plugin(registry, plugin);
		if (ok != 0) {
			cpset_unlock(&static_plugins);

			/// TODO: Remove all previously added plugins here
			return ok;
		}
	}
	cpset_unlock(&static_plugins);

	return 0;
}

int plugin_registry_ensure_plugins_initialized(struct plugin_registry *registry) {
	struct plugin_instance *instance;
	enum plugin_init_result result;
	struct pointer_set initialized_plugins;
	void **initialized_plugins_storage;
	int n_pointers;

	cpset_lock(&registry->plugins);

	n_pointers = cpset_get_count_pointers_locked(&registry->plugins);

	initialized_plugins_storage = alloca(sizeof(*initialized_plugins_storage) * n_pointers);
	memset(initialized_plugins_storage, 0, sizeof(*initialized_plugins_storage) * n_pointers);
	initialized_plugins = PSET_INITIALIZER_STATIC(initialized_plugins_storage, n_pointers);

	LOG_DEBUG("Registered plugins: ");
	for_each_pointer_in_cpset(&registry->plugins, instance) {
		LOG_DEBUG_UNPREFIXED("%s, ", instance->plugin->name);

		if (instance->initialized == false) {
			result = instance->plugin->init(registry->flutterpi, &instance->userdata);
			if (result == kError_PluginInitResult) {
				LOG_ERROR("Error initializing plugin \"%s\".\n", instance->plugin->name);
				goto fail_deinit_all_initialized;
			} else if (result == kNotApplicable_PluginInitResult) {
				// This is not an error.
				LOG_DEBUG("INFO: Plugin \"%s\" is not available in this flutter-pi instance.\n", instance->plugin->name);
			}

			instance->initialized = true;
			pset_put(&initialized_plugins, instance);
		}
	}
	LOG_DEBUG_UNPREFIXED("\n");

	cpset_unlock(&registry->plugins);
	return 0;


	fail_deinit_all_initialized:
	for_each_pointer_in_pset(&initialized_plugins, instance) {
		instance->plugin->deinit(registry->flutterpi, instance->userdata);
	}
	return EINVAL;
}

void plugin_registry_ensure_plugins_deinitialized(struct plugin_registry *registry) {
	struct plugin_instance *instance;

	cpset_lock(&registry->plugins);

	for_each_pointer_in_cpset(&registry->plugins, instance) {
		if (instance->initialized == true) {
			instance->plugin->deinit(registry->flutterpi, instance->userdata);
			instance->initialized = false;
		}
	}

	cpset_unlock(&registry->plugins);
}

/// TODO: Move this into a separate flutter messenger API
int plugin_registry_set_receiver(
	const char *channel,
	enum platch_codec codec,
	platch_obj_recv_callback callback
) {
	struct plugin_registry *registry;
	struct platch_obj_cb_data *data;
	char *channel_dup;
	int ok;

	registry = flutterpi_get_plugin_registry(flutterpi);

	cpset_lock(&registry->callbacks);
	
	channel_dup = strdup(channel);
	if (channel_dup == NULL) {
		ok = ENOMEM;
		goto fail_unlock_cbs;
	}

	data = get_cb_data_by_channel_locked(registry, channel);
	if (data == NULL) {
		data = calloc(1, sizeof *data);
		if (data == NULL) {
			ok = ENOMEM;
			goto fail_free_channel_dup;
		}

		ok = cpset_put_locked(&registry->callbacks, data);
		if (ok != 0) {
			if (ok == ENOSPC) {
				LOG_ERROR("Couldn't register platform channel listener. Callback list is filled\n");
			}
			goto fail_free_data;
		}
	}

	data->channel = channel_dup;
	data->codec = codec;
	data->callback = callback;
	data->userdata = NULL;
	cpset_unlock(&registry->callbacks);
	
	return 0;
	

	fail_free_data:
	free(data);
	
	fail_free_channel_dup:
	free(channel_dup);

	fail_unlock_cbs:
	cpset_unlock(&registry->callbacks);

	return ok;
}

int plugin_registry_remove_receiver(const char *channel) {
	struct plugin_registry *registry;
	struct platch_obj_cb_data *data;

	registry = flutterpi_get_plugin_registry(flutterpi);

	cpset_lock(&registry->callbacks);

	data = get_cb_data_by_channel_locked(registry, channel);
	if (data == NULL) {
		cpset_unlock(&registry->callbacks);
		return EINVAL;
	}

	cpset_remove_locked(&registry->callbacks, data);

	free(data->channel);
	free(data);

	cpset_unlock(&registry->callbacks);

	return 0;
}

bool plugin_registry_is_plugin_present(
	struct plugin_registry *registry,
	const char *plugin_name
) {
	return get_plugin_by_name(registry, plugin_name) != NULL;
}

void *plugin_registry_get_plugin_userdata(
	struct plugin_registry *registry,
	const char *plugin_name
) {
	struct plugin_instance *instance;

	instance = get_plugin_by_name(registry, plugin_name);

	return instance != NULL ? instance->userdata : NULL;
}


int static_plugin_registry_add_plugin(const struct flutterpi_plugin_v2 *plugin) {
	return cpset_put(&static_plugins, (void*) plugin);
}

int static_plugin_registry_remove_plugin(const char *plugin_name) {
	const struct flutterpi_plugin_v2 *plugin;

	cpset_lock(&static_plugins);

	for_each_pointer_in_cpset(&static_plugins, plugin) {
		if (strcmp(plugin->name, plugin_name) == 0) {
			break;
		}
	}

	if (plugin != NULL) {
		cpset_remove_locked(&static_plugins, plugin);
	}

	cpset_unlock(&static_plugins);

	return plugin == NULL ? EINVAL : 0;
}
