#include <sys/errno.h>
#include <string.h>

#include "platformchannel.h"
#include "pluginregistry.h"

struct FlutterPiPluginRegistry {
	struct FlutterPiPlugin *plugins;
	size_t plugin_count;
	struct FlutterPiPluginRegistryCallbackListElement *callbacks;
};

struct FlutterPiPluginRegistryCallbackListElement {	// hopefully that full type name is not used very often.
	struct FlutterPiPluginRegistryCallbackListElement *next;
	char *channel;
	void *userdata;
	bool is_methodcall;
	union {
		FlutterPiMethodCallCallback      methodcall_callback;
		FlutterPiPlatformMessageCallback message_callback;
	};
};


int PluginRegistry_init(struct FlutterPiPluginRegistry **pregistry) {
	int ok;

	*pregistry = NULL;
	*pregistry = malloc(sizeof(struct FlutterPiPluginRegistry));
	if (!(*pregistry)) return ENOMEM;

	struct FlutterPiPluginRegistry *registry = *pregistry;
	registry->plugins = hardcoded_plugins;
	registry->plugin_count = sizeof(hardcoded_plugins) / sizeof(struct FlutterPiPlugin);

	// load dynamic plugins
	// nothing for now


	for (int i = 0; i < registry->plugin_count; i++) {
		ok = registry->plugins[i].init(registry, &(registry->plugins[i].userdata));
		if (ok != 0) return ok;
	}

	return 0;
}
int PluginRegistry_onPlatformMessage(struct FlutterPiPluginRegistry *registry, FlutterPlatformMessage *message) {
	struct FlutterPiPluginRegistryCallbackListElement *element = registry->callbacks;

	for (element = registry->callbacks; element != NULL; element = element->next)
		if (strcmp(element->channel, message->channel) == 0) break;
	
	if (element != NULL) {
		if (element->is_methodcall) {
			// try decode platform message as method call
			struct MethodCall *methodcall = NULL;
			bool ok = PlatformChannel_decodeMethodCall(message->message_size, message->message, &methodcall);
			if (!ok) return EBADMSG;

			element->methodcall_callback(element->userdata, message->channel, methodcall);

			PlatformChannel_freeMethodCall(&methodcall);
		} else {
			element->message_callback(element->userdata, message);
		}
	}

	return 0;
}
int PluginRegistry_setPlatformMessageHandler(struct FlutterPiPluginRegistry *registry, char *channel,
										 FlutterPiPlatformMessageCallback callback, void *userdata) {
	
	struct FlutterPiPluginRegistryCallbackListElement *element;
	for (element = registry->callbacks; element != NULL; element = element->next)
		if (strcmp(element->channel, channel) == 0) break;
	
	if (element != NULL) {
		// change the behaviour of the existing handler

		element->is_methodcall = false;
		element->message_callback = callback;
		element->userdata = userdata;
		return 0;
	} else {
		// new handler

		element = malloc(sizeof(struct FlutterPiPluginRegistryCallbackListElement));
		if (!element) return ENOMEM;

		element->channel = calloc(strlen(channel) +1, sizeof(char));
		if (!element->channel) return ENOMEM;

		strcpy(element->channel, channel);

		element->is_methodcall = false;
		element->message_callback = callback;
		element->userdata = userdata;
		element->next = registry->callbacks;

		registry->callbacks = element;
	}

	return 0;
}
int PluginRegistry_setMethodCallHandler(struct FlutterPiPluginRegistry *registry, char *channel,
										FlutterPiMethodCallCallback callback, void *userdata) {
	struct FlutterPiPluginRegistryCallbackListElement *element;

	for (element = registry->callbacks; element != NULL; element = element->next)
		if (strcmp(element->channel, channel) == 0) break;
	
	if (element != NULL) {
		// change the behaviour of the existing handler

		element->is_methodcall = true;
		element->methodcall_callback = callback;
		element->userdata = userdata;
		return 0;
	} else {
		// new handler

		element = malloc(sizeof(struct FlutterPiPluginRegistryCallbackListElement));
		if (!element) return ENOMEM;

		element->channel = calloc(strlen(channel) +1, sizeof(char));
		if (!element->channel) return ENOMEM;

		strcpy(element->channel, channel);

		element->is_methodcall = true;
		element->methodcall_callback = callback;
		element->userdata = userdata;
		element->next = registry->callbacks;

		registry->callbacks = element;
	}

	return 0;
}
int PluginRegistry_deinit(struct FlutterPiPluginRegistry **pregistry) {
	struct FlutterPiPluginRegistryCallbackListElement *element = (*pregistry)->callbacks, *t = NULL;

	while (element != NULL) {
		free(element->channel);

		t = element;
		element = element->next;
		free(t);
	}

	free(*pregistry);
	*pregistry = NULL;
}