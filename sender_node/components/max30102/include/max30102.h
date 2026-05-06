#ifndef MAX30102_H
#define MAX30102_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define MAX30102_I2C_ADDR   0x57
#define FILTER_SIZE         5

// ─── Sample types ───────────────────────────────────────────────────
typedef struct {
    uint32_t red;
    uint32_t ir;
} max30102_sample_t;

typedef struct {
    uint32_t buffer[FILTER_SIZE];
    uint32_t sum;
    int      index;
    int      count;
} moving_avg_filter_t;

// One polling tick produces this — caller drains it into shared state.
typedef struct {
    uint32_t ir;             // raw IR sample
    uint32_t red;            // raw RED sample
    bool     body_contact;   // true = finger / skin on the sensor
    bool     beat_detected;  // true if THIS sample triggered a beat
    bool     valid;          // false if FIFO read failed
    float    bpm;            // running average BPM, NaN if unknown
    float    spo2;           // last good SpO2 %, NaN if unknown / no contact
} max30102_reading_t;

// ─── Public API ─────────────────────────────────────────────────────

// Initialise the chip. The shared I²C bus must already be up
// (call hal/i2c.c's `i2c_init()` before this).
esp_err_t max30102_init(void);

// Drain one (Red, IR) pair from the FIFO and run beat-detection /
// SpO2 update logic on it. Call at ~100 Hz from a dedicated task.
// Returns ESP_OK on success, ESP_ERR_* if the FIFO read failed
// (the `out` struct still gets the latest known BPM/SpO2 in that case).
esp_err_t max30102_step(max30102_reading_t *out);

// MAX30102 silicon die temperature in °C. NOT body temperature —
// reads the chip's internal temp sensor (typically 30–35 °C).
float max30102_read_die_temp(void);

// Backward-compat — used by the standalone test_main.c PCB selftest.
esp_err_t max30102_read_sample(max30102_sample_t *sample);

// Moving-average filter — kept so test_main.c keeps building.
void     filter_init(moving_avg_filter_t *f);
uint32_t filter_update(moving_avg_filter_t *f, uint32_t new_value);

#endif // MAX30102_H
