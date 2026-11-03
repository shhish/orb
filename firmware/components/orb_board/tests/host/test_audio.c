/* Compile the production constructor with ownership-aware fake IDF/codec APIs.
 * A failed stage must return no handles, release every allocation, and permit
 * an immediate retry. Interface deletion never implicitly owns its children. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned checks;
#define CHECK(c) do { checks++; if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_NO_MEM 2
#define ESP_ERR_INVALID_STATE 4
#define ESP_FAIL 3
#define ESP_CODEC_DEV_OK 0
#define ESP_LOGW(...) ((void)0)
#define DRIVER_ERROR 91
#define ESP_RETURN_ON_FALSE(c, e, ...) do { if (!(c)) return (e); } while (0)
#define ESP_RETURN_ON_ERROR(x, ...) do { esp_err_t e = (x); if (e) return e; } while (0)
#define ESP_GOTO_ON_ERROR(x, label, ...) do { ret = (x); if (ret) goto label; } while (0)
#define ESP_GOTO_ON_FALSE(c, e, label, ...) do { if (!(c)) { ret = (e); goto label; } } while (0)
#define I2S_PORT 0
#define I2S_ROLE_MASTER 0
#define I2S_MCLK 1
#define I2S_BCLK 2
#define I2S_WS 3
#define I2S_DOUT 4
#define I2S_DIN 5
#define I2C_PORT 0
#define PA_GPIO 6
#define I2S_DATA_BIT_WIDTH_16BIT 16
#define I2S_SLOT_MODE_MONO 1
#define I2S_SLOT_MODE_STEREO 2
#define ES8311_CODEC_DEFAULT_ADDR 24
#define ES7210_CODEC_DEFAULT_ADDR 64
#define ESP_CODEC_DEV_WORK_MODE_DAC 1
#define ESP_CODEC_DEV_TYPE_OUT 1
#define ESP_CODEC_DEV_TYPE_IN 2
#define I2S_CHANNEL_DEFAULT_CONFIG(port, role) ((i2s_chan_config_t){0})
#define I2S_STD_CLK_DEFAULT_CONFIG(rate) (rate)
#define I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(bits, mode) (bits)
#define pdMS_TO_TICKS(ms) (ms)

typedef enum { CHANNEL, DATA, CTRL, GPIO, CODEC, WRAPPER } kind_t;
typedef struct node {
    kind_t kind;
    bool initialized, enabled;
    unsigned borrowers;
    struct node *refs[2];
    int (*get_reg)(const struct node *, int, int *);
    int (*set_reg)(const struct node *, int, int);
    uint8_t regs[256];
} node_t;
typedef node_t *i2s_chan_handle_t;
typedef struct { bool is_enabled; } i2s_chan_info_t;
typedef node_t audio_codec_data_if_t;
typedef node_t audio_codec_ctrl_if_t;
typedef node_t audio_codec_gpio_if_t;
typedef node_t audio_codec_if_t;
typedef node_t *esp_codec_dev_handle_t;
typedef struct { bool auto_clear; unsigned dma_desc_num, dma_frame_num; } i2s_chan_config_t;
typedef struct { int mclk, bclk, ws, dout, din; } i2s_std_gpio_config_t;
typedef struct {
    uint32_t clk_cfg;
    int slot_cfg;
    struct { int mclk, bclk, ws, dout, din; } gpio_cfg;
} i2s_std_config_t;
typedef struct { int port; i2s_chan_handle_t rx_handle, tx_handle; } audio_codec_i2s_cfg_t;
typedef struct { int port, addr; void *bus_handle; } audio_codec_i2c_cfg_t;
typedef struct {
    const audio_codec_ctrl_if_t *ctrl_if;
    const audio_codec_gpio_if_t *gpio_if;
    int codec_mode, pa_pin;
    bool use_mclk;
    struct { float pa_voltage, codec_dac_voltage; } hw_gain;
} es8311_codec_cfg_t;
typedef struct { const audio_codec_ctrl_if_t *ctrl_if; } es7210_codec_cfg_t;
typedef struct {
    int dev_type;
    const audio_codec_if_t *codec_if;
    const audio_codec_data_if_t *data_if;
} esp_codec_dev_cfg_t;
static void *s_i2c = (void *)1;
static node_t *live[16];
static unsigned nlive, stage, fail_stage;
static bool fail_partial_channel;
static bool pa_enabled, speaker_muted;
static unsigned silence_primes;
static bool mclk_attached = true;
static unsigned runtime_calls, fail_runtime, settled_ms;
static bool short_prime;
static bool trace_resume;
static bool trace_suspend;
static unsigned restore_step;
static node_t *trace_tx, *trace_rx, *trace_dac;
static bool runtime_fail(void) { return ++runtime_calls == fail_runtime; }
static int codec_read(const node_t *p, int reg, int *value)
{
    if (runtime_fail()) return DRIVER_ERROR;
    *value = p->regs[reg];
    return ESP_OK;
}
static int codec_write(const node_t *codec, int reg, int value)
{
    if (runtime_fail()) return DRIVER_ERROR;
    node_t *p = (node_t *)codec;
    CHECK(!pa_enabled || reg == 0x31 || reg == 0x32 || reg == 0x34 || reg == 0x35 || reg == 0x37);
    if (trace_suspend && reg != 0x31) CHECK(mclk_attached && trace_tx->enabled && trace_rx->enabled);
    if (trace_resume && reg != 0x31) {
        static const uint8_t order[] = {
            0x00, 0x01, 0x02, 0x17, 0x0e, 0x12, 0x14, 0x0d, 0x15, 0x45, 0x32,
            0x01, 0x06, 0x40, 0x47, 0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x00, 0x00,
        };
        CHECK(mclk_attached && trace_tx->enabled && trace_rx->enabled);
        CHECK(restore_step < sizeof(order) && reg == order[restore_step]);
        CHECK((p == trace_dac) == (restore_step < 11));
        restore_step++;
    }
    if (trace_resume && reg == 0x31 && !(value & 0x60)) CHECK(restore_step == 22 && settled_ms == 10);
    p->regs[reg] = value;
    if (p->refs[1] && reg == 0x31) speaker_muted = (value & 0x60) == 0x60;
    return ESP_OK;
}
static void vTaskDelay(unsigned ticks)
{
    CHECK(!pa_enabled && speaker_muted && mclk_attached);
    if (trace_resume) CHECK(restore_step == 22 && silence_primes == 1);
    settled_ms += ticks;
}
static int gpio_reset_pin(int pin)
{
    CHECK(pin == I2S_MCLK);
    if (runtime_fail()) return DRIVER_ERROR;
    mclk_attached = false;
    return ESP_OK;
}
static int gpio_set_level(int pin, int level)
{
    CHECK(pin == PA_GPIO);
    if (level) CHECK(silence_primes && !speaker_muted);
    if (trace_resume && level) CHECK(restore_step == 22 && settled_ms == 10);
    pa_enabled = level != 0;
    return ESP_OK;
}
static bool fail_now(void) { return ++stage == fail_stage; }
static bool alive(const node_t *p)
{
    for (unsigned i = 0; i < nlive; i++) if (live[i] == p) return true;
    return false;
}
static node_t *allocate(kind_t kind, const node_t *a, const node_t *b)
{
    CHECK(nlive < 16);
    node_t *p = calloc(1, sizeof(*p));
    CHECK(p);
    p->kind = kind;
    p->get_reg = codec_read;
    p->set_reg = codec_write;
    if (kind == CODEC) {
        for (unsigned i = 0; i < 256; i++) p->regs[i] = (uint8_t)(i ^ 0xa5);
    }
    p->refs[0] = (node_t *)a;
    p->refs[1] = (node_t *)b;
    for (unsigned i = 0; i < 2; i++) if (p->refs[i]) {
        CHECK(alive(p->refs[i]));
        p->refs[i]->borrowers++;
    }
    live[nlive++] = p;
    return p;
}
static int release(const node_t *value, kind_t kind)
{
    node_t *p = (node_t *)value;
    CHECK(alive(p));
    CHECK(p->kind == kind && p->borrowers == 0 && !p->enabled);
    for (unsigned i = 0; i < 2; i++) if (p->refs[i]) {
        CHECK(alive(p->refs[i]) && p->refs[i]->borrowers > 0);
        p->refs[i]->borrowers--;
    }
    for (unsigned i = 0; i < nlive; i++) if (live[i] == p) {
        live[i] = live[--nlive];
        break;
    }
    free(p);
    return ESP_OK;
}
static int i2s_new_channel(const i2s_chan_config_t *cfg, i2s_chan_handle_t *tx, i2s_chan_handle_t *rx)
{
    CHECK(cfg->auto_clear && cfg->dma_desc_num == 2 && cfg->dma_frame_num == 80 && *tx == NULL && *rx == NULL);
    if (fail_now()) {
        if (fail_partial_channel) *tx = allocate(CHANNEL, NULL, NULL);
        return DRIVER_ERROR;
    }
    *tx = allocate(CHANNEL, NULL, NULL);
    *rx = allocate(CHANNEL, NULL, NULL);
    return ESP_OK;
}
static int i2s_channel_init_std_mode(i2s_chan_handle_t p, const i2s_std_config_t *cfg)
{
    CHECK(alive(p) && p->kind == CHANNEL && !p->initialized);
    CHECK(cfg->clk_cfg == 16000 && cfg->slot_cfg == 16);
    if (fail_now()) return DRIVER_ERROR;
    p->initialized = true;
    return ESP_OK;
}
static int i2s_channel_enable(i2s_chan_handle_t p)
{
    CHECK(alive(p) && p->initialized && !p->enabled);
    if (runtime_fail()) return DRIVER_ERROR;
    if (fail_now()) return DRIVER_ERROR;
    p->enabled = true;
    return ESP_OK;
}
static int i2s_channel_reconfig_std_gpio(i2s_chan_handle_t p, const i2s_std_gpio_config_t *pins)
{
    CHECK(alive(p) && !p->enabled && pins->mclk == I2S_MCLK);
    if (runtime_fail()) return DRIVER_ERROR;
    mclk_attached = true;
    return ESP_OK;
}
static int i2s_channel_disable(i2s_chan_handle_t p)
{
    CHECK(alive(p) && p->enabled);
    if (runtime_fail()) return DRIVER_ERROR;
    p->enabled = false;
    return ESP_OK;
}
static int i2s_channel_get_info(i2s_chan_handle_t p, i2s_chan_info_t *info)
{
    CHECK(alive(p));
    if (runtime_fail()) return DRIVER_ERROR;
    info->is_enabled = p->enabled;
    return ESP_OK;
}
static int i2s_del_channel(i2s_chan_handle_t p) { return release(p, CHANNEL); }
static int i2s_channel_read(i2s_chan_handle_t p, void *buffer, size_t bytes, size_t *read, uint32_t timeout)
{
    CHECK(alive(p) && p->enabled && buffer && timeout == 40);
    *read = bytes;
    return ESP_OK;
}
static int i2s_channel_write(i2s_chan_handle_t p, const void *buffer, size_t bytes, size_t *written, uint32_t timeout)
{
    CHECK(alive(p) && p->enabled && buffer && timeout == 10 && bytes == 640);
    const int16_t *pcm = buffer;
    for (size_t i = 0; i < bytes / 2; i++) CHECK(pcm[i] == 0);
    CHECK(!pa_enabled && speaker_muted);
    if (runtime_fail()) return DRIVER_ERROR;
    silence_primes++;
    *written = short_prime ? bytes - 2 : bytes;
    return ESP_OK;
}
static const audio_codec_data_if_t *audio_codec_new_i2s_data(audio_codec_i2s_cfg_t *cfg)
{
    CHECK(alive(cfg->tx_handle) && cfg->tx_handle->enabled);
    CHECK(alive(cfg->rx_handle) && cfg->rx_handle->enabled);
    return fail_now() ? NULL : allocate(DATA, cfg->tx_handle, cfg->rx_handle);
}
static const audio_codec_ctrl_if_t *audio_codec_new_i2c_ctrl(audio_codec_i2c_cfg_t *cfg)
{
    CHECK(cfg->bus_handle == s_i2c);
    return fail_now() ? NULL : allocate(CTRL, NULL, NULL);
}
static const audio_codec_gpio_if_t *audio_codec_new_gpio(void)
{
    return fail_now() ? NULL : allocate(GPIO, NULL, NULL);
}
static const audio_codec_if_t *es8311_codec_new(es8311_codec_cfg_t *cfg)
{
    CHECK(cfg->ctrl_if && cfg->gpio_if && cfg->pa_pin == -1);
    return fail_now() ? NULL : allocate(CODEC, cfg->ctrl_if, cfg->gpio_if);
}
static const audio_codec_if_t *es7210_codec_new(es7210_codec_cfg_t *cfg)
{
    CHECK(cfg->ctrl_if);
    return fail_now() ? NULL : allocate(CODEC, cfg->ctrl_if, NULL);
}
static esp_codec_dev_handle_t esp_codec_dev_new(esp_codec_dev_cfg_t *cfg)
{
    CHECK(cfg->codec_if && cfg->data_if);
    return fail_now() ? NULL : allocate(WRAPPER, cfg->codec_if, cfg->data_if);
}
static void esp_codec_dev_delete(esp_codec_dev_handle_t p)
{
    /* A wrapper that was opened closes its RX channel through the data
     * interface, before the board releases that shared interface. */
    if (p->initialized) p->refs[1]->refs[1]->enabled = false;
    release(p, WRAPPER);
}
static int audio_codec_delete_codec_if(const audio_codec_if_t *p) { return release(p, CODEC); }
static int audio_codec_delete_ctrl_if(const audio_codec_ctrl_if_t *p) { return release(p, CTRL); }
static int audio_codec_delete_gpio_if(const audio_codec_gpio_if_t *p) { return release(p, GPIO); }
static int audio_codec_delete_data_if(const audio_codec_data_if_t *p) { return release(p, DATA); }

