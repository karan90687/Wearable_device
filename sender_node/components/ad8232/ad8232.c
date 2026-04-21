/*
 * ad8232.c — ECG front-end driver for the AD8232 single-lead heart rate monitor
 *
 * Hardware interface:
 *   OUTPUT  → ADC input GPIO (analog ECG signal, 0–3.3 V)
 *   LO+     → Digital GPIO input (leads-off detection, active HIGH)
 *   LO-     → Digital GPIO input (leads-off detection, active HIGH)
 *   SDN     → tie to GND (sensor always on) or control via GPIO for power saving
 *
 * The AD8232 is a pure analog device — there are no I2C/SPI registers.
 * All configuration is done at the hardware level (resistors, capacitors).
 * The ESP32 only needs to:
 *   1. Sample the analog OUTPUT pin via ADC
 *   2. Monitor LO+ and LO- for electrode contact status
 *
 * Sampling note:
 *   ECG bandwidth is 0.5–150 Hz. Sample at ≥300 Hz (every ~3 ms) for
 *   reliable signal capture. Use a FreeRTOS task with vTaskDelay(3 ms).
 */

#include "ad8232.h"
#include "esp_adc/adc_oneshot.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_err.h"
#include <string.h>

static const char *TAG = "ad8232";

// ADC handle (ESP-IDF v5 oneshot API — matches your sdkconfig/idf version)
static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static bool s_initialized = false;

// ============================================================
// Init
// ============================================================

esp_err_t ad8232_init(void)
{
    if (s_initialized) return ESP_OK;

    esp_err_t ret;

    // --- Step 1: Configure ADC unit ---
    adc_oneshot_unit_init_cfg_t adc_cfg = {
        .unit_id  = ADC_UNIT_1,   // GPIO34 is on ADC1; ADC2 conflicts with Wi-Fi
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };

    ret = adc_oneshot_new_unit(&adc_cfg, &s_adc_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC unit init failed: %d", ret);
        return ret;
    }

    // --- Step 2: Configure ADC channel ---
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten    = AD8232_ADC_ATTEN,    // 0–3.3 V range (11 dB attenuation)
        .bitwidth = ADC_BITWIDTH_12,     // 12-bit resolution → 0–4095
    };

    ret = adc_oneshot_config_channel(s_adc_handle, AD8232_OUTPUT_ADC_CHANNEL, &chan_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC channel config failed: %d", ret);
        return ret;
    }

    // --- Step 3: Configure LO+ GPIO (leads-off positive) ---
    gpio_config_t lo_cfg = {
        .pin_bit_mask = (1ULL << AD8232_LO_PLUS_GPIO) | (1ULL << AD8232_LO_MINUS_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLDOWN_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,  // pull low; AD8232 drives HIGH when lead off
        .intr_type    = GPIO_INTR_DISABLE,
    };

    ret = gpio_config(&lo_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GPIO config for leads-off pins failed: %d", ret);
        return ret;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "AD8232 initialized successfully");
    ESP_LOGI(TAG, "  ADC channel : %d  (GPIO%d)", AD8232_OUTPUT_ADC_CHANNEL, 34);
    ESP_LOGI(TAG, "  LO+ GPIO    : %d", AD8232_LO_PLUS_GPIO);
    ESP_LOGI(TAG, "  LO- GPIO    : %d", AD8232_LO_MINUS_GPIO);

    return ESP_OK;
}

// ============================================================
// Leads-off detection
// ============================================================

bool ad8232_leads_off(void)
{
    /*
     * LO+ and LO- are HIGH when the corresponding electrode is disconnected.
     * If either pin reads HIGH, the ECG signal is invalid — discard the sample.
     */
    int lo_plus  = gpio_get_level(AD8232_LO_PLUS_GPIO);
    int lo_minus = gpio_get_level(AD8232_LO_MINUS_GPIO);

    return (lo_plus == 1) || (lo_minus == 1);
}

// ============================================================
// Sample read
// ============================================================

esp_err_t ad8232_read_sample(ad8232_sample_t *sample)
{
    // Step 1: check electrode contact first
    sample->leads_off = ad8232_leads_off();

    if (sample->leads_off) {
        sample->raw = 0;
        // Not a driver error — electrode state is valid information
        return ESP_OK;
    }

    // Step 2: read ADC
    int adc_raw = 0;
    esp_err_t ret = adc_oneshot_read(s_adc_handle, AD8232_OUTPUT_ADC_CHANNEL, &adc_raw);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC read failed: %d", ret);
        sample->raw = 0;
        return ret;
    }

    sample->raw = (uint16_t)(adc_raw & 0x0FFF);  // mask to 12-bit

    return ESP_OK;
}

// ============================================================
// Moving average filter (mirrors max30102 filter in codebase)
// ============================================================

void ad8232_filter_init(ad8232_filter_t *f)
{
    memset(f->buffer, 0, sizeof(f->buffer));
    f->sum   = 0;
    f->index = 0;
    f->count = 0;
}

uint32_t ad8232_filter_update(ad8232_filter_t *f, uint32_t new_value)
{
    // remove oldest value from sum
    f->sum -= f->buffer[f->index];

    // insert new value
    f->buffer[f->index] = new_value;
    f->sum += new_value;

    // advance circular index
    f->index = (f->index + 1) % AD8232_FILTER_SIZE;

    // track fill level for the initial warm-up phase
    if (f->count < AD8232_FILTER_SIZE) {
        f->count++;
    }

    return f->sum / f->count;
}
// to be added in the main task for the reading capture call must be done after the i2c_init 
//   xTaskCreate(ecg_task, "ecg", 2048, NULL, 5, NULL);
//   Sample rate: 300 Hz (one read every ~3 ms) covers the full ECG bandwidth.
//  Increase to 500 Hz if you need cleaner QRS peak detection.
 