#include "orb_shell_services.h"

#include <string.h>

#include "orb_apps.h"

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
#define LOCK() portENTER_CRITICAL(&s_lock)
#define UNLOCK() portEXIT_CRITICAL(&s_lock)
#else
#include <pthread.h>
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
#define LOCK() ((void)pthread_mutex_lock(&s_lock))
#define UNLOCK() ((void)pthread_mutex_unlock(&s_lock))
#endif

static orb_shell_app_state_t s_state;
static bool s_processing;

static bool valid_id(const char *id)
{
    if (!id) return true;
    size_t n = strnlen(id, ORB_SHELL_APP_ID_MAX + 1);
    if (n > ORB_SHELL_APP_ID_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        char ch = id[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_'))
            return false;
    }
    return true;
}

bool orb_shell_app_request(const char *id)
{
    if (!valid_id(id)) return false;
    LOCK();
    if (s_state.pending) { UNLOCK(); return false; }
    memset(s_state.requested, 0, sizeof(s_state.requested));
    if (id) memcpy(s_state.requested, id, strlen(id));
    s_state.pending = true;
    s_state.error = ESP_OK;
    s_state.revision++;
    UNLOCK();
    return true;
}

void orb_shell_apps_snapshot(orb_shell_app_state_t *out)
{
    if (!out) return;
    LOCK();
    *out = s_state;
    UNLOCK();
}

void orb_shell_apps_publish(void)
{
    /* Registry lifecycle is main-owned. The UI sees only this bounded copy,
     * including reliable PWR exit while the application has paused (HIG §7). */
    const orb_app_manifest_t *active = orb_apps_active();
    const char *id = active ? active->id : "";
    LOCK();
    if (strcmp(s_state.active, id)) {
        memset(s_state.active, 0, sizeof(s_state.active));
        memcpy(s_state.active, id, strlen(id));
        s_state.revision++;
    }
    UNLOCK();
}

void orb_shell_apps_service(void)
{
    char request[ORB_SHELL_APP_ID_MAX + 1];
    LOCK();
    if (!s_state.pending || s_processing) { UNLOCK(); return; }
    s_processing = true;
    memcpy(request, s_state.requested, sizeof(request));
    UNLOCK();

    esp_err_t error = ESP_OK;
    if (request[0]) error = orb_apps_enter(request);
    else orb_apps_exit();

    LOCK();
    s_state.error = error;
    s_state.pending = false;
    s_processing = false;
    s_state.revision++;
    UNLOCK();
    orb_shell_apps_publish();
}
