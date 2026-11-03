/* Compile the real host service plus its production NVS functions. The fake
 * store commits atomically and survives a reset of all service preferences. */
#include "orb_sound.c"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ESP_OK 0
#define NVS_READONLY 0
#define NVS_READWRITE 1
typedef unsigned nvs_handle_t;
static unsigned s_checks, s_commits, s_opens;
static pthread_t s_main;
static int64_t s_now, s_commit_elapsed;
static bool s_edit_during_commit;
static uint32_t s_disk, s_pending;
static bool s_present, s_fail_open, s_fail_write, s_fail_commit;
#define CHECK(c) do { s_checks++; if (!(c)) { \
    fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)

static int nvs_open(const char *name, int mode, nvs_handle_t *h)
{
    CHECK(pthread_equal(pthread_self(), s_main));
    CHECK(!strcmp(name, "orb_sound"));
    s_opens++;
    *h = (unsigned)mode;
    return s_fail_open || (!s_present && mode == NVS_READONLY);
}
static int nvs_get_u32(nvs_handle_t h, const char *key, uint32_t *value)
{
    CHECK(h == NVS_READONLY && !strcmp(key, "preferences"));
    *value = s_disk;
    return !s_present;
}
static int nvs_set_u32(nvs_handle_t h, const char *key, uint32_t value)
{
    CHECK(h == NVS_READWRITE && !strcmp(key, "preferences"));
    s_pending = value;
    return s_fail_write;
}
static int nvs_commit(nvs_handle_t h)
{
    CHECK(pthread_equal(pthread_self(), s_main));
    CHECK(h == NVS_READWRITE);
    if (s_edit_during_commit) {
        s_now += s_commit_elapsed;
        s_edit_during_commit = false;
        orb_sound_set_volume(64);
    }
    if (s_fail_commit) return 1;
    s_disk = s_pending;
    s_present = true;
    s_commits++;
    return ESP_OK;
}
static void nvs_close(nvs_handle_t h) { CHECK(h <= NVS_READWRITE); }

#include "orb_sound_preferences.c"

static void restart(void)
{
    atomic_store(&s_preferences, 60u | (22u << 8) | (7u << 13));
    atomic_store(&s_silent, false);
    atomic_store(&s_preference_generation, 0);
    atomic_store(&s_saved_generation, 0);
    atomic_store(&s_failed_generation, 0);
    orb_sound_preferences_init();
}

/* Request on a distinct audio worker thread. The real setter may wake the
 * owner, but every fake NVS entry point rejects execution on this worker. */
static void *worker_request(void *ctx)
{
    orb_sound_set_volume(*(unsigned *)ctx);
    return NULL;
}

static void save_on_device(void)
{
    orb_sound_preferences_t p;
    orb_sound_preferences(&p);
    CHECK(p.saving && !p.save_error);
    int64_t due = orb_sound_preferences_service(s_now);
    CHECK(due == s_now + 500000);
    CHECK(orb_sound_preferences_service(due - 1) == due);
    s_now = due;
    CHECK(orb_sound_preferences_service(s_now) == INT64_MAX);
    orb_sound_preferences(&p);
    CHECK(!p.saving);
}

static void worker_handoff_contract(void)
{
    unsigned opens = s_opens, commits = s_commits, volume = 18;
    pthread_t worker;
    CHECK(!pthread_create(&worker, NULL, worker_request, &volume));
    CHECK(!pthread_join(worker, NULL));
    CHECK(s_opens == opens && s_commits == commits && orb_sound_volume() == volume);
    CHECK(orb_sound_preferences_service(s_now) == s_now + 500000);
    s_now += 200000;
    volume = 37;
    CHECK(!pthread_create(&worker, NULL, worker_request, &volume));
    CHECK(!pthread_join(worker, NULL));
    CHECK(s_opens == opens && s_commits == commits);
    /* Another edit starts a fresh half second; nothing commits mid-drag. */
    CHECK(orb_sound_preferences_service(s_now) == s_now + 500000);
    CHECK(orb_sound_preferences_service(s_now + 499999) == s_now + 500000);
    s_now += 500000;
    CHECK(orb_sound_preferences_service(s_now) == INT64_MAX);
    CHECK(s_commits == commits + 1 && (s_disk & 127u) == volume);
    restart();
    CHECK(orb_sound_volume() == volume);
}

