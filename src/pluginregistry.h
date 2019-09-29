#ifndef FLUTTER_PI_REGISTRY_H_
#define FLUTTER_PI_REGISTRY_H_ 1

#include <stdbool.h>
#include <stdlib.h>
#include <flutter_embedder.h>

#include "platformchannel.h"

/// Callback for Initialization or Deinitialization.
/// Return value is 0 for success, or anything else for an error
///   (uses the errno error codes)
typedef int (*InitDeinitCallback)(void);

/// A Callback that gets called when a platform message
/// arrives on a channel you registered it with.
/// channel is the method channel that received a platform message,
/// object is the object that is the result of automatically decoding
/// the platform message using the codec given to PluginRegistry_setReceiver.
/// BE AWARE that object->type can be kNotImplemented, REGARDLESS of the codec
///   passed to PluginRegistry_setReceiver.
typedef int (*ChannelObjectReceiveCallback)(char *channel, struct ChannelObject *object, FlutterPlatformMessageResponseHandle *responsehandle);

/// details of a plugin for flutter-pi.
/// All plugins are initialized (i.e. get their "init" callbacks called)
///   when PluginRegistry_init() is called by flutter-pi.
///   In the init callback, you probably want to do stuff like
///   register callbacks for some method channels your plugin uses,
///   or dynamically allocate memory for your plugin if you need to.
///   PluginRegistry_init() and thus every plugins init is called
///   BEFORE the flutter engine is set up and running. The "engine"
///   global may even be NULL at the time "init" is called. Sending flutter messages
///   will probably cause the application to crash.
/// deinit is also called AFTER the engine is shut down.
struct FlutterPiPlugin {
    const char const* name;
	InitDeinitCallback init;
	InitDeinitCallback deinit;
};


int PluginRegistry_init();
int PluginRegistry_onPlatformMessage(FlutterPlatformMessage *message);

/// Sets the callback that should be called when a platform message arrives on channel "channel",
/// and the codec used to automatically decode the platform message.
/// Call this method with NULL as the callback parameter to remove the current listener on that channel.
int PluginRegistry_setReceiver(char *channel, enum ChannelCodec codec, ChannelObjectReceiveCallback callback);

int PluginRegistry_deinit();

#endif