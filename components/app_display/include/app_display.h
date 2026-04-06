#pragma once
#include <esp_err.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int spi_host;
    int pin_mosi;
    int pin_clk;
    int pin_cs;
    int pin_dc;
    int pin_rst;
    int pin_busy;
    int pin_pwr;               // -1 if not used
    int refresh_interval_s;    // dashboard refresh interval
} app_display_config_t;

esp_err_t app_display_init(const app_display_config_t *config);
esp_err_t app_display_stop(void);

#ifdef __cplusplus
}
#endif
