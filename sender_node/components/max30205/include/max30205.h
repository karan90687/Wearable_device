#ifndef MAX30205_H
#define MAX30205_H

#include "esp_err.h"

#define MAX30205_I2C_ADDR   0x48

// Initialize MAX30205 body temperature sensor
esp_err_t max30205_init(int i2c_port, int sda_pin, int scl_pin);

// Read body temperature in Celsius
esp_err_t max30205_read_temp(float *temperature);

#endif // MAX30205_H
