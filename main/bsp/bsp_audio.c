/**
 * @file bsp_audio.c
 * @brief ES8311 DAC + I2S TX + NS4150B 功放的板级驱动
 *
 * 关键设计：
 *  1. 板上没有独立 MCLK 走线（GPIO38 与 LCD_RST 复用），所以 I2S 只接
 *     BCLK/WS/DOUT 三根线，ES8311 配置为「从 SCLK 引脚内部倍频出 MCLK」
 *     （REG01 bit7=1）。此时内部 MCLK = BCLK = 32*fs，时钟系数表按
 *     mclk=32*fs 选行（44.1k/48k/32k/22.05k 均有官方系数）。
 *  2. 功放使能挂在 I2C IO 扩展 PCA9557 IO1 上，放音前拉高、停音拉低，
 *     避免常开功放底噪和上电爆音。
 *  3. I2S 通道 init 一次常驻，换采样率用 i2s_channel_disable/enable +
 *     重设 std 时钟即可，不销毁重建。
 */
#include "bsp_audio.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "bsp_board.h"
#include "bsp_i2c.h"
#include "bsp_pca9557.h"
#include "driver/i2s_std.h"

static const char *TAG = "bsp_audio";

/* ---- 引脚（docs/HARDWARE.md §2 音频行） ---- */
#define AUD_I2S_BCLK   GPIO_NUM_14
#define AUD_I2S_WS     GPIO_NUM_13
#define AUD_I2S_DOUT   GPIO_NUM_45

#define ES8311_ADDR    0x18

/* ES8311 寄存器（与 esp-bsp/es8311 对齐） */
#define REG_RESET      0x00
#define REG_CLK01      0x01   /* 时钟总开关：bit7=1 时从 SCLK 内部倍频出 MCLK */
#define REG_CLK02      0x02
#define REG_CLK03      0x03
#define REG_CLK04      0x04
#define REG_CLK05      0x05
#define REG_CLK06      0x06
#define REG_CLK07      0x07
#define REG_CLK08      0x08
#define REG_SDPIN      0x09
#define REG_SDPOUT     0x0A
#define REG_SYS0D      0x0D
#define REG_SYS0E      0x0E
#define REG_SYS12      0x12
#define REG_SYS13      0x13
#define REG_DAC31      0x31   /* mute bits 6:5 */
#define REG_DAC32      0x32   /* volume 0..255 */

/** 时钟系数行（mclk=32*fs 场景），数值取自 es8311 官方 coeff_div 表 */
typedef struct {
    uint32_t rate;
    uint8_t pre_div, pre_multi, fs_mode, lrck_h, lrck_l, bclk_div;
} es_coef_t;

static const es_coef_t s_coefs[] = {
    { 22050, 0x01, 0x03, 0x00, 0x00, 0xff, 0x04 },
    { 32000, 0x01, 0x02, 0x00, 0x00, 0xff, 0x04 },
    { 44100, 0x01, 0x03, 0x00, 0x00, 0xff, 0x04 },
    { 48000, 0x01, 0x03, 0x00, 0x00, 0xff, 0x04 },
};

static i2s_chan_handle_t s_tx;
static i2c_master_dev_handle_t s_es;
static bool s_inited, s_started, s_pa_on;
static int s_volume = 70;

/* ==================== ES8311 寄存器读写 ==================== */

static esp_err_t es8311_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_es, buf, 2, 100);
}

static esp_err_t es8311_read(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_es, &reg, 1, val, 1, 100);
}

