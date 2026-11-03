/* Audio ownership and I2S lifecycle for Waveshare ESP32-S3 1.75C. */
#include "orb_board.h"
#include "board_audio.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define I2C_PORT I2C_NUM_1
#define I2S_PORT I2S_NUM_1
#define I2S_MCLK GPIO_NUM_16
#define I2S_BCLK GPIO_NUM_9
#define I2S_WS GPIO_NUM_45
#define I2S_DOUT GPIO_NUM_8
#define I2S_DIN GPIO_NUM_10
#define PA_GPIO GPIO_NUM_46
static const char *TAG = "audio";
static i2c_master_bus_handle_t s_i2c;

void board_audio_init(i2c_master_bus_handle_t bus)
{
    s_i2c = bus;
    const gpio_config_t pa = { .pin_bit_mask = 1ULL << PA_GPIO, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&pa);
    gpio_set_level(PA_GPIO, 0);
}

/* Audio has one owner. The codecs borrow the interfaces, which in turn borrow
 * the channels; retain that graph so push-to-talk can release DMA while idle. */
typedef struct {
    i2s_chan_handle_t tx, rx;
    const audio_codec_data_if_t *data;
    const audio_codec_ctrl_if_t *dac_ctrl, *adc_ctrl;
    const audio_codec_gpio_if_t *gpio;
    const audio_codec_if_t *dac, *adc;
    esp_codec_dev_handle_t speaker, mic;
    bool idle, ready, suspended, powered_down;
    unsigned volume;
    uint8_t dac_saved[11], adc_saved[9];
} board_audio_t;
static board_audio_t s_audio;

