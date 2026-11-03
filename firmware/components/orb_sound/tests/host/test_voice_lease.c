/* The actual voice constructor/close, with a fake codec/board. This catches
 * missed lease release on any setup failure as well as accidental cue overlap. */
#include "orb_sound.h"
#include "../../../orb_voice/mic_dsp.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned s_checks, s_opens, s_deletes, s_begin, s_end, s_stage;
static bool s_fail_new, s_owned;
#define CHECK(c) do { s_checks++; if (!(c)) { \
    fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#define ESP_OK 0
#define ESP_CODEC_DEV_OK 0
#define ORB_MEM_VOICE_START 0
#define TAG "voice-test"
#define ESP_LOGW(...) ((void)0)
typedef int esp_err_t;
typedef void *esp_codec_dev_handle_t;
typedef struct { unsigned sample_rate, bits_per_sample, channel, channel_mask; } esp_codec_dev_sample_info_t;
static esp_codec_dev_handle_t s_speaker, s_mic;
static mic_dsp_t s_capture_dsp;
#define ORB_SPEECH_STOP 0
static void orb_voice_speech_stop(int reason) { CHECK(reason == ORB_SPEECH_STOP); }
static void mic_dsp_reset(mic_dsp_t *dsp) { *dsp = (mic_dsp_t){0}; }
#define mic_dsp_init mic_dsp_reset

static void orb_mem_begin(int phase) { (void)phase; s_begin++; }
static void orb_mem_end(int phase) { (void)phase; s_end++; }
static int orb_board_audio_new(unsigned rate, void **speaker, void **mic)
{
    CHECK(rate == 16000 && !s_owned);
    s_opens++;
    *speaker = *mic = NULL;
    if (s_fail_new) return 1;
    *speaker = (void *)1; *mic = (void *)2;
    s_owned = true;
    return 0;
}
static int orb_board_audio_delete(void *speaker, void *mic)
{
    CHECK(s_owned && speaker == (void *)1 && mic == (void *)2);
    s_owned = false;
    s_deletes++;
    return 0;
}
static int esp_codec_dev_open(void *mic, esp_codec_dev_sample_info_t *format)
{
    CHECK(s_owned && mic == s_mic && format->channel == 2 && format->channel_mask == 3 && format->sample_rate == 16000);
    return s_stage == 1;
}
static int orb_board_audio_set_mic_gain(void)
{
    CHECK(s_owned && s_mic == (void *)2);
    return s_stage == 2;
}

#include "voice_audio.inc"

int main(void)
{
    CHECK(orb_sound_init());
    for (unsigned failure = 0; failure < 3; failure++) {
        s_fail_new = failure == 0;
        s_stage = failure;
        CHECK(!audio_open(NULL));
        CHECK(!s_owned && !s_speaker && !s_mic && s_begin == s_end);
        CHECK(orb_sound_audio_acquire());
        orb_sound_audio_release();
    }
    CHECK(s_opens == 3 && s_deletes == 2);
    s_fail_new = false; s_stage = 0;
    CHECK(audio_open(NULL));
    CHECK(s_owned && s_begin == s_end);
    CHECK(!orb_sound_play(ORB_SOUND_START));
    CHECK(!orb_sound_audio_acquire());
    audio_close(NULL);
    CHECK(!s_owned && !s_speaker && !s_mic);
    CHECK(s_opens == 4 && s_deletes == 3);
    CHECK(orb_sound_play(ORB_SOUND_STOP));
    audio_close(NULL);
    CHECK(s_deletes == 3);
    printf("orb_sound voice lease: %u checks passed\n", s_checks);
    return 0;
}
