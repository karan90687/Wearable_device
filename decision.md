# Distributed Wearable Health & Safety Monitoring System  
## Finalized Architecture – Phase 1 (Prototype Version)

---

# 1. Project Overview

This project presents a **distributed wearable health and safety monitoring system** designed for team-based monitoring in high-risk environments.

The current implementation focuses on:

- Physiological monitoring
- Environmental monitoring
- Local team coordination using ESP-NOW
- Gateway-based data visualization

Long-range communication (LoRa) is planned as a future upgrade and is not part of Phase 1 implementation.

---

# 2. Current System Architecture (Phase 1)

## Communication Model

Protocol Used: **ESP-NOW**

Purpose:
- Short-range (~100m) peer-to-peer communication
- Fast intra-team coordination
- Emergency alert propagation

Architecture:

Wearable Nodes (ESP32 + Sensors)
        ↓ ESP-NOW
Gateway Node (ESP32)
        ↓ USB
Laptop (Monitoring Interface)

---

# 3. Node Structure

## Wearable Node (Team Unit)

Each wearable node contains:

- ESP32 microcontroller
- MAX30102 (Heart Rate + SpO₂)
- Digital Temperature Sensor
- Gas Sensor (VOC / Air Quality)
- Battery power supply

Responsibilities:
- Read sensor data
- Process basic thresholds
- Transmit structured packets via ESP-NOW
- Generate emergency alerts if required

---

## Gateway Node

Contains:
- ESP32
- USB connection to laptop

Responsibilities:
- Receive ESP-NOW packets
- Decode sensor data
- Forward data via Serial to laptop
- Display team status

---

# 4. Finalized Sensors & Measurable Parameters

## 4.1 Physiological Monitoring

### MAX30102
Measures:
- Heart Rate (BPM)
- SpO₂ (%)

---

### Digital Temperature Sensor (e.g., TMP117 / MCP9808)
Measures:
- Skin / Body Temperature (°C)

---

## 4.2 Environmental Monitoring

### VOC Gas Sensor (e.g., SGP30 / SGP40)

Measures:
- TVOC level
- Equivalent CO₂ (eCO₂)
- Air quality classification

---

# 5. Data Parameters Transmitted

Each wearable node transmits:

- Node ID
- Heart Rate
- SpO₂
- Temperature
- TVOC / Air Quality
- Status flag (Normal / Alert / SOS)

Data transmission is:
- Event-driven
- Low-latency
- Optimized for small payload size

---

# 6. Demonstration Model

For project evaluation:

- 2–4 wearable nodes
- 1 gateway node connected to laptop
- Real-time display of:
  - Vital signs
  - Air quality
  - Alert status

Scenarios demonstrated:
- Normal monitoring
- Abnormal vitals alert
- Node connectivity verification

---

# 7. Microcontroller Decision

Selected: **ESP32**

Reasons:
- Built-in ESP-NOW support
- Reduced hardware complexity
- Integrated wireless stack
- Suitable for multi-node communication demo
- Rapid prototyping capability

---

# 8. Future Scope (Phase 2 – Not Implemented Yet)

Planned enhancement:
- LoRa module integration for long-range communication
- Periodic summary transmission to remote command center
- Kilometer-level range support

LoRa will function as:
- Long-range backhaul layer
- Emergency escalation channel

This is an architectural extension and not part of the current prototype.

---

# 9. Scope Clarification

This project is:
- A functional distributed prototype
- Architecture-focused
- Scalable

This project is not:
- A certified medical device
- A military-grade certified system
- A production-ready industrial deployment

---

# 10. Summary

The current Phase 1 system demonstrates:

- Multi-node wearable monitoring
- Real-time physiological & environmental sensing
- Low-latency ESP-NOW coordination
- Gateway-based monitoring interface
- Expandable communication architecture

The system is technically feasible, defendable, and suitable for final-year engineering evaluation.
