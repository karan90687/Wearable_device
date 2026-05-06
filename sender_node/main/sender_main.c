#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "driver/gpio.h"

#include "espnow_comm.h"
#include "protocol.h"
#include "i2c.h"
#include "max30102.h"
#include "ad8232.h"

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

// Master ESP MAC — 14:2B:2F:C0:68:E0
static const uint8_t MASTER_MAC[6] = {0x14, 0x2B, 0x2F, 0xC0, 0x68, 0xE0};
// Liveness timeout: if we don't hear from master for this long, drop to IDLE
#define MASTER_TIMEOUT_MS    3000
#define HEARTBEAT_PERIOD_MS  1000
#define VITALS_PERIOD_MS     500
#define MAX30102_POLL_MS     10        // ~100 Hz polling
#define ECG_POLL_MS          4         // 250 Hz sampling
#define TEMP_FAKE_PERIOD_MS  1000      // 1 Hz random walk

// ============================================================
// FSM
// ============================================================
typedef enum {
    SENDER_IDLE = 0,
    SENDER_CONNECTED,
    SENDER_STREAMING,
} sender_state_t;

static volatile sender_state_t s_state = SENDER_IDLE;

// uint32_t reads/writes are atomic on ESP32 — safe across callback / FSM.
static volatile uint32_t s_last_master_seen_ms = 0;

typedef enum {
    EV_RX_HELLO,
    EV_RX_CMD_START,
    EV_RX_CMD_STOP,
    EV_RX_HEARTBEAT,
} sender_event_t;

static QueueHandle_t event_queue   = NULL;
static QueueHandle_t ecg_tx_queue  = NULL;   // ecg_packet_t entries

// ============================================================
// Latest vitals — written by sample tasks, snapshotted by vitals_tx_task.
// All access (read or write) goes through s_vitals_lock.
// ============================================================
typedef struct {
    bool   body_contact;   // MAX30102 finger detection
    float  hr;             // BPM, NaN if unknown / no contact
    float  spo2;           // %, NaN if unknown / unreliable
    float  body_temp;      // Celsius, slow random walk (TMP117 broken)
    bool   max30102_ok;    // false → driver init failed; ship NaNs
} latest_vitals_t;

static latest_vitals_t   s_vitals     = {0};
static SemaphoreHandle_t s_vitals_lock = NULL;

static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// ============================================================
// LED + buzzer helpers
// ============================================================
// Indicator policy (final-day behaviour):
//   LED    : ON whenever connected to the master (CONNECTED or STREAMING),
//            OFF in IDLE. The dedicated buzzer task does not touch the LED;
//            state transition handlers below drive it directly.
//   BUZZER : pulses 200 ms ON / 200 ms OFF while IDLE (so the user audibly
//            knows the link isn't up yet). On IDLE → CONNECTED transition,
//            solid 2 s beep, then silent for the rest of CONNECTED/STREAMING.
//            Pulsing resumes if we ever fall back to IDLE.
static void led_set(bool on)    { gpio_set_level(PIN_LED_STATUS, on ? 1 : 0); }
static void buzzer_set(bool on) { gpio_set_level(PIN_BUZZER,     on ? 1 : 0); }

// ============================================================
// ESP-NOW helpers
// ============================================================
static void send_ctrl(uint8_t packet_type)
{
    ctrl_packet_t pkt = { .packet_type = packet_type, .node_id = NODE_ID };
    espnow_comm_send(MASTER_MAC, (const uint8_t *)&pkt, sizeof(pkt));
}

// ============================================================
// ESP-NOW receive callback
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
    send_ctrl(PACKET_TYPE_READY);
    s_state = SENDER_CONNECTED;
    led_set(true);                 // LED stays on while connected
    ESP_LOGI(TAG, "→ CONNECTED (HELLO ack'd)");
    // Buzzer 2 s solid beep is handled by buzzer_task on the IDLE→CONNECTED
    // transition, so we don't need to block this handler.
}

static void enter_streaming(void)
{
    s_state = SENDER_STREAMING;
    led_set(true);                 // already on, kept on
    ESP_LOGI(TAG, "→ STREAMING");
}

static void enter_connected_from_streaming(void)
{
    s_state = SENDER_CONNECTED;
    led_set(true);                 // still connected → still on
    ESP_LOGI(TAG, "→ CONNECTED (stop)");
}

// ============================================================
// State task
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
                break;
            }
        }

        if (s_state != SENDER_IDLE) {
            uint32_t age = now_ms() - s_last_master_seen_ms;
            if (age > MASTER_TIMEOUT_MS) {
                ESP_LOGW(TAG, "master timeout (%lu ms) — dropping to IDLE",
                         (unsigned long)age);
                enter_idle();
            }
        }
    }
}

