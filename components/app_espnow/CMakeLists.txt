idf_component_register(
    SRCS "app_espnow.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_event esp_timer app_event app_storage app_protocol
)
