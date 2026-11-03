#include "orb_settings.h"

#include <string.h>
#if defined(ESP_PLATFORM) || defined(ORB_SETTINGS_TEST_STORAGE)
#include "orb_fs.h"
#endif
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
#define LOCK() portENTER_CRITICAL(&s_lock)
#define UNLOCK() portEXIT_CRITICAL(&s_lock)
#else
#include <pthread.h>
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
#define LOCK() ((void)pthread_mutex_lock(&s_lock))
#define UNLOCK() ((void)pthread_mutex_unlock(&s_lock))
#endif

#define DEFAULT_PREFERENCES { \
    .version = 2, .face = ORB_FACE_ORB, .colour = 0, \
    .slot = {ORB_FACE_CLAUDE, ORB_FACE_HOME, ORB_FACE_BATTERY}, \
    .meridian_slot = {ORB_FACE_WEATHER, ORB_FACE_CLAUDE, ORB_FACE_BATTERY, ORB_FACE_DATE}, \
    .activity_slot = {ORB_FACE_CLAUDE, ORB_FACE_CODEX, ORB_FACE_HOME}, \
}

_Static_assert(sizeof(orb_faces_prefs_t) == 16, "Faces storage ABI must remain unchanged");

typedef struct {
    orb_faces_prefs_t prefs;
    uint64_t ticket;
    orb_settings_result_t result;
    bool retained, claimed;
} face_request_t;

static const orb_faces_prefs_t s_defaults = DEFAULT_PREFERENCES;
static orb_faces_prefs_t s_current = DEFAULT_PREFERENCES;
static orb_settings_snapshot_t s_view = {.revision = 1};
static face_request_t s_requests[ORB_SETTINGS_FACE_REQUESTS];
static uint64_t s_sequence, s_latest;

static void changed(void)
{
    if (++s_view.revision == 0) ++s_view.revision;
}

bool orb_settings_faces_valid(const orb_faces_prefs_t *prefs)
{
    if (!prefs || prefs->version != 2 || prefs->face >= ORB_FACE_COUNT || prefs->colour >= 9) return false;
    for (unsigned i = 0; i < 3; i++)
        if (prefs->slot[i] >= ORB_FACE_PROVIDER_COUNT || prefs->activity_slot[i] >= ORB_FACE_PROVIDER_COUNT) return false;
    for (unsigned i = 0; i < 4; i++)
        if (prefs->meridian_slot[i] >= ORB_FACE_PROVIDER_COUNT) return false;
    return true;
}

static face_request_t *find_request(uint64_t ticket)
{
    if (!ticket) return NULL;
    for (unsigned i = 0; i < ORB_SETTINGS_FACE_REQUESTS; i++)
        if (s_requests[i].ticket == ticket) return &s_requests[i];
    return NULL;
}

static bool pending(void)
{
    for (unsigned i = 0; i < ORB_SETTINGS_FACE_REQUESTS; i++)
        if (s_requests[i].result == ORB_SETTINGS_PENDING) return true;
    return false;
}

static void complete(face_request_t *request, bool saved)
{
    request->result = saved ? ORB_SETTINGS_SAVED : ORB_SETTINGS_FAILED;
    request->claimed = false;
    if (saved) s_current = request->prefs;
    /* A prior write finishing cannot replace the status of a newer request.
     * Its own retained ticket still reports its exact authoritative result. */
    if (request->ticket == s_latest) s_view.save_error = !saved;
    s_view.saving = pending();
    changed();
    if (!request->retained) memset(request, 0, sizeof(*request));
}

void orb_settings_init(void)
{
    orb_faces_prefs_t current = s_defaults;
#if defined(ESP_PLATFORM) || defined(ORB_SETTINGS_TEST_STORAGE)
    if (orb_fs_ready()) {
        orb_faces_prefs_t loaded;
        size_t size = 0;
        if (orb_fs_read("faces", "preferences", &loaded, sizeof(loaded), &size) == ESP_OK &&
            size == sizeof(loaded) && orb_settings_faces_valid(&loaded)) current = loaded;
    }
#endif
    LOCK();
    s_current = current;
    memset(s_requests, 0, sizeof(s_requests));
    s_sequence = s_latest = 0;
    s_view = (orb_settings_snapshot_t){.revision = 1};
    UNLOCK();
}

