idf_component_register(
    SRCS "ad8232.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_adc
)


/*
 * ECG task — drop this into sender_main.c alongside spo2_task()
 *
 * 1. Add #include "ad8232.h" at the top of sender_main.c
 * 2. Call i2c_init() first (already done in app_main), then ad8232_init()
 * 3. Create the task in app_main with xTaskCreate()
 *

