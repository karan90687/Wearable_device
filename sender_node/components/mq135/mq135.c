#include "mq135.h"
#include "esp_log.h"

static const char *TAG = "mq135";

// TODO: Stage 2 — implement ADC read + voltage-to-PPM conversion

esp_err_t mq135_init(int adc_channel)
{
    ESP_LOGI(TAG, "MQ-135 init (stub) — ADC channel=%d", adc_channel);
    return ESP_OK;
}

esp_err_t mq135_read_ppm(float *ppm)
{
    if (ppm) *ppm = 0.0f;
    return ESP_ERR_NOT_FINISHED;
}