#include "orb_board_audio_impl.inc"

static void success_and_release(bool opened)
{
    CHECK(nlive == 0);
    stage = fail_stage = 0;
    esp_codec_dev_handle_t spk = NULL, mic = NULL;
    CHECK(orb_board_audio_new(16000, &spk, &mic) == ESP_OK);
    CHECK(stage == 13 && nlive == 10);
    CHECK(alive(spk) && alive(mic) && spk != mic);
    CHECK(spk->refs[1] == mic->refs[1]); /* Exactly one shared data owner. */
    esp_codec_dev_handle_t extra_spk = NULL, extra_mic = NULL;
    CHECK(orb_board_audio_new(16000, &extra_spk, &extra_mic) == ESP_ERR_INVALID_STATE);
    CHECK(!extra_spk && !extra_mic && nlive == 10);
    char samples[640]; size_t bytes = 0;
    CHECK(orb_board_audio_read(mic, samples, sizeof(samples), &bytes, 40) == ESP_OK);
    CHECK(bytes == sizeof(samples));
    CHECK(orb_board_audio_read(spk, samples, sizeof(samples), &bytes, 40) == ESP_ERR_INVALID_ARG);
    CHECK(orb_board_audio_delete(mic, spk) == ESP_ERR_INVALID_ARG && nlive == 10);
    mic->initialized = opened;
    CHECK(orb_board_audio_delete(spk, mic) == ESP_OK && nlive == 0);
    CHECK(orb_board_audio_delete(spk, mic) == ESP_ERR_INVALID_ARG);
}
static void fail_every_stage(void)
{
    for (unsigned failed = 1; failed <= 13; failed++) {
        stage = 0;
        fail_stage = failed;
        esp_codec_dev_handle_t spk = (void *)1, mic = (void *)1;
        int expected = failed <= 5 ? DRIVER_ERROR : (failed == 9 || failed == 12) ? ESP_FAIL : ESP_ERR_NO_MEM;
        CHECK(orb_board_audio_new(16000, &spk, &mic) == expected);
        CHECK(stage == failed && !spk && !mic && nlive == 0);
        success_and_release((failed & 1) != 0);
    }
}
static void idle_and_resume(void)
{
    stage = fail_stage = 0;
    esp_codec_dev_handle_t spk = NULL, mic = NULL;
    CHECK(orb_board_audio_new(16000, &spk, &mic) == ESP_OK);
    CHECK(orb_board_audio_set_volume(60) == ESP_OK);
    pa_enabled = true;
    CHECK(orb_board_audio_idle(true) == ESP_OK);
    CHECK(!pa_enabled && speaker_muted && !s_audio.tx->enabled && !s_audio.rx->enabled && !mclk_attached);
    unsigned previous = stage;
    CHECK(orb_board_audio_idle(true) == ESP_OK && stage == previous);
    CHECK(orb_board_audio_idle(false) == ESP_OK);
    CHECK(pa_enabled && !speaker_muted && s_audio.tx->enabled && s_audio.rx->enabled && silence_primes == 1 && mclk_attached);
    previous = stage;
    CHECK(orb_board_audio_idle(false) == ESP_OK && stage == previous);
    CHECK(orb_board_audio_idle(true) == ESP_OK);
    CHECK(orb_board_audio_delete(spk, mic) == ESP_OK && !pa_enabled && nlive == 0);
    CHECK(orb_board_audio_idle(false) == ESP_ERR_INVALID_STATE);
}

