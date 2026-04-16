# Smart Wearable Body Vitals Monitoring System

## 📌 Project Overview

A wearable embedded system for continuous body vital monitoring, built as a final-year engineering project. The system captures raw physiological data on a wearable ESP32 node, transmits it wirelessly to a master ESP32 over ESP-NOW, and streams it to a Python dashboard on a laptop for real-time signal processing and visualization.

The current scope is strictly limited to body vitals, emphasizing **reliable sensing, clean data capture, and offline analysis**.

---

## 🩺 Vitals Monitored

| Vital | Sensor | Method |
|-------|--------|--------|
| Heart Rate + SpO₂ | MAX30102 | Optical PPG (Red + IR) |
| Body Temperature | TMP117 | I²C digital, ±0.1°C accuracy |
| ECG | AD8232 | Analog ECG via chest probes → ESP32 ADC |

---

## 🧱 System Architecture

```
[ Wearable Node ]                [ Master Node ]            [ Laptop ]
  ESP32 (sender)    ESP-NOW →    ESP32 (master)   USB/UART →  Python Dashboard
  ├── MAX30102                   └── Forwards raw            ├── serial_reader.py
  ├── TMP117                         packet over             ├── gui.py
  └── AD8232                         serial                  └── Signal analysis
```

- Sensor drivers run on the **sender node** — raw samples collected and packed
- **ESP-NOW** handles wireless transfer to the master node (no WiFi overhead)
- Master node forwards data over USB serial to the laptop
- **Python dashboard** handles all signal processing, visualization, and analysis — keeping the ESP32 lean and deterministic

> PCB design is currently in progress. Development and testing is being done on dev boards.

---

## 📂 Repository Structure

```
Wearable_device/
├── common/                         # Shared code between sender and master
│   ├── espnow_comm/                # ESP-NOW send/receive abstraction
│   └── protocol/                   # Shared data packet definitions
│
├── sender_node/                    # Wearable ESP32 — sensor data capture
│   ├── components/
│   │   ├── max30102/               # PPG sensor driver (HR + SpO₂)
│   │   ├── tmp117/                 # Temperature sensor driver
│   │   └── buzzer/                 # Buzzer component
│   ├── hal/
│   │   ├── i2c.c / i2c.h          # I²C HAL shared by sensor drivers
│   └── main/
│       └── sender_main.c           # Entry point — sampling loop + ESP-NOW TX
│
├── master_node/                    # Master ESP32 — receives and forwards data
│   └── main/
│       └── master_main.c           # ESP-NOW RX → USB serial forward
│
├── dashboard/                      # Python — signal processing + visualization
│   ├── main.py
│   ├── serial_reader.py            # Reads structured packets from USB serial
│   ├── gui.py                      # Real-time plotting and display
│   └── requirements.txt
│
└── project_plan.md
```

---

## 💻 Software Approach

**On the ESP32 (sender node):**
- Low-level sensor drivers written from scratch in C (ESP-IDF)
- I²C HAL layer shared across TMP117 and MAX30102
- AD8232 ECG sampled via ESP32 ADC
- Raw samples packed using a shared protocol struct and sent over ESP-NOW

**On the laptop (Python):**
- Serial reader parses incoming packets from master node
- Signal processing done with `scipy` and `numpy` — peak detection, SpO₂ R-value computation, ECG analysis
- GUI plots live waveforms and computed vitals

This split keeps the ESP firmware simple and deterministic — no heavy computation on the MCU.

---

## 🧩 Hardware

- 2× sender ESP32 and one master ESP32 development boards 
- MAX30102 — optical PPG sensor (I²C)
- TMP117 — high-accuracy digital temperature sensor (I²C), ±0.1°C
- AD8232 — ECG front-end module with chest probes (analog output → ADC)
- Single wearable PCB — design in progress
- Li-ion battery 

---

## 🎯 Objectives

- Capture reliable raw physiological data across all three sensors
- Transmit data wirelessly with minimal latency using ESP-NOW
- Process and visualize signals in Python to extract meaningful health metrics
- Design a single PCB wearable form factor
- Build a scalable base for future expansion (GPS, wireless alerts, cloud logging)
