#include "espnow_comm.h"

#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"

static const char *TAG = "espnow_comm";

// User-registered receive callback
static espnow_recv_cb_t s_recv_callback = NULL;

// Broadcast MAC address
static const uint8_t BROADCAST_MAC[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Internal ESP-NOW send callback
static void espnow_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    if (status != ESP_NOW_SEND_SUCCESS) {
        ESP_LOGW(TAG, "Send to " MACSTR " failed", MAC2STR(mac_addr));
    }
}

// Internal ESP-NOW receive callback (ESP-IDF v5.x signature)
static void espnow_recv_cb(const esp_now_recv_info_t *recv_info,
                            const uint8_t *data, int data_len)
{
    if (recv_info == NULL || data == NULL || data_len <= 0) {
        return;
    }

    int rssi = 0;
    if (recv_info->rx_ctrl != NULL) {
        rssi = recv_info->rx_ctrl->rssi;
    }

    if (s_recv_callback != NULL) {
        s_recv_callback(recv_info->src_addr, data, data_len, rssi);
    }
}

// Initialize NVS, WiFi (STA mode), and ESP-NOW
esp_err_t espnow_comm_init(espnow_recv_cb_t recv_callback)
{
    s_recv_callback = recv_callback;

    // Initialize NVS — required by WiFi
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize networking and event loop
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Initialize WiFi in station mode (required for ESP-NOW)
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Set long range mode for better RSSI readings (optional)
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA,
        WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR));

    // Initialize ESP-NOW
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(espnow_send_cb));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(espnow_recv_cb));

    ESP_LOGI(TAG, "ESP-NOW initialized successfully");

    // Print own MAC address for reference
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    ESP_LOGI(TAG, "My MAC: " MACSTR, MAC2STR(mac));

    return ESP_OK;
}

esp_err_t espnow_comm_add_peer(const uint8_t *peer_mac)
{
    if (peer_mac == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_now_peer_info_t peer_info = {0};
    memcpy(peer_info.peer_addr, peer_mac, ESP_NOW_ETH_ALEN);
    peer_info.channel = 0;  // use current channel
    peer_info.encrypt = false;

    if (esp_now_is_peer_exist(peer_mac)) {
        ESP_LOGW(TAG, "Peer " MACSTR " already exists", MAC2STR(peer_mac));
        return ESP_OK;
    }

    esp_err_t ret = esp_now_add_peer(&peer_info);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Added peer: " MACSTR, MAC2STR(peer_mac));
    } else {
        ESP_LOGE(TAG, "Failed to add peer: " MACSTR " err=%d", MAC2STR(peer_mac), ret);
    }
    return ret;
}

esp_err_t espnow_comm_send(const uint8_t *peer_mac, const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // If peer_mac is NULL, send to broadcast address
    const uint8_t *target = (peer_mac != NULL) ? peer_mac : BROADCAST_MAC;
    return esp_now_send(target, data, len);
}

esp_err_t espnow_comm_broadcast(const uint8_t *data, size_t len)
{
    // Ensure broadcast peer is added
    if (!esp_now_is_peer_exist(BROADCAST_MAC)) {
        esp_now_peer_info_t peer_info = {0};
        memcpy(peer_info.peer_addr, BROADCAST_MAC, ESP_NOW_ETH_ALEN);
        peer_info.channel = 0;
        peer_info.encrypt = false;
        esp_now_add_peer(&peer_info);
    }

    return esp_now_send(BROADCAST_MAC, data, len);
}

void espnow_comm_deinit(void)
{
    esp_now_deinit();
    esp_wifi_stop();
    esp_wifi_deinit();
    ESP_LOGI(TAG, "ESP-NOW deinitialized");
}
