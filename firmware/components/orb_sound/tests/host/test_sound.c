#include "orb_sound_core.h"
#include "orb_sound_save.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static unsigned s_checks;
#define CHECK(c) do { s_checks++; if (!(c)) { \
    fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)

static void pcm_contract(void)
{
    CHECK(orb_sound_pcm_init());
    int16_t *pcm = malloc(2 * ORB_SOUND_SAMPLE_RATE * sizeof(*pcm));
    CHECK(pcm);
    int16_t chunk[ORB_SOUND_CHUNK_SAMPLES];
    const unsigned milliseconds[] = {8,12,90,90,240,180,120,70,70,160,10,10,2000,2000,60,80,45,35,90};
    for (int cue = 0; cue < ORB_SOUND_CUE_COUNT; cue++) {
        size_t count = orb_sound_pcm_count(cue);
        CHECK(count == milliseconds[cue] * 32);
        CHECK(orb_sound_pcm(cue, 0, pcm, 2 * ORB_SOUND_SAMPLE_RATE) == count);
        CHECK(pcm[0] == 0 && pcm[count - 1] == 0);
        int64_t dc = 0;
        unsigned peak = 0;
        for (size_t i = 0; i < count; i++) {
            unsigned magnitude = (unsigned)abs(pcm[i]);
            if (magnitude > peak) peak = magnitude;
            dc += pcm[i];
        }
        CHECK(dc == 0 && peak == 29204);
        for (size_t at = 0; at < count;) {
            size_t n = orb_sound_pcm(cue, at, chunk, ORB_SOUND_CHUNK_SAMPLES);
            CHECK(n && !memcmp(pcm + at, chunk, n * sizeof(*pcm)));
            at += n;
        }
        CHECK(!orb_sound_pcm(cue, count, chunk, ORB_SOUND_CHUNK_SAMPLES));
    }
    CHECK(!orb_sound_pcm_count(-1) && !orb_sound_pcm_count(ORB_SOUND_CUE_COUNT));
    CHECK(!orb_sound_pcm(ORB_SOUND_TAP, 0, NULL, 12));
    free(pcm);
}

