#include "max30102.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "max30102";

// forward declarations of helper functions
esp_err_t max30102_read_reg(uint8_t reg, uint8_t *data, size_t len);
esp_err_t max30102_write_reg(uint8_t reg, uint8_t data);
esp_err_t max30102_read_sample(max30102_sample_t *sample);
esp_err_t reset_registers();
esp_err_t fifo_configure(uint8_t samples);
esp_err_t spo2_configure(uint8_t adc, uint8_t sr, uint8_t pw);
esp_err_t set_led_current(uint8_t red, uint8_t ir);
esp_err_t set_mode_spo2();
esp_err_t set_mode_heart_rate();
esp_err_t set_mode_multi_led();
esp_err_t reset_fifo();
esp_err_t max30102_check_device();
esp_err_t max30102_init();
 

esp_err_t max30102_check_device()
{
    uint8_t part_id;
    esp_err_t ret;

    ret = max30102_read_reg(0xFF, &part_id, 1);
    if (ret != ESP_OK) return ret;

    if (part_id != 0x15) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t max30102_init()
{
    // device check 
esp_err_t reti = max30102_check_device();if (reti != ESP_OK) return reti;
    // Step 1: reset : we will set the reset bit high

esp_err_t ret;
ret = reset_registers();
if (ret != ESP_OK) return ret;    

    // Step 2: configure FIFO
    fifo_configure(0x50);     // 0x50 = 0101 0000 -> 4 samples, rollover enabled, A_FULL = 0

    // Step 3: configure SPO2
    spo2_configure(0x01, 0x02, 0x03);     // 0x5F = 0101 1111 -> ADC range = 4096 nA, sample rate = 100 Hz, pulse width = 411 µs

    // Step 4: set LED current
    set_led_current(0x24, 0x24);     // RED = 0x24 → ~7 mA, IR  = 0x24 → ~7 mA

    // Step 5: set mode
    set_mode_spo2();     // 0x03 = 0000 0011 → SpO2 mode, sensor ON, normal operation

    
    // Step 6: clear FIFO
    reset_fifo();
ESP_LOGI(TAG, "MAX30102 initialized successfully");
    return ESP_OK;
}

esp_err_t fifo_configure(uint8_t samples){
    /*
[SMP_AVE][ROLLOVER][A_FULL]
  010        1        0000

Sample Averaging (SMP_AVE)
👉 Reduces noise we will use 4 or 8 samples

FIFO Rollover
  👉 What happens when FIFO is full 
0 → stop writing (data loss)
1 → overwrite old data

FIFO Almost Full (A_FULL)
👉 When interrupt triggers (optional)
Value = how many empty slots left
for now Set to default (0 or small value) as we are not using interrupts
*/

    int ret = max30102_write_reg(0x08, samples);  
    if (ret != ESP_OK) return ret;   
    // max30102_write_reg(0x08, 0x78);     // 0x78 = 0111 1000 -> 8 samples, rollover enabled, A_FULL = 0
    // max30102_write_reg(0x08, 0x50);     // 0x50 = 0101 0000 -> 4 samples, rollover enabled, A_FULL = 0
    return ESP_OK;
}

esp_err_t spo2_configure(uint8_t adc, uint8_t sr, uint8_t pw){
       /*
| Bits | Field   | Meaning     |
| ---- | ------- | ----------- |
| 6:5  | ADC_RGE | ADC range   |
| 4:2  | SR      | Sample rate |
| 1:0  | LED_PW  | Pulse width |

ADC Range (bits 6:5)
Controls how strong signal can be before saturation.
Options:
2048 nA → high sensitivity
4096 nA → balanced
8192 nA → wider range
16384 nA → very wide

Sample Rate (bits 4:2)
How many samples per second. {50,100,200,400,800,1000,1600,3200....} hz

LED Pulse Width (bits 1:0)
Controls: Resolution Measurement-time {69,118,215,411} µs.     high resolution better quality but more power 

[ADC_RGE][SR][PW]
   01      011  11
*/
uint8_t value = (adc << 5) | (sr << 2) | pw;
    int ret = max30102_write_reg(0x0A, value);     // 0x5F = 0101 1111 -> ADC range = 4096 nA, sample rate = 100 Hz, pulse width = 411 µs
    if (ret != ESP_OK) return ret;
    return ESP_OK;

}

esp_err_t reset_registers()
{
    esp_err_t ret;
    uint8_t val;

    // Step 1: set reset bit (bit 6)
    ret = max30102_write_reg(0x09, (1 << 6));
    if (ret != ESP_OK) return ret;

    // Step 2: poll until reset bit clears (auto-cleared by chip)
    do {
        vTaskDelay(pdMS_TO_TICKS(1));
        max30102_read_reg(0x09, &val, 1);
    } while (val & (1 << 6));

    return ESP_OK;
}

esp_err_t set_led_current(uint8_t red_current, uint8_t ir_current){
    /* 0x00 → LED OFF  
0xFF → Maximum current 
| Value | Current | Use              |
| ----- | ------- | ---------------- |
| 0x10  | ~3 mA   | Too low          |
| 0x24  | ~7 mA   | Low              |
| 0x3F  | ~12 mA  | Good             |
| 0x7F  | ~25 mA  | Strong           |
| 0xFF  | ~50 mA  | Too high (avoid) |

for the stable reading 
RED = 0x24 → ~7 mA  
IR  = 0x24 → ~7 mA
*/


    // set LED current for RED and IR LEDs
    int ret = max30102_write_reg(0x0C, red_current);     // LED1_PA (RED)
    if (ret != ESP_OK) return ret;
    ret = max30102_write_reg(0x0D, ir_current);      // LED2_PA (IR)
    if (ret != ESP_OK) return ret;
    return ESP_OK;
}

/*
    | Mode Value | Name            | What it does            |
| ---------- | --------------- | ----------------------- |
| `010`      | Heart Rate Mode | Uses **IR LED only**    |
| `011`      | SpO2 Mode       | Uses **RED + IR LEDs**  |
| `111`      | Multi-LED Mode  | Flexible LED sequencing |

*/
esp_err_t set_mode_spo2(){
        /*
    MODE = 011 (SpO2 mode)
    SHDN = 0 → sensor ON
RESET = 0 → normal operation
SHDN RESET  MODE
  0     0    011
  */
int ret = max30102_write_reg(0x09, 0x03);     // 0x03 = 0000 0011 → SpO2 mode, sensor ON, normal operation
    if (ret != ESP_OK) return ret;
    return ESP_OK;
}

esp_err_t set_mode_heart_rate(){
        /*
    MODE = 010 (Heart Rate mode)
    SHDN = 0 → sensor ON
RESET = 0 → normal operation
SHDN RESET  MODE
  0     0    010
  */
int ret = max30102_write_reg(0x09, 0x02);     // 0x02 = 0000 0010 → Heart Rate mode, sensor ON, normal operation
    if (ret != ESP_OK) return ret;
    return ESP_OK;
}

esp_err_t set_mode_multi_led(){
        /*
    MODE = 111 (Multi-LED mode)
    SHDN = 0 → sensor ON
RESET = 0 → normal operation
SHDN RESET  MODE
  0     0    111
  */
int ret = max30102_write_reg(0x09, 0x07);     // 0x07 = 0000 0111 → Multi-LED mode, sensor ON, normal operation
    if (ret != ESP_OK) return ret;
    return ESP_OK;
}

esp_err_t reset_fifo(){
    /*
    To clear FIFO, we can reset the FIFO pointers and overflow counter:
    FIFO_WR_PTR = 0
    OVF_COUNTER = 0
    FIFO_RD_PTR = 0
    */
    int ret;
    ret = max30102_write_reg(0x04, 0x00);     // FIFO_WR_PTR
    if (ret != ESP_OK) return ret;
    ret = max30102_write_reg(0x05, 0x00);     // OVF_COUNTER
    if (ret != ESP_OK) return ret;
    ret = max30102_write_reg(0x06, 0x00);     // FIFO_RD_PTR
    if (ret != ESP_OK) return ret;

    return ESP_OK;
}

esp_err_t max30102_read_reg(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_write_read_device(
        I2C_NUM_0,
        MAX30102_I2C_ADDR,
        &reg,          // register address
        1,
        data,          //  buffer
        len,           //  number of bytes
        pdMS_TO_TICKS(100)
    );
}
esp_err_t max30102_write_reg(uint8_t reg, uint8_t data)
{
    uint8_t buffer[2] = {reg, data};

    return i2c_master_write_to_device(
        I2C_NUM_0,
        MAX30102_I2C_ADDR,
        buffer,        //  buffer pointer
        2,             //  length
        pdMS_TO_TICKS(100)
    );
}


esp_err_t max30102_read_sample(max30102_sample_t *sample)
{
    // NOTE: reads only one sample per call.
// Caller must invoke this in a loop with task delay ≤ 10ms to keep up with 100Hz sample rate.
// If called slower than 100Hz, FIFO will accumulate samples and rollover will silently overwrite old data.
// TODO: replace with burst read (Option A) if data loss is observed.

    uint8_t wr, rd;
    uint8_t buffer[6];
    esp_err_t ret;

    // Step 1: check FIFO
    ret = max30102_read_reg(0x04, &wr, 1);
    if (ret != ESP_OK) return ret;

    ret = max30102_read_reg(0x06, &rd, 1);
    if (ret != ESP_OK) return ret;

    uint8_t samples = (wr - rd) & 0x1F;

    if (samples == 0) {
        return ESP_ERR_NOT_FOUND;   // no data
    }

    // Step 2: read FIFO
    ret = max30102_read_reg(0x07, buffer, 6);
    if (ret != ESP_OK) return ret;

    // Step 3: convert RED
    sample->red = ((uint32_t)buffer[0] << 16) |
                  ((uint32_t)buffer[1] << 8)  |
                  (uint32_t)buffer[2];
    sample->red &= 0x03FFFF;

    // Step 4: convert IR
    sample->ir = ((uint32_t)buffer[3] << 16) |
                 ((uint32_t)buffer[4] << 8)  |
                 (uint32_t)buffer[5];
    sample->ir &= 0x03FFFF;

    return ESP_OK;
}

void filter_init(moving_avg_filter_t *f)
{
    memset(f->buffer, 0, sizeof(f->buffer));
    f->sum = 0;
    f->index = 0;
    f->count = 0;
}

uint32_t filter_update(moving_avg_filter_t *f, uint32_t new_value)
{
    // remove old value
    f->sum -= f->buffer[f->index];

    // insert new value
    f->buffer[f->index] = new_value;
    f->sum += new_value;

    // move index
    f->index = (f->index + 1) % FILTER_SIZE;

    // track count (for initial phase)
    if (f->count < FILTER_SIZE) {
        f->count++;
    }

    // return average
    return f->sum / f->count;
}



