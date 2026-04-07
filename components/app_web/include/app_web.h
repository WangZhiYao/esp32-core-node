#pragma once

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the web server component
 *
 * Starts the HTTP server and registers all API endpoints and static file handlers.
 * Should be called after network is ready (WiFi connected with IP).
 *
 * @return ESP_OK on success
 */
esp_err_t app_web_init(void);

/**
 * @brief Stop the web server
 *
 * @return ESP_OK on success
 */
esp_err_t app_web_stop(void);

#ifdef __cplusplus
}
#endif
