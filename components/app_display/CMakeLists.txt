idf_component_register(
    SRCS "app_display.c" "epd_4in0e.c" "epd_paint.c"
         "fonts/font12.c" "fonts/font16.c" "fonts/font20.c" "fonts/font24.c"
    INCLUDE_DIRS "include"
    REQUIRES driver app_event app_protocol app_espnow app_mqtt esp_timer app_weather json
)