// ============================================================
// Buzzer task — drives the audible link-state indicator.
//   IDLE                       → 200 ms on / 200 ms off pulse
//   IDLE → CONNECTED (edge)    → solid beep for 2 s
//   CONNECTED / STREAMING      → silent
//   any → IDLE                 → pulsing resumes automatically
// Polls s_state at ~100 ms; latency is fine for an indicator.
// ============================================================
#define BUZZER_PULSE_ON_MS    200
#define BUZZER_PULSE_OFF_MS   200
#define BUZZER_CONNECT_MS    2000

static void buzzer_task(void *pv)
{
    sender_state_t prev = SENDER_IDLE;

    while (1) {
        sender_state_t now = s_state;

        // Edge: just left IDLE → solid 2 s beep (skip if we already missed
        // the edge during a pulse cycle — only fires once per transition).
        if (prev == SENDER_IDLE && now != SENDER_IDLE) {
            buzzer_set(true);
            vTaskDelay(pdMS_TO_TICKS(BUZZER_CONNECT_MS));
            buzzer_set(false);
            prev = now;
            continue;
        }

        if (now == SENDER_IDLE) {
            buzzer_set(true);
            vTaskDelay(pdMS_TO_TICKS(BUZZER_PULSE_ON_MS));
            buzzer_set(false);
            vTaskDelay(pdMS_TO_TICKS(BUZZER_PULSE_OFF_MS));
        } else {
            buzzer_set(false);
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        prev = now;
    }
}

// ============================================================
// Heartbeat TX — sender → master while CONNECTED only
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
// MAX30102 sampling task — pulls FIFO at ~100 Hz, runs HR + SpO2
// algorithms inside the driver, posts the latest into s_vitals.
// ============================================================
static void max30102_sample_task(void *pv)
{
    if (!s_vitals.max30102_ok) {
        ESP_LOGW(TAG, "max30102 init failed — sample task exiting");
        vTaskDelete(NULL);
    }

    while (1) {
        max30102_reading_t r;
        if (max30102_step(&r) == ESP_OK || r.valid == false) {
            // r is filled either way (invalid = persists last known values)
            xSemaphoreTake(s_vitals_lock, portMAX_DELAY);
            s_vitals.body_contact = r.body_contact;
            s_vitals.hr           = r.body_contact ? r.bpm  : NAN;
            s_vitals.spo2         = r.body_contact ? r.spo2 : NAN;
            xSemaphoreGive(s_vitals_lock);
        }
        vTaskDelay(pdMS_TO_TICKS(MAX30102_POLL_MS));
    }
}

// ============================================================
// Body temperature — TMP117 is broken on this PCB, so we generate
// a slow random walk in the normal-adult range (36.5–37.5 °C).
// ============================================================
static void temp_fake_task(void *pv)
{
    float current = 36.7f;
    float target  = 36.7f;

    while (1) {
        // Occasionally pick a new target inside the band
        if ((esp_random() % 100) < 25) {
            float r = (float)(esp_random() % 1001) / 1000.0f;  // 0..1
            target = 36.5f + r * 1.0f;                         // 36.5..37.5
        }
        // Drift toward target
        current += (target - current) * 0.1f;
        if (current < 36.5f) current = 36.5f;
        if (current > 37.5f) current = 37.5f;

        xSemaphoreTake(s_vitals_lock, portMAX_DELAY);
        s_vitals.body_temp = current;
        xSemaphoreGive(s_vitals_lock);

        vTaskDelay(pdMS_TO_TICKS(TEMP_FAKE_PERIOD_MS));
    }
}

// ============================================================
// AD8232 sampling task — 250 Hz reads, batched 32 samples per packet,
// posted onto ecg_tx_queue. Only fires while STREAMING (no point
// filling RAM otherwise).
// ============================================================
static void ecg_sample_task(void *pv)
{
    static uint16_t seq = 0;
    ecg_packet_t pkt = { .packet_type = PACKET_TYPE_ECG, .node_id = NODE_ID };
    int idx = 0;

    while (1) {
        if (s_state != SENDER_STREAMING) {
            // Drop pending sample buffer when not streaming so we don't
            // ship stale samples on the next START.
            idx = 0;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        ad8232_sample_t s;
        if (ad8232_read_sample(&s) == ESP_OK) {
            uint16_t v = s.leads_off ? 0xFFFF : (s.raw & 0x0FFF);
            pkt.samples[idx++] = v;

            if (idx >= ECG_SAMPLES_PER_PACKET) {
                pkt.seq = seq++;
                xQueueSend(ecg_tx_queue, &pkt, 0);
                idx = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(ECG_POLL_MS));
    }
}

// ============================================================
// ECG TX task — drains ecg_tx_queue, sends to master while STREAMING
// ============================================================
static void ecg_tx_task(void *pv)
{
    ecg_packet_t pkt;
    while (1) {
        if (xQueueReceive(ecg_tx_queue, &pkt, portMAX_DELAY) == pdTRUE) {
            if (s_state == SENDER_STREAMING) {
                espnow_comm_send(MASTER_MAC, (const uint8_t *)&pkt, sizeof(pkt));
            }
        }
    }
}

// ============================================================
// Vitals TX — every 500 ms while STREAMING; snapshots s_vitals
// ============================================================
static void vitals_tx_task(void *pv)
{
    sensor_packet_t pkt = {
        .packet_type = PACKET_TYPE_SENSOR_DATA,
        .node_id     = NODE_ID,
    };
    while (1) {
        if (s_state == SENDER_STREAMING) {
            xSemaphoreTake(s_vitals_lock, portMAX_DELAY);
            pkt.body_contact = s_vitals.body_contact ? 1 : 0;
            pkt.heart_rate   = s_vitals.hr;          // NaN ok
            pkt.spo2         = s_vitals.spo2;        // NaN ok
            // Body temp follows the same "needs contact" rule as HR/SpO2.
            // The fake temp keeps drifting in the background; we just
            // mask it out here when the user isn't on the sensor so the
            // dashboard treats all three vitals consistently.
            pkt.body_temp    = s_vitals.body_contact
                                  ? s_vitals.body_temp : NAN;
            xSemaphoreGive(s_vitals_lock);

            pkt._pad     = 0;
            pkt.env_temp = NAN;          // unused in this build
            pkt.gas_ppm  = NAN;          // unused in this build
            pkt.rssi_peer = -127;

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

    gpio_reset_pin(PIN_AD8232_SDN);
    gpio_set_direction(PIN_AD8232_SDN, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_AD8232_SDN, 1);

    ESP_ERROR_CHECK(i2c_init());

    // Initial state — temp comes from the fake task; HR/SpO2 stay NaN
    // until a finger is on the sensor and the algorithm settles.
    s_vitals.body_contact = false;
    s_vitals.hr           = NAN;
    s_vitals.spo2         = NAN;
    s_vitals.body_temp    = 36.7f;
    s_vitals.max30102_ok  = false;

    if (max30102_init() == ESP_OK) {
        s_vitals.max30102_ok = true;
        ESP_LOGI(TAG, "MAX30102 ready");
    } else {
        ESP_LOGE(TAG, "MAX30102 init FAILED — HR/SpO2 will report null");
    }

    if (ad8232_init() != ESP_OK) {
        ESP_LOGE(TAG, "AD8232 init failed — ECG samples will report leads-off");
    } else {
        ESP_LOGI(TAG, "AD8232 ready");
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== Sender Node %d Starting ===", NODE_ID);

    peripherals_init();

    s_vitals_lock = xSemaphoreCreateMutex();
    event_queue   = xQueueCreate(16, sizeof(sender_event_t));
    ecg_tx_queue  = xQueueCreate(8,  sizeof(ecg_packet_t));
    if (s_vitals_lock == NULL || event_queue == NULL || ecg_tx_queue == NULL) {
        ESP_LOGE(TAG, "queue / mutex alloc failed");
        return;
    }

    ESP_ERROR_CHECK(espnow_comm_init(on_data_received));
    espnow_comm_add_peer(MASTER_MAC);

    xTaskCreate(state_task,           "state",     4096, NULL, 6, NULL);
    xTaskCreate(buzzer_task,          "buzzer",    2048, NULL, 3, NULL);
    xTaskCreate(heartbeat_tx_task,    "hb_tx",     2048, NULL, 4, NULL);
    xTaskCreate(vitals_tx_task,       "vitals_tx", 4096, NULL, 4, NULL);
    xTaskCreate(temp_fake_task,       "temp_fake", 2048, NULL, 3, NULL);

    if (s_vitals.max30102_ok) {
        xTaskCreate(max30102_sample_task, "max_smp", 4096, NULL, 5, NULL);
    }
    xTaskCreate(ecg_sample_task,      "ecg_smp",   4096, NULL, 5, NULL);
    xTaskCreate(ecg_tx_task,          "ecg_tx",    4096, NULL, 4, NULL);

    ESP_LOGI(TAG, "Sender ready, waiting for HELLO from master");
}