static void mixer_contract(void)
{
    orb_sound_core_t core;
    int16_t mixed[ORB_SOUND_CHUNK_SAMPLES], reference[ORB_SOUND_CHUNK_SAMPLES];
    orb_sound_core_init(&core);
    CHECK(orb_sound_core_queue(&core, ORB_SOUND_DETENT));
    CHECK(orb_sound_core_queue(&core, ORB_SOUND_DETENT));
    CHECK(!orb_sound_core_queue(&core, ORB_SOUND_TAP));
    CHECK(orb_sound_core_render(&core, mixed, ORB_SOUND_CHUNK_SAMPLES, false) == 2);
    CHECK(orb_sound_pcm(ORB_SOUND_DETENT, 0, reference, ORB_SOUND_CHUNK_SAMPLES));
    for (size_t i = 0; i < ORB_SOUND_CHUNK_SAMPLES; i++)
        CHECK(mixed[i] == reference[i] / 2 + ((int32_t)reference[i] * 99 / 100) / 2);
    orb_sound_core_render(&core, mixed, ORB_SOUND_CHUNK_SAMPLES, false);
    CHECK(!orb_sound_core_render(&core, mixed, ORB_SOUND_CHUNK_SAMPLES, false));
    for (size_t i = 0; i < ORB_SOUND_CHUNK_SAMPLES; i++) CHECK(!mixed[i]);

    orb_sound_core_init(&core);
    CHECK(orb_sound_core_queue(&core, ORB_SOUND_TAP));
    CHECK(orb_sound_core_render(&core, mixed, ORB_SOUND_CHUNK_SAMPLES, true) == 1);
    CHECK(orb_sound_pcm(ORB_SOUND_TAP, 0, reference, ORB_SOUND_CHUNK_SAMPLES));
    for (size_t i = 0; i < ORB_SOUND_CHUNK_SAMPLES; i++)
        CHECK(mixed[i] == (int32_t)reference[i] * 3277 / 32768);

    orb_sound_core_mute(&core, true);
    CHECK(!orb_sound_core_queue(&core, ORB_SOUND_TAP));
    CHECK(orb_sound_core_queue(&core, ORB_SOUND_ALARM));
    orb_sound_core_render(&core, mixed, ORB_SOUND_CHUNK_SAMPLES, false);
    CHECK(core.voices[0].active && !core.voices[1].active);
    for (unsigned i = 0; i < 4100; i++) orb_sound_core_render(&core, mixed, ORB_SOUND_CHUNK_SAMPLES, false);
    CHECK(core.voices[0].active && core.voices[0].age > 20 * ORB_SOUND_SAMPLE_RATE);
    size_t at = core.voices[0].offset;
    orb_sound_core_render(&core, mixed, ORB_SOUND_CHUNK_SAMPLES, false);
    orb_sound_pcm_variant(ORB_SOUND_ALARM, at, reference, ORB_SOUND_CHUNK_SAMPLES,
                           core.voices[0].variation);
    for (size_t i = 0; i < ORB_SOUND_CHUNK_SAMPLES; i++) CHECK(abs(mixed[i] - reference[i] / 2) <= 1);
    orb_sound_core_stop(&core, ORB_SOUND_ALARM);
    CHECK(!orb_sound_core_render(&core, mixed, ORB_SOUND_CHUNK_SAMPLES, false));
    CHECK(orb_sound_core_queue(&core, ORB_SOUND_TIMER));
    atomic_store(&core.hard_muted, true);
    CHECK(!orb_sound_core_queue(&core, ORB_SOUND_ALARM));
    CHECK(!orb_sound_core_render(&core, mixed, ORB_SOUND_CHUNK_SAMPLES, false));
    atomic_store(&core.hard_muted, false);
    atomic_store(&core.capture_waiting, true);
    CHECK(!orb_sound_core_queue(&core, ORB_SOUND_TIMER));
    CHECK(!orb_sound_core_render(&core, mixed, ORB_SOUND_CHUNK_SAMPLES + 1, false));
    /* A toggle that begins and ends between worker blocks still cancels cues. */
    orb_sound_core_init(&core);
    CHECK(orb_sound_core_queue(&core, ORB_SOUND_OPEN));
    orb_sound_core_mute(&core, true); orb_sound_core_mute(&core, false);
    CHECK(!orb_sound_core_render(&core, mixed, ORB_SOUND_CHUNK_SAMPLES, false));
    CHECK(orb_sound_core_queue(&core, ORB_SOUND_DONE));
    CHECK(orb_sound_core_render(&core, mixed, ORB_SOUND_CHUNK_SAMPLES, false));
    orb_sound_core_mute(&core, true); orb_sound_core_mute(&core, false);
    CHECK(!orb_sound_core_render(&core, mixed, ORB_SOUND_CHUNK_SAMPLES, false));
    CHECK(orb_sound_core_queue(&core, ORB_SOUND_TIMER));
    atomic_store(&core.capture_active, true);
    CHECK(!orb_sound_core_render(&core, mixed, ORB_SOUND_CHUNK_SAMPLES, false));
    atomic_store(&core.capture_active, false);
    CHECK(!orb_sound_core_render(&core, mixed, ORB_SOUND_CHUNK_SAMPLES, false));
}

static void preview_contract(void)
{
    orb_sound_core_t core;
    int16_t mixed[ORB_SOUND_CHUNK_SAMPLES];
    for (int cue = 0; cue < ORB_SOUND_CUE_COUNT; cue++) {
        orb_sound_core_init(&core);
        CHECK(orb_sound_core_preview_at(&core, cue, 42));
        CHECK(orb_sound_core_render(&core, mixed, ORB_SOUND_CHUNK_SAMPLES, false) == 1);
        CHECK(core.voices[0].cue == (orb_sound_cue_t)cue && core.voices[0].preview);
        size_t blocks = (orb_sound_pcm_count(cue) + ORB_SOUND_CHUNK_SAMPLES - 1) / ORB_SOUND_CHUNK_SAMPLES;
        for (size_t i = 1; i < blocks; i++) orb_sound_core_render(&core, mixed, ORB_SOUND_CHUNK_SAMPLES, false);
        CHECK(!core.voices[0].active); /* Timer/alarm previews finish once. */
        CHECK(!orb_sound_core_render(&core, mixed, ORB_SOUND_CHUNK_SAMPLES, false));
    }
    orb_sound_core_init(&core);
    orb_sound_core_mute(&core, true);
    CHECK(!orb_sound_core_preview_at(&core, ORB_SOUND_ALARM, 0));
    CHECK(orb_sound_core_queue(&core, ORB_SOUND_ALARM));
    CHECK(orb_sound_core_render(&core, mixed, ORB_SOUND_CHUNK_SAMPLES, false) == 1);
    orb_sound_core_mute(&core, false);
    CHECK(orb_sound_core_preview_at(&core, ORB_SOUND_TIMER, 0));
    CHECK(orb_sound_core_preview_at(&core, ORB_SOUND_ALARM, 0));
    orb_sound_core_render(&core, mixed, ORB_SOUND_CHUNK_SAMPLES, false);
    CHECK(core.voices[0].active && core.voices[0].cue == ORB_SOUND_ALARM && !core.voices[0].preview);
    CHECK(core.voices[1].preview);
    orb_sound_core_mute(&core, true); orb_sound_core_mute(&core, false);
    orb_sound_core_render(&core, mixed, ORB_SOUND_CHUNK_SAMPLES, false);
    CHECK(core.voices[0].active && !core.voices[1].active);
    CHECK(orb_sound_core_preview_at(&core, ORB_SOUND_ALARM, 0));
    orb_sound_core_mute(&core, true); orb_sound_core_mute(&core, false);
    orb_sound_core_render(&core, mixed, ORB_SOUND_CHUNK_SAMPLES, false);
    CHECK(core.voices[0].active && !core.voices[1].active);
}

