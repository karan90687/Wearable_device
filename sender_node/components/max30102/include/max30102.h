#ifndef MAX30102_H
#define MAX30102_H

#include "esp_err.h"

#define MAX30102_I2C_ADDR   0x57

// Initialize MAX30102 sensor on the given I2C port
esp_err_t max30102_init(int i2c_port, int sda_pin, int scl_pin);

// Read heart rate (BPM) and SpO2 (%) values
// Returns ESP_OK on success, values written to pointers
esp_err_t max30102_read(float *heart_rate, float *spo2);

#endif // MAX30102_H
