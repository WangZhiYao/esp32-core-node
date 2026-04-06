#include "epd_4in0e.h"

#include <stdbool.h>
#include <string.h>

#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "epd"
#define EPD_BUSY_TIMEOUT_MS 30000

/* ───────────────────────── Internal State ───────────────────────── */

static epd_pin_config_t s_pins;
static spi_device_handle_t s_spi = NULL;

/* ───────────────────────── Low-level Helpers ───────────────────────── */

static void epd_delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static void epd_gpio_set(int pin, int level)
{
    gpio_set_level((gpio_num_t)pin, level);
}

static int epd_gpio_get(int pin)
{
    return gpio_get_level((gpio_num_t)pin);
}

static void epd_spi_write_byte(uint8_t data)
{
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &data,
    };
    esp_err_t err = spi_device_polling_transmit(s_spi, &t);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI byte tx failed: %s", esp_err_to_name(err));
    }
}

static void epd_spi_write_buf(const uint8_t *buf, size_t len)
{
    /* SPI DMA transfer — max 32KB per transaction on some targets,
     * split into chunks if needed */
    const size_t chunk = 32 * 1024;
    size_t offset = 0;

    while (offset < len) {
        size_t remaining = len - offset;
        size_t send_len = (remaining > chunk) ? chunk : remaining;
        spi_transaction_t t = {
            .length = send_len * 8,
            .tx_buffer = buf + offset,
        };
        esp_err_t err = spi_device_transmit(s_spi, &t);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "SPI buffer tx failed: %s", esp_err_to_name(err));
            break;
        }
        offset += send_len;
    }
}

/* ───────────────────────── EPD Protocol ───────────────────────── */

static void epd_send_command(uint8_t cmd)
{
    epd_gpio_set(s_pins.pin_dc, 0);
    epd_gpio_set(s_pins.pin_cs, 0);
    epd_spi_write_byte(cmd);
    epd_gpio_set(s_pins.pin_cs, 1);
}

static void epd_send_data(uint8_t data)
{
    epd_gpio_set(s_pins.pin_dc, 1);
    epd_gpio_set(s_pins.pin_cs, 0);
    epd_spi_write_byte(data);
    epd_gpio_set(s_pins.pin_cs, 1);
}

static void epd_send_data_buf(const uint8_t *buf, size_t len)
{
    epd_gpio_set(s_pins.pin_dc, 1);
    epd_gpio_set(s_pins.pin_cs, 0);
    epd_spi_write_buf(buf, len);
    epd_gpio_set(s_pins.pin_cs, 1);
}

static bool epd_wait_busy(uint32_t timeout_ms)
{
    TickType_t start_tick = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);

    ESP_LOGD(TAG, "Waiting for BUSY...");
    while (epd_gpio_get(s_pins.pin_busy) == 0) {
        if ((xTaskGetTickCount() - start_tick) > timeout_ticks) {
            ESP_LOGE(TAG, "BUSY wait timeout after %lu ms", (unsigned long)timeout_ms);
            return false;
        }
        epd_delay_ms(10);
    }
    epd_delay_ms(200);
    ESP_LOGD(TAG, "BUSY released");
    return true;
}

static void epd_reset(void)
{
    epd_gpio_set(s_pins.pin_rst, 1);
    epd_delay_ms(20);
    epd_gpio_set(s_pins.pin_rst, 0);
    epd_delay_ms(2);
    epd_gpio_set(s_pins.pin_rst, 1);
    epd_delay_ms(20);
}

static bool epd_turn_on_display(void)
{
    epd_send_command(0x04);  /* POWER_ON */
    if (!epd_wait_busy(EPD_BUSY_TIMEOUT_MS)) {
        return false;
    }
    epd_delay_ms(200);

    /* Second setting */
    epd_send_command(0x06);
    epd_send_data(0x6F);
    epd_send_data(0x1F);
    epd_send_data(0x17);
    epd_send_data(0x27);
    epd_delay_ms(200);

    epd_send_command(0x12);  /* DISPLAY_REFRESH */
    epd_send_data(0x00);
    if (!epd_wait_busy(EPD_BUSY_TIMEOUT_MS)) {
        return false;
    }

    epd_send_command(0x02);  /* POWER_OFF */
    epd_send_data(0x00);
    if (!epd_wait_busy(EPD_BUSY_TIMEOUT_MS)) {
        return false;
    }
    epd_delay_ms(200);
    return true;
}

/* ───────────────────────── Public API ───────────────────────── */

