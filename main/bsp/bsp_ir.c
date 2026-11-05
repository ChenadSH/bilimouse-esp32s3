#include "bsp_ir.h"

#include <string.h>

#include "driver/rmt_rx.h"
#include "driver/rmt_tx.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "bsp_ir";

/* 1MHz 分辨率：1 tick = 1us，NEC 的 560us/1690us 正好是整数 tick */
#define IR_RESOLUTION_HZ   1000000
/* 每通道 96 个 symbol（S3 一共 4 个 48 字的 block，收发各占 2 个）。
   NEC 一帧 = 引导码 2 + 32 位 ×2 + 结束位 1 = 67 symbol，够用。 */
#define IR_MEM_SYMBOLS     96
#define IR_CARRIER_HZ      38000
#define IR_CARRIER_DUTY    33      /* 占空比 33%，红外 LED 的常规值 */

#define BIT_RX_DONE        BIT0

static rmt_channel_handle_t s_tx_chan;
static rmt_channel_handle_t s_rx_chan;
static rmt_encoder_handle_t s_copy_enc;
static EventGroupHandle_t    s_rx_evt;
static volatile size_t       s_rx_count;
static bool                  s_ready;

/* ==================== 接收 ==================== */

static bool IRAM_ATTR on_rx_done(rmt_channel_handle_t chan,
                                const rmt_rx_done_event_data_t *edata,
                                void *user_data)
{
    (void)chan;
    (void)user_data;
    BaseType_t hp = pdFALSE;

    /* edata->received_symbols 在回调返回后就会被驱动复用，只记数量，
       数据本身已经被驱动直接写进我们传进去的 buffer 里了。 */
    s_rx_count = edata->num_symbols;
    xEventGroupSetBitsFromISR(s_rx_evt, BIT_RX_DONE, &hp);
    return hp == pdTRUE;
}

/* ==================== 发射 ==================== */

static inline void sym_set(rmt_symbol_word_t *s, uint16_t d0, bool l0, uint16_t d1, bool l1)
{
    s->duration0 = d0;
    s->level0    = l0;
    s->duration1 = d1;
    s->level1    = l1;
}

esp_err_t bsp_ir_send_raw(const rmt_symbol_word_t *syms, size_t count)
{
    if (!s_ready || !s_tx_chan) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!syms || count == 0 || count > IR_MEM_SYMBOLS) {
        return ESP_ERR_INVALID_ARG;
    }

    rmt_transmit_config_t tx_cfg = {
        .loop_count = 0,          /* 不发重复码 */
        .flags.eot_level = 0,     /* 发完把线拉低，避免 LED 常亮 */
    };
    ESP_RETURN_ON_ERROR(rmt_transmit(s_tx_chan, s_copy_enc, syms,
                                     count * sizeof(rmt_symbol_word_t), &tx_cfg),
                        TAG, "transmit failed");
    return rmt_tx_wait_all_done(s_tx_chan, 1000);
}

esp_err_t bsp_ir_send_nec(uint8_t addr, uint8_t cmd)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 引导码 + 32 位（地址、地址反码、命令、命令反码，均为 LSB 优先）
       + 结束位。共 2 + 64 + 1 = 67 个 symbol。 */
    rmt_symbol_word_t frame[2 + 32 * 2 + 1];
    size_t n = 0;

    sym_set(&frame[n++], 9000, true, 4500, false);   /* 引导码 */

    uint8_t bytes[4] = { addr, (uint8_t)~addr, cmd, (uint8_t)~cmd };
    for (int b = 0; b < 4; b++) {
        for (int bit = 0; bit < 8; bit++) {
            bool one = (bytes[b] >> bit) & 0x01;
            /* bit=0: 560us 亮 + 560us 灭；bit=1: 560us 亮 + 1690us 灭 */
            sym_set(&frame[n++], 560, true, one ? 1690 : 560, false);
        }
    }
    sym_set(&frame[n++], 560, true, 0, false);       /* 结束位 */

    ESP_LOGI(TAG, "NEC send addr=0x%02x cmd=0x%02x (%u syms)", addr, cmd, (unsigned)n);
    return bsp_ir_send_raw(frame, n);
}

