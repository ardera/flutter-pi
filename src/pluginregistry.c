#include <sys/errno.h>
#include <string.h>

#include <platformchannel.h>
#include <pluginregistry.h>

// hardcoded plugin headers
#include "plugins/services-plugin.h"
#include "plugins/testplugin.h"
#include "plugins/elm327plugin.h"


struct ChannelObjectReceiverData {
	char *channel;
	enum ChannelCodec codec;
	ChannelObjectReceiveCallback callback;
};
struct {
	struct FlutterPiPlugin *plugins;
	size_t plugin_count;
	struct ChannelObjectReceiverData *callbacks;
	size_t callbacks_size;
} pluginregistry;

/// array of plugins that are statically included in flutter-pi.
struct FlutterPiPlugin hardcoded_plugins[] = {
	{.name = "services",     .init = Services_init, .deinit = Services_deinit},

#ifdef INCLUDE_TESTPLUGIN	
	{.name = "testplugin",   .init = TestPlugin_init, .deinit = TestPlugin_deinit}
#endif

#ifdef INCLUDE_ELM327PLUGIN
	{.name = "elm327",       .init = ELM327Plugin_init, .deinit = ELM327Plugin_deinit}
#endif
};
//size_t hardcoded_plugins_count;


int PluginRegistry_init() {
	int ok;

	memset(&pluginregistry, 0, sizeof(pluginregistry));

	pluginregistry.callbacks_size = 20;
	pluginregistry.callbacks = calloc(pluginregistry.callbacks_size, sizeof(struct ChannelObjectReceiverData));
	
	pluginregistry.plugins = hardcoded_plugins;
	pluginregistry.plugin_count = sizeof(hardcoded_plugins) / sizeof(struct FlutterPiPlugin);

	// insert code for dynamically loading plugins here

	// call all the init methods for all plugins
	for (int i = 0; i < pluginregistry.plugin_count; i++) {
		if (pluginregistry.plugins[i].init) {
			ok = pluginregistry.plugins[i].init();
			if (ok != 0) return ok;
		}
	}

	return 0;
}
int PluginRegistry_onPlatformMessage(FlutterPlatformMessage *message) {
	struct ChannelObject object;
	int ok;

	for (int i = 0; i < pluginregistry.callbacks_size; i++) {
		if ((pluginregistry.callbacks[i].callback) && (strcmp(pluginregistry.callbacks[i].channel, message->channel) == 0)) {
			ok = PlatformChannel_decode((uint8_t*) message->message, message->message_size, pluginregistry.callbacks[i].codec, &object);
			if (ok != 0) return ok;

			pluginregistry.callbacks[i].callback((char*) message->channel, &object, (FlutterPlatformMessageResponseHandle*) message->response_handle);

			PlatformChannel_free(&object);
			return 0;
		}
	}

	// we didn't find a callback for the specified channel.
	// just respond with a null buffer to tell the VM-side
	// that the feature is not implemented.

	return PlatformChannel_respondNotImplemented((FlutterPlatformMessageResponseHandle *) message->response_handle);
}
int PluginRegistry_setReceiver(char *channel, enum ChannelCodec codec, ChannelObjectReceiveCallback callback) {
	/// the index in 'callback' of the ChannelObjectReceiverData that will be added / updated.
	int index = -1;

	/// find the index with channel name 'channel', or else, the first unoccupied index.
	for (int i = 0; i < pluginregistry.callbacks_size; i++) {
		if (pluginregistry.callbacks[i].channel == NULL) {
			if (index == -1) {
				index = i;
			}
		} else if (strcmp(channel, pluginregistry.callbacks[i].channel) == 0) {
			index = i;
			break;
		}
	}
	
	/// no matching or unoccupied index found.
	if (index == -1) {
		if (!callback) return 0;
		
		/// expand array
		size_t currentsize = pluginregistry.callbacks_size * sizeof(struct ChannelObjectReceiverData);
		
		pluginregistry.callbacks = realloc(pluginregistry.callbacks, 2 * currentsize);
		memset(&pluginregistry.callbacks[pluginregistry.callbacks_size], currentsize, 0);

		index = pluginregistry.callbacks_size;
		pluginregistry.callbacks_size = 2*pluginregistry.callbacks_size;
	}

	if (callback) {
		char *channelCopy = malloc(strlen(channel) +1);
		if (!channelCopy) return ENOMEM;
		strcpy(channelCopy, channel);

		pluginregistry.callbacks[index].channel = channelCopy;
		pluginregistry.callbacks[index].codec = codec;
		pluginregistry.callbacks[index].callback = callback;
	} else if (pluginregistry.callbacks[index].callback) {
		free(pluginregistry.callbacks[index].channel);
		pluginregistry.callbacks[index].channel = NULL;
		pluginregistry.callbacks[index].callback = NULL;
	}

	return 0;
	
}
int PluginRegistry_deinit() {
	int i, ok;
	
	/// call each plugins 'deinit'
	for (i = 0; i < pluginregistry.plugin_count; i++) {
		if (pluginregistry.plugins[i].deinit) {
			ok = pluginregistry.plugins[i].deinit();
			if (ok != 0) return ok;
		}
	}

	/// free all the channel names from the callback list.
	for (int i=0; i < pluginregistry.callbacks_size; i++) {
		if (pluginregistry.callbacks[i].channel)
			free(pluginregistry.callbacks[i].channel);
	}

	/// free the rest
	free(pluginregistry.callbacks);
}
