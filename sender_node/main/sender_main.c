#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "driver/gpio.h"

#include "espnow_comm.h"
#include "protocol.h"
#include "i2c.h"

static const char *TAG = "sender";

// ============================================================
// CONFIGURATION
// Sender 1: NODE_ID = NODE_ID_SENDER_1
// Sender 2 (future): NODE_ID = NODE_ID_SENDER_2 — only line that changes
// ============================================================
#define NODE_ID  NODE_ID_SENDER_1

// Pin map (per project pin spec)
#define PIN_LED_STATUS   GPIO_NUM_15   // active-high, 220Ω series
#define PIN_LED_ALERT    GPIO_NUM_26   // unused in Stage 2 (alarms in Stage 6)
#define PIN_BUZZER       GPIO_NUM_27   // active buzzer — drive HIGH to beep
#define PIN_AD8232_SDN   GPIO_NUM_4    // chip-enable for AD8232 (HIGH = on)

// Master ESP MAC — hardcoded
static const uint8_t MASTER_MAC[6] = {0x9C, 0x13, 0x9E, 0x90, 0xC6, 0xE0};

// Liveness timeout: if we don't hear from master for this long, drop to IDLE
#define MASTER_TIMEOUT_MS   3000
#define HEARTBEAT_PERIOD_MS 1000
#define VITALS_PERIOD_MS    500

// ============================================================
// FSM
// ============================================================
typedef enum {
    SENDER_IDLE = 0,    // waiting for HELLO from master
    SENDER_CONNECTED,   // handshake done, awaiting CMD_START
    SENDER_STREAMING,   // pushing vitals every VITALS_PERIOD_MS
} sender_state_t;

static volatile sender_state_t s_state = SENDER_IDLE;

// Single-millisecond clock used for liveness tracking. uint32_t reads/writes
// are atomic on ESP32, so no mutex needed across the recv callback / FSM.
static volatile uint32_t s_last_master_seen_ms = 0;

typedef enum {
    EV_RX_HELLO,
    EV_RX_CMD_START,
    EV_RX_CMD_STOP,
    EV_RX_HEARTBEAT,
} sender_event_t;

static QueueHandle_t event_queue = NULL;

static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// ============================================================
// LED + buzzer helpers
// ============================================================
static void led_set(bool on)      { gpio_set_level(PIN_LED_STATUS, on ? 1 : 0); }
static void buzzer_set(bool on)   { gpio_set_level(PIN_BUZZER,     on ? 1 : 0); }

// Single 200 ms pulse — used on entering CONNECTED.
static void led_blink_once(void)
{
    led_set(true);
    vTaskDelay(pdMS_TO_TICKS(200));
    led_set(false);
}

// Two beeps separated by 200 ms — connect indicator.
static void buzzer_beep_twice(void)
{
    buzzer_set(true);  vTaskDelay(pdMS_TO_TICKS(200));
    buzzer_set(false); vTaskDelay(pdMS_TO_TICKS(200));
    buzzer_set(true);  vTaskDelay(pdMS_TO_TICKS(200));
    buzzer_set(false);
}

// ============================================================
// ESP-NOW helpers
// ============================================================
static void send_ctrl(uint8_t packet_type)
{
    ctrl_packet_t pkt = { .packet_type = packet_type, .node_id = NODE_ID };
    espnow_comm_send(MASTER_MAC, (const uint8_t *)&pkt, sizeof(pkt));
}

// ============================================================
// ESP-NOW receive callback — runs in WiFi task context
// ============================================================
static void on_data_received(const uint8_t *src_mac, const uint8_t *data,
                              int data_len, int rssi)
{
    (void)src_mac; (void)rssi;
    if (data_len < 1) return;

    sender_event_t ev;
    bool          post = true;
    switch (data[0]) {
        case PACKET_TYPE_HELLO:     ev = EV_RX_HELLO;     break;
        case PACKET_TYPE_CMD_START: ev = EV_RX_CMD_START; break;
        case PACKET_TYPE_CMD_STOP:  ev = EV_RX_CMD_STOP;  break;
        case PACKET_TYPE_HEARTBEAT: ev = EV_RX_HEARTBEAT; break;
        default:                    post = false;         break;
    }
    if (post) {
        s_last_master_seen_ms = now_ms();
        xQueueSend(event_queue, &ev, 0);
    }
}

// ============================================================
// State transitions
// ============================================================
static void enter_idle(void)
{
    s_state = SENDER_IDLE;
    led_set(false);
    ESP_LOGI(TAG, "→ IDLE");
}

static void enter_connected_from_hello(void)
{
    // Reply READY first so master leaves CONNECTING ASAP, then do the
    // user-facing LED + buzzer indication.
    send_ctrl(PACKET_TYPE_READY);
    s_state = SENDER_CONNECTED;
    ESP_LOGI(TAG, "→ CONNECTED (HELLO ack'd)");
    led_blink_once();
    buzzer_beep_twice();
}