static void alert_recovery_contract(void)
{
    orb_sound_core_t core;
    int16_t pcm[ORB_SOUND_CHUNK_SAMPLES];
    orb_sound_core_init(&core);
    CHECK(orb_sound_core_try_lease(&core));
    CHECK(orb_sound_core_queue_at(&core, ORB_SOUND_ALARM, 123));
    CHECK(orb_sound_core_render(&core, pcm, ORB_SOUND_CHUNK_SAMPLES, false) == 1);
    uint64_t age = core.voices[0].age;
    size_t offset = core.voices[0].offset;
    unsigned variation = core.voices[0].variation;
    /* A failed TX closes/reopens hardware and releases this lease. It must
     * leave an already-started real alert looping until its explicit stop. */
    orb_sound_core_release(&core);
    orb_sound_core_mute(&core, true);
    CHECK(core.voices[0].active && core.voices[0].age == age && core.voices[0].offset == offset);
    CHECK(orb_sound_core_try_lease(&core));
    CHECK(orb_sound_core_render(&core, pcm, ORB_SOUND_CHUNK_SAMPLES, false) == 1);
    CHECK(core.voices[0].active && core.voices[0].cue == ORB_SOUND_ALARM && !core.voices[0].preview);
    CHECK(core.voices[0].age == age + ORB_SOUND_CHUNK_SAMPLES);
    CHECK(core.voices[0].offset == offset + ORB_SOUND_CHUNK_SAMPLES);
    CHECK(core.voices[0].variation == variation && !core.voices[0].began_this_block);
    orb_sound_core_stop(&core, ORB_SOUND_ALARM);
    CHECK(!orb_sound_core_render(&core, pcm, ORB_SOUND_CHUNK_SAMPLES, false));
}

static void malformed_contract(void)
{
    unsigned char header[24 + 16 * ORB_SOUND_CUE_COUNT] = {0};
    CHECK(!orb_sound_pcm_bind(NULL, sizeof(header)));
    CHECK(!orb_sound_pcm_bind(header, sizeof(header)));
    FILE *file = fopen("tools/sounds/out/palette.orbs", "rb");
    if (!file) file = fopen("../../../../../tools/sounds/out/palette.orbs", "rb");
    CHECK(file);
    CHECK(!fseek(file, 0, SEEK_END)); size_t bytes = (size_t)ftell(file); rewind(file);
    unsigned char *copy = malloc(bytes); CHECK(copy);
    CHECK(fread(copy, 1, bytes, file) == bytes); fclose(file);
    const unsigned offsets[] = {0, 8, 12, 16, 20, 24, 28, 32, 36};
    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        unsigned offset = offsets[i];
        unsigned char saved = copy[offset]; copy[offset] ^= 0x80;
        CHECK(!orb_sound_pcm_bind(copy, bytes));
        copy[offset] = saved;
    }
    copy[28] = 0; copy[29] = 0; copy[30] = 0; copy[31] = 0;
    CHECK(!orb_sound_pcm_bind(copy, bytes)); /* points inside directory */
    free(copy);
}