static void power_pair(esp_codec_dev_handle_t *speaker, esp_codec_dev_handle_t *mic)
{
    stage = fail_stage = runtime_calls = fail_runtime = silence_primes = settled_ms = 0;
    pa_enabled = short_prime = false;
    CHECK(orb_board_audio_new(16000, speaker, mic) == ESP_OK);
    CHECK(!pa_enabled);
    CHECK(orb_board_audio_set_volume(60) == ESP_OK);
    /* Real start values, with a nonzero divider to catch the vendor restart's
     * missing 0x02 restoration. Unchanged registers retain sentinel values. */
    ((node_t *)s_audio.dac)->regs[0x00] = 0x80;
    ((node_t *)s_audio.dac)->regs[0x01] = 0x3f;
    ((node_t *)s_audio.dac)->regs[0x02] = 0x20;
    ((node_t *)s_audio.dac)->regs[0x0d] = 0x01;
    CHECK(orb_board_audio_suspend() == ESP_ERR_INVALID_STATE);
    CHECK(orb_board_audio_idle(true) == ESP_OK);
    CHECK(!pa_enabled && speaker_muted && !mclk_attached);
}

static void codec_power_states(void)
{
    esp_codec_dev_handle_t spk, mic;
    power_pair(&spk, &mic);
    uint8_t dac_before[256], adc_before[256];
    for (unsigned i = 0; i < 256; i++) {
        dac_before[i] = s_audio.dac->regs[i];
        adc_before[i] = s_audio.adc->regs[i];
    }
    trace_tx = s_audio.tx; trace_rx = s_audio.rx;
    trace_suspend = true;
    CHECK(orb_board_audio_suspend() == ESP_OK);
    trace_suspend = false;
    CHECK(s_audio.suspended && s_audio.powered_down && !pa_enabled && !mclk_attached);
    CHECK(s_audio.dac->regs[0x0e] == 0xff && s_audio.dac->regs[0x12] == 2);
    CHECK(s_audio.dac->regs[0x0d] == 0xfc && s_audio.dac->regs[0x01] == 0 && s_audio.dac->regs[0x02] == 0);
    CHECK(s_audio.adc->regs[0x40] == 0xc0 && s_audio.adc->regs[0x01] == 0x7f && s_audio.adc->regs[0x06] == 7);
    for (unsigned i = 0x47; i <= 0x4c; i++) CHECK(s_audio.adc->regs[i] == 0xff);
    unsigned previous = runtime_calls;
    CHECK(orb_board_audio_suspend() == ESP_OK && runtime_calls == previous);
    /* Preference changes while suspended update the resume volume without
     * enabling the PA or losing DRC/ramp and microphone gain. */
    CHECK(orb_board_audio_set_volume(23) == ESP_OK && !pa_enabled && speaker_muted);
    trace_tx = s_audio.tx; trace_rx = s_audio.rx; trace_dac = (node_t *)s_audio.dac;
    trace_resume = true; restore_step = 0;
    CHECK(orb_board_audio_idle(false) == ESP_OK);
    CHECK(restore_step == 22); trace_resume = false;
    CHECK(!s_audio.suspended && !s_audio.powered_down && s_audio.ready && !s_audio.idle);
    CHECK(pa_enabled && !speaker_muted && settled_ms == 10 && silence_primes == 1);
    for (unsigned i = 0; i < sizeof(s_dac_restore); i++) {
        unsigned reg = s_dac_restore[i];
        CHECK(s_audio.dac->regs[reg] == (reg == 0x32 ? orb_audio_volume_register(23) : dac_before[reg]));
    }
    for (unsigned i = 0; i < sizeof(s_adc_restore); i++) CHECK(s_audio.adc->regs[s_adc_restore[i]] == adc_before[s_adc_restore[i]]);
    CHECK(s_audio.adc->regs[0x00] == 0x41);
    CHECK(s_audio.dac->regs[0x37] == 0x12 && s_audio.dac->regs[0x34] == 0x80 && s_audio.dac->regs[0x35] == 0xf0);
    CHECK(s_audio.adc->regs[0x43] == adc_before[0x43] && s_audio.adc->regs[0x1e] == adc_before[0x1e]);
    CHECK(orb_board_audio_idle(true) == ESP_OK);
    CHECK(orb_board_audio_suspend() == ESP_OK);
    CHECK(orb_board_audio_set_volume(0) == ESP_OK);
    CHECK(orb_board_audio_idle(false) == ESP_OK && !pa_enabled && speaker_muted);
    previous = silence_primes;
    CHECK(orb_board_audio_set_volume(60) == ESP_OK && !pa_enabled && speaker_muted);
    CHECK(orb_board_audio_idle(false) == ESP_OK && pa_enabled && silence_primes == previous + 1);
    CHECK(orb_board_audio_set_volume(0) == ESP_OK && !pa_enabled && speaker_muted);
    CHECK(orb_board_audio_set_volume(60) == ESP_OK && !pa_enabled && speaker_muted);
    CHECK(orb_board_audio_idle(false) == ESP_OK && pa_enabled && silence_primes == previous + 2);
    CHECK(orb_board_audio_delete(spk, mic) == ESP_OK && nlive == 0);
}

