#ifndef MAX30102_H
#define MAX30102_H

#include "esp_err.h"

#define MAX30102_I2C_ADDR   0x57

typedef struct {
    uint32_t red;
    uint32_t ir;
} max30102_sample_t;

// Initialize MAX30102 sensor on the given I2C port
esp_err_t max30102_init();

// Read heart rate (BPM) and SpO2 (%) values
// Returns ESP_OK on success, values written to pointers
esp_err_t max30102_read_sample(max30102_sample_t *sample);

#endif // MAX30102_H
