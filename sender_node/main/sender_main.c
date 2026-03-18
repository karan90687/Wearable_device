#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_mac.h"

#include "espnow_comm.h"
#include "protocol.h"

static const char *TAG = "sender";

// ============================================================
// CONFIGURATION — Change NODE_ID per device before flashing
// Sender 1: NODE_ID = NODE_ID_SENDER_1 (1)
// Sender 2: NODE_ID = NODE_ID_SENDER_2 (2)
// ============================================================
#define NODE_ID  NODE_ID_SENDER_1

// MAC addresses — fill in after discovering each ESP's MAC
// Run once with any code to print MAC, then fill these in
// Master ESP MAC address
static const uint8_t MASTER_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};  // TODO: replace with actual master MAC

// Peer sender MAC address (the other sender)
static const uint8_t PEER_SENDER_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};  // TODO: replace with other sender MAC

// Latest RSSI from peer sender
static volatile int8_t peer_rssi = -127;

// ESP-NOW receive callback
static void on_data_received(const uint8_t *src_mac, const uint8_t *data,
                              int data_len, int rssi)
{
    if (data_len < 1) return;

    uint8_t packet_type = data[0];

    if (packet_type == PACKET_TYPE_PING && data_len >= sizeof(ping_packet_t)) {
        // Received ping from peer sender — store RSSI
        peer_rssi = (int8_t)rssi;
        ESP_LOGI(TAG, "Ping from " MACSTR " RSSI=%d", MAC2STR(src_mac), rssi);
    }
}

// Task: periodically read sensors and send data to master
static void sensor_send_task(void *pvParameters)
{
    sensor_packet_t packet = {0};
    packet.packet_type = PACKET_TYPE_SENSOR_DATA;
    packet.node_id = NODE_ID;

    while (1) {
        // ---- Stage 1: Dummy data (replace with real sensor reads in Stage 2) ----
        packet.heart_rate = 72.0f + (float)(esp_random() % 10);
        packet.spo2       = 95.0f + (float)(esp_random() % 5);
        packet.body_temp  = 36.5f + (float)(esp_random() % 10) / 10.0f;
        packet.env_temp   = 24.0f + (float)(esp_random() % 5);
        packet.gas_ppm    = 100.0f + (float)(esp_random() % 50);
        packet.rssi_peer  = peer_rssi;

        // Send sensor data to master
        esp_err_t ret = espnow_comm_send(MASTER_MAC, (uint8_t *)&packet, sizeof(packet));
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to send sensor data: %d", ret);
        } else {
            ESP_LOGI(TAG, "Sent: HR=%.1f SpO2=%.1f BTemp=%.1f ETemp=%.1f Gas=%.1f RSSI=%d",
                     packet.heart_rate, packet.spo2, packet.body_temp,
                     packet.env_temp, packet.gas_ppm, packet.rssi_peer);
        }

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

// Task: periodically send ping to peer sender for RSSI measurement
static void ping_task(void *pvParameters)
{
    ping_packet_t ping = {
        .packet_type = PACKET_TYPE_PING,
        .node_id = NODE_ID,
    };

    while (1) {
        espnow_comm_send(PEER_SENDER_MAC, (uint8_t *)&ping, sizeof(ping));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== Sender Node %d Starting ===", NODE_ID);

    // Initialize ESP-NOW
    ESP_ERROR_CHECK(espnow_comm_init(on_data_received));

    // Add peers
    espnow_comm_add_peer(MASTER_MAC);
    espnow_comm_add_peer(PEER_SENDER_MAC);

    // Start tasks
    xTaskCreate(sensor_send_task, "sensor_send", 4096, NULL, 5, NULL);
    xTaskCreate(ping_task, "ping", 2048, NULL, 4, NULL);
}
