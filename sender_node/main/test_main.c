/*
 * test_main.c — Sender PCB self-test
 *
 * Standalone application: NO ESP-NOW, NO master node, NO protocol headers.
 * Only purpose is to exercise every sensor + indicator on the sender board
 * so we can verify the PCB is wired correctly before bringing up the radio.
 *
 * Boot sequence:
 *   1. Configure status LED (GPIO 15), buzzer (GPIO 27), AD8232 SDN (GPIO 4)
 *   2. Beep buzzer twice (200 ms on / 200 ms off, ×2)
 *   3. Init I²C bus, then MAX30102, TMP117, AD8232 in turn
 *   4. Drive status LED solid HIGH
 *   5. Spawn three sampling tasks that print readings to stdout forever
 *
 * To run:
 *   - In sender_node/main/CMakeLists.txt swap SRCS "sender_main.c" → "test_main.c"
 *   - idf.py build flash monitor
 *   - When done, swap SRCS back to sender_main.c for normal firmware.
 */

#include <stdio.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/gpio.h"

#include "i2c.h"
#include "max30102.h"
#include "ad8232.h"

static const char *TAG = "sender_test";

// ============================================================
// Pin map (matches the project spec)
// ============================================================
#define PIN_LED_STATUS    GPIO_NUM_15   // green LED — solid ON while running
#define PIN_BUZZER        GPIO_NUM_27   // active buzzer — drive HIGH to beep
#define PIN_AD8232_SDN    GPIO_NUM_4    // AD8232 chip-enable, must be HIGH

// ============================================================
// Print throttling — keep the terminal readable
// ============================================================
#define MAX30102_PRINT_EVERY    10   // 10 ms loop  → print every 100 ms
#define AD8232_PRINT_EVERY      33   //  3 ms loop  → print every ~100 ms
#define AD8232_LEADS_LOG_EVERY 100   // log "leads off" once per ~300 ms

// ============================================================
// GPIO + buzzer helpers
// ============================================================
static void gpio_setup(void)
{
    gpio_reset_pin(PIN_LED_STATUS);
    gpio_set_direction(PIN_LED_STATUS, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_LED_STATUS, 0);

    gpio_reset_pin(PIN_BUZZER);
    gpio_set_direction(PIN_BUZZER, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_BUZZER, 0);

    gpio_reset_pin(PIN_AD8232_SDN);
    gpio_set_direction(PIN_AD8232_SDN, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_AD8232_SDN, 1);   // enable AD8232 analog front-end
}

// Probe every 7-bit I²C address by issuing a zero-byte write; an ACK means a
// device is present at that address. Helps isolate wiring/pull-up problems
// from driver-level issues.
static void i2c_bus_scan(void)
{
    printf("[i2c_scan]  scanning 0x03..0x77 on port %d (SDA=%d SCL=%d)\n",
           I2C_MASTER_PORT, I2C_MASTER_SDA_GPIO, I2C_MASTER_SCL_GPIO);
    int found = 0;
    for (uint8_t addr = 0x03; addr < 0x78; addr++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        esp_err_t ret = i2c_master_cmd_begin(I2C_MASTER_PORT, cmd, pdMS_TO_TICKS(50));
        i2c_cmd_link_delete(cmd);
        if (ret == ESP_OK) {
            printf("[i2c_scan]  found device @ 0x%02X\n", addr);
            found++;
        }
    }
    if (found == 0) {
        printf("[i2c_scan]  NO devices acked — check SDA/SCL wiring, pull-ups (4.7k to 3V3), and sensor power\n");
    } else {
        printf("[i2c_scan]  %d device(s) responded\n", found);
    }
}

