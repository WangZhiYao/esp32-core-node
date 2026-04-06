idf_component_register(
    SRCS "app_weather.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client json app_event esp_timer esp-tls
)