static void enter_streaming(void)
{
    s_state = SENDER_STREAMING;
    led_set(true);
    ESP_LOGI(TAG, "→ STREAMING");
}

static void enter_connected_from_streaming(void)
{
    s_state = SENDER_CONNECTED;
    led_set(false);
    ESP_LOGI(TAG, "→ CONNECTED (stop)");
}

// ============================================================
// State task — single consumer of the event queue.
// Polls every 100 ms so it can also detect master heartbeat timeout.
// ============================================================
static void state_task(void *pv)
{
    s_last_master_seen_ms = now_ms();

    while (1) {
        sender_event_t ev;
        if (xQueueReceive(event_queue, &ev, pdMS_TO_TICKS(100)) == pdTRUE) {
            switch (ev) {
            case EV_RX_HELLO:
                if (s_state == SENDER_IDLE) {
                    enter_connected_from_hello();
                } else {
                    // Already connected — just re-ack so master settles.
                    send_ctrl(PACKET_TYPE_READY);
                }
                break;

            case EV_RX_CMD_START:
                if (s_state == SENDER_CONNECTED) enter_streaming();
                break;

            case EV_RX_CMD_STOP:
                if (s_state == SENDER_STREAMING) enter_connected_from_streaming();
                break;

            case EV_RX_HEARTBEAT:
                // s_last_master_seen_ms already bumped in callback
                break;
            }
        }

        // Periodic tick — master liveness check
        if (s_state != SENDER_IDLE) {
            uint32_t age = now_ms() - s_last_master_seen_ms;
            if (age > MASTER_TIMEOUT_MS) {
                ESP_LOGW(TAG, "master timeout (%u ms) — dropping to IDLE", age);
                enter_idle();
            }
        }
    }
}

// ============================================================
// Heartbeat TX — sender → master, only while CONNECTED.
// Suppressed during STREAMING (vitals packets prove liveness).
// ============================================================
static void heartbeat_tx_task(void *pv)
{
    while (1) {
        if (s_state == SENDER_CONNECTED) {
            send_ctrl(PACKET_TYPE_HEARTBEAT);
        }
        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_PERIOD_MS));
    }
}

// ============================================================
// Vitals TX — only fires while STREAMING.
// Stage 2 still uses dummy values; real sensor wiring is Stage 3.
// ============================================================
static void vitals_tx_task(void *pv)
{
    sensor_packet_t pkt = {
        .packet_type = PACKET_TYPE_SENSOR_DATA,
        .node_id     = NODE_ID,
    };
    while (1) {
        if (s_state == SENDER_STREAMING) {
            pkt.heart_rate = 72.0f + (float)(esp_random() % 10);
            pkt.spo2       = 95.0f + (float)(esp_random() % 5);
            pkt.body_temp  = 36.5f + (float)(esp_random() % 10) / 10.0f;
            pkt.env_temp   = 24.0f + (float)(esp_random() % 5);
            pkt.gas_ppm    = 100.0f + (float)(esp_random() % 50);
            pkt.rssi_peer  = -127;
            espnow_comm_send(MASTER_MAC, (const uint8_t *)&pkt, sizeof(pkt));
        }
        vTaskDelay(pdMS_TO_TICKS(VITALS_PERIOD_MS));
    }
}

// ============================================================
// Peripheral init
// ============================================================
static void peripherals_init(void)
{
    gpio_reset_pin(PIN_LED_STATUS);
    gpio_set_direction(PIN_LED_STATUS, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_LED_STATUS, 0);

    gpio_reset_pin(PIN_LED_ALERT);
    gpio_set_direction(PIN_LED_ALERT, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_LED_ALERT, 0);

    gpio_reset_pin(PIN_BUZZER);
    gpio_set_direction(PIN_BUZZER, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_BUZZER, 0);

    // AD8232 SDN tied to GPIO 4 — must be HIGH for the analog front-end
    // to operate. Stage 3 will actually read the ADC; we enable the chip
    // here so it's stable by then.
    gpio_reset_pin(PIN_AD8232_SDN);
    gpio_set_direction(PIN_AD8232_SDN, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_AD8232_SDN, 1);

    ESP_ERROR_CHECK(i2c_init());
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== Sender Node %d Starting ===", NODE_ID);

    peripherals_init();

    event_queue = xQueueCreate(16, sizeof(sender_event_t));
    if (event_queue == NULL) {
        ESP_LOGE(TAG, "event queue alloc failed");
        return;
    }

    ESP_ERROR_CHECK(espnow_comm_init(on_data_received));
    espnow_comm_add_peer(MASTER_MAC);

    xTaskCreate(state_task,        "state",     4096, NULL, 5, NULL);
    xTaskCreate(heartbeat_tx_task, "hb_tx",     2048, NULL, 4, NULL);
    xTaskCreate(vitals_tx_task,    "vitals_tx", 4096, NULL, 4, NULL);

    ESP_LOGI(TAG, "Sender ready, waiting for HELLO from master");
}