static void preferences_contract(void)
{
    CHECK(!orb_sound_quiet_at(false, 22, 7, 23));
    CHECK(!orb_sound_quiet_at(true, 22, 7, -1));
    CHECK(orb_sound_quiet_at(true, 22, 7, 22));
    CHECK(orb_sound_quiet_at(true, 22, 7, 0));
    CHECK(!orb_sound_quiet_at(true, 22, 7, 7));
    CHECK(orb_sound_quiet_at(true, 8, 8, 14));
    CHECK(!orb_sound_quiet_at(true, 25, 8, 14));
    CHECK(orb_sound_audio_acquire());
    CHECK(orb_sound_init() && orb_sound_init());
    CHECK(!orb_sound_audio_acquire()); /* startup cannot reset an earlier capture lease */
    orb_sound_audio_release();
    orb_sound_set_volume(500); CHECK(orb_sound_volume() == 100);
    orb_sound_set_volume(35); CHECK(orb_sound_volume() == 35);
    orb_sound_preferences_t read; orb_sound_preferences(&read);
    CHECK(!read.saving && !read.save_error && read.volume == 35);
    orb_sound_set_silent(true);
    orb_sound_preferences(&read); CHECK(read.silent && !read.saving && !read.save_error);
    orb_sound_set_volume(64);
    orb_sound_preferences(&read); CHECK(read.silent && read.volume == 64);
    CHECK(!orb_sound_play(ORB_SOUND_TAP)); CHECK(orb_sound_play(ORB_SOUND_TIMER));
    CHECK(!orb_sound_speech_allowed());
    orb_sound_set_silent(false);
    orb_sound_preferences(&read); CHECK(!read.silent);
    /* Always On and face-down drop ordinary cues and speech, never an alarm. */
    orb_sound_set_always_on(true);
    CHECK(!orb_sound_play(ORB_SOUND_TAP)); CHECK(orb_sound_play(ORB_SOUND_ALARM));
    CHECK(!orb_sound_speech_allowed());
    orb_sound_set_always_on(false);
    orb_sound_set_muted(false, true);
    orb_sound_preferences(&read); CHECK(!read.silent);
    CHECK(!orb_sound_play(ORB_SOUND_TAP)); CHECK(orb_sound_play(ORB_SOUND_ALARM));
    CHECK(!orb_sound_speech_allowed());
    orb_sound_set_muted(false, false);
    orb_sound_stop_all();
    CHECK(orb_sound_audio_acquire()); CHECK(!orb_sound_play(ORB_SOUND_TIMER));
    CHECK(!orb_sound_audio_acquire()); orb_sound_audio_release();
    CHECK(orb_sound_play(ORB_SOUND_TAP));
    orb_sound_set_volume(0); CHECK(!orb_sound_play(ORB_SOUND_ALARM));
    CHECK(!orb_sound_set_preferences(NULL));
    read.volume = 101; CHECK(!orb_sound_set_preferences(&read));
}

static atomic_uint s_active, s_violations, s_leases;
static orb_sound_core_t s_shared;
static void *lease_thread(void *ctx)
{
    (void)ctx;
    for (unsigned i = 0; i < 10000; i++) {
        if (!orb_sound_core_try_lease(&s_shared)) continue;
        if (atomic_fetch_add(&s_active, 1) != 0) atomic_fetch_add(&s_violations, 1);
        atomic_fetch_add(&s_leases, 1);
        if (atomic_fetch_sub(&s_active, 1) != 1) atomic_fetch_add(&s_violations, 1);
        orb_sound_core_release(&s_shared);
    }
    return NULL;
}

static void concurrent_lease_contract(void)
{
    orb_sound_core_init(&s_shared);
    pthread_t threads[4];
    for (unsigned i = 0; i < 4; i++) CHECK(!pthread_create(&threads[i], NULL, lease_thread, NULL));
    for (unsigned i = 0; i < 4; i++) CHECK(!pthread_join(threads[i], NULL));
    CHECK(!atomic_load(&s_violations) && atomic_load(&s_leases));
}

static void save_wait_contract(void)
{
    orb_sound_save_t save = {0};
    CHECK(orb_sound_save_wait(&save, 0, 0, 1000000) == UINT32_MAX);
    CHECK(orb_sound_save_wait(&save, 1, 0, 1000000) == 500);
    CHECK(orb_sound_save_wait(&save, 1, 0, 1200000) == 300);
    CHECK(orb_sound_save_wait(&save, 2, 0, 1200000) == 500);
    CHECK(orb_sound_save_wait(&save, 2, 0, 1699999) == 1);
    CHECK(orb_sound_save_wait(&save, 2, 0, 1700000) == 0);
    CHECK(orb_sound_save_wait(&save, 2, 2, 1700000) == UINT32_MAX);
    /* A change during a commit is a new pending generation, never idle. */
    CHECK(orb_sound_save_wait(&save, 3, 2, 1700001) == 500);
}

int main(void)
{
    save_wait_contract();
    pcm_contract(); malformed_contract(); alert_recovery_contract(); mixer_contract(); preview_contract(); preferences_contract(); concurrent_lease_contract();
    printf("orb_sound: %u checks passed\n", s_checks);
    return 0;
}
