#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "cJSON.h"

#include "espnow_comm.h"
#include "protocol.h"

static const char *TAG = "master";

// Onboard LED on the ESP32 DevKit V1 (active-high)
#define ONBOARD_LED_GPIO    GPIO_NUM_2

// ============================================================
// Hardcoded peer MACs (1-sender prototype)
// ============================================================
// Sender ESP MAC — E0:8C:FE:32:DD:14
static const uint8_t SENDER1_MAC[6] = {0xE0, 0x8C, 0xFE, 0x32, 0xDD, 0x14};
// static const uint8_t SENDER2_MAC[6] = {0x14, 0x2B, 0x2F, 0xC0, 0x68, 0xE0};  // Stage 5

// ============================================================
// Timings
// ============================================================
#define HELLO_RETRY_MS       500   // resend HELLO every 500 ms while CONNECTING
#define CONNECTING_EMIT_MS  1000   // emit "connecting" status at most once per 1 s
#define HEARTBEAT_TX_MS     1000   // master → sender heartbeat
#define SENDER_TIMEOUT_MS   3000   // no traffic from sender for this long → disconnect
#define READY_BEACON_MS     2000   // re-emit "master_ready" while idle so a
                                    // dashboard that connects mid-flight catches up

// ============================================================
// FSM
// ============================================================
typedef enum {
    MASTER_IDLE_NO_PEER = 0,   // master_ready emitted; awaiting Connect
    MASTER_CONNECTING,          // sending HELLO, waiting for READY
    MASTER_CONNECTED,           // handshake done; awaiting Start
    MASTER_STREAMING,           // forwarding vitals
} master_state_t;

static master_state_t s_state = MASTER_IDLE_NO_PEER;

// ============================================================
// Inbound queue: vitals packets → serial output task
// ============================================================
typedef struct {
    sensor_packet_t packet;
    int             rssi;
} received_data_t;

static QueueHandle_t data_queue = NULL;
static QueueHandle_t ecg_queue  = NULL;

// ============================================================
// Event queue: UART commands + ESP-NOW receives → state task
// ============================================================
typedef enum {
    EV_NONE = 0,
    EV_CMD_CONNECT,
    EV_CMD_START,
    EV_CMD_STOP,
    EV_RX_READY,
    EV_RX_HEARTBEAT,
    EV_RX_VITALS,
} event_t;

static QueueHandle_t event_queue = NULL;

