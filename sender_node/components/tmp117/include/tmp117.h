#ifndef TMP117_H
#define TMP117_H

#include "esp_err.h"

// Initialize TMP117 temperature sensor (I2C must already be initialized)
esp_err_t tmp117_init(void);

// Read environment temperature in Celsius
esp_err_t tmp117_read_temp(float *temperature);

// Verify TMP117 device ID
esp_err_t tmp117_check_device(void);

#endif // TMP117_H
