#ifdef __STDC_ALLOC_LIB__
#define __STDC_WANT_LIB_EXT2__ 1
#else
#define _POSIX_C_SOURCE 200809L
#endif

#include <unistd.h>
#include <string.h>
#include <sys/select.h>

#include <platformchannel.h>
#include <pluginregistry.h>
#include <collection.h>

#include <plugins/services.h>
#include <plugins/raw_keyboard.h>

#ifdef BUILD_TEXT_INPUT_PLUGIN
#	include <plugins/text_input.h>
#endif
#ifdef BUILD_TEST_PLUGIN
#	include <plugins/testplugin.h>
#endif
#ifdef BUILD_GPIOD_PLUGIN
#	include <plugins/gpiod_plugin.h>
#endif
#ifdef BUILD_SPIDEV_PLUGIN
#	include <plugins/spidev.h>
#endif
#ifdef BUILD_OMXPLAYER_VIDEO_PLAYER_PLUGIN
#	include <plugins/omxplayer_video_player.h>
#endif
#ifdef BUILD_ANDROID_AUTO_PLUGIN
#	include <plugins/android_auto/android_auto.h>
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
struct flutterpi_plugin {
    char *name;
	init_deinit_cb init;
	init_deinit_cb deinit;
	void *userdata;
	bool initialized;
};

/**
 * @brief array of plugins statically included in flutter-pi.
 */
struct flutterpi_plugin hardcoded_plugins[] = {
	{.name = "services",     .init = services_init, .deinit = services_deinit},
	{.name = "raw_keyboard", .init = rawkb_init, .deinit = rawkb_deinit},

#ifdef BUILD_TEXT_INPUT_PLUGIN
	{.name = "text_input",   .init = textin_init, .deinit = textin_deinit},
#endif

#ifdef BUILD_TEST_PLUGIN
	{.name = "testplugin",   .init = testp_init, .deinit = testp_deinit},
#endif

#ifdef BUILD_OMXPLAYER_VIDEO_PLAYER_PLUGIN
	{.name = "omxplayer_video_player", .init = omxpvidpp_init, .deinit = omxpvidpp_deinit},
#endif

#ifdef BUILD_ANDROID_AUTO_PLUGIN
	{.name = "android_auto", .init = aaplugin_init, .deinit = aaplugin_deinit}
#endif
};

struct platch_obj_callback_data {
	char *channel;
	enum platch_codec codec;
	platch_obj_recv_callback callback;
	void *userdata;
	int n_refs;
};

struct plugin_registry {
	//size_t n_plugins;
	//struct flutterpi_plugin *plugins;

	struct flutterpi *flutterpi;
	
	struct concurrent_pointer_set plugins;

	struct concurrent_pointer_set callbacks;

	pthread_mutex_t msg_handling_thread_lock;
	pthread_t msg_handling_thread;
} plugin_registry;


static struct platch_obj_callback_data *get_callback_data_by_channel_name_locked(const char *channel) {
	struct platch_obj_callback_data *data;

	for_each_pointer_in_cpset(&plugin_registry.callbacks, data) {
		if (strcmp(data->channel, channel) == 0) {
			data->n_refs++;
			return data;
		}
	}

	return NULL;
}

static struct platch_obj_callback_data *get_callback_data_by_channel_name(const char *channel) {
	struct platch_obj_callback_data *data;

	cpset_lock(&plugin_registry.callbacks);
	data = get_callback_data_by_channel_name_locked(channel);
	data->n_refs++;
	cpset_unlock(&plugin_registry.callbacks);

	return data;
}


struct plugin_registry *plugin_registry_new(struct flutterpi *flutterpi) {
	struct flutterpi_plugin *plugin;
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

	for (unsigned int i = 0; i < (sizeof(hardcoded_plugins) / sizeof(*hardcoded_plugins)); i++) {
		ok = plugin_registry_add_plugin(
			reg,
			hardcoded_plugins[i].name,
			hardcoded_plugins[i].init,
			hardcoded_plugins[i].deinit
		);
		if (ok != 0) {
			goto fail_free_added_plugins;
		}
	}

