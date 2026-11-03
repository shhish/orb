#include "speech_core.h"
#include "mic_dsp.h"
#include "orb_mic_levels.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static speech_core_t s_speech;
static unsigned s_checks;
#define CHECK(x) do { ++s_checks; assert(x); } while (0)

static bool packet(uint32_t sequence, unsigned samples, int16_t value, uint64_t now)
{
    uint8_t frame[1031] = {2, 0, 1};
    frame[3] = sequence >> 24; frame[4] = sequence >> 16;
    frame[5] = sequence >> 8; frame[6] = sequence;
    for (unsigned i = 0; i < samples; ++i) {
        frame[7 + i * 2] = (uint16_t)value;
        frame[8 + i * 2] = (uint16_t)value >> 8;
    }
    return speech_core_packet(&s_speech, frame, 7 + samples * 2, now);
}

static void begin(uint32_t rate)
{
    speech_core_init(&s_speech);
    CHECK(speech_core_begin(&s_speech, 1, rate, "Spoken caption", 0));
}

static void jitter(void)
{
    int16_t out[320];
    speech_core_init(&s_speech);
    CHECK(!speech_core_begin(&s_speech, 1, 16000, "\xc0\x80", 0));
    CHECK(!speech_core_begin(&s_speech, 1, 16000, "\xed\xa0\x80", 0));
    CHECK(!speech_core_begin(&s_speech, 1, 32000, "caption", 0));
    begin(16000);
    for (unsigned i = 0; i < 7; ++i) CHECK(packet(i, 320, 1234, i * 20));
    CHECK(speech_core_read(&s_speech, out, 320, 32000, 140) == 320);
    CHECK(!s_speech.playing && s_speech.count == 2240 && !out[0]);
    CHECK(packet(7, 320, 1234, 140));
    for (unsigned i = 0; i < 16; ++i) {
        CHECK(speech_core_read(&s_speech, out, 320, 32000, 150 + i * 10) == 320);
        CHECK(out[0] == 1234 && out[319] == 1234);
    }
    CHECK(s_speech.count == 0);
    speech_core_read(&s_speech, out, 320, 32000, 320);
    CHECK(!s_speech.playing && s_speech.active && s_speech.underruns == 1 && out[0] == 0);
    CHECK(packet(8, 320, 2000, 330));
    speech_core_read(&s_speech, out, 320, 32000, 340);
    CHECK(!s_speech.playing && out[0] == 0);
    CHECK(speech_core_end(&s_speech, 1, 9));
    for (unsigned i = 0; i < 3; ++i) speech_core_read(&s_speech, out, 320, 32000, 350 + i * 10);
    CHECK(!s_speech.active && s_speech.stopped && s_speech.reason == SPEECH_COMPLETE);
    CHECK(!strcmp(s_speech.caption, "Spoken caption"));
    CHECK(!speech_core_begin(&s_speech, 1, 24000, "old stream", 400));
    CHECK(speech_core_begin(&s_speech, 2, 24000, "next stream", 400));
    speech_core_read(&s_speech, out, 320, 32000, 1400);
    CHECK(!s_speech.active && s_speech.reason == SPEECH_UNDERRUN);

    begin(24000);
    CHECK(packet(0, 240, -2000, 0));
    CHECK(speech_core_end(&s_speech, 1, 1));
    CHECK(speech_core_read(&s_speech, out, 320, 32000, 1) == 320);
    CHECK(out[0] == -2000 && out[319] == -2000 && s_speech.count == 0);
    speech_core_read(&s_speech, out, 320, 32000, 11);
    CHECK(s_speech.reason == SPEECH_COMPLETE);

    begin(24000);
    CHECK(packet(0, 320, 1000, 0));
    CHECK(!packet(0, 320, 1000, 10));
    CHECK(s_speech.reason == SPEECH_BAD_AUDIO);
    begin(16000);
    CHECK(!packet(1, 320, 1000, 0));
    CHECK(s_speech.reason == SPEECH_BAD_AUDIO);
    begin(16000);
    CHECK(packet(0, 320, 1000, 0));
    CHECK(!speech_core_end(&s_speech, 1, 2));
    CHECK(s_speech.reason == SPEECH_BAD_AUDIO);
    begin(16000);
    for (unsigned i = 0; i < 25; ++i) CHECK(packet(i, 320, 1000, i));
    CHECK(!packet(25, 320, 1000, 25));
    CHECK(s_speech.reason == SPEECH_OVERFLOW && !s_speech.count);
    begin(16000);
    speech_core_stop(&s_speech, SPEECH_DOUBLE_TAP);
    CHECK(!s_speech.active && s_speech.reason == SPEECH_DOUBLE_TAP);
    CHECK(!strcmp(speech_stop_name(s_speech.reason), "double_tap"));

    /* An ended reply must not play stale buffered audio after output stalls.
     * A codec/lease failure can prevent the reader from running at all. */
    begin(24000);
    CHECK(packet(0, 240, 2000, 0));
    CHECK(speech_core_end(&s_speech, 1, 1));
    CHECK(speech_core_read(&s_speech, out, 320, 32000, 1000) == 0);
    CHECK(!s_speech.active && s_speech.reason == SPEECH_UNDERRUN && !out[0]);
    begin(16000);
    speech_core_tick(&s_speech, 999);
    CHECK(s_speech.active);
    speech_core_tick(&s_speech, 1000);
    CHECK(!s_speech.active && s_speech.reason == SPEECH_UNDERRUN);
    begin(24000);
    CHECK(packet(0, 240, 2000, 10));
    CHECK(speech_core_end(&s_speech, 1, 1));
    speech_core_tick(&s_speech, 1010);
    CHECK(!s_speech.active && s_speech.reason == SPEECH_UNDERRUN);
}

