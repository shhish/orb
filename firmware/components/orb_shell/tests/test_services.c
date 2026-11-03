#include "orb_shell_services.h"
#include "orb_apps.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static unsigned s_checks;
static atomic_uint s_accepted;
#define CHECK(value) do { s_checks++; assert(value); } while (0)

static orb_shell_app_state_t snapshot(void)
{
    orb_shell_app_state_t value;
    orb_shell_apps_snapshot(&value);
    return value;
}

static void *request_once(void *ctx)
{
    (void)ctx;
    if (orb_shell_app_request("marble")) atomic_fetch_add(&s_accepted, 1);
    return NULL;
}

int main(void)
{
    orb_apps_init(ORB_APP_NEEDS_IMU);
    CHECK(!snapshot().pending && !snapshot().active[0]);
    CHECK(!orb_shell_app_request("../marble"));
    CHECK(!orb_shell_app_request("abcdefghijklmnopqrstuvwxy"));
    CHECK(!orb_shell_app_request("Marble"));
    uint32_t previous = snapshot().revision;
    char id[] = "marble";
    CHECK(orb_shell_app_request(id));
    memset(id, 'x', sizeof(id) - 1);
    CHECK(snapshot().revision > previous);
    CHECK(snapshot().pending && !strcmp(snapshot().requested, "marble"));
    CHECK(!orb_apps_active()); /* UI enqueue never invokes an entry callback. */
    CHECK(!orb_shell_app_request("other"));
    CHECK(!orb_shell_app_request(NULL));
    orb_shell_apps_service();
    CHECK(!snapshot().pending && !snapshot().error);
    CHECK(!strcmp(snapshot().active, "marble"));
    CHECK(orb_apps_active() == orb_apps_find("marble"));
    previous = snapshot().revision;
    orb_shell_apps_service();
    orb_shell_apps_publish();
    CHECK(snapshot().revision == previous); /* An idle poll never repaints. */

    CHECK(orb_shell_app_request("marble"));
    orb_shell_apps_service();
    CHECK(snapshot().error == ESP_ERR_INVALID_STATE);
    CHECK(!strcmp(snapshot().active, "marble"));
    CHECK(orb_shell_app_request(NULL));
    orb_shell_apps_service();
    CHECK(!snapshot().active[0] && !snapshot().error);
    CHECK(orb_shell_app_request("missing"));
    orb_shell_apps_service();
    CHECK(snapshot().error == ESP_ERR_NOT_FOUND);
    CHECK(!snapshot().active[0]);

    /* Main's direct PWR exit remains authoritative even while an app is paused. */
    CHECK(orb_shell_app_request("marble"));
    orb_shell_apps_service();
    orb_apps_poll(0, true, false, 0, 0, 1, false);
    orb_apps_poll(ORB_APPS_PWR_EXIT_US, true, false, 0, 0, 1, false);
    CHECK(!orb_apps_active());
    orb_shell_apps_publish();
    CHECK(!snapshot().active[0]);

    /* Concurrent producers can occupy exactly one bounded slot. */
    pthread_t producers[8];
    for (unsigned i = 0; i < 8; i++) CHECK(!pthread_create(&producers[i], NULL, request_once, NULL));
    for (unsigned i = 0; i < 8; i++) CHECK(!pthread_join(producers[i], NULL));
    CHECK(atomic_load(&s_accepted) == 1);
    CHECK(snapshot().pending && !strcmp(snapshot().requested, "marble"));
    orb_shell_apps_service();
    CHECK(!snapshot().pending && !snapshot().error);
    CHECK(orb_shell_app_request(""));
    orb_shell_apps_service();
    CHECK(!snapshot().active[0]);
    orb_shell_apps_snapshot(NULL);
    printf("shell services: %u checks passed\n", s_checks);
    return 0;
}