	return reg;

	fail_free_added_plugins:
	for_each_pointer_in_cpset(&reg->plugins, plugin) {
		free(plugin->name);
		free(plugin);
	}

	pthread_mutex_destroy(&reg->msg_handling_thread_lock);

	fail_deinit_callbacks_cpset:
	cpset_deinit(&reg->callbacks);

	fail_deinit_plugins_cpset:
	cpset_deinit(&reg->plugins);
	
	fail_free_registry:
	free(reg);

	return NULL;
}

void plugin_registry_destroy(struct plugin_registry *registry) {
	struct flutterpi_plugin *plugin;

	plugin_registry_ensure_plugins_deinitialized(registry);
	
	for_each_pointer_in_cpset(&registry->plugins, plugin) {
		free(plugin->name);
		free(plugin);
	}

	pthread_mutex_destroy(&registry->msg_handling_thread_lock);
	cpset_deinit(&registry->callbacks);
	cpset_deinit(&registry->plugins);
	free(registry);
}

int  plugin_registry_add_plugin(struct plugin_registry *registry, const char *name, init_deinit_cb on_init, init_deinit_cb on_deinit) {
	struct flutterpi_plugin *plugin;
	int ok;

	plugin = malloc(sizeof *plugin);
	if (plugin == NULL) {
		return ENOMEM;
	}

	plugin->name = strdup(name);
	if (plugin->name == NULL) {
		free(plugin);
		return ENOMEM;
	}

	plugin->initialized = false;
	plugin->init = on_init;
	plugin->deinit = on_deinit;

	ok = cpset_put(&registry->plugins, plugin);
	if (ok != 0) {
		free(plugin->name);
		free(plugin);
		return ok;
	}

	return 0;
}

int  plugin_registry_ensure_plugins_initialized(struct plugin_registry *registry) {
	struct flutterpi_plugin *plugin;
	int ok;

	cpset_lock(&registry->plugins);

	for_each_pointer_in_cpset(&registry->plugins, plugin) {
		if (plugin->initialized == false) {
			ok = plugin->init(registry->flutterpi, &plugin->userdata);
			if (ok != 0) {
				fprintf(stderr, "[flutter-pi] Could not initialize plugin \"%s\": %s\n", plugin->name, strerror(ok));
				// we don't return here, but we also don't set initialized to true.
				continue;
			}

			plugin->initialized = true;
		}
	}

	cpset_unlock(&registry->plugins);

	return 0;
}

void plugin_registry_ensure_plugins_deinitialized(struct plugin_registry *registry) {
	struct flutterpi_plugin *plugin;
	int ok;

	cpset_lock(&registry->plugins);

	for_each_pointer_in_cpset(&registry->plugins, plugin) {
		if (plugin->initialized == true) {
			plugin->deinit(registry->flutterpi, &plugin->userdata);
			plugin->initialized = false;
		}
	}

	cpset_unlock(&registry->plugins);
}

