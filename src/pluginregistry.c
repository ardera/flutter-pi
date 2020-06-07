#include <unistd.h>
#include <string.h>
#include <sys/select.h>

#include <platformchannel.h>
#include <pluginregistry.h>

#include <plugins/services.h>

#include <plugins/raw_keyboard.h>

#ifdef BUILD_TEXT_INPUT_PLUGIN
#	include <plugins/text_input.h>
#endif
#ifdef BUILD_TEST_PLUGIN
#	include <plugins/testplugin.h>
#endif
#ifdef BUILD_ELM327_PLUGIN
#	include <plugins/elm327plugin.h>
#endif
#ifdef BUILD_GPIOD_PLUGIN
#	include <plugins/gpiod_plugin.h>
#endif
#ifdef BUILD_SPIDEV_PLUGIN
#	include <plugins/spidev.h>
#endif
#ifdef BUILD_OMXPLAYER_VIDEO_PLAYER_PLUGIN
#	include <plugins/video_player.h>
#endif


struct platch_obj_recv_data {
	char *channel;
	enum platch_codec codec;
	platch_obj_recv_callback callback;
};
struct {
	struct flutterpi_plugin *plugins;
	size_t plugin_count;

	// platch_obj callbacks
	struct platch_obj_recv_data *platch_obj_cbs;
	size_t platch_obj_cbs_size;

} pluginregistry;

/// array of plugins that are statically included in flutter-pi.
struct flutterpi_plugin hardcoded_plugins[] = {
	{.name = "services",     .init = services_init, .deinit = services_deinit},
	{.name = "raw_keyboard", .init = rawkb_init, .deinit = rawkb_deinit},

#ifdef BUILD_TEXT_INPUT_PLUGIN
	{.name = "text_input",   .init = textin_init, .deinit = textin_deinit},
#endif

#ifdef BUILD_TEST_PLUGIN
	{.name = "testplugin",   .init = testp_init, .deinit = testp_deinit},
#endif

#ifdef BUILD_ELM327_PLUGIN
	{.name = "elm327plugin", .init = ELM327Plugin_init, .deinit = ELM327Plugin_deinit},
#endif

#ifdef BUILD_GPIOD_PLUGIN
	{.name = "flutter_gpiod",  .init = gpiodp_init, .deinit = gpiodp_deinit},
#endif

#ifdef BUILD_SPIDEV_PLUGIN
	{.name = "flutter_spidev", .init = spidevp_init, .deinit = spidevp_deinit},
#endif

#ifdef BUILD_OMXPLAYER_VIDEO_PLAYER_PLUGIN
	{.name = "omxplayer_video_player", .init = omxpvidpp_init, .deinit = omxpvidpp_deinit},
#endif
};


int plugin_registry_init() {
	int ok;

	memset(&pluginregistry, 0, sizeof(pluginregistry));

	pluginregistry.platch_obj_cbs_size = 20;
	pluginregistry.platch_obj_cbs = calloc(pluginregistry.platch_obj_cbs_size, sizeof(struct platch_obj_recv_data));

	if (!pluginregistry.platch_obj_cbs) {
		fprintf(stderr, "[plugin-registry] Could not allocate memory for platform channel message callbacks.\n");
		return ENOMEM;
	}
	
	pluginregistry.plugins = hardcoded_plugins;
	pluginregistry.plugin_count = sizeof(hardcoded_plugins) / sizeof(struct flutterpi_plugin);

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
int plugin_registry_on_platform_message(FlutterPlatformMessage *message) {
	struct platch_obj object;
	int ok;

	for (int i = 0; i < pluginregistry.platch_obj_cbs_size; i++) {
		if ((pluginregistry.platch_obj_cbs[i].callback) && (strcmp(pluginregistry.platch_obj_cbs[i].channel, message->channel) == 0)) {
			ok = platch_decode((uint8_t*) message->message, message->message_size, pluginregistry.platch_obj_cbs[i].codec, &object);
			if (ok != 0) return ok;

			pluginregistry.platch_obj_cbs[i].callback((char*) message->channel, &object, (FlutterPlatformMessageResponseHandle*) message->response_handle);

			platch_free_obj(&object);
			return 0;
		}
	}

	// we didn't find a callback for the specified channel.
	// just respond with a null buffer to tell the VM-side
	// that the feature is not implemented.

	return platch_respond_not_implemented((FlutterPlatformMessageResponseHandle *) message->response_handle);
}
int plugin_registry_set_receiver(char *channel, enum platch_codec codec, platch_obj_recv_callback callback) {
	/// the index in 'callback' of the platch_obj_recv_data that will be added / updated.
	int index = -1;

	/// find the index with channel name 'channel', or else, the first unoccupied index.
	for (int i = 0; i < pluginregistry.platch_obj_cbs_size; i++) {
		if (pluginregistry.platch_obj_cbs[i].channel == NULL) {
			if (index == -1) {
				index = i;
			}
		} else if (strcmp(channel, pluginregistry.platch_obj_cbs[i].channel) == 0) {
			index = i;
			break;
		}
	}
	
	/// no matching or unoccupied index found.
	if (index == -1) {
		if (!callback) return 0;
		
		/// expand array
		size_t currentsize = pluginregistry.platch_obj_cbs_size * sizeof(struct platch_obj_recv_data);
		
		pluginregistry.platch_obj_cbs = realloc(pluginregistry.platch_obj_cbs, 2 * currentsize);
		memset(&pluginregistry.platch_obj_cbs[pluginregistry.platch_obj_cbs_size], currentsize, 0);

		index = pluginregistry.platch_obj_cbs_size;
		pluginregistry.platch_obj_cbs_size = 2*pluginregistry.platch_obj_cbs_size;
	}

	if (callback) {
		char *channelCopy = malloc(strlen(channel) +1);
		if (!channelCopy) return ENOMEM;
		strcpy(channelCopy, channel);

		pluginregistry.platch_obj_cbs[index].channel = channelCopy;
		pluginregistry.platch_obj_cbs[index].codec = codec;
		pluginregistry.platch_obj_cbs[index].callback = callback;
	} else if (pluginregistry.platch_obj_cbs[index].callback) {
		free(pluginregistry.platch_obj_cbs[index].channel);
		pluginregistry.platch_obj_cbs[index].channel = NULL;
		pluginregistry.platch_obj_cbs[index].callback = NULL;
	}

	return 0;
	
}
int plugin_registry_deinit() {
	int i, ok;
	
	/// call each plugins 'deinit'
	for (i = 0; i < pluginregistry.plugin_count; i++) {
		if (pluginregistry.plugins[i].deinit) {
			ok = pluginregistry.plugins[i].deinit();
			if (ok != 0) return ok;
		}
	}

	/// free all the channel names from the callback list.
	for (int i=0; i < pluginregistry.platch_obj_cbs_size; i++) {
		if (pluginregistry.platch_obj_cbs[i].channel)
			free(pluginregistry.platch_obj_cbs[i].channel);
	}

	/// free the rest
	free(pluginregistry.platch_obj_cbs);

	return 0;
}
