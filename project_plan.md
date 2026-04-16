# ESP-NOW Sensor Network — Project Plan

## Overview
A 3-ESP32 sensor network: 2 sender nodes collect health & environmental data and transmit via ESP-NOW to 1 master node connected to a laptop via USB. A Python GUI dashboard displays live data. The two senders also monitor proximity via RSSI — buzzer triggers if they drift too far apart.

**Framework:** ESP-IDF (C)
**Boards:** 3x ESP32
**Distance method:** RSSI-based estimation (offline, no extra hardware)

## mac address of esp
### esp32 whose led not blink sometimes
'''
MAC Address: C0:CD:D6:CE:27:58
'''
### esp32s3 
'''
MAC Address: 9C:13:9E:90:C6:E0 
'''
### esp32 white back 
MAC Address: C0:CD:D6:CE:27:58
## Sensors
| Sensor | Interface | Measures |
|--------|-----------|----------|
| MAX30102 | I2C | Heart rate, SpO2 |
| MAX30205 | I2C | Body temperature |
| MQ-135 | ADC | Air quality (gas PPM) |
| TMP117 | OneWire (GPIO) | Environment temperature |

## Architecture
```
  [Sender ESP #1]  <--ESP-NOW-->  [Sender ESP #2]
    (sensors + buzzer)              (sensors + buzzer)
         |                               |
         |  ESP-NOW                      |  ESP-NOW
         v                               v
              [Master ESP (receiver)]
                       |
                    USB Serial
                       |
              [Laptop - Python GUI]
```

## Folder Structure
```
fyp/
├── common/                         # Shared code (sender & master)
│   ├── espnow_comm/               # ESP-NOW init, send, receive wrappers
│   └── protocol/                  # Shared packet structures & node IDs
├── sender_node/                   # Firmware for both sensor ESPs
│   ├── main/                      # sender_main.c (app_main)
│   └── components/
│       ├── max30102/              # Heart rate & SpO2 (I2C)
│       ├── max30205/              # Body temp (I2C)
│       ├── mq135/                 # Gas sensor (ADC)
│       ├── tmp117/               # Env temp (OneWire)
│       └── buzzer/                # Buzzer (GPIO)
├── master_node/                   # Firmware for master/receiver ESP
│   └── main/                      # master_main.c (app_main)
├── dashboard/                     # Python GUI (runs on laptop)
│   ├── main.py
│   ├── serial_reader.py
│   └── gui.py
└── project_plan.md                # This file
```

## Implementation Stages

### Stage 1: Project Scaffolding & Basic ESP-NOW
- Create folder structure & boilerplate
- Define shared protocol (sensor_packet_t struct)
- Implement ESP-NOW wrapper (WiFi STA init, ESP-NOW init, send/recv)
- Sender sends dummy packets every 2s
- Master receives & prints JSON to serial
- **Test:** Flash sender + master → JSON on `idf.py monitor`

### Stage 2: Sensor Drivers
- MAX30102 driver (I2C — HR/SpO2 algorithm)
- MAX30205 driver (I2C — temperature register read)
- MQ-135 driver (ADC — voltage to PPM conversion)
- TMP117 driver (OneWire — bit-bang protocol)
- Integrate into sender_main.c
- **Test:** Real sensor values appear on master serial

### Stage 3: Dual Sender + RSSI
- Both senders transmit to master with unique node_id
- Senders also ping each other via ESP-NOW
- Extract RSSI from receive callback
- Master differentiates node 1 vs node 2
- **Test:** Master shows data from both senders with RSSI values

### Stage 4: Buzzer — Distance Threshold
- Buzzer driver (GPIO on/off/pattern)
- RSSI threshold check with hysteresis
- Buzzer triggers when RSSI < threshold (too far apart)
- **Test:** Walk apart → buzzer sounds; come closer → stops

### Stage 5: Python Dashboard
- Serial protocol: JSON lines over UART
- serial_reader.py — parse serial data
- gui.py — tkinter live dashboard (2 panels, color alerts)
- **Test:** `python main.py` shows live data from both nodes

### Stage 6: Polish & Future
- LED indicators, physical buttons
- Data logging to CSV
- Configurable thresholds from dashboard
- Low battery / disconnect detection

## Pin Assignments (to be finalized)
| Function | GPIO |
|----------|------|
| I2C SDA (MAX30102, MAX30205) | GPIO 21 |
| I2C SCL (MAX30102, MAX30205) | GPIO 22 |
| MQ-135 analog out | GPIO 34 (ADC1_CH6) |
| TMP117 data | GPIO 4 |
| Buzzer | GPIO 5 |

## Notes
- Both senders run identical firmware, differentiated by `node_id`
- MAX30102 and MAX30205 share the same I2C bus (different addresses: 0x57 and 0x48)
- ESP-IDF v5.x+ required (for `esp_now_recv_info_t` with RSSI)
- Common components shared via `EXTRA_COMPONENT_DIRS` in CMakeLists.txt
