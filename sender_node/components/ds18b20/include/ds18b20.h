#ifndef DS18B20_H
#define DS18B20_H

#include "esp_err.h"

// Initialize DS18B20 temperature sensor on the given GPIO pin
esp_err_t ds18b20_init(int gpio_pin);

// Read environment temperature in Celsius
esp_err_t ds18b20_read_temp(float *temperature);

#endif // DS18B20_H
