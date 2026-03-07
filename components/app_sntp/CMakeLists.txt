idf_component_register(
    SRCS "app_sntp.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_event app_event
)
