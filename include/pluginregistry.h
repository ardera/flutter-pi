#ifndef FLUTTER_PI_REGISTRY_H_
#define FLUTTER_PI_REGISTRY_H_ 1

#include <unistd.h>
#include <string.h>
#include <sys/select.h>

#include <platformchannel.h>

struct flutterpi;
struct platform_message_response_handle;
struct plugin_registry;

/// Callback for Initialization or Deinitialization.
/// Return value is 0 for success, or anything else for an error
///   (uses the errno error codes)
typedef int (*init_deinit_cb)(struct flutterpi *flutterpi, void **userdata);

/// A Callback that gets called when a platform message
/// arrives on a channel you registered it with.
/// channel is the method channel that received a platform message,
/// object is the object that is the result of automatically decoding
/// the platform message using the codec given to plugin_registry_set_receiver.
/// BE AWARE that object->type can be kNotImplemented, REGARDLESS of the codec
///   passed to plugin_registry_set_receiver.
typedef int (*platch_obj_recv_callback)(
	const char *channel,
	struct platch_obj *object, 
	struct platform_message_response_handle *responsehandle,
	void *userdata
);

/**
 * @brief Create a new plugin registry instance and add the hardcoded plugins, but don't initialize them yet.
 */
struct plugin_registry *plugin_registry_new(struct flutterpi *flutterpi);

void plugin_registry_destroy(struct plugin_registry *registry);

int plugin_registry_add_plugin(
	struct plugin_registry *registry,
	const char *name,
	init_deinit_cb on_init,
	init_deinit_cb on_deinit
);

/**
 * @brief Initialize all not-yet initialized plugins.
 */
int  plugin_registry_ensure_plugins_initialized(struct plugin_registry *registry);

/**
 * @brief Deinitialize all initialized plugins.
 */
void plugin_registry_ensure_plugins_deinitialized(struct plugin_registry *registry);

/**
 * @brief Called by flutter-pi when a platform message arrives.
 */
int plugin_registry_on_platform_message(
	struct plugin_registry *registry,
	const char *channel,
	const uint8_t *message,
	size_t message_size,
	const struct platform_message_response_handle *responsehandle
);

void *plugin_registry_get_plugin_userdata(
	struct plugin_registry *registry,
	const char *plugin_name
);

/**
 * @brief Returns true @ref registry has a plugin with name @ref plugin_name.
 */
bool plugin_registry_is_plugin_present(
	struct plugin_registry *registry,
	const char *plugin_name
);

#endif