/*
int plugin_registry_on_platform_message(
	struct plugin_registry *registry,
	const char *channel,
	const uint8_t *message,
	size_t message_size,
	const struct platform_message_response_handle *response_handle
) {
	struct platch_obj_callback_data *data;
	struct platch_obj object;
	int ok;

	cpset_lock(&registry->callbacks);

	pthread_mutex_lock(&registry->msg_handling_thread_lock);
	registry->msg_handling_thread = pthread_self();
	pthread_mutex_unlock(&registry->msg_handling_thread_lock);

	data = get_callback_data_by_channel_name_locked(channel);
	if (data == NULL) {
		platch_respond_not_implemented((FlutterPlatformMessageResponseHandle*) response_handle);
		ok = 0;
		goto fail_unlock_callbacks;
	}

	ok = platch_decode((uint8_t*) message, message_size, data->codec, &object);
	if (ok != 0) {
		goto fail_unlock_callbacks;
	}
	
	ok = data->callback(
		(char*) channel,
		&object,
		response_handle,
		data->userdata
	);
	if (ok != 0) {
		fprintf(stderr, "[plugin registry] Error executing platform message callback: %s\n", strerror(ok));
	}

	platch_free_obj(&object);

	pthread_mutex_lock(&registry->msg_handling_thread_lock);
	registry->msg_handling_thread = NULL;
	pthread_mutex_unlock(&registry->msg_handling_thread_lock);

	cpset_unlock(&registry->callbacks);

	return ok;


	fail_reset_msg_handling_thread:
	pthread_mutex_lock(&registry->msg_handling_thread_lock);
	registry->msg_handling_thread = NULL;
	pthread_mutex_unlock(&registry->msg_handling_thread_lock);

	fail_unlock_callbacks:
	cpset_unlock(&registry->callbacks);

	fail_return_ok:
	return ok;
}


int plugin_registry_set_receiver(
	struct plugin_registry *registry,
	const char *channel,
	enum platch_codec codec,
	platch_obj_recv_callback callback,
	void *userdata
) {
	struct platch_obj_callback_data *data;
	pthread_t msg_handling_thread;
	char *channel_dup;
	int ok;

	pthread_mutex_lock(&registry->msg_handling_thread_lock);
	msg_handling_thread = registry->msg_handling_thread;
	pthread_mutex_unlock(&registry->msg_handling_thread_lock);

	if (msg_handling_thread != pthread_self()) {
		cpset_lock(&registry->callbacks);
	}

	data = get_callback_data_by_channel_name_locked(channel);
	if (data == NULL) {
		data = malloc(sizeof *data);
		if (data == NULL) {
			ok = ENOMEM;
			goto fail_maybe_unlock;
		}

		channel_dup = strdup(channel);
		if (channel_dup == NULL) {
			ok = ENOMEM;
			goto fail_free_data;
		}

		data->n_refs  = 1;
		data->channel = channel_dup;

		ok = cpset_put_locked(&registry->callbacks, data);
		if (ok != 0) {
			goto fail_free_channel_dup;
		}
	}

	data->codec    = codec;
	data->callback = callback;
	data->userdata = userdata;

	if (msg_handling_thread != pthread_self()) {
		cpset_unlock(&registry->callbacks);
	}

	return 0;


	fail_free_channel_dup:
	free(channel_dup);

	fail_free_data:
	free(data);

	fail_maybe_unlock:
	if (msg_handling_thread != pthread_self()) {
		cpset_unlock(&registry->callbacks);
	}

	return ok;
}

int plugin_registry_remove_receiver(
	struct plugin_registry *registry,
	const char *channel
) {
	struct platch_obj_callback_data *data;
	pthread_t msg_handling_thread;
	int ok;

	pthread_mutex_lock(&registry->msg_handling_thread_lock);
	msg_handling_thread = registry->msg_handling_thread;
	pthread_mutex_unlock(&registry->msg_handling_thread_lock);

	if (msg_handling_thread != pthread_self()) {
		cpset_lock(&registry->callbacks);
	}

	data = get_callback_data_by_channel_name_locked(channel);
	if (data == NULL) {
		ok = EINVAL;
		goto fail_maybe_unlock;
	}

	cpset_remove_locked(&registry->callbacks, data);

	free(data->channel);
	free(data);

	if (msg_handling_thread != pthread_self()) {
		cpset_unlock(&registry->callbacks);
	}

	return 0;


	fail_maybe_unlock:
	if (msg_handling_thread != pthread_self()) {
		cpset_unlock(&registry->callbacks);
	}

	return ok;
}
*/

bool plugin_registry_is_plugin_present(
	struct plugin_registry *registry,
	const char *plugin_name
) {
	struct flutterpi_plugin *plugin;

	cpset_lock(&registry->plugins);

	for_each_pointer_in_cpset(&registry->plugins, plugin) {
		if (strcmp(plugin->name, plugin_name) == 0) {
			break;
		}
	}

	cpset_unlock(&registry->plugins);

	if (plugin) {
		return true;
	} else {
		return false;
	}
}

