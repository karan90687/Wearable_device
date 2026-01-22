# Smart Wearable Body Vitals Monitoring System

## 📌 Project Overview
This project focuses on the design and development of a **wearable embedded system** for **continuous body vital monitoring**, intended as the foundational phase of a larger soldier-safety and mission-support platform.

The current scope is **strictly limited to body vitals**, emphasizing **feasibility, low power consumption, and reliable sensing**.

---

## 🎯 Objectives
- Monitor critical body vitals in real time  
- Design a **single-PCB wearable system**
- Ensure low power operation suitable for battery-powered use
- Build a scalable architecture for future feature expansion

---

## 🩺 Body Vitals Covered (Phase 1)
The first phase focuses on **core physiological parameters** only:

- **Heart Rate (HR)** – via optical PPG sensing  
- **Blood Oxygen Saturation (SpO₂)** – via optical PPG  
- **Body / Skin Temperature** – for heat stress and trend analysis  
- **Motion Context** – using IMU data to validate vitals (movement vs rest)

> No GPS, gas sensors, wireless communication, or cloud features are included in this phase.

---

## 🧱 System Architecture (High Level)

- Sensor data is processed locally on the MCU  
- Focus on **signal quality, reliability, and power efficiency**  
- Communication features are deferred to later phases  

---

## 🧩 Hardware Scope
- Single wearable PCB
- Integrated sensors:
  - PPG sensor (HR + SpO₂)
  - Temperature sensor
  - IMU (accelerometer)
- Battery-powered design
- Modular PCB layout to support future expansion

---

## 💻 Software Scope
- Low-level sensor drivers
- Periodic sampling & digital filtering
- Motion-aware validation of vitals
- Power-aware task scheduling

---

## 📂 Repository Structure