/** 按 mclk = 32*fs 配置分频系数（系数表见文件头注释） */
static esp_err_t es8311_clock_config(uint32_t rate)
{
    const es_coef_t *c = NULL;
    for (size_t i = 0; i < sizeof(s_coefs) / sizeof(s_coefs[0]); i++) {
        if (s_coefs[i].rate == rate) {
            c = &s_coefs[i];
            break;
        }
    }
    if (!c) {
        ESP_LOGW(TAG, "rate %lu no exact coef, using 44100", (unsigned long)rate);
        c = &s_coefs[2];   /* 44100 */
    }

    uint8_t regv;
    /* REG02: pre-divider + pre-multiplier */
    ESP_RETURN_ON_ERROR(es8311_read(REG_CLK02, &regv), TAG, "read02");
    regv = (regv & 0x07) | ((c->pre_div - 1) << 5) | (c->pre_multi << 3);
    ESP_RETURN_ON_ERROR(es8311_write(REG_CLK02, regv), TAG, "write02");

    /* REG03/04: ADC/DAC OSR；REG05: ADC/DAC 分频 */
    ESP_RETURN_ON_ERROR(es8311_write(REG_CLK03, (c->fs_mode << 6) | 0x10), TAG, "write03");
    ESP_RETURN_ON_ERROR(es8311_write(REG_CLK04, 0x10), TAG, "write04");
    ESP_RETURN_ON_ERROR(es8311_write(REG_CLK05, 0x00), TAG, "write05");

    /* REG06: BCLK 分频 + SCLK 极性 */
    ESP_RETURN_ON_ERROR(es8311_read(REG_CLK06, &regv), TAG, "read06");
    regv = (regv & 0xE0) | ((c->bclk_div < 19) ? (c->bclk_div - 1) : c->bclk_div);
    ESP_RETURN_ON_ERROR(es8311_write(REG_CLK06, regv), TAG, "write06");

    /* REG07/08: LRCK 计数高/低字节 */
    ESP_RETURN_ON_ERROR(es8311_read(REG_CLK07, &regv), TAG, "read07");
    regv = (regv & 0xC0) | c->lrck_h;
    ESP_RETURN_ON_ERROR(es8311_write(REG_CLK07, regv), TAG, "write07");
    ESP_RETURN_ON_ERROR(es8311_write(REG_CLK08, c->lrck_l), TAG, "write08");

    return ESP_OK;
}

static esp_err_t es8311_hw_init(uint32_t rate)
{
    esp_err_t err;

    /* 复位序列（esp-bsp 同款）：0x1F -> 停 20ms -> 0x00 -> 0x80 上电 */
    ESP_RETURN_ON_ERROR(es8311_write(REG_RESET, 0x1F), TAG, "rst1");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(es8311_write(REG_RESET, 0x00), TAG, "rst2");
    ESP_RETURN_ON_ERROR(es8311_write(REG_RESET, 0x80), TAG, "pwr");

    /* 时钟源：0x3F = 打开全部内部时钟；bit7=1 表示 MCLK 取自 SCLK 引脚 */
    ESP_RETURN_ON_ERROR(es8311_write(REG_CLK01, 0x3F | 0x80), TAG, "clk01");
    ESP_RETURN_ON_ERROR(es8311_clock_config(rate), TAG, "clkcfg");

    /* SDP In/Out：16bit 标准 I2S（值 0x0C<<2 由 16bit 编码 (3<<2) 得来） */
    ESP_RETURN_ON_ERROR(es8311_write(REG_SDPIN, 0x0C), TAG, "sdpin");
    ESP_RETURN_ON_ERROR(es8311_write(REG_SDPOUT, 0x0C), TAG, "sdpout");

    /* 模拟部分上电（顺序与官方驱动一致） */
    ESP_RETURN_ON_ERROR(es8311_write(REG_SYS0D, 0x01), TAG, "0d");
    ESP_RETURN_ON_ERROR(es8311_write(REG_SYS0E, 0x02), TAG, "0e");
    ESP_RETURN_ON_ERROR(es8311_write(REG_SYS12, 0x00), TAG, "12");
    ESP_RETURN_ON_ERROR(es8311_write(REG_SYS13, 0x10), TAG, "13");
    ESP_RETURN_ON_ERROR(es8311_write(0x1C, 0x6A), TAG, "1c");   /* ADC 均衡旁路 */
    ESP_RETURN_ON_ERROR(es8311_write(0x37, 0x08), TAG, "37");   /* DAC 均衡旁路 */

    /* 音量 + 解除静音 */
    es8311_write(REG_DAC32, (uint8_t)((s_volume * 256 / 100) - 1));
    uint8_t m;
    if (es8311_read(REG_DAC31, &m) == ESP_OK) {
        es8311_write(REG_DAC31, m & ~0x60);
    }

    err = es8311_read(REG_CLK01, &m);
    ESP_LOGI(TAG, "es8311 ready (rate=%lu, reg01=0x%02x)", (unsigned long)rate, err == ESP_OK ? m : 0);
    return ESP_OK;
}