static void buzzer_beep_twice(void)
{
    for (int i = 0; i < 2; i++) {
        gpio_set_level(PIN_BUZZER, 1);
        vTaskDelay(pdMS_TO_TICKS(200));
        gpio_set_level(PIN_BUZZER, 0);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

// ============================================================
// Sampling tasks
// ============================================================
//
// MAX30102 task — mirrors the maxi2c.c standalone log format:
//   - "♥ BEAT | Avg BPM: ..." on each detected beat
//   - "IR: ... | Red: ... | Avg BPM: ... | Finger ON" every 10 samples
//   - "IR: ... | No finger detected" (warn) when no contact
//   - Periodic summary every 100 samples while finger is on:
//       SpO2 / Heart Rate / Temp / IR / Red
// All driven by the production driver's max30102_step() so test +
// production firmware agree on detection thresholds.
static void max30102_task(void *pv)
{
    static const char *MAX_TAG = "MAX30102";
    ESP_LOGI(MAX_TAG, "Starting MAX30102 reading task...");
    ESP_LOGI(MAX_TAG, "Place finger on sensor.");

    uint32_t sample_count = 0;
    while (1) {
        max30102_reading_t r;
        esp_err_t err = max30102_step(&r);
        if (err != ESP_OK) {
            ESP_LOGE(MAX_TAG, "FIFO read error (%d)", err);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        sample_count++;

        // Beat-detection log
        if (r.beat_detected) {
            ESP_LOGI(MAX_TAG, "♥ BEAT | Avg BPM: %.1f", r.bpm);
        }

        // Periodic summary every 100 samples (matches BUFFER_LEN inside the
        // driver — that's when SpO2 is recomputed).
        if (sample_count % 100 == 0 && r.body_contact) {
            float die_temp = max30102_read_die_temp();
            if (!isnan(r.spo2)) {
                ESP_LOGI(MAX_TAG, "─────────────────────────────────");
                ESP_LOGI(MAX_TAG, " SpO2      : %.1f %%", r.spo2);
                ESP_LOGI(MAX_TAG, " Heart Rate: %.1f BPM (avg)", r.bpm);
                ESP_LOGI(MAX_TAG, " Temp      : %.2f °C", die_temp);
                ESP_LOGI(MAX_TAG, " IR        : %lu | Red: %lu",
                         (unsigned long)r.ir, (unsigned long)r.red);
                ESP_LOGI(MAX_TAG, "─────────────────────────────────");
            } else {
                ESP_LOGW(MAX_TAG, "SpO2 calculation unreliable. Keep finger still.");
            }
        }

        // Raw data line every 10 samples
        if (sample_count % 10 == 0) {
            if (r.body_contact) {
                ESP_LOGI(MAX_TAG,
                         "IR: %6lu | Red: %6lu | Avg BPM: %.1f | Finger ON",
                         (unsigned long)r.ir, (unsigned long)r.red, r.bpm);
            } else {
                ESP_LOGW(MAX_TAG,
                         "IR: %6lu | No finger detected",
                         (unsigned long)r.ir);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));   // ~100 Hz polling
    }
}

// tmp117_task removed — sensor broken on this PCB; production firmware
// fakes body temperature in sender_main.c instead.

static void ad8232_task(void *pv)
{
    ad8232_filter_t f;
    ad8232_filter_init(&f);

    int counter        = 0;
    int leads_counter  = 0;
    while (1) {
        ad8232_sample_t s;
        if (ad8232_read_sample(&s) == ESP_OK) {
            if (s.leads_off) {
                if (++leads_counter >= AD8232_LEADS_LOG_EVERY) {
                    leads_counter = 0;
                    printf("[AD8232]    leads off — reattach electrodes\n");
                }
            } else {
                leads_counter = 0;
                uint32_t filt = ad8232_filter_update(&f, s.raw);
                if (++counter >= AD8232_PRINT_EVERY) {
                    counter = 0;
                    printf("[AD8232]    ECG raw=%4u  filt=%4lu\n",
                           s.raw, (unsigned long)filt);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(3));   // ~333 Hz sample rate
    }
}

// ============================================================
// app_main
// ============================================================
void app_main(void)
{
    ESP_LOGI(TAG, "================ SENDER PCB SELF-TEST ================");

    gpio_setup();

    ESP_LOGI(TAG, "Buzzer self-test (2 beeps)...");
    buzzer_beep_twice();

    ESP_LOGI(TAG, "Initialising I²C bus on SDA=21 / SCL=22...");
    if (i2c_init() != ESP_OK) {
        ESP_LOGE(TAG, "I²C init FAILED — halting");
        return;
    }

    i2c_bus_scan();

    ESP_LOGI(TAG, "Initialising MAX30102 (HR/SpO2)...");
    if (max30102_init() != ESP_OK) {
        ESP_LOGE(TAG, "MAX30102 init FAILED");
    } else {
        ESP_LOGI(TAG, "MAX30102 OK");
    }

    // TMP117 is broken on this PCB — production firmware fakes the
    // body temperature instead. Skip init + sample task entirely.
    ESP_LOGW(TAG, "TMP117 disabled (hardware broken on this PCB)");

    ESP_LOGI(TAG, "Initialising AD8232 (ECG)...");
    if (ad8232_init() != ESP_OK) {
        ESP_LOGE(TAG, "AD8232 init FAILED");
    } else {
        ESP_LOGI(TAG, "AD8232 OK");
    }

    // All inits attempted — turn the green status LED ON solid to indicate
    // we made it past boot and the sample tasks are about to run.
    gpio_set_level(PIN_LED_STATUS, 1);
    ESP_LOGI(TAG, "Status LED ON — starting sample tasks");
    ESP_LOGI(TAG, "======================================================");

    xTaskCreate(max30102_task, "max30102", 4096, NULL, 5, NULL);
    // tmp117_task removed — sensor broken on this PCB
    xTaskCreate(ad8232_task,   "ad8232",   4096, NULL, 5, NULL);
}
