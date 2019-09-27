#ifndef FLUTTER_PI_REGISTRY_H_
#define FLUTTER_PI_REGISTRY_H_ 1

#include <stdbool.h>
#include <stdlib.h>
#include <flutter_embedder.h>

#include "platformchannel.h"

typedef int (*InitDeinitCallback)(void);
typedef int (*ChannelObjectReceiveCallback)(char*, struct ChannelObject*, FlutterPlatformMessageResponseHandle *responsehandle);

struct FlutterPiPluginRegistry;

struct FlutterPiPlugin {
    const char const* name;
	InitDeinitCallback init;
	InitDeinitCallback deinit;
};


extern int hardcoded_plugins_count;
extern struct FlutterPiPlugin hardcoded_plugins[];
extern struct FlutterPiPluginRegistry *pluginregistry;

int PluginRegistry_init();
int PluginRegistry_onPlatformMessage(FlutterPlatformMessage *message);
int PluginRegistry_setReceiver(char *channel, enum ChannelCodec codec, ChannelObjectReceiveCallback callback);
int PluginRegistry_deinit();

#endif