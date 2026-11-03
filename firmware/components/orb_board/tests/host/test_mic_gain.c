#include "orb_mic_levels.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_CODEC_DEV_OK 0
#define ESP_FAIL 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_RETURN_ON_FALSE(condition, error, ...) do { if (!(condition)) return (error); } while (0)
typedef struct audio_codec_if audio_codec_if_t;
struct audio_codec_if {
    int (*get_reg)(const audio_codec_if_t *, int, int *);
    int (*set_reg)(const audio_codec_if_t *, int, int);
};
static struct { const audio_codec_if_t *adc; } s_audio;
static unsigned s_step, s_fail, s_writes;
static unsigned char s_regs[256];

static int read_reg(const audio_codec_if_t *adc, int reg, int *value)
{
    assert(adc == s_audio.adc);
    if (++s_step == s_fail) return ESP_FAIL;
    *value = s_regs[reg];
    return ESP_OK;
}

static int write_reg(const audio_codec_if_t *adc, int reg, int value)
{
    assert(adc == s_audio.adc);
    if (++s_step == s_fail) return ESP_FAIL;
    assert(reg == 0x16 || reg == 0x1a || reg == 0x43 || reg == 0x44 || reg == 0x1e || reg == 0x1d);
    s_writes++;
    s_regs[reg] = value;
    return ESP_OK;
}

#include "mic_gain.inc"

static void reset(unsigned fail)
{
    memset(s_regs, 0xa5, sizeof(s_regs));
    s_regs[0x43] = 0xfa;
    s_regs[0x44] = 0x5a;
    s_regs[0x16] = 0xff;
    s_regs[0x1a] = 0xef;
    s_step = s_writes = 0;
    s_fail = fail;
}

int main(void)
{
    const audio_codec_if_t adc = {.get_reg = read_reg, .set_reg = write_reg};
    assert(orb_board_audio_set_mic_gain() == ESP_ERR_INVALID_STATE);
    s_audio.adc = &adc;
    reset(0);
    assert(orb_board_audio_set_mic_gain() == ESP_OK);
    assert(s_step == 10 && s_writes == 6);
    assert(s_regs[0x43] == 0xfd && s_regs[0x44] == 0x5d); /* PGA36, selection preserved. */
    assert(s_regs[0x1e] == 0xdf && s_regs[0x1d] == 0xdf); /* ADC1/2 +16 dB. */
    assert(s_regs[0x16] == 0xf3); /* ADC12 ALC off, ADC34/reserved preserved. */
    assert(s_regs[0x1a] == 0x9e); /* Soft ramp, independent ADC12 gain. */
    assert(s_regs[0x1b] == 0xa5 && s_regs[0x1c] == 0xa5 && s_regs[0x45] == 0xa5 && s_regs[0x46] == 0xa5);
    for (unsigned failure = 1; failure <= 10; ++failure) {
        reset(failure);
        assert(orb_board_audio_set_mic_gain() == ESP_FAIL);
        assert(s_step == failure);
        if (failure <= 4) assert(s_writes == 0);
        reset(0);
        assert(orb_board_audio_set_mic_gain() == ESP_OK);
    }
    puts("ES7210 gain: both channels, preserved bits, all 10 failure stages and retry passed");
}