void orb_settings_faces_current(orb_faces_prefs_t *out)
{
    if (!out) return;
    LOCK();
    *out = s_current;
    UNLOCK();
}

void orb_settings_snapshot(orb_settings_snapshot_t *out)
{
    if (!out) return;
    LOCK();
    *out = s_view;
    UNLOCK();
}

bool orb_settings_faces_accept(const orb_faces_prefs_t *prefs)
{
    if (!orb_settings_faces_valid(prefs)) return false;
    LOCK();
    s_current = *prefs;
    s_latest = 0;
    s_view.save_error = false;
    changed();
    UNLOCK();
    return true;
}

uint64_t orb_settings_faces_request(const orb_faces_prefs_t *prefs)
{
    bool valid = orb_settings_faces_valid(prefs);
    LOCK();
    face_request_t *request = NULL;
    for (unsigned i = 0; valid && i < ORB_SETTINGS_FACE_REQUESTS; i++) {
        if (!s_requests[i].ticket) { request = &s_requests[i]; break; }
    }
    if (!request) {
        s_latest = 0;
        s_view.save_error = true;
        changed();
        UNLOCK();
        return 0;
    }
    do { ++s_sequence; } while (!s_sequence || find_request(s_sequence));
    uint64_t ticket = s_sequence;
    *request = (face_request_t){.prefs = *prefs, .ticket = ticket,
        .result = ORB_SETTINGS_PENDING, .retained = true};
    s_latest = ticket;
    s_view.saving = true;
    s_view.save_error = false;
    changed();
#if !defined(ESP_PLATFORM) && !defined(ORB_SETTINGS_TEST_STORAGE)
    /* Ordinary simulator views have always kept preferences only in memory.
     * Preserve that completion timing without adding a host filesystem. */
    complete(request, true);
#endif
    UNLOCK();
    return ticket;
}

orb_settings_result_t orb_settings_faces_result(uint64_t ticket)
{
    LOCK();
    face_request_t *request = find_request(ticket);
    orb_settings_result_t result = request && request->retained ? request->result : ORB_SETTINGS_UNKNOWN;
    UNLOCK();
    return result;
}

void orb_settings_faces_release(uint64_t ticket)
{
    LOCK();
    face_request_t *request = find_request(ticket);
    if (request) {
        request->retained = false;
        if (request->result != ORB_SETTINGS_PENDING) memset(request, 0, sizeof(*request));
    }
    UNLOCK();
}

void orb_settings_service(void)
{
#if defined(ESP_PLATFORM) || defined(ORB_SETTINGS_TEST_STORAGE)
    LOCK();
    face_request_t *request = NULL;
    for (unsigned i = 0; i < ORB_SETTINGS_FACE_REQUESTS; i++) {
        face_request_t *candidate = &s_requests[i];
        if (candidate->claimed) { UNLOCK(); return; }
        if (candidate->result == ORB_SETTINGS_PENDING && (!request || candidate->ticket < request->ticket))
            request = candidate;
    }
    if (!request) { UNLOCK(); return; }
    request->claimed = true;
    orb_faces_prefs_t prefs = request->prefs;
    UNLOCK();

    /* The main owner performs all storage work. Queue/result access never waits
     * for flash and a detached view cannot cancel its already copied request. */
    bool saved = orb_fs_ready() && orb_fs_app_prepare("faces") == ESP_OK &&
        orb_fs_write("faces", "preferences", &prefs, sizeof(prefs)) == ESP_OK;

    LOCK();
    complete(request, saved);
    UNLOCK();
#endif
}
