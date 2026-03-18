#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

// Node identifiers
#define NODE_ID_SENDER_1    1
#define NODE_ID_SENDER_2    2
#define NODE_ID_MASTER      0

// Packet type identifiers
#define PACKET_TYPE_SENSOR_DATA   0x01
#define PACKET_TYPE_PING          0x02

// RSSI distance threshold (dBm) — calibrate based on environment
#define RSSI_THRESHOLD_FAR   -70   // buzzer ON if below this
#define RSSI_THRESHOLD_NEAR  -60   // buzzer OFF if above this
#define RSSI_HYSTERESIS_COUNT  3   // consecutive readings before state change

// Sensor data packet sent from sender → master
typedef struct __attribute__((packed)) {
    uint8_t packet_type;       // PACKET_TYPE_SENSOR_DATA
    uint8_t node_id;           // NODE_ID_SENDER_1 or NODE_ID_SENDER_2
    float   heart_rate;        // BPM from MAX30102
    float   spo2;              // % from MAX30102
    float   body_temp;         // Celsius from MAX30205
    float   env_temp;          // Celsius from DS18B20
    float   gas_ppm;           // PPM from MQ-135
    int8_t  rssi_peer;         // RSSI of the other sender (-127 if unknown)
} sensor_packet_t;

// Lightweight ping packet sent between senders for RSSI measurement
typedef struct __attribute__((packed)) {
    uint8_t packet_type;       // PACKET_TYPE_PING
    uint8_t node_id;           // sender ID
} ping_packet_t;

#endif // PROTOCOL_H
