idf_component_register(
    SRCS "app_mqtt.c"
    INCLUDE_DIRS "include"
    REQUIRES mqtt esp_event app_event
)