size_t bsp_ir_capture(rmt_symbol_word_t *out, size_t max_symbols, uint32_t timeout_ms)
{
    if (!s_ready || !s_rx_chan || !out || max_symbols == 0) {
        return 0;
    }
    if (max_symbols > IR_MEM_SYMBOLS) {
        max_symbols = IR_MEM_SYMBOLS;
    }

    xEventGroupClearBits(s_rx_evt, BIT_RX_DONE);
    s_rx_count = 0;

    /* signal_range_max_ns = 40ms：接收头在最后一个跳变后静默 40ms
       就自动判定一帧结束，不需要额外的超时逻辑。 */
    rmt_receive_config_t rx_cfg = {
        .signal_range_min_ns = 1000,          /* <1us 当毛刺丢掉 */
        .signal_range_max_ns = 40 * 1000 * 1000,
    };

    esp_err_t err = rmt_receive(s_rx_chan, out, max_symbols * sizeof(rmt_symbol_word_t), &rx_cfg);
    if (err != ESP_OK) {
        /* 上一次接收还没收尾时会返回 INVALID_STATE，复位通道再试一次 */
        ESP_LOGW(TAG, "rx start failed (%s), resetting channel", esp_err_to_name(err));
        rmt_disable(s_rx_chan);
        rmt_enable(s_rx_chan);
        err = rmt_receive(s_rx_chan, out, max_symbols * sizeof(rmt_symbol_word_t), &rx_cfg);
        if (err != ESP_OK) {
            return 0;
        }
    }

    EventBits_t bits = xEventGroupWaitBits(s_rx_evt, BIT_RX_DONE, pdTRUE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));
    if (!(bits & BIT_RX_DONE)) {
        /* 超时：把通道复位，免得下次调用永远 INVALID_STATE */
        rmt_disable(s_rx_chan);
        rmt_enable(s_rx_chan);
        return 0;
    }

    size_t cnt = s_rx_count;
    if (cnt > max_symbols) {
        cnt = max_symbols;
    }
    return cnt;
}

/* ==================== 初始化 ==================== */

bool bsp_ir_ready(void)
{
    return s_ready;
}

esp_err_t bsp_ir_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    s_rx_evt = xEventGroupCreate();
    if (!s_rx_evt) {
        return ESP_ERR_NO_MEM;
    }

    rmt_tx_channel_config_t tx_cfg = {
        .gpio_num = BSP_IR_TX_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = IR_RESOLUTION_HZ,
        .mem_block_symbols = IR_MEM_SYMBOLS,
        .trans_queue_depth = 4,
    };
    ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&tx_cfg, &s_tx_chan), TAG, "tx channel failed");

    rmt_carrier_config_t carrier = {
        .frequency_hz = IR_CARRIER_HZ,
        .duty_cycle = (float)IR_CARRIER_DUTY / 100.0f,
        .flags.polarity_active_low = false,
        .flags.always_on = false,
    };
    ESP_RETURN_ON_ERROR(rmt_apply_carrier(s_tx_chan, &carrier), TAG, "carrier failed");

    rmt_copy_encoder_config_t enc_cfg = {};
    ESP_RETURN_ON_ERROR(rmt_new_copy_encoder(&enc_cfg, &s_copy_enc), TAG, "encoder failed");
    ESP_RETURN_ON_ERROR(rmt_enable(s_tx_chan), TAG, "tx enable failed");

    rmt_rx_channel_config_t rx_cfg = {
        .gpio_num = BSP_IR_RX_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = IR_RESOLUTION_HZ,
        .mem_block_symbols = IR_MEM_SYMBOLS,
        .flags.invert_in = true,   /* 接收头低电平有效 -> 翻成 level=1 表示有载波 */
        .flags.with_dma = false,
    };
    ESP_RETURN_ON_ERROR(rmt_new_rx_channel(&rx_cfg, &s_rx_chan), TAG, "rx channel failed");

    rmt_rx_event_callbacks_t cbs = {
        .on_recv_done = on_rx_done,
    };
    ESP_RETURN_ON_ERROR(rmt_rx_register_event_callbacks(s_rx_chan, &cbs, NULL), TAG, "rx cb failed");
    ESP_RETURN_ON_ERROR(rmt_enable(s_rx_chan), TAG, "rx enable failed");

    s_ready = true;
    ESP_LOGI(TAG, "initialized (tx=GPIO%d rx=GPIO%d, %dHz carrier)",
             BSP_IR_TX_GPIO, BSP_IR_RX_GPIO, IR_CARRIER_HZ);
    return ESP_OK;
}
