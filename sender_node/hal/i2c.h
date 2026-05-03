#ifndef WEARABLE_I2C_H
#define WEARABLE_I2C_H

#include "esp_err.h"
#include "driver/i2c.h"

// Shared I²C bus configuration — used by MAX30102 (0x57) and TMP117 (0x48).
// Pins follow the project pin map (sender PCB).
#define I2C_MASTER_PORT       I2C_NUM_0
#define I2C_MASTER_SDA_GPIO   21
#define I2C_MASTER_SCL_GPIO   22
#define I2C_MASTER_FREQ_HZ    100000
#define I2C_MASTER_TIMEOUT_MS 1000

/**
 * @brief Initialise the shared I²C master bus.
 *
 * Idempotent — safe to call from multiple drivers; second call is a no-op.
 * Configures pins, internal pull-ups, and installs the I²C driver.
 *
 * @return ESP_OK on success, error code from underlying ESP-IDF i2c_* calls.
 */
esp_err_t i2c_init(void);

#endif // WEARABLE_I2C_H
