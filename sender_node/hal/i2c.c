#include "i2c.h"
#include "esp_log.h"

static const char *TAG = "wearable_i2c";

esp_err_t i2c_init(void)
{
    static bool initialized = false;
    if (initialized) return ESP_OK;

    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_GPIO,
        .scl_io_num = I2C_MASTER_SCL_GPIO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };

    ESP_ERROR_CHECK(i2c_param_config(I2C_MASTER_PORT, &conf));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_MASTER_PORT, conf.mode, 0, 0, 0));

    ESP_LOGI(TAG, "I2C master init on SDA=%d SCL=%d @ %d Hz",
             I2C_MASTER_SDA_GPIO, I2C_MASTER_SCL_GPIO, I2C_MASTER_FREQ_HZ);
    initialized = true;
    return ESP_OK;
}
