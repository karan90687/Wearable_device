# MAX30102 Driver Component

ESP-IDF driver for the **MAX30102** pulse-oximeter and heart-rate sensor, used by the wearable sender node. This component exposes a minimal API to initialize the sensor over I²C and read raw RED/IR samples from its FIFO, plus a small moving-average helper to smooth the output.

Source: [max30102.c](max30102.c), [include/max30102.h](include/max30102.h)

---

## 1. Overview

| Property | Value |
| --- | --- |
| Device | Maxim MAX30102 (Pulse Ox + HR) |
| Interface | I²C (7-bit address `0x57`) |
| ESP-IDF bus | `I2C_NUM_0` |
| Timeout | 100 ms per transaction |
| Log tag | `max30102` |
| Dependencies | `driver`, `esp_adc` (see [CMakeLists.txt](CMakeLists.txt)) |

The driver assumes the I²C master bus (`I2C_NUM_0`) has already been installed and configured by the application before `max30102_init()` is called.

---

## 2. Public API

Declared in [include/max30102.h](include/max30102.h).

### Types

```c
typedef struct {
    uint32_t red;   // 18-bit RED ADC count
    uint32_t ir;    // 18-bit IR  ADC count
} max30102_sample_t;

typedef struct {
    uint32_t buffer[FILTER_SIZE];   // FILTER_SIZE = 5
    uint32_t sum;
    int index;
    int count;
} moving_avg_filter_t;
```

### Functions

#### `esp_err_t max30102_init(void)`
Runs the full power-on sequence:
1. `max30102_check_device()` — reads PART_ID (`0xFF`) and verifies it is `0x15`.
2. `reset_registers()` — sets the RESET bit in MODE_CONFIG (`0x09`) and waits 10 ms.
3. `fifo_configure(0x50)` — 4-sample averaging, rollover enabled, A_FULL = 0.
4. `spo2_configure(0x01, 0x03, 0x03)` — ADC range 4096 nA, 100 Hz sample rate, 411 µs pulse width.
5. `set_led_current(0x24, 0x24)` — RED and IR LEDs at ~7 mA each.
6. `set_mode_spo2()` — enables SpO₂ mode (both LEDs).
7. `reset_fifo()` — clears FIFO pointers and overflow counter.

Returns `ESP_OK` on success or the first failing `esp_err_t`.

#### `esp_err_t max30102_read_sample(max30102_sample_t *sample)`
Reads one sample (RED + IR) from the FIFO:
- Computes the number of available samples from `FIFO_WR_PTR - FIFO_RD_PTR` (masked to 5 bits).
- Returns `ESP_ERR_NOT_FOUND` when the FIFO is empty.
- Otherwise reads 6 bytes from `FIFO_DATA` (`0x07`) and unpacks two 18-bit words into `sample->red` and `sample->ir`.

---

## 3. Internal helpers

These are defined in [max30102.c](max30102.c) and are not exported through the header; they exist to keep `max30102_init()` readable and are easy to repurpose if you need a different startup profile.

| Helper | Register | Purpose |
| --- | --- | --- |
| `max30102_check_device()` | `0xFF` PART_ID | Verifies chip identity (`0x15`). |
| `reset_registers()` | `0x09` MODE_CONFIG | Read–modify–write of the RESET bit (bit 6). |
| `fifo_configure(samples)` | `0x08` FIFO_CONFIG | SMP_AVE / ROLLOVER_EN / A_FULL. |
| `spo2_configure(adc, sr, pw)` | `0x0A` SPO2_CONFIG | `(adc<<5) \| (sr<<2) \| pw`. |
| `set_led_current(red, ir)` | `0x0C`, `0x0D` | LED1_PA / LED2_PA pulse amplitudes. |
| `set_mode_spo2()` | `0x09` | MODE = `011`. |
| `set_mode_heart_rate()` | `0x09` | MODE = `010` (IR only). |
| `set_mode_multi_led()` | `0x09` | MODE = `111`. |
| `reset_fifo()` | `0x04`, `0x05`, `0x06` | Zeroes FIFO_WR_PTR, OVF_COUNTER, FIFO_RD_PTR. |
| `max30102_read_reg()` | — | `i2c_master_write_read_device` wrapper. |
| `max30102_write_reg()` | — | `i2c_master_write_to_device` wrapper. |

### Configuration field notes

