/*
 * MAX30102 driver — library form of the working maxi2c.c demo.
 *
 * The original maxi2c.c was a standalone ESP-IDF app (its own app_main, its
 * own I²C init, its own scan). This file ports the same algorithms — beat
 * detection, SpO2 calculation, FIFO read, die-temp read — into a clean
 * library. The shared `hal/i2c.c` initialises the bus at 400 kHz; this
 * driver only configures the chip itself.
 *
 * Public API in max30102.h:
 *   max30102_init()              — chip init (call after i2c_init())
 *   max30102_step(out)           — one polling tick at ~100 Hz; updates
 *                                   internal state + fills the reading struct
 *   max30102_read_die_temp()     — silicon temp, not body temp
 *   max30102_read_sample(out)    — kept for test_main.c backward compat
 *   filter_init / filter_update  — moving-average helpers (test_main.c uses)
 */

#include "max30102.h"

#include <math.h>
#include <string.h>

#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "max30102";

// ─── I²C config (shared bus is brought up by hal/i2c.c at 400 kHz) ──
#define MAX30102_I2C_PORT    I2C_NUM_0
#define MAX30102_TIMEOUT_MS  1000

// ─── Register map ───────────────────────────────────────────────────
#define REG_INT_STATUS1     0x00
#define REG_INT_STATUS2     0x01
#define REG_INT_ENABLE1     0x02
#define REG_INT_ENABLE2     0x03
#define REG_FIFO_WR_PTR     0x04
#define REG_OVF_COUNTER     0x05
#define REG_FIFO_RD_PTR     0x06
#define REG_FIFO_DATA       0x07
#define REG_FIFO_CONFIG     0x08
#define REG_MODE_CONFIG     0x09
#define REG_SPO2_CONFIG     0x0A
#define REG_LED1_PA         0x0C
#define REG_LED2_PA         0x0D
#define REG_TEMP_INT        0x1F
#define REG_TEMP_FRAC       0x20
#define REG_TEMP_CONFIG     0x21
#define REG_PART_ID         0xFF

// ─── Mode + SpO2 config values (same as maxi2c.c) ───────────────────
#define MODE_SPO2           0x03
#define SPO2_ADC_RGE        0x20   // 4096 nA
#define SPO2_SR             0x04   // 100 sps
#define SPO2_PW             0x03   // 411 µs
#define FIFO_SMP_AVE        0x00
#define FIFO_ROLLOVER_EN    0x10
#define FIFO_A_FULL         0x0F

// ─── Algorithm parameters (same as maxi2c.c standalone test) ────────
// FINGER_THRESHOLD: IR must exceed this for body_contact = true.
// 50000 was empirically validated on this hardware in standalone tests
// (no finger → IR < 50000, finger on → IR > 56000).
#define BUFFER_LEN          100
#define FINGER_THRESHOLD    50000
#define BEAT_HISTORY        4
#define BEAT_AC_THRESHOLD   500.0f

// ─── Internal state ─────────────────────────────────────────────────
static uint32_t s_ir_buffer[BUFFER_LEN];
static uint32_t s_red_buffer[BUFFER_LEN];
static uint32_t s_sample_count = 0;

static long    s_last_beat_time = 0;
static float   s_bpm_history[BEAT_HISTORY] = {0};
static uint8_t s_bpm_idx = 0;
static float   s_avg_bpm = NAN;

static float   s_ir_dc = 0;
static float   s_prev_ac = 0;

static float   s_last_spo2 = NAN;

// ─── I²C helpers ────────────────────────────────────────────────────
static esp_err_t i2c_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t data[2] = {reg, value};
    return i2c_master_write_to_device(
        MAX30102_I2C_PORT, MAX30102_I2C_ADDR,
        data, sizeof(data),
        pdMS_TO_TICKS(MAX30102_TIMEOUT_MS));
}

static esp_err_t i2c_read_reg(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_write_read_device(
        MAX30102_I2C_PORT, MAX30102_I2C_ADDR,
        &reg, 1,
        buf, len,
        pdMS_TO_TICKS(MAX30102_TIMEOUT_MS));
}

