idf_component_register(
    SRCS "sender_main.c"
    INCLUDE_DIRS "."
    REQUIRES espnow_comm protocol esp_timer
)
