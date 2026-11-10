// SPDX-License-Identifier: MIT
#include <errno.h>

#include <unity.h>

#include "notifier_listener.h"

static struct notifier notifier;
static int initial_lock_result;
static int updates;

void setUp(void) {
    TEST_ASSERT_EQUAL_INT(0, value_notifier_init(&notifier, NULL, NULL));
    updates = 0;
}
void tearDown(void) {
    notifier_deinit(&notifier);
}

static enum listener_return observe(void *value, void *userdata) {
    (void) value;
    (void) userdata;
    initial_lock_result = pthread_mutex_trylock(&notifier.mutex);
    if (initial_lock_result == 0)
        pthread_mutex_unlock(&notifier.mutex);
    updates++;
    return kNoAction;
}
static enum listener_return unlisten(void *value, void *userdata) {
    (void) value;
    (void) userdata;
    return kUnlisten;
}
void test_initial_value_is_protected_against_concurrent_replacement(void) {
    struct listener *listener = notifier_listen(&notifier, observe, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(EBUSY, initial_lock_result);
    TEST_ASSERT_NOT_NULL(listener);
    notifier_notify(&notifier, NULL);
    TEST_ASSERT_EQUAL_INT(2, updates);
    notifier_unlisten(&notifier, listener);
    notifier_notify(&notifier, NULL);
    TEST_ASSERT_EQUAL_INT(2, updates);
}
void test_immediate_unlisten_releases_the_notifier_lock(void) {
    TEST_ASSERT_NULL(notifier_listen(&notifier, unlisten, NULL, NULL));
    int result = pthread_mutex_trylock(&notifier.mutex);
    if (result == 0)
        pthread_mutex_unlock(&notifier.mutex);
    TEST_ASSERT_EQUAL_INT(0, result);
}
int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_initial_value_is_protected_against_concurrent_replacement);
    RUN_TEST(test_immediate_unlisten_releases_the_notifier_lock);
    return UNITY_END();
}