static esp_err_t audio_codecs_off(const audio_codec_if_t *dac, const audio_codec_if_t *adc)
{
    /* Espressif ES8311 suspend / ES7210 stop. Keep ES8311 0x0d=FC as shipped:
     * its PDN_VREF table is contradictory, so toggling bit2 is unjustified. */
    static const uint8_t dac_off[][2] = {
        {0x32, 0x00}, {0x17, 0x00}, {0x0e, 0xff}, {0x12, 0x02}, {0x14, 0x00},
        {0x0d, 0xfa}, {0x15, 0x00}, {0x02, 0x10}, {0x00, 0x00}, {0x00, 0x1f},
        {0x01, 0x30}, {0x01, 0x00}, {0x45, 0x00}, {0x0d, 0xfc}, {0x02, 0x00},
    };
    static const uint8_t adc_off[][2] = {
        {0x47, 0xff}, {0x48, 0xff}, {0x49, 0xff}, {0x4a, 0xff}, {0x4b, 0xff},
        {0x4c, 0xff}, {0x40, 0xc0}, {0x01, 0x7f}, {0x06, 0x07},
    };
    int ret = ESP_CODEC_DEV_OK;
    /* Attempt every domain after a bus error. This also covers constructors
     * that powered analog before a wrapper was opened: close may skip them. */
    if (dac) for (size_t i = 0; i < sizeof(dac_off) / sizeof(dac_off[0]); i++)
        ret |= dac->set_reg(dac, dac_off[i][0], dac_off[i][1]);
    if (adc) for (size_t i = 0; i < sizeof(adc_off) / sizeof(adc_off[0]); i++)
        ret |= adc->set_reg(adc, adc_off[i][0], adc_off[i][1]);
    return ret == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

esp_err_t orb_board_audio_new(uint32_t sample_rate, esp_codec_dev_handle_t *speaker, esp_codec_dev_handle_t *mic)
{
    if (speaker) *speaker = NULL;
    if (mic) *mic = NULL;
    ESP_RETURN_ON_FALSE(sample_rate && speaker && mic && speaker != mic, ESP_ERR_INVALID_ARG, TAG, "audio arguments");

    ESP_RETURN_ON_FALSE(!s_audio.data, ESP_ERR_INVALID_STATE, TAG, "audio already owned");

    /* The wrappers borrow their interfaces; the I2S data interface borrows the
     * channels. Keep every owner local until the whole duplex pair is ready. */
    esp_err_t ret = ESP_OK;
    i2s_chan_handle_t tx = NULL, rx = NULL;
    bool tx_enabled = false, rx_enabled = false;
    const audio_codec_data_if_t *data = NULL;
    const audio_codec_ctrl_if_t *dac_ctrl = NULL, *adc_ctrl = NULL;
    const audio_codec_gpio_if_t *gpio = NULL;
    const audio_codec_if_t *dac = NULL, *adc = NULL;
    esp_codec_dev_handle_t spk = NULL, input = NULL;

    /* Two physical slots are needed to retain both ES7210 microphones. The
     * codec data adapter independently selects mono TX / stereo RX on open. */
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    chan.auto_clear = true;
    /* Two 2.5 ms descriptors at 32 kHz bound queued output to 5 ms. Persistent
     * playback avoids codec setup on the UI path. Both directions use 1280 B
     * of sample DMA in stereo, excluding descriptor/driver bookkeeping. */
    chan.dma_desc_num = 2;
    chan.dma_frame_num = 80;
    /* IDF may publish TX before failing to allocate RX; unwind even this call. */
    ESP_GOTO_ON_ERROR(i2s_new_channel(&chan, &tx, &rx), failed, TAG, "i2s");
    const i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = { .mclk = I2S_MCLK, .bclk = I2S_BCLK, .ws = I2S_WS, .dout = I2S_DOUT, .din = I2S_DIN },
    };
    ESP_GOTO_ON_ERROR(i2s_channel_init_std_mode(tx, &std), failed, TAG, "i2s tx");
    ESP_GOTO_ON_ERROR(i2s_channel_init_std_mode(rx, &std), failed, TAG, "i2s rx");
    ESP_GOTO_ON_ERROR(i2s_channel_enable(tx), failed, TAG, "i2s tx on");
    tx_enabled = true;
    ESP_GOTO_ON_ERROR(i2s_channel_enable(rx), failed, TAG, "i2s rx on");
    rx_enabled = true;
    data = audio_codec_new_i2s_data(
        &(audio_codec_i2s_cfg_t){ .port = I2S_PORT, .rx_handle = rx, .tx_handle = tx });
    ESP_GOTO_ON_FALSE(data, ESP_ERR_NO_MEM, failed, TAG, "i2s data");

    dac_ctrl = audio_codec_new_i2c_ctrl(
        &(audio_codec_i2c_cfg_t){ .port = I2C_PORT, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = s_i2c });
    ESP_GOTO_ON_FALSE(dac_ctrl, ESP_ERR_NO_MEM, failed, TAG, "es8311 i2c");
    gpio = audio_codec_new_gpio();
    ESP_GOTO_ON_FALSE(gpio, ESP_ERR_NO_MEM, failed, TAG, "codec gpio");
    es8311_codec_cfg_t dac_cfg = {
        .ctrl_if = dac_ctrl,
        .gpio_if = gpio,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        /* Only the board may expose the PA, after silent DMA and bias settle. */
        .pa_pin = -1,
        .use_mclk = true,
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    dac = es8311_codec_new(&dac_cfg);
    ESP_GOTO_ON_FALSE(dac, ESP_FAIL, failed, TAG, "es8311");
    spk = esp_codec_dev_new(&(esp_codec_dev_cfg_t){
        .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = dac, .data_if = data });
    ESP_GOTO_ON_FALSE(spk, ESP_ERR_NO_MEM, failed, TAG, "speaker");

    adc_ctrl = audio_codec_new_i2c_ctrl(
        &(audio_codec_i2c_cfg_t){ .port = I2C_PORT, .addr = ES7210_CODEC_DEFAULT_ADDR, .bus_handle = s_i2c });
    ESP_GOTO_ON_FALSE(adc_ctrl, ESP_ERR_NO_MEM, failed, TAG, "es7210 i2c");
    adc = es7210_codec_new(&(es7210_codec_cfg_t){ .ctrl_if = adc_ctrl });
    ESP_GOTO_ON_FALSE(adc, ESP_FAIL, failed, TAG, "es7210");
    input = esp_codec_dev_new(&(esp_codec_dev_cfg_t){
        .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = adc, .data_if = data });
    ESP_GOTO_ON_FALSE(input, ESP_ERR_NO_MEM, failed, TAG, "microphone");
    s_audio = (board_audio_t){
        .tx = tx, .rx = rx, .data = data, .dac_ctrl = dac_ctrl, .adc_ctrl = adc_ctrl,
        .gpio = gpio, .dac = dac, .adc = adc, .speaker = spk, .mic = input,
    };
    *speaker = spk;
    *mic = input;
    return ESP_OK;

failed:
    /* Codec close still needs its control/GPIO interfaces. Delete the shared
     * data interface once, then stop only channels this call enabled. No
     * wrapper has been opened or published, so no other task owns these. */
    gpio_set_level(PA_GPIO, 0);
    if (audio_codecs_off(dac, adc) != ESP_OK) ESP_LOGW(TAG, "codec cleanup failed");
    if (input) esp_codec_dev_delete(input);
    if (spk) esp_codec_dev_delete(spk);
    if (adc) audio_codec_delete_codec_if(adc);
    if (dac) audio_codec_delete_codec_if(dac);
    if (adc_ctrl) audio_codec_delete_ctrl_if(adc_ctrl);
    if (dac_ctrl) audio_codec_delete_ctrl_if(dac_ctrl);
    if (gpio) audio_codec_delete_gpio_if(gpio);
    if (data) audio_codec_delete_data_if(data);
    if (rx_enabled) i2s_channel_disable(rx);
    if (tx_enabled) i2s_channel_disable(tx);
    if (rx) i2s_del_channel(rx);
    if (tx) i2s_del_channel(tx);
    return ret;
}

esp_err_t orb_board_audio_read(esp_codec_dev_handle_t mic, void *buffer, size_t bytes,
                              size_t *read, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(mic && mic == s_audio.mic && buffer && read && bytes && !(bytes & 1),
                        ESP_ERR_INVALID_ARG, TAG, "audio read arguments");
    *read = 0;
    /* Use a finite driver deadline so cancellation and the 60 s recording cap
     * still run when the codec or its clock stops producing samples. */
    return i2s_channel_read(s_audio.rx, buffer, bytes, read, timeout_ms);
}

esp_err_t orb_board_audio_delete(esp_codec_dev_handle_t speaker, esp_codec_dev_handle_t mic)
{
    ESP_RETURN_ON_FALSE(s_audio.data && speaker == s_audio.speaker && mic == s_audio.mic,
                        ESP_ERR_INVALID_ARG, TAG, "audio owner");
    /* The voice task is the sole owner and has stopped reading before teardown.
     * Wrapper deletion closes each codec while its control interface still lives. */
    gpio_set_level(PA_GPIO, 0);
    esp_err_t ret = audio_codecs_off(s_audio.dac, s_audio.adc);
    esp_codec_dev_delete(s_audio.mic);
    esp_codec_dev_delete(s_audio.speaker);
    audio_codec_delete_codec_if(s_audio.adc);
    audio_codec_delete_codec_if(s_audio.dac);
    audio_codec_delete_ctrl_if(s_audio.adc_ctrl);
    audio_codec_delete_ctrl_if(s_audio.dac_ctrl);
    audio_codec_delete_gpio_if(s_audio.gpio);
    audio_codec_delete_data_if(s_audio.data);
    i2s_chan_info_t info = {0};
    esp_err_t err = i2s_channel_get_info(s_audio.rx, &info);
    if (err == ESP_OK && info.is_enabled) err = i2s_channel_disable(s_audio.rx);
    if (ret == ESP_OK) ret = err;
    err = i2s_channel_get_info(s_audio.tx, &info);
    if (err == ESP_OK && info.is_enabled) err = i2s_channel_disable(s_audio.tx);
    if (ret == ESP_OK) ret = err;
    err = i2s_del_channel(s_audio.rx);
    if (ret == ESP_OK) ret = err;
    err = i2s_del_channel(s_audio.tx);
    if (ret == ESP_OK) ret = err;
    s_audio = (board_audio_t){0};
    return ret;
}

static esp_err_t audio_mute(bool mute)
{
    const audio_codec_if_t *dac = s_audio.dac;
    int value;
    /* esp_codec_dev_set_out_mute and the ES8311 mute callback discard write
     * failures. Check the bus result before exposing the amplifier. */
    ESP_RETURN_ON_FALSE(dac->get_reg(dac, 0x31, &value) == ESP_CODEC_DEV_OK &&
                        dac->set_reg(dac, 0x31, (value & ~0x60) | (mute ? 0x60 : 0)) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "speaker mute");
    return ESP_OK;
}

static esp_err_t audio_transport_stop(void)
{
    gpio_set_level(PA_GPIO, 0);
    s_audio.ready = false;
    esp_err_t ret = ESP_OK;
    const i2s_chan_handle_t channels[] = {s_audio.rx, s_audio.tx};
    for (unsigned i = 0; i < 2; i++) {
        i2s_chan_info_t info = {0};
        esp_err_t err = i2s_channel_get_info(channels[i], &info);
        if (err == ESP_OK && info.is_enabled) err = i2s_channel_disable(channels[i]);
        if (ret == ESP_OK) ret = err;
    }
    /* Disabling I2S releases PM locks but leaves the external MCLK route. */
    esp_err_t err = gpio_reset_pin(I2S_MCLK);
    if (ret == ESP_OK) ret = err;
    s_audio.idle = ret == ESP_OK;
    return ret;
}

static esp_err_t audio_transport_start(void)
{
    const i2s_std_gpio_config_t pins = {
        .mclk = I2S_MCLK, .bclk = I2S_BCLK, .ws = I2S_WS, .dout = I2S_DOUT, .din = I2S_DIN,
    };
    ESP_RETURN_ON_ERROR(i2s_channel_reconfig_std_gpio(s_audio.tx, &pins), TAG, "restore MCLK");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_audio.tx), TAG, "resume TX");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_audio.rx), TAG, "resume RX");
    return ESP_OK;
}