// ─── Initialisation ─────────────────────────────────────────────────
esp_err_t max30102_init(void)
{
    // Reset all internal state — re-init must produce clean readings
    memset(s_ir_buffer,  0, sizeof(s_ir_buffer));
    memset(s_red_buffer, 0, sizeof(s_red_buffer));
    s_sample_count    = 0;
    s_last_beat_time  = 0;
    s_bpm_idx         = 0;
    s_avg_bpm         = NAN;
    s_ir_dc           = 0;
    s_prev_ac         = 0;
    s_last_spo2       = NAN;
    for (int i = 0; i < BEAT_HISTORY; i++) s_bpm_history[i] = 0;

    uint8_t part_id = 0;
    esp_err_t err = i2c_read_reg(REG_PART_ID, &part_id, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Part ID read failed: %d", err);
        return err;
    }
    if (part_id != 0x15) {
        ESP_LOGE(TAG, "Wrong Part ID: 0x%02X (expected 0x15)", part_id);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "MAX30102 found. Part ID: 0x%02X", part_id);

    // Soft reset — write reset bit, give the chip a moment
    if (i2c_write_reg(REG_MODE_CONFIG, 0x40) != ESP_OK) return ESP_FAIL;
    vTaskDelay(pdMS_TO_TICKS(100));

    // Clear FIFO pointers
    if (i2c_write_reg(REG_FIFO_WR_PTR, 0x00) != ESP_OK) return ESP_FAIL;
    if (i2c_write_reg(REG_OVF_COUNTER, 0x00) != ESP_OK) return ESP_FAIL;
    if (i2c_write_reg(REG_FIFO_RD_PTR, 0x00) != ESP_OK) return ESP_FAIL;

    // FIFO config: no averaging, rollover enabled, almost-full @ 15 samples
    if (i2c_write_reg(REG_FIFO_CONFIG,
                      FIFO_SMP_AVE | FIFO_ROLLOVER_EN | FIFO_A_FULL) != ESP_OK)
        return ESP_FAIL;

    // SpO2 mode (Red + IR)
    if (i2c_write_reg(REG_MODE_CONFIG, MODE_SPO2) != ESP_OK) return ESP_FAIL;

    // SpO2 config: ADC range 4096 nA, 100 sps, 411 µs pulse width
    if (i2c_write_reg(REG_SPO2_CONFIG,
                      SPO2_ADC_RGE | SPO2_SR | SPO2_PW) != ESP_OK)
        return ESP_FAIL;

    // LED currents — ~7.4 mA each, same as maxi2c.c demo
    if (i2c_write_reg(REG_LED1_PA, 0x24) != ESP_OK) return ESP_FAIL;
    if (i2c_write_reg(REG_LED2_PA, 0x24) != ESP_OK) return ESP_FAIL;

    // Enable FIFO-almost-full interrupt (we don't read it, but matches demo)
    if (i2c_write_reg(REG_INT_ENABLE1, 0x80) != ESP_OK) return ESP_FAIL;

    ESP_LOGI(TAG, "MAX30102 initialised in SpO2 mode @ 100 sps");
    return ESP_OK;
}

// ─── FIFO read ──────────────────────────────────────────────────────
// SpO2 mode → 6 bytes per sample (Red 3B + IR 3B), 18-bit values
static esp_err_t read_fifo_pair(uint32_t *red, uint32_t *ir)
{
    uint8_t raw[6];
    esp_err_t err = i2c_read_reg(REG_FIFO_DATA, raw, 6);
    if (err != ESP_OK) return err;

    *red = ((uint32_t)(raw[0] & 0x03) << 16) |
           ((uint32_t)raw[1] << 8) |
            (uint32_t)raw[2];

    *ir  = ((uint32_t)(raw[3] & 0x03) << 16) |
           ((uint32_t)raw[4] << 8) |
            (uint32_t)raw[5];
    return ESP_OK;
}

// ─── Beat detection ─────────────────────────────────────────────────
static bool detect_beat(uint32_t ir_value)
{
    float ir_f  = (float)ir_value;
    s_ir_dc     = s_ir_dc * 0.99f + ir_f * 0.01f;
    float ir_ac = ir_f - s_ir_dc;

    bool beat = false;
    if (s_prev_ac < BEAT_AC_THRESHOLD && ir_ac >= BEAT_AC_THRESHOLD) {
        beat = true;
    }
    s_prev_ac = ir_ac;
    return beat;
}

static void update_heart_rate(void)
{
    long now   = xTaskGetTickCount() * portTICK_PERIOD_MS;
    long delta = now - s_last_beat_time;
    s_last_beat_time = now;

    if (delta > 200 && delta < 3000) {  // valid 20–300 BPM
        float bpm = 60000.0f / (float)delta;
        s_bpm_history[s_bpm_idx++ % BEAT_HISTORY] = bpm;

        float sum = 0;
        for (int i = 0; i < BEAT_HISTORY; i++) sum += s_bpm_history[i];
        s_avg_bpm = sum / BEAT_HISTORY;
    }
}

