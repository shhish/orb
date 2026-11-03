#include "speech_core.h"

#include <string.h>

void speech_core_init(speech_core_t *s)
{
    memset(s, 0, sizeof(*s));
}

void speech_core_stop(speech_core_t *s, speech_stop_t reason)
{
    if (!s->active) return;
    s->active = s->playing = false;
    s->count = s->phase = 0;
    s->reason = reason;
    s->stopped = true;
}

bool speech_text_valid(const char *text, size_t max_bytes)
{
    if (!text || !text[0]) return false;
    size_t n = strlen(text);
    if (n > max_bytes) return false;
    for (size_t i = 0; i < n;) {
        uint8_t c = (uint8_t)text[i++];
        if (c < 0x80) continue;
        unsigned need;
        uint32_t value, minimum;
        if (c >= 0xc2 && c <= 0xdf) { need = 1; value = c & 31; minimum = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { need = 2; value = c & 15; minimum = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { need = 3; value = c & 7; minimum = 0x10000; }
        else return false;
        if (n - i < need) return false;
        while (need--) {
            c = (uint8_t)text[i++];
            if ((c & 0xc0) != 0x80) return false;
            value = (value << 6) | (c & 63);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
    }
    return true;
}

bool speech_core_begin(speech_core_t *s, uint16_t stream, uint32_t rate,
                       const char *caption, uint64_t now)
{
    if (!stream || stream <= s->last_stream || (rate != 16000 && rate != 24000) ||
        !speech_text_valid(caption, SPEECH_CAPTION_BYTES - 1)) return false;
    /* Caller retains the previous stream identity for an interrupted receipt. */
    s->head = s->count = 0;
    s->phase = s->next_sequence = 0;
    s->stream = s->last_stream = stream;
    s->rate = rate;
    s->active = true;
    s->playing = s->ended = s->stopped = false;
    s->underruns = 0;
    s->progress_ms = now;
    memcpy(s->caption, caption, strlen(caption) + 1);
    return true;
}

bool speech_core_packet(speech_core_t *s, const uint8_t *frame, size_t bytes, uint64_t now)
{
    if (!frame || bytes < 7) {
        speech_core_stop(s, SPEECH_BAD_AUDIO);
        return false;
    }
    uint16_t stream = ((uint16_t)frame[1] << 8) | frame[2];
    if (!s->active || stream != s->stream) return false;
    uint32_t sequence = ((uint32_t)frame[3] << 24) | ((uint32_t)frame[4] << 16) |
                        ((uint32_t)frame[5] << 8) | frame[6];
    if (frame[0] != 2 || bytes < 9 || bytes > 1031 || ((bytes - 7) & 1) ||
        sequence != s->next_sequence || sequence == UINT32_MAX || s->ended) {
        speech_core_stop(s, SPEECH_BAD_AUDIO);
        return false;
    }
    size_t n = (bytes - 7) / 2;
    if (n > s->rate / 2u - s->count) {
        speech_core_stop(s, SPEECH_OVERFLOW);
        return false;
    }
    size_t tail = (s->head + s->count) % SPEECH_RING_SAMPLES;
    for (size_t i = 0; i < n; ++i) {
        s->pcm[tail] = (int16_t)((uint16_t)frame[7 + 2 * i] | ((uint16_t)frame[8 + 2 * i] << 8));
        tail = (tail + 1) % SPEECH_RING_SAMPLES;
    }
    s->count += n;
    ++s->next_sequence;
    s->progress_ms = now;
    return true;
}

bool speech_core_end(speech_core_t *s, uint16_t stream, uint32_t chunks)
{
    if (!s->active || stream != s->stream) return false;
    if (chunks != s->next_sequence || !chunks) {
        speech_core_stop(s, SPEECH_BAD_AUDIO);
        return false;
    }
    s->ended = true;
    return true;
}

void speech_core_tick(speech_core_t *s, uint64_t now)
{
    /* The ring holds at most 500 ms. After a second without input it is stale,
     * including an ended reply whose output codec never became available. */
    if (s->active && now - s->progress_ms >= 1000)
        speech_core_stop(s, SPEECH_UNDERRUN);
}

size_t speech_core_read(speech_core_t *s, int16_t *out, size_t samples,
                        uint32_t output_rate, uint64_t now)
{
    if (!out || output_rate < 24000 || output_rate > 48000) return 0;
    memset(out, 0, samples * sizeof(*out));
    speech_core_tick(s, now);
    if (!s->active) return 0;
    if (!s->playing) {
        if (s->count < s->rate * 150u / 1000u && !s->ended) return samples;
        s->playing = true;
    }
    for (size_t i = 0; i < samples; ++i) {
        if (!s->count) {
            if (s->ended) speech_core_stop(s, SPEECH_COMPLETE);
            else {
                s->playing = false;
                ++s->underruns;
            }
            break;
        }
        int32_t a = s->pcm[s->head];
        int32_t b = s->count > 1 ? s->pcm[(s->head + 1) % SPEECH_RING_SAMPLES] : a;
        out[i] = (int16_t)(a + (int64_t)(b - a) * s->phase / output_rate);
        s->phase += s->rate;
        while (s->phase >= output_rate && s->count) {
            s->phase -= output_rate;
            s->head = (s->head + 1) % SPEECH_RING_SAMPLES;
            --s->count;
        }
    }
    return samples;
}

const char *speech_stop_name(speech_stop_t reason)
{
    static const char *const names[] = {"complete", "stop", "double_tap", "talk", "silent",
                                       "interrupted", "underrun", "overflow", "bad_audio"};
    return (unsigned)reason < sizeof(names) / sizeof(*names) ? names[reason] : "stop";
}
