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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/gpio.h"

#include "i2c.h"
#include "max30102.h"
#include "tmp117.h"
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
static void max30102_task(void *pv)
{
    moving_avg_filter_t ir_f, red_f;
    filter_init(&ir_f);
    filter_init(&red_f);

    int counter = 0;
    while (1) {
        max30102_sample_t s;
        if (max30102_read_sample(&s) == ESP_OK) {
            uint32_t fir  = filter_update(&ir_f,  s.ir);
            uint32_t fred = filter_update(&red_f, s.red);
            if (++counter >= MAX30102_PRINT_EVERY) {
                counter = 0;
                printf("[MAX30102]  IR raw=%-6lu filt=%-6lu  |  RED raw=%-6lu filt=%-6lu\n",
                       (unsigned long)s.ir,  (unsigned long)fir,
                       (unsigned long)s.red, (unsigned long)fred);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void tmp117_task(void *pv)
{
    while (1) {
        float t = 0.0f;
        if (tmp117_read_temp(&t) == ESP_OK) {
            printf("[TMP117]    Body temp = %.2f °C\n", t);
        } else {
            printf("[TMP117]    read failed\n");
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

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

    ESP_LOGI(TAG, "Initialising MAX30102 (HR/SpO2)...");
    if (max30102_init() != ESP_OK) {
        ESP_LOGE(TAG, "MAX30102 init FAILED");
    } else {
        ESP_LOGI(TAG, "MAX30102 OK");
    }

    ESP_LOGI(TAG, "Initialising TMP117 (body temp)...");
    if (tmp117_init() != ESP_OK) {
        ESP_LOGE(TAG, "TMP117 init FAILED");
    } else {
        ESP_LOGI(TAG, "TMP117 OK");
    }

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
    xTaskCreate(tmp117_task,   "tmp117",   3072, NULL, 4, NULL);
    xTaskCreate(ad8232_task,   "ad8232",   4096, NULL, 5, NULL);
}
