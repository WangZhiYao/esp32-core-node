#pragma once

#include <esp_err.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ───────────────────────── Data Types ───────────────────────── */

/**
 * @brief Current weather data
 */
typedef struct {
    int8_t   temp;           /**< Temperature in Celsius */
    int8_t   feels_like;     /**< Feels-like temperature */
    uint16_t icon_code;      /**< QWeather icon code */
    char     text[32];       /**< Weather description (English) */
    uint8_t  humidity;       /**< Relative humidity % */
    uint8_t  wind_scale;     /**< Wind scale (Beaufort) */
    char     wind_dir[16];   /**< Wind direction text */
} app_weather_now_t;

/**
 * @brief Daily forecast data
 */
typedef struct {
    char     date[12];       /**< Forecast date "YYYY-MM-DD" */
    int8_t   temp_max;       /**< Maximum temperature */
    int8_t   temp_min;       /**< Minimum temperature */
    uint16_t icon_day;       /**< Daytime icon code */
    char     text_day[32];   /**< Daytime weather text */
} app_weather_daily_t;

/**
 * @brief Combined weather data cache
 */
typedef struct {
    app_weather_now_t   now;       /**< Current weather */
    app_weather_daily_t daily[3];  /**< 3-day forecast */
    bool                valid;     /**< Data is valid */
    int64_t             timestamp; /**< Last update (epoch seconds) */
} app_weather_data_t;

/* ───────────────────────── API ───────────────────────── */

/**
 * @brief Initialize the weather component
 *
 * Creates a periodic timer to fetch weather data.
 * Fetching starts once WiFi is connected (listens for GOT_IP event).
 *
 * @return ESP_OK on success
 */
esp_err_t app_weather_init(void);

/**
 * @brief Get a snapshot of the current weather data
 *
 * Thread-safe. Copies cached data into the provided struct.
 *
 * @param[out] data Pointer to destination struct
 * @return ESP_OK if data is valid, ESP_ERR_NOT_FOUND if no data yet
 */
esp_err_t app_weather_get(app_weather_data_t *data);

#ifdef __cplusplus
}
#endif