static void racing_completion_contract(void)
{
    for (unsigned failed = 0; failed < 2; failed++) {
        orb_sound_set_volume(22);
        CHECK(orb_sound_preferences_service(s_now) == s_now + 500000);
        s_now += 500000;
        s_fail_commit = failed;
        s_edit_during_commit = true;
        s_commit_elapsed = 200000;
        int64_t began = s_now;
        CHECK(orb_sound_preferences_service(began) == began);
        CHECK(s_now == began + s_commit_elapsed);
        /* Blocking flash time cannot count toward a racing edit's debounce. */
        CHECK(orb_sound_preferences_service(s_now) == s_now + 500000);
        CHECK(orb_sound_preferences_service(s_now + 499999) == s_now + 500000);
        orb_sound_preferences_t p;
        orb_sound_preferences(&p);
        CHECK(p.volume == 64 && p.saving && !p.save_error);
        s_fail_commit = false;
        s_now += 500000;
        CHECK(orb_sound_preferences_service(s_now) == INT64_MAX);
        orb_sound_preferences(&p);
        CHECK(!p.saving && !p.save_error);
        restart();
        CHECK(orb_sound_volume() == 64);
    }
}

int main(void)
{
    s_main = pthread_self();
    restart();
    worker_handoff_contract();
    racing_completion_contract();
    s_present = false;
    restart();
    orb_sound_preferences_t p;
    orb_sound_preferences(&p);
    CHECK(!p.silent && p.spoken_replies && p.volume == 60 && !p.quiet_enabled);

    for (unsigned flags = 0; flags < 8; flags++) {
        p.volume = flags ? 47 : 0;
        p.quiet_enabled = flags & 1;
        p.spoken_replies = flags & 2;
        p.silent = flags & 4;
        p.quiet_start = 23;
        p.quiet_end = 0;
        CHECK(orb_sound_set_preferences(&p));
        save_on_device();
        unsigned commits = s_commits;
        CHECK(orb_sound_preferences_service(s_now + 1000000) == INT64_MAX);
        CHECK(s_commits == commits); /* already saved: no flash wear */
        restart();
        orb_sound_preferences_t read;
        orb_sound_preferences(&read);
        CHECK(read.volume == p.volume && read.quiet_enabled == p.quiet_enabled &&
              read.spoken_replies == p.spoken_replies && read.silent == p.silent &&
              read.quiet_start == 23 && read.quiet_end == 0 && !read.saving && !read.save_error);
        CHECK(atomic_load(&s_silent) == p.silent);
    }

    /* Legacy values have no Silent bit. They retain volume/speech/hours. */
    s_disk = 31u | 128u | (21u << 8) | (6u << 13) | (1u << 18);
    restart();
    orb_sound_preferences(&p);
    CHECK(!p.silent && !p.spoken_replies && p.quiet_enabled && p.volume == 31 &&
          p.quiet_start == 21 && p.quiet_end == 6);
    orb_sound_set_silent(true);
    orb_sound_set_volume(100);
    unsigned generation = atomic_load(&s_preference_generation);
    orb_sound_set_silent(true);
    CHECK(generation == atomic_load(&s_preference_generation));
    orb_sound_set_muted(false, true);
    CHECK(generation == atomic_load(&s_preference_generation));
    save_on_device();
    restart();
    orb_sound_preferences(&p);
    CHECK(p.silent && p.volume == 100 && !p.spoken_replies && p.quiet_enabled &&
          p.quiet_start == 21 && p.quiet_end == 6);

    uint32_t good = s_disk;
    for (unsigned failure = 0; failure < 3; failure++) {
        orb_sound_set_silent(false);
        s_fail_open = failure == 0;
        s_fail_write = failure == 1;
        s_fail_commit = failure == 2;
        save_on_device();
        orb_sound_preferences(&p);
        CHECK(p.save_error && !p.saving && s_disk == good);
        unsigned opens = s_opens;
        CHECK(orb_sound_preferences_service(s_now + 1000000) == INT64_MAX);
        CHECK(s_opens == opens); /* An error stays visible until a new request. */
        s_fail_open = s_fail_write = s_fail_commit = false;
        restart();
        orb_sound_preferences(&p);
        CHECK(p.silent);
    }

    const uint32_t invalid[] = {101u, 60u | (24u << 8), 60u | (24u << 13)};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(*invalid); i++) {
        s_disk = invalid[i] | PREFERENCE_SILENT;
        restart();
        orb_sound_preferences(&p);
        CHECK(p.volume == 60 && !p.silent && p.quiet_start == 22 && p.quiet_end == 7);
    }
    CHECK(s_commits && s_opens);
    printf("orb_sound preference storage: %u checks passed\n", s_checks);
    return 0;
}