/* ==================== NVS 音量 ==================== */

static void volume_load(void)
{
    nvs_handle_t h;
    if (nvs_open("audio", NVS_READONLY, &h) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(h, "vol", &v) == ESP_OK && v <= 100) {
            s_volume = v;
        }
        nvs_close(h);
    }
}

esp_err_t bsp_audio_volume_set(int vol)
{
    if (vol < 0) {
        vol = 0;
    }
    if (vol > 100) {
        vol = 100;
    }
    s_volume = vol;
    if (s_es) {
        uint8_t reg = vol == 0 ? 0 : (uint8_t)((vol * 256 / 100) - 1);
        es8311_write(REG_DAC32, reg);
    }
    nvs_handle_t h;
    if (nvs_open("audio", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "vol", (uint8_t)vol);
        nvs_commit(h);
        nvs_close(h);
    }
    return ESP_OK;
}

int bsp_audio_volume_get(void)
{
    return s_volume;
}

/* ==================== 对外接口 ==================== */

esp_err_t bsp_audio_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    volume_load();

    /* 挂 ES8311 到共享 I2C 总线 */
    esp_err_t err = bsp_i2c_add_device(ES8311_ADDR, &s_es);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "es8311 i2c add failed: %s", esp_err_to_name(err));
        return err;
    }
    if (!bsp_i2c_probe(ES8311_ADDR)) {
        ESP_LOGW(TAG, "es8311 not found on i2c, audio disabled");
        return ESP_ERR_NOT_FOUND;
    }

    /* I2S 标准 TX 通道，16bit 立体声 */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 512;   /* 8*512 帧 ≈ 93ms @44.1k，够抗抖 */
    err = i2s_new_channel(&chan_cfg, &s_tx, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s new channel failed: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,     /* 无 MCLK 线，ES8311 内部从 SCLK 取 */
            .bclk = AUD_I2S_BCLK,
            .ws = AUD_I2S_WS,
            .dout = AUD_I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    err = i2s_channel_init_std_mode(s_tx, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s init std failed: %s", esp_err_to_name(err));
        return err;
    }

    s_inited = true;
    ESP_LOGI(TAG, "audio inited (i2s0, es8311@0x18, vol=%d%%)", s_volume);
    return ESP_OK;
}

esp_err_t bsp_audio_start(uint32_t sample_rate)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }

    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    clk.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    ESP_RETURN_ON_ERROR(i2s_channel_disable(s_tx), TAG, "dis");
    ESP_RETURN_ON_ERROR(i2s_channel_reconfig_std_clock(s_tx, &clk), TAG, "clk");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "en");

    ESP_RETURN_ON_ERROR(es8311_hw_init(sample_rate), TAG, "es8311");

    /* 最后开功放，避免配置期间的爆音 */
    esp_err_t e = bsp_pca9557_set_pin(1, true);
    s_pa_on = (e == ESP_OK);
    s_started = true;
    ESP_LOGI(TAG, "audio started @%lu Hz, PA %s", (unsigned long)sample_rate, s_pa_on ? "on" : "FAIL");
    return ESP_OK;
}

esp_err_t bsp_audio_write(const int16_t *pcm, size_t samples)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    size_t bytes = samples * sizeof(int16_t);
    size_t written = 0;
    esp_err_t err = i2s_channel_write(s_tx, pcm, bytes, &written, portMAX_DELAY);
    return (err == ESP_OK && written == bytes) ? ESP_OK : ESP_FAIL;
}

esp_err_t bsp_audio_stop(void)
{
    if (!s_inited || !s_started) {
        return ESP_OK;
    }
    /* 短暂淡出再静音，然后关功放 */
    if (s_es) {
        uint8_t m;
        if (es8311_read(REG_DAC31, &m) == ESP_OK) {
            es8311_write(REG_DAC31, m | 0x60);
        }
    }
    i2s_channel_disable(s_tx);
    bsp_pca9557_set_pin(1, false);
    s_pa_on = false;
    s_started = false;
    ESP_LOGI(TAG, "audio stopped");
    return ESP_OK;
}