esp_err_t epd_init(const epd_pin_config_t *pins)
{
    esp_err_t err;

    if (pins == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_pins = *pins;

    /* Configure GPIO outputs */
    gpio_config_t out_conf = {
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    out_conf.pin_bit_mask = (1ULL << s_pins.pin_cs) |
                            (1ULL << s_pins.pin_dc) |
                            (1ULL << s_pins.pin_rst);
    if (s_pins.pin_pwr >= 0) {
        out_conf.pin_bit_mask |= (1ULL << s_pins.pin_pwr);
    }
    err = gpio_config(&out_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO out config failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Configure BUSY pin as input */
    gpio_config_t in_conf = {
        .pin_bit_mask = (1ULL << s_pins.pin_busy),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&in_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO in config failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Power on */
    if (s_pins.pin_pwr >= 0) {
        epd_gpio_set(s_pins.pin_pwr, 1);
    }
    epd_gpio_set(s_pins.pin_cs, 1);

    /* Initialize SPI bus */
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = s_pins.pin_mosi,
        .miso_io_num = -1,  /* Write-only display */
        .sclk_io_num = s_pins.pin_clk,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 32 * 1024,
    };
    err = spi_bus_initialize(s_pins.spi_host, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(err));
        return err;
    }

    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = 2 * 1000 * 1000,  /* 2 MHz */
        .mode = 0,                            /* SPI Mode 0 */
        .spics_io_num = -1,                   /* CS managed manually */
        .queue_size = 4,
    };
    err = spi_bus_add_device(s_pins.spi_host, &dev_cfg, &s_spi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI add device failed: %s", esp_err_to_name(err));
        spi_bus_free(s_pins.spi_host);
        return err;
    }

    /* Run display init sequence (from official Waveshare driver) */
    epd_reset();
    if (!epd_wait_busy(EPD_BUSY_TIMEOUT_MS)) {
        spi_bus_remove_device(s_spi);
        s_spi = NULL;
        spi_bus_free(s_pins.spi_host);
        return ESP_ERR_TIMEOUT;
    }
    epd_delay_ms(30);

    epd_send_command(0xAA);  /* CMDH */
    epd_send_data(0x49);
    epd_send_data(0x55);
    epd_send_data(0x20);
    epd_send_data(0x08);
    epd_send_data(0x09);
    epd_send_data(0x18);

    epd_send_command(0x01);
    epd_send_data(0x3F);

    epd_send_command(0x00);
    epd_send_data(0x5F);
    epd_send_data(0x69);

    epd_send_command(0x05);
    epd_send_data(0x40);
    epd_send_data(0x1F);
    epd_send_data(0x1F);
    epd_send_data(0x2C);

    epd_send_command(0x08);
    epd_send_data(0x6F);
    epd_send_data(0x1F);
    epd_send_data(0x1F);
    epd_send_data(0x22);

    epd_send_command(0x06);
    epd_send_data(0x6F);
    epd_send_data(0x1F);
    epd_send_data(0x17);
    epd_send_data(0x17);

    epd_send_command(0x03);
    epd_send_data(0x00);
    epd_send_data(0x54);
    epd_send_data(0x00);
    epd_send_data(0x44);

    epd_send_command(0x60);
    epd_send_data(0x02);
    epd_send_data(0x00);

    epd_send_command(0x30);
    epd_send_data(0x08);

    epd_send_command(0x50);
    epd_send_data(0x3F);

    epd_send_command(0x61);  /* Resolution setting: 0x0190 = 400, 0x0258 = 600 */
    epd_send_data(0x01);
    epd_send_data(0x90);
    epd_send_data(0x02);
    epd_send_data(0x58);

    epd_send_command(0xE3);
    epd_send_data(0x2F);

    epd_send_command(0x84);
    epd_send_data(0x01);
    if (!epd_wait_busy(EPD_BUSY_TIMEOUT_MS)) {
        spi_bus_remove_device(s_spi);
        s_spi = NULL;
        spi_bus_free(s_pins.spi_host);
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "EPD 4in0e initialized (600x400, 6-color)");
    return ESP_OK;
}

void epd_clear(uint8_t color)
{
    uint8_t fill = (color << 4) | color;
    size_t row_bytes = EPD_WIDTH / 2;

    epd_send_command(0x10);  /* Data Start Transmission */

    /* Send row-by-row using a small stack buffer to leverage DMA */
    uint8_t row_buf[200];  /* 400/2 = 200 bytes per row */
    memset(row_buf, fill, row_bytes);

    epd_gpio_set(s_pins.pin_dc, 1);
    epd_gpio_set(s_pins.pin_cs, 0);
    for (int y = 0; y < EPD_HEIGHT; y++) {
        epd_spi_write_buf(row_buf, row_bytes);
    }
    epd_gpio_set(s_pins.pin_cs, 1);

    if (!epd_turn_on_display()) {
        ESP_LOGE(TAG, "Display refresh failed in clear");
        return;
    }
    ESP_LOGI(TAG, "Screen cleared with color 0x%X", color);
}

void epd_display(const uint8_t *buf)
{
    if (buf == NULL) {
        return;
    }

    epd_send_command(0x10);  /* Data Start Transmission */
    epd_send_data_buf(buf, EPD_BUF_SIZE);
    if (!epd_turn_on_display()) {
        ESP_LOGE(TAG, "Display refresh failed in epd_display");
    }
}

void epd_sleep(void)
{
    epd_send_command(0x07);  /* DEEP_SLEEP */
    epd_send_data(0xA5);
    ESP_LOGI(TAG, "EPD entered sleep mode");
}

void epd_deinit(void)
{
    if (s_spi != NULL) {
        spi_bus_remove_device(s_spi);
        s_spi = NULL;
    }
    spi_bus_free(s_pins.spi_host);

    if (s_pins.pin_pwr >= 0) {
        epd_gpio_set(s_pins.pin_pwr, 0);
    }
    epd_gpio_set(s_pins.pin_rst, 0);

    ESP_LOGI(TAG, "EPD deinitialized");
}