/* Registers changed by Espressif's ES8311 suspend / ES7210 stop, in start
 * order. Retaining exact values also restores ES8311 0x02 (its start routine
 * does not), output gain and the selected ES7210 pair without reopening DMA. */
static const uint8_t s_dac_restore[] = {0x00, 0x01, 0x02, 0x17, 0x0e, 0x12, 0x14, 0x0d, 0x15, 0x45, 0x32};
static const uint8_t s_adc_restore[] = {0x01, 0x06, 0x40, 0x47, 0x48, 0x49, 0x4a, 0x4b, 0x4c};

static esp_err_t audio_register_save(const audio_codec_if_t *codec, const uint8_t *regs,
                                     uint8_t *values, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        int value;
        ESP_RETURN_ON_FALSE(codec->get_reg(codec, regs[i], &value) == ESP_CODEC_DEV_OK,
                            ESP_FAIL, TAG, "codec save");
        values[i] = value;
    }
    return ESP_OK;
}

static esp_err_t audio_register_restore(const audio_codec_if_t *codec, const uint8_t *regs,
                                        const uint8_t *values, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        ESP_RETURN_ON_FALSE(codec->set_reg(codec, regs[i], values[i]) == ESP_CODEC_DEV_OK,
                            ESP_FAIL, TAG, "codec restore");
    }
    return ESP_OK;
}