static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// ============================================================
// LED
// ============================================================
static void onboard_led_init(void)
{
    gpio_reset_pin(ONBOARD_LED_GPIO);
    gpio_set_direction(ONBOARD_LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(ONBOARD_LED_GPIO, 0);
}

// ============================================================
// JSON emit helpers — every line tagged with "type"
// ============================================================
static void emit_status_global(const char *state)
{
    printf("{\"type\":\"status\",\"state\":\"%s\"}\n", state);
}

static void emit_status_node(int node_id, const char *state)
{
    printf("{\"type\":\"status\",\"node\":%d,\"state\":\"%s\"}\n", node_id, state);
}

// ============================================================
// ESP-NOW helpers
// ============================================================
static void send_ctrl_to_sender(uint8_t packet_type)
{
    ctrl_packet_t pkt = { .packet_type = packet_type, .node_id = NODE_ID_MASTER };
    espnow_comm_send(SENDER1_MAC, (const uint8_t *)&pkt, sizeof(pkt));
}

// ============================================================
// ESP-NOW receive callback
// ============================================================
static void on_data_received(const uint8_t *src_mac, const uint8_t *data,
                              int data_len, int rssi)
{
    if (data_len < 1) return;
    uint8_t t = data[0];

    if (t == PACKET_TYPE_SENSOR_DATA && data_len >= sizeof(sensor_packet_t)) {
        // Forward vitals to the serial output queue
        received_data_t rx = {0};
        memcpy(&rx.packet, data, sizeof(sensor_packet_t));
        rx.rssi = rssi;
        if (memcmp(src_mac, SENDER1_MAC, 6) == 0) {
            rx.packet.node_id = NODE_ID_SENDER_1;
        }
        xQueueSend(data_queue, &rx, 0);

        // Also bump liveness via the FSM
        event_t ev = EV_RX_VITALS;
        xQueueSend(event_queue, &ev, 0);
        return;
    }

    if (t == PACKET_TYPE_ECG && data_len >= sizeof(ecg_packet_t)) {
        // Forward ECG straight to the ECG output queue
        ecg_packet_t pkt;
        memcpy(&pkt, data, sizeof(pkt));
        if (memcmp(src_mac, SENDER1_MAC, 6) == 0) {
            pkt.node_id = NODE_ID_SENDER_1;
        }
        xQueueSend(ecg_queue, &pkt, 0);

        // Bump sender liveness — ECG packets count as "sender alive"
        event_t ev = EV_RX_VITALS;
        xQueueSend(event_queue, &ev, 0);
        return;
    }

    if (t == PACKET_TYPE_READY) {
        event_t ev = EV_RX_READY;
        xQueueSend(event_queue, &ev, 0);
        return;
    }
    if (t == PACKET_TYPE_HEARTBEAT) {
        event_t ev = EV_RX_HEARTBEAT;
        xQueueSend(event_queue, &ev, 0);
        return;
    }
}

// ============================================================
// JSON helpers — NaN-aware float printing for vitals fields
// ============================================================
static void emit_float_field(const char *key, float v, bool last)
{
    if (isnan(v)) {
        printf("\"%s\":null%s", key, last ? "" : ",");
    } else {
        printf("\"%s\":%.2f%s", key, v, last ? "" : ",");
    }
}

// ============================================================
// Serial output task — drains data_queue, emits {"type":"vitals", ...}
// Floats serialise as JSON null when NaN (sensor failure / no body
// contact) so the dashboard can show "no body contact" or just "—".
// ============================================================
static void serial_output_task(void *pv)
{
    received_data_t rx;
    while (1) {
        if (xQueueReceive(data_queue, &rx, portMAX_DELAY) == pdTRUE) {
            printf("{\"type\":\"vitals\",");
            printf("\"node\":%d,",         rx.packet.node_id);
            printf("\"body_contact\":%d,", rx.packet.body_contact ? 1 : 0);
            emit_float_field("hr",        rx.packet.heart_rate, false);
            emit_float_field("spo2",      rx.packet.spo2,       false);
            emit_float_field("body_temp", rx.packet.body_temp,  false);
            emit_float_field("env_temp",  rx.packet.env_temp,   false);
            emit_float_field("gas_ppm",   rx.packet.gas_ppm,    false);
            printf("\"rssi_master\":%d}\n", rx.rssi);
        }
    }
}

// ============================================================
// ECG output task — forwards batched ECG samples as JSON
// ============================================================
static void ecg_output_task(void *pv)
{
    ecg_packet_t pkt;
    while (1) {
        if (xQueueReceive(ecg_queue, &pkt, portMAX_DELAY) == pdTRUE) {
            printf("{\"type\":\"ecg\",\"node\":%d,\"seq\":%u,\"samples\":[",
                   pkt.node_id, pkt.seq);
            for (int i = 0; i < ECG_SAMPLES_PER_PACKET; i++) {
                printf("%u%s", pkt.samples[i],
                       (i < ECG_SAMPLES_PER_PACKET - 1) ? "," : "");
            }
            printf("]}\n");
        }
    }
}

// ============================================================
// UART RX task — JSON command parser
// ============================================================
static event_t parse_cmd_string(const char *s)
{
    if (strcmp(s, "connect") == 0) return EV_CMD_CONNECT;
    if (strcmp(s, "start")   == 0) return EV_CMD_START;
    if (strcmp(s, "stop")    == 0) return EV_CMD_STOP;
    return EV_NONE;
}

static void handle_command_line(char *line)
{
    while (*line && isspace((unsigned char)*line)) line++;
    size_t n = strlen(line);
    while (n > 0 && isspace((unsigned char)line[n - 1])) line[--n] = '\0';
    if (n == 0) return;

    cJSON *root = cJSON_Parse(line);
    if (root == NULL) {
        ESP_LOGW(TAG, "Bad JSON on UART: %s", line);
        return;
    }
    cJSON *cmd_item = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    if (cJSON_IsString(cmd_item) && cmd_item->valuestring != NULL) {
        event_t ev = parse_cmd_string(cmd_item->valuestring);
        if (ev != EV_NONE) {
            xQueueSend(event_queue, &ev, 0);
            printf("{\"type\":\"ack\",\"cmd\":\"%s\",\"ok\":true}\n",
                   cmd_item->valuestring);
        } else {
            printf("{\"type\":\"ack\",\"cmd\":\"%s\",\"ok\":false,"
                   "\"err\":\"unknown\"}\n", cmd_item->valuestring);
        }
    }
    cJSON_Delete(root);
}

static void uart_rx_task(void *pv)
{
    char   buf[256];
    size_t len = 0;

    while (1) {
        int ch = getchar();
        if (ch == EOF) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (ch == '\r') continue;
        if (ch == '\n') {
            buf[len] = '\0';
            handle_command_line(buf);
            len = 0;
            continue;
        }
        if (len < sizeof(buf) - 1) {
            buf[len++] = (char)ch;
        } else {
            len = 0;  // resync on next newline
        }
    }
}

// ============================================================
// State task — owns the FSM + periodic ticks
// ============================================================
static void state_task(void *pv)
{
    uint32_t last_hello_tx_ms     = 0;
    uint32_t last_connecting_emit = 0;
    uint32_t last_hb_tx_ms        = 0;
    uint32_t last_sender_seen_ms  = 0;
    uint32_t last_ready_beacon_ms = 0;

    while (1) {
        event_t ev = EV_NONE;
        BaseType_t got = xQueueReceive(event_queue, &ev, pdMS_TO_TICKS(100));
        uint32_t now = now_ms();

        if (got == pdTRUE) {
            switch (ev) {
            case EV_CMD_CONNECT:
                if (s_state == MASTER_IDLE_NO_PEER) {
                    s_state = MASTER_CONNECTING;
                    last_hello_tx_ms     = 0;  // force immediate HELLO
                    last_connecting_emit = 0;
                    ESP_LOGI(TAG, "→ CONNECTING");
                    emit_status_global("connecting");
                }
                break;

            case EV_CMD_START:
                if (s_state == MASTER_CONNECTED) {
                    send_ctrl_to_sender(PACKET_TYPE_CMD_START);
                    s_state = MASTER_STREAMING;
                    ESP_LOGI(TAG, "→ STREAMING");
                    emit_status_node(NODE_ID_SENDER_1, "streaming");
                }
                break;

            case EV_CMD_STOP:
                if (s_state == MASTER_STREAMING) {
                    send_ctrl_to_sender(PACKET_TYPE_CMD_STOP);
                    s_state = MASTER_CONNECTED;
                    ESP_LOGI(TAG, "→ CONNECTED (stop)");
                    emit_status_node(NODE_ID_SENDER_1, "connected");
                } else if (s_state == MASTER_CONNECTING) {
                    // User cancelled the connect attempt
                    s_state = MASTER_IDLE_NO_PEER;
                    ESP_LOGI(TAG, "connect cancelled → IDLE_NO_PEER");
                    emit_status_global("master_ready");
                }
                break;

            case EV_RX_READY:
                last_sender_seen_ms = now;
                if (s_state == MASTER_CONNECTING) {
                    s_state = MASTER_CONNECTED;
                    ESP_LOGI(TAG, "READY received → CONNECTED");
                    emit_status_node(NODE_ID_SENDER_1, "connected");
                }
                break;

            case EV_RX_HEARTBEAT:
            case EV_RX_VITALS:
                last_sender_seen_ms = now;
                break;

            case EV_NONE:
                break;
            }
        }

        // Periodic actions
        switch (s_state) {
        case MASTER_CONNECTING:
            if (now - last_hello_tx_ms >= HELLO_RETRY_MS) {
                send_ctrl_to_sender(PACKET_TYPE_HELLO);
                last_hello_tx_ms = now;
            }
            if (now - last_connecting_emit >= CONNECTING_EMIT_MS) {
                emit_status_global("connecting");
                last_connecting_emit = now;
            }
            break;

        case MASTER_CONNECTED:
        case MASTER_STREAMING:
            if (now - last_hb_tx_ms >= HEARTBEAT_TX_MS) {
                send_ctrl_to_sender(PACKET_TYPE_HEARTBEAT);
                last_hb_tx_ms = now;
            }
            if (last_sender_seen_ms != 0 &&
                now - last_sender_seen_ms > SENDER_TIMEOUT_MS) {
                ESP_LOGW(TAG, "sender timeout → IDLE_NO_PEER");
                s_state = MASTER_IDLE_NO_PEER;
                emit_status_node(NODE_ID_SENDER_1, "disconnected");
            }
            break;

        case MASTER_IDLE_NO_PEER:
            // Beacon master_ready every few seconds so a dashboard that
            // attaches after boot still picks up our state.
            if (now - last_ready_beacon_ms >= READY_BEACON_MS) {
                emit_status_global("master_ready");
                last_ready_beacon_ms = now;
            }
            break;
        default:
            break;
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== Master Node Starting ===");

    onboard_led_init();

    data_queue  = xQueueCreate(20, sizeof(received_data_t));
    ecg_queue   = xQueueCreate(16, sizeof(ecg_packet_t));
    event_queue = xQueueCreate(16, sizeof(event_t));
    if (data_queue == NULL || ecg_queue == NULL || event_queue == NULL) {
        ESP_LOGE(TAG, "queue alloc failed");
        return;
    }

    ESP_ERROR_CHECK(espnow_comm_init(on_data_received));
    espnow_comm_add_peer(SENDER1_MAC);

    xTaskCreate(serial_output_task, "serial_out",  4096, NULL, 5, NULL);
    xTaskCreate(ecg_output_task,    "ecg_out",     4096, NULL, 5, NULL);
    xTaskCreate(uart_rx_task,       "uart_rx",     4096, NULL, 5, NULL);
    xTaskCreate(state_task,         "state",       4096, NULL, 6, NULL);

    // Master is up. Solid LED = master alive. master_ready unblocks the
    // dashboard's Connect button.
    gpio_set_level(ONBOARD_LED_GPIO, 1);
    emit_status_global("master_ready");

    ESP_LOGI(TAG, "Master ready — awaiting commands from dashboard");
}
