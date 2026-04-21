#ifndef AD8232_H
#define AD8232_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

// ============================================================
// AD8232 is an analog ECG front-end — it does NOT use I2C.
// It outputs a single analog voltage on the OUTPUT pin,
// and two digital leads-off detection pins (LO+ and LO-).
//
// GPIO Configuration — update to match your PCB layout:
// ============================================================
#define AD8232_OUTPUT_ADC_CHANNEL   ADC_CHANNEL_6   // GPIO34 on ESP32 (input only, safe choice)
#define AD8232_LO_PLUS_GPIO         32              // Leads-off detection positive
#define AD8232_LO_MINUS_GPIO        33              // Leads-off detection negative

// ADC resolution: 12-bit → 0–4095
#define AD8232_ADC_WIDTH            ADC_WIDTH_BIT_12
#define AD8232_ADC_ATTEN            ADC_ATTEN_DB_11  // 0–3.3V input range

// Moving average filter size (same as max30102 driver)
#define AD8232_FILTER_SIZE          5

// ============================================================
// Data Types
// ============================================================

// Raw ECG sample from one ADC read
typedef struct {
    uint16_t raw;        // 12-bit ADC value (0–4095)
    bool     leads_off;  // true if either electrode is disconnected
} ad8232_sample_t;

// Moving average filter (mirrors max30102's moving_avg_filter_t)
typedef struct {
    uint32_t buffer[AD8232_FILTER_SIZE];
    uint32_t sum;
    int      index;
    int      count;
} ad8232_filter_t;

// ============================================================
// Public API
// ============================================================

/**
 * @brief  Initialize the AD8232 driver.
 *         Configures the ADC channel and LO+/LO- GPIO pins.
 *         Call once at startup (after i2c_init() if sharing power rail).
 *
 * @return ESP_OK on success, ESP_FAIL on ADC/GPIO error
 */
esp_err_t ad8232_init(void);

/**
 * @brief  Read one ECG sample.
 *         Checks leads-off pins first; if electrodes are off, sets
 *         sample->leads_off = true and sample->raw = 0.
 *
 * @param  sample  Pointer to output struct
 * @return ESP_OK always (leads-off is not an error, just a flag)
 */
esp_err_t ad8232_read_sample(ad8232_sample_t *sample);

/**
 * @brief  Check if electrodes are properly attached.
 *
 * @return true  — at least one electrode is disconnected (leads off)
 *         false — both electrodes are attached (leads on)
 */
bool ad8232_leads_off(void);

/**
 * @brief  Initialize a moving average filter for ECG smoothing.
 *         Same algorithm as the max30102 filter in this codebase.
 *
 * @param  f  Pointer to filter struct
 */
void ad8232_filter_init(ad8232_filter_t *f);

/**
 * @brief  Feed a new raw ADC value into the filter.
 *
 * @param  f          Pointer to filter struct
 * @param  new_value  Latest raw ADC reading
 * @return Smoothed (averaged) value
 */
uint32_t ad8232_filter_update(ad8232_filter_t *f, uint32_t new_value);

#endif // AD8232_H