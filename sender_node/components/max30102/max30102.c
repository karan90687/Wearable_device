#include "max30102.h"
#include "esp_log.h"

static const char *TAG = "max30102";

// TODO: Stage 2 — implement full I2C driver
// Register map, FIFO read, HR/SpO2 algorithm

esp_err_t max30102_init(int i2c_port, int sda_pin, int scl_pin)
{
    ESP_LOGI(TAG, "MAX30102 init (stub) — I2C port=%d SDA=%d SCL=%d", i2c_port, sda_pin, scl_pin);
    return ESP_OK;
}

esp_err_t max30102_read(float *heart_rate, float *spo2)
{
    // Stub: return dummy values
    if (heart_rate) *heart_rate = 0.0f;
    if (spo2) *spo2 = 0.0f;
    return ESP_ERR_NOT_FINISHED;
}
