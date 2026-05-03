# Pick which entrypoint to build:
#   - sender_main.c  → normal firmware (ESP-NOW + master FSM)
#   - test_main.c    → standalone PCB self-test (no ESP-NOW)
# Comment out the one you don't want.
# set(MAIN_SRC "sender_main.c")
set(MAIN_SRC "test_main.c")

idf_component_register(
    SRCS ${MAIN_SRC} "../hal/i2c.c"
    INCLUDE_DIRS "." "../hal"
    REQUIRES
        esp_wifi
        esp_timer
        esp_adc
        driver
        protocol
        espnow_comm
        max30102
        tmp117
        ad8232
        buzzer
)
