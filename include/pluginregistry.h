#ifndef FLUTTER_PI_REGISTRY_H_
#define FLUTTER_PI_REGISTRY_H_

#include <unistd.h>
#include <string.h>
#include <sys/select.h>

#include <platformchannel.h>

struct flutterpi;
struct plugin_registry;

typedef enum plugin_init_result (*plugin_init_t)(struct flutterpi *flutterpi, void **userdata_out);

typedef void (*plugin_deinit_t)(struct flutterpi *flutterpi, void *userdata);

struct flutterpi_plugin_v2 {
	const char *name;
	plugin_init_t init;
	plugin_deinit_t deinit;
};

#define STREQ(a, b) (strcmp((a), (b)) == 0)

/// The return value of a plugin initializer function.
enum plugin_init_result {
	kInitialized_PluginInitResult,   	///< The plugin was successfully initialized.
	kNotApplicable_PluginInitResult, 	///< The plugin couldn't be initialized, because it's not compatible with the flutter-pi instance.
										///  For example, the plugin requires OpenGL but flutter-pi is using software rendering.
										///  This is not an error, and flutter-pi will continue initializing the other plugins.
	kError_PluginInitResult				///< The plugin couldn't be initialized because an unexpected error ocurred.
										///  Flutter-pi may decide to abort the startup phase of the whole flutter-pi instance at that point.
};

struct _FlutterPlatformMessageResponseHandle;
typedef struct _FlutterPlatformMessageResponseHandle
    FlutterPlatformMessageResponseHandle;

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
	FlutterPlatformMessageResponseHandle *responsehandle
);

/**
 * @brief Create a new plugin registry instance and add the hardcoded plugins, but don't initialize them yet.
 */
struct plugin_registry *plugin_registry_new(struct flutterpi *flutterpi);

void plugin_registry_destroy(struct plugin_registry *registry);

int plugin_registry_add_plugin(struct plugin_registry *registry, const struct flutterpi_plugin_v2 *plugin);

int plugin_registry_add_plugins_from_static_registry(struct plugin_registry *registry);

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
int plugin_registry_on_platform_message(struct plugin_registry *registry, FlutterPlatformMessage *message);

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

int plugin_registry_deinit(void);

int static_plugin_registry_add_plugin(const struct flutterpi_plugin_v2 *plugin);

int static_plugin_registry_remove_plugin(const char *plugin_name);

#define FLUTTERPI_PLUGIN(_name, _identifier_name, _init, _deinit) \
__attribute__((constructor)) static void __reg_plugin_##_identifier_name() { \
	static struct flutterpi_plugin_v2 plugin = { \
		.name = (_name), \
		.init = (_init), \
		.deinit = (_deinit) \
	}; \
	int ok; \
	\
	ok = static_plugin_registry_add_plugin(&plugin); \
	if (ok != 0) { \
		fprintf(stderr, "Couldn't register plugin " _name " to plugin registry.\n"); \
		abort(); \
	} \
} \
\
__attribute__((destructor)) static void __unreg_plugin_##_identifier_name() { \
	int ok; \
	ok = static_plugin_registry_remove_plugin(_name); \
	if (ok != 0) { \
		fprintf(stderr, "Couldn't remove plugin " _name " from plugin registry.\n"); \
	} \
}

#endif