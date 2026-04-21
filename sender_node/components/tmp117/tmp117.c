#include "tmp117.h"
#include "tmp117_reg.h"
#include "driver/i2c.h"
#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "tmp117";

#define tmp117_I2C_ADDR 0x48    // TODO : check the address wiht sensor also 

esp_err_t tmp117_init(void)
{
    esp_err_t ret;

    ret = tmp117_check_device();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Device not found or ID mismatch");
        return ret;
    }
vTaskDelay(pdMS_TO_TICKS(20));
    ESP_LOGI(TAG, "TMP117 initialized successfully");

// TMP117 Configuration Register reset value: 0x0220
// MOD[1:0]  = 00  → Continuous conversion
// CONV[2:0] = 100 → 1-second conversion cycle
// AVG[1:0]  = 10  → 8 averages (best accuracy, ±0.1°C)
// No configuration needed — reset defaults are optimal for wearable body temp monitoring

    return ESP_OK;
}

esp_err_t tmp117_read_temp(float *temperature)
{
    uint8_t data[2];
    esp_err_t ret;

    // Read 2 bytes from the Temperature Result register
    ret = tmp117_read_reg(TMP117_RESULT, data, 2);
    if (ret != ESP_OK) return ret;

    // TMP117 is big-endian: MSB first
    int16_t raw = (int16_t)((data[0] << 8) | data[1]);

    // Resolution: 0.0078125 °C per LSB (datasheet page 23)
    *temperature = raw * 0.0078125f;

    return ESP_OK;
}


esp_err_t tmp117_check_device(void)
{
    uint8_t data[2];
    esp_err_t ret;

    ret = tmp117_read_reg(TMP117_DEVICE_ID, data, 2);
    if (ret != ESP_OK) return ret;

    // TMP117 is big-endian: MSB first
    uint16_t part_id = (data[0] << 8) | data[1];

    // Mask out revision bits [15:12], check only DID[11:0] = 0x0117
    if ((part_id & 0x0FFF) != 0x0117) {
        ESP_LOGE(TAG, "Unexpected device ID: 0x%04X (expected DID=0x0117)", part_id);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "TMP117 detected, ID=0x%04X", part_id);
    return ESP_OK;
} 


static esp_err_t tmp117_read_reg(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_write_read_device(
        I2C_NUM_0,
        TMP117_I2C_ADDR,
        &reg,          // register address (pointer register)
        1,
        data,          // read buffer
        len,           // number of bytes to read
        pdMS_TO_TICKS(100)
    );
}

// TMP117 registers are ALL 16-bit — must send MSB then LSB
static esp_err_t tmp117_write_reg(uint8_t reg, uint16_t data)
{
    uint8_t buffer[3] = {reg, (data >> 8) & 0xFF, data & 0xFF};

    return i2c_master_write_to_device(
        I2C_NUM_0,
        TMP117_I2C_ADDR,
        buffer,        // [pointer_reg, data_MSB, data_LSB]
        3,             // 3 bytes total
        pdMS_TO_TICKS(100)
    );
}

/* Usage example in main:
float temp;
esp_err_t status = tmp117_read_temp(&temp);

if (status == ESP_OK) {
    printf("Temperature: %.2f °C\n", temp);
}
*/