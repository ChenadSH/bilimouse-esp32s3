#include "bsp_imu.h"
#include "bsp_board.h"

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bsp_imu";

/* ---- QMI8658 寄存器 ---- */
#define QMI8658_REG_WHO_AM_I   0x00
#define QMI8658_REG_CTRL1      0x02
#define QMI8658_REG_CTRL2      0x03
#define QMI8658_REG_CTRL3      0x04
#define QMI8658_REG_CTRL7      0x08
#define QMI8658_REG_STATUS0    0x2E
#define QMI8658_REG_TEMP_L     0x33
#define QMI8658_REG_AX_L       0x35
#define QMI8658_REG_GX_L       0xB3 & 0xFF   /* 占位，实际用 AX_L + 连续读取 */
#define QMI8658_REG_RESET      0x60

#define QMI8658_WHO_AM_I_VAL   0x05
#define QMI8658_RESET_CMD      0xB0

/* CTRL1: bit6 = 地址自增（连续读多字节） */
#define QMI8658_CTRL1_ADDR_AI  (1 << 6)

/* CTRL2: bit6:4 = 量程, bit3:0 = ODR */
#define QMI8658_ACC_RANGE_4G   0x10   /* ±4g */
#define QMI8658_ACC_ODR_200HZ  0x04   /* 200Hz */
#define QMI8658_CTRL2_VAL      (QMI8658_ACC_RANGE_4G | QMI8658_ACC_ODR_200HZ)

/* CTRL3: bit6:4 = 量程, bit3:0 = ODR */
#define QMI8658_GYR_RANGE_1024 0x60   /* ±1024 dps */
#define QMI8658_GYR_ODR_200HZ  0x04   /* 200Hz */
#define QMI8658_CTRL3_VAL      (QMI8658_GYR_RANGE_1024 | QMI8658_GYR_ODR_200HZ)

/* CTRL7: bit0 = acc 使能, bit1 = gyro 使能 */
#define QMI8658_CTRL7_ACC_EN   (1 << 0)
#define QMI8658_CTRL7_GYR_EN   (1 << 1)
#define QMI8658_CTRL7_VAL      (QMI8658_CTRL7_ACC_EN | QMI8658_CTRL7_GYR_EN)

/* 连续读取：温度(2) + 加速度(6) + 陀螺仪(6) = 14 字节 */
#define QMI8658_DATA_LEN       14
#define QMI8658_OFF_TEMP       0
#define QMI8658_OFF_ACC        2
#define QMI8658_OFF_GYR        8

/* 刻度系数 */
#define QMI8658_ACC_SCALE      (4.0f / 32768.0f)     /* ±4g */
#define QMI8658_GYR_SCALE      (1024.0f / 32768.0f)  /* ±1024 dps */
#define QMI8658_TEMP_SCALE     (1.0f / 256.0f)

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;

static esp_err_t imu_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = { reg, value };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), pdMS_TO_TICKS(100));
}

static esp_err_t imu_read_reg(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, pdMS_TO_TICKS(100));
}

bool bsp_imu_probe(void)
{
    uint8_t who = 0;
    if (imu_read_reg(QMI8658_REG_WHO_AM_I, &who, 1) != ESP_OK) {
        return false;
    }
    ESP_LOGI(TAG, "WHO_AM_I = 0x%02X (expect 0x%02X)", who, QMI8658_WHO_AM_I_VAL);
    return who == QMI8658_WHO_AM_I_VAL;
}

esp_err_t bsp_imu_init(void)
{
    /* I2C 总线可能已被 PCA9557 初始化，复用已有的 bus handle */
    esp_err_t err = i2c_new_master_bus(&(i2c_master_bus_config_t){
        .i2c_port = BSP_I2C_PORT,
        .sda_io_num = BSP_I2C_SDA,
        .scl_io_num = BSP_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = 1,
    }, &s_bus);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "i2c bus init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = i2c_master_bus_add_device(s_bus, &(i2c_device_config_t){
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BSP_QMI8658_ADDR,
        .scl_speed_hz = BSP_I2C_FREQ_HZ,
    }, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add device failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 探测芯片 */
    uint8_t who = 0;
    if (imu_read_reg(QMI8658_REG_WHO_AM_I, &who, 1) != ESP_OK) {
        ESP_LOGE(TAG, "QMI8658 not responding at 0x%02X", BSP_QMI8658_ADDR);
        return ESP_ERR_NOT_FOUND;
    }
    if (who != QMI8658_WHO_AM_I_VAL) {
        ESP_LOGE(TAG, "unexpected WHO_AM_I 0x%02X (expect 0x%02X)", who, QMI8658_WHO_AM_I_VAL);
        return ESP_ERR_INVALID_VERSION;
    }

    /* 软复位 */
    ESP_RETURN_ON_ERROR(imu_write_reg(QMI8658_REG_RESET, QMI8658_RESET_CMD), TAG, "soft reset failed");
    vTaskDelay(pdMS_TO_TICKS(20));

    /* 打开地址自增，之后可连续读取 */
    ESP_RETURN_ON_ERROR(imu_write_reg(QMI8658_REG_CTRL1, QMI8658_CTRL1_ADDR_AI), TAG, "ctrl1 failed");
    ESP_RETURN_ON_ERROR(imu_write_reg(QMI8658_REG_CTRL2, QMI8658_CTRL2_VAL), TAG, "ctrl2 failed");
    ESP_RETURN_ON_ERROR(imu_write_reg(QMI8658_REG_CTRL3, QMI8658_CTRL3_VAL), TAG, "ctrl3 failed");
    ESP_RETURN_ON_ERROR(imu_write_reg(QMI8658_REG_CTRL7, QMI8658_CTRL7_VAL), TAG, "ctrl7 failed");
    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_LOGI(TAG, "initialized (acc ±4g/200Hz, gyro ±1024dps/200Hz)");
    return ESP_OK;
}

esp_err_t bsp_imu_read(bsp_imu_data_t *data)
{
    uint8_t buf[QMI8658_DATA_LEN];
    ESP_RETURN_ON_ERROR(imu_read_reg(QMI8658_REG_TEMP_L, buf, sizeof(buf)), TAG, "read data failed");

    int16_t raw_acc[3], raw_gyr[3], raw_temp;
    raw_temp = (int16_t)(buf[QMI8658_OFF_TEMP] | (buf[QMI8658_OFF_TEMP + 1] << 8));
    for (int i = 0; i < 3; i++) {
        int o = QMI8658_OFF_ACC + i * 2;
        raw_acc[i] = (int16_t)(buf[o] | (buf[o + 1] << 8));
        o = QMI8658_OFF_GYR + i * 2;
        raw_gyr[i] = (int16_t)(buf[o] | (buf[o + 1] << 8));
    }

    data->temperature_c = 25.0f + (float)raw_temp * QMI8658_TEMP_SCALE;
    for (int i = 0; i < 3; i++) {
        data->accel_g[i] = (float)raw_acc[i] * QMI8658_ACC_SCALE;
        data->gyro_dps[i] = (float)raw_gyr[i] * QMI8658_GYR_SCALE;
    }
    return ESP_OK;
}
