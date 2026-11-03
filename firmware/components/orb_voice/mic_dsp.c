#include "mic_dsp.h"
#include "orb_mic_levels.h"

#include <math.h>
#include <string.h>

void mic_dsp_init(mic_dsp_t *s)
{
    memset(s, 0, sizeof(*s));
    /* Seed at the existing minimum, not a guessed room level. The measured
     * -52 dBFS speech RMS was below the old -32 dBFS interrupt floor. */
    s->noise = 0.0003f * ORB_MIC_LEVEL_SCALE;
    s->gain = 1.0f;
}

void mic_dsp_mono(mic_dsp_t *s, const int16_t *stereo, int16_t *mono, size_t frames, uint32_t rate)
{
    if (!stereo || !mono || !frames || !rate) return;
    float power = 0;
    for (size_t i = 0; i < frames * 2; ++i) {
        float v = stereo[i] / 32768.0f;
        power += v * v;
    }
    float rms = sqrtf(power / (float)(frames * 2));
    /* Track stationary room/keyboard noise slowly; speech attacks never raise
     * the floor quickly. The 18 dB floor avoids an unnatural hard gate. */
    float alpha = rms < s->noise * 1.8f ? 0.04f : 0.0002f;
    s->noise += alpha * (rms - s->noise);
    if (s->noise < 0.0003f * ORB_MIC_LEVEL_SCALE) s->noise = 0.0003f * ORB_MIC_LEVEL_SCALE;
    float gain = 1.0f - (1.4f * s->noise * s->noise) /
                        (rms * rms + 1e-9f * ORB_MIC_LEVEL_SCALE * ORB_MIC_LEVEL_SCALE);
    if (gain < 0.125f) gain = 0.125f;
    float dc_step = 1.0f - expf(-2.0f * 3.14159265f * 90.0f / rate);
    float low_step = 1.0f - expf(-2.0f * 3.14159265f * 6500.0f / rate);
    /* Capture starts with the codec's power-up pop and the mics' DC offset.
     * Seed the DC filter from the first samples instead of zero, and fade in
     * over 10 ms, so every recording no longer opens with a full-scale click. */
    if (!s->warm_samples) {
        s->dc[0] = stereo[0];
        s->dc[1] = stereo[1];
    }
    uint32_t fade = rate / 100;
    for (size_t i = 0; i < frames; ++i) {
        float mixed = 0;
        for (unsigned ch = 0; ch < 2; ++ch) {
            float v = stereo[2 * i + ch];
            s->dc[ch] += dc_step * (v - s->dc[ch]);
            s->low[ch] += low_step * (v - s->dc[ch] - s->low[ch]);
            mixed += s->low[ch] * 0.5f;
        }
        s->gain += 0.02f * (gain - s->gain);
        float output = mixed * s->gain;
        if (s->warm_samples < fade) output *= (float)s->warm_samples++ / (float)fade;
        mono[i] = output > 32767 ? 32767 : output < -32768 ? -32768 : (int16_t)output;
    }
}

bool mic_dsp_interrupt(mic_dsp_t *s, const int16_t *stereo, size_t frames,
                       uint32_t rate, float playback_envelope, bool playing)
{
    if (!playing || !stereo || !frames || !rate) {
        s->playback_ms = s->excess_ms = 0;
        s->echo_coupling = 0;
        return false;
    }
    float energy[2] = {0};
    for (size_t i = 0; i < frames; ++i)
        for (unsigned ch = 0; ch < 2; ++ch) {
            float v = stereo[2 * i + ch] / 32768.0f;
            energy[ch] += v * v;
        }
    float power = fminf(energy[0], energy[1]) / frames;
    float rms = sqrtf(power);
    uint32_t ms = (uint32_t)((frames * 1000u) / rate);
    if (!ms) ms = 1;
    s->playback_ms += ms;
    float reference = fmaxf(playback_envelope, 0.0001f);
    float coupling = rms / reference;
    if (s->playback_ms <= 200) {
        s->echo_coupling = fmaxf(s->echo_coupling, fminf(coupling, 8.0f * ORB_MIC_LEVEL_SCALE));
        return false;
    }
    /* A conservative 3 dB margin absorbs room echoes and envelope latency.
     * AEC is unavailable offline, so do not claim speaker cancellation. */
    float echo = s->echo_coupling * playback_envelope;
    float residual = fmaxf(0, power - 2.0f * echo * echo);
    /* -56.5 dBFS before the +22 dB profile (-34.5 after) gives the reviewed
     * -52 dBFS speech 4.5 dB margin. Still require both mics, learned echo
     * headroom, 4x noise amplitude and 120 ms sustained excess. */
    float floor = 0.0015f * ORB_MIC_LEVEL_SCALE;
    float threshold = fmaxf(floor * floor, 16.0f * s->noise * s->noise);
    if (residual > threshold) s->excess_ms += ms;
    else s->excess_ms = 0;
    if (residual <= threshold && coupling < s->echo_coupling)
        s->echo_coupling += 0.002f * (coupling - s->echo_coupling);
    return s->excess_ms >= 120;
}
