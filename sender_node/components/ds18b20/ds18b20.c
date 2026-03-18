#include "ds18b20.h"
#include "esp_log.h"

static const char *TAG = "ds18b20";

// TODO: Stage 2 — implement OneWire protocol + temperature conversion

esp_err_t ds18b20_init(int gpio_pin)
{
    ESP_LOGI(TAG, "DS18B20 init (stub) — GPIO=%d", gpio_pin);
    return ESP_OK;
}

esp_err_t ds18b20_read_temp(float *temperature)
{
    if (temperature) *temperature = 0.0f;
    return ESP_ERR_NOT_FINISHED;
}