// ─── SpO2 calculation ───────────────────────────────────────────────
// R = (AC_red / DC_red) / (AC_ir / DC_ir);  SpO2 ≈ 110 - 25 * R
static float compute_spo2(const uint32_t *red_buf, const uint32_t *ir_buf, int len)
{
    float red_mean = 0, ir_mean = 0;
    for (int i = 0; i < len; i++) {
        red_mean += red_buf[i];
        ir_mean  += ir_buf[i];
    }
    red_mean /= len;
    ir_mean  /= len;

    float red_rms = 0, ir_rms = 0;
    for (int i = 0; i < len; i++) {
        float rd = (float)red_buf[i] - red_mean;
        float id = (float)ir_buf[i]  - ir_mean;
        red_rms += rd * rd;
        ir_rms  += id * id;
    }
    red_rms = sqrtf(red_rms / len);
    ir_rms  = sqrtf(ir_rms  / len);

    if (ir_mean < 1 || ir_rms < 1) return NAN;

    float R = (red_rms / red_mean) / (ir_rms / ir_mean);
    float spo2 = 110.0f - 25.0f * R;
    if (spo2 > 100.0f) spo2 = 100.0f;
    if (spo2 < 80.0f)  return NAN;   // unreliable
    return spo2;
}

// ─── Public step API ────────────────────────────────────────────────
esp_err_t max30102_step(max30102_reading_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;

    uint32_t red = 0, ir = 0;
    esp_err_t err = read_fifo_pair(&red, &ir);
    if (err != ESP_OK) {
        out->valid         = false;
        out->ir            = 0;
        out->red           = 0;
        out->body_contact  = false;
        out->beat_detected = false;
        out->bpm           = s_avg_bpm;
        out->spo2          = s_last_spo2;
        return err;
    }

    s_ir_buffer [s_sample_count % BUFFER_LEN] = ir;
    s_red_buffer[s_sample_count % BUFFER_LEN] = red;
    s_sample_count++;

    bool finger = (ir > FINGER_THRESHOLD);
    bool beat   = false;
    if (finger) {
        if (detect_beat(ir)) {
            update_heart_rate();
            beat = true;
        }
    } else {
        // Lost contact — drop HR confidence so we don't display stale numbers
        s_avg_bpm  = NAN;
    }

    // Recompute SpO2 every BUFFER_LEN samples while finger is on
    if (finger && s_sample_count > 0 && (s_sample_count % BUFFER_LEN == 0)) {
        float v = compute_spo2(s_red_buffer, s_ir_buffer, BUFFER_LEN);
        if (!isnan(v)) {
            s_last_spo2 = v;
        }
    }
    if (!finger) s_last_spo2 = NAN;

    out->ir            = ir;
    out->red           = red;
    out->body_contact  = finger;
    out->beat_detected = beat;
    out->bpm           = s_avg_bpm;
    out->spo2          = s_last_spo2;
    out->valid         = true;
    return ESP_OK;
}

// ─── Die temperature (silicon, not body) ────────────────────────────
float max30102_read_die_temp(void)
{
    if (i2c_write_reg(REG_TEMP_CONFIG, 0x01) != ESP_OK) return NAN;
    vTaskDelay(pdMS_TO_TICKS(30));

    uint8_t temp_int = 0, temp_frac = 0;
    if (i2c_read_reg(REG_TEMP_INT,  &temp_int,  1) != ESP_OK) return NAN;
    if (i2c_read_reg(REG_TEMP_FRAC, &temp_frac, 1) != ESP_OK) return NAN;

    return (float)(int8_t)temp_int + (temp_frac * 0.0625f);
}

// ─── Backward-compat surface used by test_main.c ────────────────────
esp_err_t max30102_read_sample(max30102_sample_t *sample)
{
    if (sample == NULL) return ESP_ERR_INVALID_ARG;
    return read_fifo_pair(&sample->red, &sample->ir);
}

void filter_init(moving_avg_filter_t *f)
{
    memset(f->buffer, 0, sizeof(f->buffer));
    f->sum   = 0;
    f->index = 0;
    f->count = 0;
}

uint32_t filter_update(moving_avg_filter_t *f, uint32_t new_value)
{
    f->sum -= f->buffer[f->index];
    f->buffer[f->index] = new_value;
    f->sum += new_value;
    f->index = (f->index + 1) % FILTER_SIZE;
    if (f->count < FILTER_SIZE) f->count++;
    return f->sum / f->count;
}
