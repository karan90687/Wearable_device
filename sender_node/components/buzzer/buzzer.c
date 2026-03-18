#include "buzzer.h"
#include "esp_log.h"

static const char *TAG = "buzzer";

// TODO: Stage 4 — implement GPIO-based buzzer control

esp_err_t buzzer_init(int gpio_pin)
{
    ESP_LOGI(TAG, "Buzzer init (stub) — GPIO=%d", gpio_pin);
    return ESP_OK;
}

void buzzer_set(bool on)
{
    ESP_LOGD(TAG, "Buzzer %s (stub)", on ? "ON" : "OFF");
}

void buzzer_beep(int on_ms, int off_ms, int count)
{
    ESP_LOGD(TAG, "Buzzer beep (stub): %dms on, %dms off, %dx", on_ms, off_ms, count);
}