/* The board's first capture samples are a codec pop on a DC offset. */
static void capture_start_is_quiet(void)
{
    mic_dsp_t dsp;
    mic_dsp_init(&dsp);
    int16_t stereo[640], mono[320];
    for (unsigned i = 0; i < 320; ++i) stereo[2 * i] = stereo[2 * i + 1] = 1200;
    stereo[0] = stereo[1] = 32767; stereo[2] = stereo[3] = -30000;
    mic_dsp_mono(&dsp, stereo, mono, 320, 16000);
    int peak = 0;
    for (unsigned i = 0; i < 320; ++i) peak = abs(mono[i]) > peak ? abs(mono[i]) : peak;
    CHECK(peak < 3277); /* below -20 dBFS: no full-scale click opens a recording */
}

static void microphones(void)
{
    mic_dsp_t dsp;
    mic_dsp_init(&dsp);
    int16_t stereo[640], mono[320];
    for (unsigned block = 0; block < 120; ++block) {
        for (unsigned i = 0; i < 320; ++i) {
            stereo[2 * i] = 1000;
            stereo[2 * i + 1] = 1000;
        }
        mic_dsp_mono(&dsp, stereo, mono, 320, 16000);
    }
    CHECK(abs(mono[319]) < 3); /* DC removal on both channels. */
    mic_dsp_init(&dsp);
    for (unsigned i = 0; i < 320; ++i) {
        stereo[2 * i] = (int16_t)(4000 * sinf(i * 0.4f));
        stereo[2 * i + 1] = -stereo[2 * i];
    }
    mic_dsp_mono(&dsp, stereo, mono, 320, 16000);
    for (unsigned i = 0; i < 320; ++i) CHECK(mono[i] == 0); /* Both slots contribute equally. */
    mic_dsp_init(&dsp);
    /* Speaker echo alone is below its learned envelope budget. */
    for (unsigned i = 0; i < 640; ++i) stereo[i] = (i & 1) ? -400 : 400;
    for (unsigned block = 0; block < 60; ++block)
        CHECK(!mic_dsp_interrupt(&dsp, stereo, 320, 16000, 0.1f, true));
    /* A single typing click cannot interrupt. */
    for (unsigned i = 0; i < 640; ++i) stereo[i] = 6000;
    CHECK(!mic_dsp_interrupt(&dsp, stereo, 320, 16000, 0.1f, true));
    for (unsigned i = 0; i < 640; ++i) stereo[i] = 400;
    CHECK(!mic_dsp_interrupt(&dsp, stereo, 320, 16000, 0.1f, true));
    for (unsigned i = 0; i < 640; ++i) stereo[i] = 3000;
    for (unsigned block = 0; block < 5; ++block)
        CHECK(!mic_dsp_interrupt(&dsp, stereo, 320, 16000, 0.1f, true));
    CHECK(mic_dsp_interrupt(&dsp, stereo, 320, 16000, 0.1f, true));
    CHECK(!mic_dsp_interrupt(&dsp, stereo, 320, 16000, 0, false));

    mic_dsp_init(&dsp);
    /* The reviewed -31 dBFS raw peak becomes -9 dBFS after +22 dB. Check
     * capture retains target headroom through filters, mixing and suppression. */
    int peak = 0;
    for (unsigned block = 0; block < 20; ++block) {
        for (unsigned i = 0; i < 320; ++i)
            stereo[2 * i] = stereo[2 * i + 1] =
                (int16_t)(924.0f * ORB_MIC_LEVEL_SCALE * sinf(i * 0.39269908f));
        mic_dsp_mono(&dsp, stereo, mono, 320, 16000);
        for (unsigned i = 0; i < 320; ++i)
            if (abs(mono[i]) > peak) peak = abs(mono[i]);
    }
    CHECK(peak > 8230 && peak < 16422); /* -12 to -6 dBFS, no saturation. */
    CHECK(dsp.noise >= 0.0003f * ORB_MIC_LEVEL_SCALE);

    mic_dsp_init(&dsp);
    /* Echo calibration uses amplified mic units and unchanged output units. */
    for (unsigned i = 0; i < 640; ++i) stereo[i] = 200;
    for (unsigned block = 0; block < 60; ++block)
        CHECK(!mic_dsp_interrupt(&dsp, stereo, 320, 16000, 0.1f, true));
    for (unsigned i = 0; i < 640; ++i) stereo[i] = 12000;
    CHECK(!mic_dsp_interrupt(&dsp, stereo, 320, 16000, 0.1f, true));
    for (unsigned i = 0; i < 640; ++i) stereo[i] = 200;
    CHECK(!mic_dsp_interrupt(&dsp, stereo, 320, 16000, 0.1f, true));
    /* -52 dBFS input RMS, then +22 dB: about -30 dBFS / 1032 counts. */
    for (unsigned i = 0; i < 640; ++i) stereo[i] = (int16_t)(82 * ORB_MIC_LEVEL_SCALE);
    for (unsigned block = 0; block < 5; ++block)
        CHECK(!mic_dsp_interrupt(&dsp, stereo, 320, 16000, 0.1f, true));
    CHECK(mic_dsp_interrupt(&dsp, stereo, 320, 16000, 0.1f, true));

    mic_dsp_init(&dsp);
    /* Higher mic gain also raises speaker coupling; do not retain the old
     * coupling clamp and mistake this steady echo for sustained speech. */
    for (unsigned i = 0; i < 640; ++i) stereo[i] = 9830;
    for (unsigned block = 0; block < 60; ++block)
        CHECK(!mic_dsp_interrupt(&dsp, stereo, 320, 16000, 0.01f, true));
}

int main(void)
{
    jitter();
    capture_start_is_quiet();
    microphones();
    printf("speech/dual-mic DSP: %u checks passed; state=%zu bytes, ring=%u bytes\n",
           s_checks, sizeof(s_speech), SPEECH_RING_SAMPLES * 2);
}
