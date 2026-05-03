#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

// Node identifiers
#define NODE_ID_SENDER_1 1
#define NODE_ID_SENDER_2 2
#define NODE_ID_MASTER 0

// Packet type identifiers
// Data packets (sender → master)
#define PACKET_TYPE_SENSOR_DATA 0x01  // vitals (HR/SpO2/temp) — Stage 0/1
#define PACKET_TYPE_ECG         0x02  // batched ECG samples — wired in Stage 3
// Control packets — handshake + start/stop, used in Stage 2
#define PACKET_TYPE_HELLO       0x10  // master → sender: are you there?
#define PACKET_TYPE_READY       0x11  // sender → master: yes, sensors initialised
#define PACKET_TYPE_CMD_START   0x12  // master → sender: begin streaming
#define PACKET_TYPE_CMD_STOP    0x13  // master → sender: stop streaming
#define PACKET_TYPE_HEARTBEAT   0x14  // bidirectional liveness
// Legacy alias (unused — left for source compatibility, drop later)
#define PACKET_TYPE_PING        PACKET_TYPE_HEARTBEAT

// RSSI distance threshold (dBm) — calibrate based on environment
#define RSSI_THRESHOLD_FAR -70  // buzzer ON if below this
#define RSSI_THRESHOLD_NEAR -60 // buzzer OFF if above this
#define RSSI_HYSTERESIS_COUNT 3 // consecutive readings before state change

// Sensor data packet sent from sender → master
typedef struct __attribute__((packed))
{
    uint8_t packet_type; // PACKET_TYPE_SENSOR_DATA
    uint8_t node_id;     // NODE_ID_SENDER_1 or NODE_ID_SENDER_2
    float heart_rate;    // BPM from MAX30102
    float spo2;          // % from MAX30102
    float body_temp;     // Celsius from MAX30205
    float env_temp;      // Celsius from TMP117
    float gas_ppm;       // PPM from MQ-135
    int8_t rssi_peer;    // RSSI of the other sender (-127 if unknown)
} sensor_packet_t;

// Lightweight ping packet sent between senders for RSSI measurement (legacy)
typedef struct __attribute__((packed))
{
    uint8_t packet_type; // PACKET_TYPE_PING
    uint8_t node_id;     // sender ID
} ping_packet_t;

// Control packet — used for HELLO / READY / CMD_START / CMD_STOP / HEARTBEAT
typedef struct __attribute__((packed))
{
    uint8_t packet_type;
    uint8_t node_id;
} ctrl_packet_t;

// ECG batched packet — sender → master while streaming
#define ECG_SAMPLES_PER_PACKET 32
typedef struct __attribute__((packed))
{
    uint8_t  packet_type;                          // PACKET_TYPE_ECG
    uint8_t  node_id;
    uint16_t seq;                                  // monotonically increasing
    uint16_t samples[ECG_SAMPLES_PER_PACKET];      // 12-bit ADC; 0xFFFF = leads-off
} ecg_packet_t;

#endif // PROTOCOL_H
