idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES app_event app_storage app_network app_mqtt app_espnow app_sntp app_display app_weather
)
