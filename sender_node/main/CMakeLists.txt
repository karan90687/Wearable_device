idf_component_register(
    SRCS "sender_main.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi protocol espnow_comm esp_timer
)