static void suspend_failures(void)
{
    for (unsigned failure = 1; failure <= 54; failure++) {
        esp_codec_dev_handle_t spk, mic;
        power_pair(&spk, &mic);
        runtime_calls = 0; fail_runtime = failure;
        CHECK(orb_board_audio_suspend() == ESP_FAIL);
        CHECK(!pa_enabled && !s_audio.powered_down);
        fail_runtime = 0;
        CHECK(orb_board_audio_idle(true) == ESP_OK && !mclk_attached);
        CHECK(orb_board_audio_suspend() == ESP_OK);
        CHECK(orb_board_audio_idle(false) == ESP_OK);
        CHECK(s_audio.dac->regs[0x02] == 0x20 && s_audio.dac->regs[0x32] == orb_audio_volume_register(60));
        CHECK(orb_board_audio_delete(spk, mic) == ESP_OK && nlive == 0);
    }
}

static void resume_failures(void)
{
    unsigned stages = 0;
    for (unsigned failure = 0; failure <= stages; failure++) {
        esp_codec_dev_handle_t spk, mic;
        power_pair(&spk, &mic);
        CHECK(orb_board_audio_suspend() == ESP_OK);
        runtime_calls = 0; fail_runtime = failure;
        int result = orb_board_audio_idle(false);
        if (!failure) { stages = runtime_calls; CHECK(result == ESP_OK && stages > 25); }
        else {
            CHECK(result != ESP_OK && !pa_enabled && !s_audio.ready);
            CHECK(!s_audio.tx->enabled && !s_audio.rx->enabled && !mclk_attached);
            fail_runtime = 0;
            CHECK(orb_board_audio_suspend() == ESP_OK);
            CHECK(s_audio.dac->regs[0x0d] == 0xfc && s_audio.adc->regs[0x40] == 0xc0);
            CHECK(orb_board_audio_idle(false) == ESP_OK && pa_enabled && s_audio.ready);
            CHECK(s_audio.dac->regs[0x02] == 0x20 && s_audio.dac->regs[0x32] == orb_audio_volume_register(60));
        }
        CHECK(orb_board_audio_delete(spk, mic) == ESP_OK && nlive == 0);
    }
    esp_codec_dev_handle_t spk, mic;
    power_pair(&spk, &mic);
    CHECK(orb_board_audio_suspend() == ESP_OK);
    short_prime = true;
    CHECK(orb_board_audio_idle(false) == ESP_FAIL && !pa_enabled);
    short_prime = false;
    CHECK(orb_board_audio_idle(false) == ESP_OK && pa_enabled);
    CHECK(orb_board_audio_delete(spk, mic) == ESP_OK && nlive == 0);
}
int main(void)
{
    esp_codec_dev_handle_t output = (void *)1, other = (void *)1;
    CHECK(orb_board_audio_new(16000, NULL, &output) == ESP_ERR_INVALID_ARG && !output);
    output = (void *)1;
    CHECK(orb_board_audio_new(16000, &output, NULL) == ESP_ERR_INVALID_ARG && !output);
    CHECK(orb_board_audio_new(16000, &output, &output) == ESP_ERR_INVALID_ARG && !output);
    CHECK(orb_board_audio_new(0, &output, &other) == ESP_ERR_INVALID_ARG && !output && !other);
    CHECK(stage == 0 && nlive == 0);
    fail_every_stage();
    fail_partial_channel = true;
    stage = 0; fail_stage = 1;
    CHECK(orb_board_audio_new(16000, &output, &other) == DRIVER_ERROR);
    CHECK(stage == 1 && !output && !other && nlive == 0);
    success_and_release(true);
    idle_and_resume();
    CHECK(orb_board_audio_suspend() == ESP_ERR_INVALID_STATE);
    codec_power_states();
    suspend_failures();
    resume_failures();
    printf("audio: %u checks passed (13 constructor stages, 54 suspend faults, every resume fault, register/clock/settle/PA order, zero volume, teardown and retry)\n", checks);
}