esp_err_t orb_board_audio_suspend(void)
{
    ESP_RETURN_ON_FALSE(s_audio.data && s_audio.idle, ESP_ERR_INVALID_STATE, TAG, "audio idle owner");
    if (s_audio.powered_down) return ESP_OK;
    if (!s_audio.suspended) {
        ESP_RETURN_ON_ERROR(audio_register_save(s_audio.dac, s_dac_restore, s_audio.dac_saved,
                                                sizeof(s_dac_restore)), TAG, "DAC save");
        ESP_RETURN_ON_ERROR(audio_register_save(s_audio.adc, s_adc_restore, s_audio.adc_saved,
                                                sizeof(s_adc_restore)), TAG, "ADC save");
        /* A failed write can leave either chip partly asleep. Preserve this
         * snapshot until a complete resume, including across suspend retries. */
        s_audio.suspended = true;
    }
    /* ES7210 rev21 p10 note4: enter register low power before stopping its
     * input clock. Briefly restore clocks under mute/PA-low after warm idle. */
    esp_err_t ret = audio_mute(true);
    if (ret == ESP_OK) ret = audio_transport_start();
    esp_err_t off = audio_codecs_off(s_audio.dac, s_audio.adc);
    esp_err_t stopped = audio_transport_stop();
    s_audio.powered_down = ret == ESP_OK && off == ESP_OK && stopped == ESP_OK;
    ESP_RETURN_ON_FALSE(s_audio.powered_down, ESP_FAIL, TAG, "codec suspend");
    return ESP_OK;
}

