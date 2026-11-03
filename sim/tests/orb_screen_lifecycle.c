#include "orb_screen_lifecycle.h"

#include <assert.h>
#include <stdlib.h>

static void create_updated(orb_screen_instance_t *instance, orb_screen_host_t *host,
                           const orb_lifecycle_case_t *test,
                           const orb_screen_update_t *update)
{
    assert(orb_screen_create(instance, host, test->descriptor, lv_screen_active(), test->args, false));
    assert(orb_screen_update(instance, update));
}

static void destroy_checked(orb_screen_instance_t *instance, const orb_lifecycle_case_t *test)
{
    orb_screen_destroy(instance);
    assert(!orb_screen_alive(instance));
    orb_screen_usage_t usage;
    orb_screen_usage(instance, &usage);
    assert(!usage.state && !usage.context && !usage.payload && !usage.allocations);
    for (size_t i = 0; i < 4; i++) assert(usage.resources[i] == 0);
    test->quiescent();
    /* Both explicit close and parent deletion leave an idempotent handle. */
    orb_screen_destroy(instance);
}

void orb_test_screen_lifecycle(orb_screen_host_t *host, const orb_lifecycle_case_t *test)
{
    assert(test && test->descriptor && test->initial && test->changed && test->removed && test->empty);
    assert(test->exercise && test->verify && test->quiescent);
    orb_screen_instance_t instance = {0};
    size_t size = test->descriptor->context_bytes;
    orb_screen_context_t *context = calloc(1, size);
    assert(context);

    create_updated(&instance, host, test, test->initial);
    test->verify(&instance, ORB_LIFECYCLE_INITIAL);
    assert(orb_screen_update(&instance, test->initial));
    test->verify(&instance, ORB_LIFECYCLE_IDEMPOTENT);
    assert(orb_screen_update(&instance, test->changed));
    test->verify(&instance, ORB_LIFECYCLE_CHANGED);
    test->exercise(&instance);
    assert(orb_screen_save(&instance, context, size));
    destroy_checked(&instance, test);

    create_updated(&instance, host, test, test->changed);
    assert(orb_screen_restore(&instance, context, size));
    test->verify(&instance, ORB_LIFECYCLE_RESTORED);
    destroy_checked(&instance, test);

    create_updated(&instance, host, test, test->removed);
    assert(orb_screen_restore(&instance, context, size));
    test->verify(&instance, ORB_LIFECYCLE_REMOVED_ID);
    destroy_checked(&instance, test);

    create_updated(&instance, host, test, test->empty);
    assert(orb_screen_restore(&instance, context, size));
    test->verify(&instance, ORB_LIFECYCLE_EMPTY);
    destroy_checked(&instance, test);
    free(context);
}
