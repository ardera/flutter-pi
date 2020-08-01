#ifndef FLUTTER_PI_REGISTRY_H_
#define FLUTTER_PI_REGISTRY_H_ 1

#include <unistd.h>
#include <string.h>
#include <sys/select.h>

#include <platformchannel.h>

#define STREQ(a, b) (strcmp(a, b) == 0)

/// Callback for Initialization or Deinitialization.
/// Return value is 0 for success, or anything else for an error
///   (uses the errno error codes)
typedef int (*init_deinit_cb)(void);

/// A Callback that gets called when a platform message
/// arrives on a channel you registered it with.
/// channel is the method channel that received a platform message,
/// object is the object that is the result of automatically decoding
/// the platform message using the codec given to plugin_registry_set_receiver.
/// BE AWARE that object->type can be kNotImplemented, REGARDLESS of the codec
///   passed to plugin_registry_set_receiver.
typedef int (*platch_obj_recv_callback)(
	char *channel,
	struct platch_obj *object, 
	FlutterPlatformMessageResponseHandle *responsehandle);

/// details of a plugin for flutter-pi.
/// All plugins are initialized (i.e. get their "init" callbacks called)
///   when plugin_registry_init() is called by flutter-pi.
///   In the init callback, you probably want to do stuff like
///   register callbacks for some method channels your plugin uses,
///   or dynamically allocate memory for your plugin if you need to.
///   plugin_registry_init() and thus every plugins init is called
///   BEFORE the flutter engine is set up and running. The "engine"
///   global may even be NULL at the time "init" is called. Sending flutter messages
///   will probably cause the application to crash.
/// deinit is also called AFTER the engine is shut down.
struct flutterpi_plugin {
    const char* name;
	init_deinit_cb init;
	init_deinit_cb deinit;
};


int plugin_registry_init(void);

int plugin_registry_on_platform_message(
	FlutterPlatformMessage *message
);

/// Sets the callback that should be called when a platform message arrives on channel "channel",
/// and the codec used to automatically decode the platform message.
/// Call this method with NULL as the callback parameter to remove the current listener on that channel.
int plugin_registry_set_receiver(
	const char *channel,
	enum platch_codec codec,
	platch_obj_recv_callback callback
);

int plugin_registry_remove_receiver(
	const char *channel
);

bool plugin_registry_is_plugin_present(
	const char *plugin_name
);

int plugin_registry_deinit(void);

#endif