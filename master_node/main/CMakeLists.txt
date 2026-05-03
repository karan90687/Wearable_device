idf_component_register(
    SRCS "master_main.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi protocol espnow_comm driver json esp_timer
)
