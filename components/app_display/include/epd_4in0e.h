#pragma once

#include <esp_err.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Display resolution (native orientation is portrait: 400 wide x 600 tall) */
#define EPD_WIDTH   400
#define EPD_HEIGHT  600

/* Framebuffer size: 4-bit per pixel, 2 pixels per byte */
#define EPD_BUF_SIZE  (EPD_WIDTH / 2 * EPD_HEIGHT)  /* 120,000 bytes */

/* 6-color index (4-bit per pixel) */
#define EPD_COLOR_BLACK   0x0
#define EPD_COLOR_WHITE   0x1
#define EPD_COLOR_YELLOW  0x2
#define EPD_COLOR_RED     0x3
#define EPD_COLOR_BLUE    0x5
#define EPD_COLOR_GREEN   0x6

/**
 * @brief EPD hardware pin configuration
 */
typedef struct {
    int spi_host;   /**< SPI host: SPI2_HOST or SPI3_HOST */
    int pin_mosi;
    int pin_clk;
    int pin_cs;
    int pin_dc;
    int pin_rst;
    int pin_busy;
    int pin_pwr;    /**< Power control pin, -1 if not used */
} epd_pin_config_t;

/**
 * @brief Initialize EPD hardware: SPI bus, GPIOs, and run display init sequence
 */
esp_err_t epd_init(const epd_pin_config_t *pins);

/**
 * @brief Clear screen with a single color
 */
void epd_clear(uint8_t color);

/**
 * @brief Display a full framebuffer (EPD_BUF_SIZE bytes, 4-bit per pixel)
 *
 * Framebuffer layout: row-major, 2 pixels per byte (high nibble = left pixel).
 * Total size must be EPD_BUF_SIZE (120,000 bytes).
 */
void epd_display(const uint8_t *buf);

/**
 * @brief Enter deep sleep mode (must re-init to wake)
 */
void epd_sleep(void);

/**
 * @brief Deinitialize EPD: release SPI bus and GPIOs
 */
void epd_deinit(void);

#ifdef __cplusplus
}
#endif
