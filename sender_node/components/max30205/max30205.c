#include "max30205.h"
#include "esp_log.h"

static const char *TAG = "max30205";

// TODO: Stage 2 — implement I2C temperature register read

esp_err_t max30205_init(int i2c_port, int sda_pin, int scl_pin)
{
    ESP_LOGI(TAG, "MAX30205 init (stub) — I2C port=%d SDA=%d SCL=%d", i2c_port, sda_pin, scl_pin);
    return ESP_OK;
}

esp_err_t max30205_read_temp(float *temperature)
{
    if (temperature) *temperature = 0.0f;
    return ESP_ERR_NOT_FINISHED;
}
