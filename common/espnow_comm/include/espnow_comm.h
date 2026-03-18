#ifndef ESPNOW_COMM_H
#define ESPNOW_COMM_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_now.h"

// Callback type for received data
// Parameters: src_mac, data, data_len, rssi
typedef void (*espnow_recv_cb_t)(const uint8_t *src_mac, const uint8_t *data,
                                  int data_len, int rssi);

// Initialize WiFi in station mode and ESP-NOW
esp_err_t espnow_comm_init(espnow_recv_cb_t recv_callback);

// Add a peer by MAC address
esp_err_t espnow_comm_add_peer(const uint8_t *peer_mac);

// Send data to a specific peer (NULL mac = broadcast)
esp_err_t espnow_comm_send(const uint8_t *peer_mac, const uint8_t *data, size_t len);

// Send data to all registered peers
esp_err_t espnow_comm_broadcast(const uint8_t *data, size_t len);

// Deinitialize ESP-NOW
void espnow_comm_deinit(void);

#endif // ESPNOW_COMM_H
