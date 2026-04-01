#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_mac.h"

#include "espnow_comm.h"
#include "protocol.h"

static const char *TAG = "master";

// Queue to pass received packets from callback to main task
static QueueHandle_t data_queue = NULL;

// Struct to hold received data + source info
typedef struct {
    sensor_packet_t packet;
    int rssi;
} received_data_t;

// MAC addresses of senders — fill in after discovering MACs
static const uint8_t SENDER1_MAC[6] = {0xC0, 0xCD, 0xD6, 0xCE, 0x27, 0x58};  // TODO: replace
// static const uint8_t SENDER2_MAC[6] = {0x14, 0x2B, 0x2F, 0xC0, 0x68, 0xE0};
// ESP-NOW receive callback
static void on_data_received(const uint8_t *src_mac, const uint8_t *data,
                              int data_len, int rssi)
{
    if (data_len < 1) return;

    uint8_t packet_type = data[0];

    if (packet_type == PACKET_TYPE_SENSOR_DATA && data_len >= sizeof(sensor_packet_t)) {
        received_data_t rx = {0};
        memcpy(&rx.packet, data, sizeof(sensor_packet_t));
        rx.rssi = rssi;

        // Override node_id based on source MAC address
        // if (memcmp(src_mac, SENDER1_MAC, 6) == 0) {
        //     rx.packet.node_id = 1;
        // } else if (memcmp(src_mac, SENDER2_MAC, 6) == 0) {
        //     rx.packet.node_id = 2;
        // }
        if (memcmp(src_mac, SENDER1_MAC, 6) == 0) {
            rx.packet.node_id = 1;
        }

        // Send to queue (don't block in ISR-like callback)
        xQueueSendFromISR(data_queue, &rx, NULL);
    }
}

// Task: read from queue and print JSON to serial
static void serial_output_task(void *pvParameters)
{
    received_data_t rx;

    while (1) {
        if (xQueueReceive(data_queue, &rx, portMAX_DELAY) == pdTRUE) {
            // Print as JSON line — this is what the Python dashboard will parse
            printf("{\"node\":%d,\"hr\":%.1f,\"spo2\":%.1f,\"body_temp\":%.1f,"
                   "\"env_temp\":%.1f,\"gas_ppm\":%.1f,\"rssi_peer\":%d,\"rssi_master\":%d}\n",
                   rx.packet.node_id,
                   rx.packet.heart_rate,
                   rx.packet.spo2,
                   rx.packet.body_temp,
                   rx.packet.env_temp,
                   rx.packet.gas_ppm,
                   rx.packet.rssi_peer,
                   rx.rssi);

            // ESP_LOGI(TAG, "Node %d: HR=%.1f SpO2=%.1f BTemp=%.1f ETemp=%.1f Gas=%.1f RSSI_peer=%d",
            //          rx.packet.node_id,
            //          rx.packet.heart_rate,
            //          rx.packet.spo2,
            //          rx.packet.body_temp,
            //          rx.packet.env_temp,
            //          rx.packet.gas_ppm,
            //          rx.packet.rssi_peer);
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== Master Node Starting ===");

    // Create receive queue
    data_queue = xQueueCreate(20, sizeof(received_data_t));
    if (data_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create queue");
        return;
    }

    // Initialize ESP-NOW
    ESP_ERROR_CHECK(espnow_comm_init(on_data_received));

    // Add sender peers
    espnow_comm_add_peer(SENDER1_MAC);
    // espnow_comm_add_peer(SENDER2_MAC);

    // Start serial output task
    xTaskCreate(serial_output_task, "serial_out", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Master ready — waiting for data from senders...");
}