**FIFO_CONFIG (`0x08`)**
```
[ SMP_AVE | ROLLOVER_EN | A_FULL ]
   7:5         4          3:0
```
- `SMP_AVE`: 1 / 2 / 4 / 8 / 16 / 32 sample averaging.
- `ROLLOVER_EN`: 0 = stop on full, 1 = overwrite.
- `A_FULL`: almost-full interrupt threshold (unused here).

**SPO2_CONFIG (`0x0A`)**
- `ADC_RGE` (6:5): 2048 / 4096 / 8192 / 16384 nA full-scale.
- `SR` (4:2): 50 / 100 / 200 / 400 / 800 / 1000 / 1600 / 3200 Hz.
- `LED_PW` (1:0): 69 / 118 / 215 / 411 µs → 15 / 16 / 17 / 18-bit resolution.

**LED current (`0x0C`, `0x0D`)**
| Value | Approx. current | Notes |
| ----- | --------------- | ----- |
| `0x00` | 0 mA | LED off |
| `0x24` | ~7 mA | Used at boot — stable for finger contact |
| `0x3F` | ~12 mA | Stronger signal |
| `0x7F` | ~25 mA | High |
| `0xFF` | ~50 mA | Max (avoid) |

---

## 4. Moving-average filter

A small ring-buffer filter for smoothing raw FIFO samples before downstream heart-rate / SpO₂ processing.

```c
moving_avg_filter_t red_filter;
filter_init(&red_filter);

uint32_t smoothed = filter_update(&red_filter, sample.red);
```

- `FILTER_SIZE` is fixed at 5 (change in [include/max30102.h](include/max30102.h)).
- Returns the running average once the buffer is warm; during warm-up it divides by `count` so early samples are still usable.

> Note: `filter_init` / `filter_update` are currently defined in `max30102.c` but not declared in the header — add prototypes there before calling them from outside this component.

---

## 5. Usage

The MAX30102 driver does **not** install the I²C peripheral itself — you must bring up `I2C_NUM_0` before calling `max30102_init()`. In this project the bus is initialized by `i2c_init()` in [sender_node/hal/i2c.c](../../hal/i2c.c) (SDA = GPIO 21, SCL = GPIO 22, 100 kHz, pull-ups enabled).

```c
#include "max30102.h"
#include "i2c.h"   // sender_node/hal/i2c.c — provides i2c_init()

// 1. Initialize the I2C peripheral (I2C_NUM_0) — must happen first.
ESP_ERROR_CHECK(i2c_init());

// 2. Initialize the sensor.
ESP_ERROR_CHECK(max30102_init());

// 3. Poll the FIFO.
max30102_sample_t s;
while (1) {
    esp_err_t err = max30102_read_sample(&s);
    if (err == ESP_OK) {
        ESP_LOGI("app", "RED=%lu  IR=%lu", s.red, s.ir);
    } else if (err != ESP_ERR_NOT_FOUND) {
        ESP_LOGE("app", "read failed: %s", esp_err_to_name(err));
    }
    vTaskDelay(pdMS_TO_TICKS(10));
}
```

Default boot profile produced by `max30102_init()`: SpO₂ mode, 100 Hz sample rate, 411 µs pulse width, ADC range 4096 nA, ~7 mA on both LEDs, 4-sample FIFO averaging with rollover.

---

## 6. Register map quick reference

| Addr | Name | Used for |
| ---- | ---- | -------- |
| `0x04` | FIFO_WR_PTR | Write pointer / reset |
| `0x05` | OVF_COUNTER | Overflow count / reset |
| `0x06` | FIFO_RD_PTR | Read pointer / reset |
| `0x07` | FIFO_DATA | Sample data (3 bytes RED, 3 bytes IR) |
| `0x08` | FIFO_CONFIG | SMP_AVE, ROLLOVER_EN, A_FULL |
| `0x09` | MODE_CONFIG | SHDN, RESET, MODE |
| `0x0A` | SPO2_CONFIG | ADC_RGE, SR, LED_PW |
| `0x0C` | LED1_PA | RED LED current |
| `0x0D` | LED2_PA | IR LED current |
| `0xFF` | PART_ID | `0x15` for MAX30102 |

---

## 7. Limitations & TODO

- No I²C bus installation inside the driver — caller must set up `I2C_NUM_0` first.
- No interrupt support (A_FULL / PPG_RDY); sampling is poll-based.
- `filter_init` / `filter_update` need prototypes in the header to be used outside this file.
- No BPM / SpO₂ computation yet — `max30102_read_sample()` returns raw ADC counts only.
- Configuration values are hard-coded inside `max30102_init()`; there is no runtime config struct.
