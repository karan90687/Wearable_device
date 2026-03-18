#ifndef BUZZER_H
#define BUZZER_H

#include "esp_err.h"
#include <stdbool.h>

// Initialize buzzer on the given GPIO pin
esp_err_t buzzer_init(int gpio_pin);

// Turn buzzer on/off
void buzzer_set(bool on);

// Beep pattern: on for on_ms, off for off_ms, repeat count times
void buzzer_beep(int on_ms, int off_ms, int count);

#endif // BUZZER_H
