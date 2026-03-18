#ifndef MQ135_H
#define MQ135_H

#include "esp_err.h"

// Initialize MQ-135 gas sensor on the given ADC pin
esp_err_t mq135_init(int adc_channel);

// Read gas concentration in PPM
esp_err_t mq135_read_ppm(float *ppm);

#endif // MQ135_H
