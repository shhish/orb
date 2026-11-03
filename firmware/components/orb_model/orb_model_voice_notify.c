#include "orb_model.h"
#include "orb_model_voice_notify.h"

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static void lock(void) { portENTER_CRITICAL(&s_lock); }
static void unlock(void) { portEXIT_CRITICAL(&s_lock); }
#else
#include <pthread.h>
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static void lock(void) { pthread_mutex_lock(&s_lock); }
static void unlock(void) { pthread_mutex_unlock(&s_lock); }
#endif

static void (*s_notify)(void *context);
static void *s_context;

bool orb_model_voice_notify(bool queued)
{
    lock();
    void (*notify)(void *) = s_notify;
    void *context = s_context;
    unlock();
    if (queued && notify) notify(context);
    return queued;
}

void orb_model_set_voice_notify(void (*notify)(void *context), void *context)
{
    lock();
    s_notify = notify;
    s_context = context;
    unlock();
    /* A latched wake also covers requests queued before registration, without
     * inspecting the store or running a callback under either mutex. */
    if (notify) notify(context);
}
