#ifndef _FLUTTERPI_INCLUDE_NOTIFIER_LISTENER_H
#define _FLUTTERPI_INCLUDE_NOTIFIER_LISTENER_H

#include <collection.h>

enum listener_return {
    kNoAction,
    kUnlisten
};

typedef enum listener_return (*listener_cb_t)(void *arg, void *userdata);

struct listener;

struct notifier {
    pthread_mutex_t mutex;

    struct pointer_set listeners;

    bool is_value_notifier;
    void *state;
    void_callback_t value_destroy_callback;
};

/**
 * @brief Initialize this pre-allocated notifier object as a change notifier.
 * 
 * Change notifiers will only notify their listeners when @ref notifier_notify
 * is called. They don't call any new listeners with the last notified value, as
 * value notifiers do.
 * 
 */
int change_notifier_init(struct notifier *notifier);

/**
 * @brief Initialize this pre-allocated notifier object as a value notifier.
 * 
 * Value notifiers will remember the last notified value and immediately call
 * any new listeners with the last notified value (or the one given to this
 * initializer, if @ref notifier_notify was never called).
 * 
 */
int value_notifier_init(struct notifier *notifier, void *initial_value, void_callback_t value_destroy_callback);

/**
 * @brief Create a new heap allocated change notifier.
 * 
 * For the behaviour of change notifiers, see @ref change_notifier_init.
 * 
 */
struct notifier *change_notifier_new();

/**
 * @brief Create a new heap allocated value notifier.
 * 
 * For the behaviour of value notifiers, see @ref value_notifier_init.
 * 
 */
struct notifier *value_notifier_new(void *initial_value, void_callback_t value_destroy_callback);

/**
 * @brief De-initialize this notifier, destroying all listeners and freeing all
 * allocated resources. (But not the memory @arg notifier points to itself).
 * 
 * Use this if you use @ref change_notifier_init or @ref value_notifier_init to
 * setup your notifier.
 * 
 * If value_destroy_callback is not NULL, will invoke the value_destroy_callback on
 * the last value (either initial_value or the last value given to notifier_notify).
 * 
 * Note that this does not wait for any currently executing callbacks to complete.
 * So if this is a value notifier and you call @ref notifier_deinit while any other thread
 * is currently inside  @ref notifier_notify, this could destroy the void* value passed to
 * any listener callbacks while the listener callback is running. Other stuff could be racy
 * too. So the lifetime needs to be managed externally too. (as always basically)
 */
void notifier_deinit(struct notifier *notifier);

/**
 * @brief De-initialize this notifier AND free the memory @arg notifier points to.
 * 
 * Use this if you used @ref change_notifier_new or @ref value_notifier_new to 
 * setup your notifier.
 * 
 * Note that this does not wait for any currently executing listener callbacks to finish.
 * 
 * 
 */
void notifier_destroy(struct notifier *notifier);

DECLARE_LOCK_OPS(notifier)

/**
 * @brief Add a listener that should listen to this notifier.
 * 
 * @param notifier The notifier to listen to.
 * @param notify   will be called when the event-producing object calls notifier_notify, and additionally
 *                 (if the notifier is a value notifier) one time with the current value immediately inside
 *                 the notifier_listen function.
 * @param destroy  will be called when the listener is destroyed for some reason. Either when @ref notifier_unlisten
 *                 was called or when the notify callback returned kUnlisten, or when the notifier is destroyed.
 * @param userdata The userdata to be passed to the @param notify and @param destroy callbacks.
 * 
 * @returns        On success: A new listener object, only really useful for calling @ref notifier_unlisten,
 *                             or NULL when the @arg notifier is a value notifier, and the @arg notify function returned
 *                             kUnlisten when it was called synchronously inside the @ref notifier_listen function.
 *                 On failure: NULL (shouldn't happen though)
 */
struct listener *notifier_listen(struct notifier *notifier, listener_cb_t notify, void_callback_t destroy, void *userdata);

/**
 * @brief If @param listener is currently registered as a listener to @param notifier, de-register
 * it and destroy it. Otherwise, do nothing and return an error code.
 * 
 * This is only one way to de-register the listener. The other way is to return `kUnlisten` from
 * the listener callback.
 * 
 * @param notifier The notifier from which the listener should be removed.
 * @param listener The listener that should no longer receive events and be destroyed
 *                 (will only be destroyed when it's currently registered as a listener to notifier)
 * @returns        0 on success, a positive errno error code otherwise.
 */
int notifier_unlisten(struct notifier *notifier, struct listener *listener);

/**
 * @brief Notify all listeners about a new value. For any listeners registered
 * to @arg notifier, call the listener callback with @arg arg as the value.
 * 
 * @param notifier The notifier for which all the listener callbacks should be called.
 * @param arg      The value that should be send to the listeners.
 */
void notifier_notify(struct notifier *notifier, void *arg);

#endif // _FLUTTERPI_INCLUDE_NOTIFIER_LISTENER_H