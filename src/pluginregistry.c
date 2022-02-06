#ifdef __STDC_ALLOC_LIB__
#define __STDC_WANT_LIB_EXT2__ 1
#else
#define _POSIX_C_SOURCE 200809L
#endif

#include <unistd.h>
#include <string.h>
#include <alloca.h>
#include <sys/select.h>

#include <platformchannel.h>
#include <pluginregistry.h>
#include <collection.h>

#define LOG_ERROR(...) fprintf(stderr, "[plugin registry] " __VA_ARGS__)
#ifdef DEBUG
#	define LOG_DEBUG(...) fprintf(stderr, "[plugin registry] " __VA_ARGS__)
#else
#	define LOG_DEBUG(...) do { } while (0)
#endif

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

struct plugin_registry {
	struct flutterpi *flutterpi;
	struct concurrent_pointer_set plugins;
	struct concurrent_pointer_set callbacks;
	pthread_mutex_t msg_handling_thread_lock;
	pthread_t msg_handling_thread;
} plugin_registry;

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

	ok = pthread_mutex_init(&reg->msg_handling_thread_lock, NULL);
	if (ok != 0) {
		goto fail_deinit_callbacks_cpset;
	}

	reg->flutterpi = flutterpi;

	return reg;

	fail_deinit_callbacks_cpset:
	cpset_deinit(&reg->callbacks);

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
		free(instance);
	}
	pthread_mutex_destroy(&registry->msg_handling_thread_lock);
	cpset_deinit(&registry->callbacks);
	cpset_deinit(&registry->plugins);
	free(registry);
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
	int n_pointers;

	cpset_lock(&registry->plugins);

	n_pointers = cpset_get_count_pointers_locked(&registry->plugins);
	initialized_plugins = PSET_INITIALIZER_STATIC(alloca(sizeof(void*) * n_pointers), n_pointers);

	for_each_pointer_in_cpset(&registry->plugins, instance) {
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