static esp_err_t audio_resume(void)
{
    /* Any partial restore invalidates the completed-off claim, while the
     * saved active profile must remain available for a subsequent retry. */
    s_audio.powered_down = false;
    ESP_RETURN_ON_ERROR(audio_transport_stop(), TAG, "stop before resume");
    ESP_RETURN_ON_ERROR(audio_mute(true), TAG, "resume mute");
    ESP_RETURN_ON_ERROR(audio_transport_start(), TAG, "resume clocks");
    if (s_audio.suspended) {
        ESP_RETURN_ON_ERROR(audio_register_restore(s_audio.dac, s_dac_restore, s_audio.dac_saved,
                                                   sizeof(s_dac_restore)), TAG, "DAC resume");
        ESP_RETURN_ON_ERROR(audio_register_restore(s_audio.adc, s_adc_restore, s_audio.adc_saved,
                                                   sizeof(s_adc_restore)), TAG, "ADC resume");
        const audio_codec_if_t *adc = s_audio.adc;
        ESP_RETURN_ON_FALSE(adc->set_reg(adc, 0x00, 0x71) == ESP_CODEC_DEV_OK &&
                            adc->set_reg(adc, 0x00, 0x41) == ESP_CODEC_DEV_OK,
                            ESP_FAIL, TAG, "ADC restart");
    }
    /* At 32 kHz the two 80-frame DMA descriptors hold 5 ms. Prime both
     * with silence before the amplifier sees clocks, then ramp digitally. */
    const int16_t silence[320] = {0};
    size_t written = 0;
    ESP_RETURN_ON_ERROR(i2s_channel_write(s_audio.tx, silence, sizeof(silence), &written, 10), TAG, "silent prime");
    ESP_RETURN_ON_FALSE(written == sizeof(silence), ESP_FAIL, TAG, "short silent prime");
    if (s_audio.suspended) {
        /* Clocks run with zero DMA and PA low while the analog path settles.
         * This is a bounded allowance, not a measured pop/latency guarantee. */
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_RETURN_ON_ERROR(audio_mute(!s_audio.volume), TAG, "resume unmute");
    ESP_RETURN_ON_ERROR(gpio_set_level(PA_GPIO, s_audio.volume != 0), TAG, "resume PA");
    s_audio.idle = s_audio.suspended = s_audio.powered_down = false;
    s_audio.ready = true;
    return ESP_OK;
}

esp_err_t orb_board_audio_idle(bool idle)
{
    ESP_RETURN_ON_FALSE(s_audio.data, ESP_ERR_INVALID_STATE, TAG, "audio owner");
    if (idle) {
        if (s_audio.idle) return ESP_OK;
        /* PA off is unconditional, including a failed mute transaction. */
        gpio_set_level(PA_GPIO, 0);
        esp_err_t ret = audio_mute(true);
        esp_err_t err = audio_transport_stop();
        return ret == ESP_OK ? err : ret;
    }
    if (s_audio.ready) return ESP_OK;
    esp_err_t ret = audio_resume();
    if (ret != ESP_OK) audio_transport_stop();
    return ret;
}

#include "orb_audio_levels.h"
#include "orb_mic_levels.h"

esp_err_t orb_board_audio_set_mic_gain(void)
{
    ESP_RETURN_ON_FALSE(s_audio.adc, ESP_ERR_INVALID_STATE, TAG, "microphone owner");
    const audio_codec_if_t *adc = s_audio.adc;
    int mic1, mic2, alc, ramp;
    /* ES7210 rev21 pp16,18-19,22: ADC1/2 direct gain lives at 0x1E/1D
     * (reverse channel order). Preserve input selection and unrelated bits.
     * Fixed gain avoids ALC pumping the suppression/echo reference. */
    ESP_RETURN_ON_FALSE(adc->get_reg(adc, 0x43, &mic1) == ESP_CODEC_DEV_OK &&
                        adc->get_reg(adc, 0x44, &mic2) == ESP_CODEC_DEV_OK &&
                        adc->get_reg(adc, 0x16, &alc) == ESP_CODEC_DEV_OK &&
                        adc->get_reg(adc, 0x1a, &ramp) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "microphone controls read");
    ESP_RETURN_ON_FALSE(adc->set_reg(adc, 0x16, alc & ~0x0c) == ESP_CODEC_DEV_OK &&
                        adc->set_reg(adc, 0x1a, (ramp & ~0x71) | 0x10) == ESP_CODEC_DEV_OK &&
                        adc->set_reg(adc, 0x43, (mic1 & ~0x0f) | ORB_MIC_PGA_REGISTER) == ESP_CODEC_DEV_OK &&
                        adc->set_reg(adc, 0x44, (mic2 & ~0x0f) | ORB_MIC_PGA_REGISTER) == ESP_CODEC_DEV_OK &&
                        adc->set_reg(adc, 0x1e, ORB_MIC_DIGITAL_REGISTER) == ESP_CODEC_DEV_OK &&
                        adc->set_reg(adc, 0x1d, ORB_MIC_DIGITAL_REGISTER) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "microphone controls write");
    return ESP_OK;
}

esp_err_t orb_board_audio_write(esp_codec_dev_handle_t speaker, const void *buffer, size_t bytes,
                               size_t *written, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(speaker && speaker == s_audio.speaker && buffer && written && bytes && !(bytes & 1),
                        ESP_ERR_INVALID_ARG, TAG, "audio write arguments");
    *written = 0;
    return i2s_channel_write(s_audio.tx, buffer, bytes, written, timeout_ms);
}

esp_err_t orb_board_audio_set_volume(unsigned volume)
{
    ESP_RETURN_ON_FALSE(s_audio.dac && volume <= 100, ESP_ERR_INVALID_ARG, TAG, "speaker volume");
    if (!volume || !s_audio.volume) {
        /* Crossing zero requires a new silent prime before PA enable. This
         * also covers a rapid 0 -> audible edit before the 100 ms idle gate. */
        gpio_set_level(PA_GPIO, 0);
        s_audio.ready = false;
    }
    const audio_codec_if_t *dac = s_audio.dac;
    /* ES8311 DS rev10 pp25-27: DRC max -6dB/min -30.1dB, fast attack,
     * 0.25dB/4LRCK soft ramp. Preserve unrelated mute and EQ bits. */
    int mute, ramp;
    ESP_RETURN_ON_FALSE(dac->get_reg(dac, 0x31, &mute) == ESP_CODEC_DEV_OK &&
                        dac->get_reg(dac, 0x37, &ramp) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "speaker controls read");
    ESP_RETURN_ON_FALSE(dac->set_reg(dac, 0x35, 0xf0) == ESP_CODEC_DEV_OK &&
                        dac->set_reg(dac, 0x34, 0x80) == ESP_CODEC_DEV_OK &&
                        dac->set_reg(dac, 0x37, (ramp & 0x0f) | 0x10) == ESP_CODEC_DEV_OK &&
                        dac->set_reg(dac, 0x32, orb_audio_volume_register(volume)) == ESP_CODEC_DEV_OK &&
                        dac->set_reg(dac, 0x31, (mute & ~0x60) | (volume && s_audio.ready ? 0 : 0x60)) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "speaker controls write");
    s_audio.volume = volume;
    if (s_audio.suspended) s_audio.dac_saved[10] = orb_audio_volume_register(volume);
    return ESP_OK;
}
