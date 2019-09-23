#include <stdbool.h>
#include <stdlib.h>

#include "platformchannel.h"

#ifndef FLUTTER_PI_REGISTRY_H_
#define FLUTTER_PI_REGISTRY_H_

typedef bool (*FlutterPiPluginRegistryCallback)(struct FlutterPiPluginRegistry *registry, void **userdata);
typedef bool (*FlutterPiMethodCallCallback)(void *userdata, char *channel, struct MethodCall *methodcall);
typedef bool (*FlutterPiPlatformMessageCallback)(void *userdata, FlutterPlatformMessage *message);
struct FlutterPiPluginRegistry;

struct FlutterPiPlugin {
    const char const* name;
	FlutterPiPluginRegistryCallback init;
	FlutterPiPluginRegistryCallback deinit;
    void *userdata;
};

const struct FlutterPiPlugin hardcoded_plugins[] = {
    {.name="connectivity", .init=NULL, .deinit=NULL, .userdata=NULL}
};

extern int PluginRegistry_init(struct FlutterPiPluginRegistry **registry);
extern int PluginRegistry_onMethodCall(struct FlutterPiPluginRegistry *registry, char *channel, struct MethodCall *methodcall);
extern int PluginRegistry_setMethodCallHandler(struct FlutterPiPluginRegistry *registry, char *methodchannel,
                                               FlutterPiMethodCallCallback callback,  void *userdata);
extern int PluginRegistry_deinit(struct FlutterPiPluginRegistry **registry);

#endif