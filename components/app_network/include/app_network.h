#pragma once

#include <esp_err.h>
#include <esp_netif.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    char *ssid;         /*!< WiFi SSID */
    char *password;     /*!< WiFi Password */
    int   max_retry;    /*!< WiFi Maximum Retry Count */
} app_network_config_t;

/**
 * @brief AP Mode Configuration (optional)
 */
typedef struct
{
    char *ssid;         /*!< AP SSID (NULL to disable AP) */
    char *password;     /*!< AP Password (min 8 chars for WPA2) */
    uint8_t channel;    /*!< AP Channel (0 = auto) */
    uint8_t max_conn;   /*!< AP Max connections (default 4) */
} app_network_ap_config_t;

/**
 * @brief Initialize WiFi STA and start connection
 *
 * Dependencies:
 *  - app_storage_init() MUST be called before this (NVS initialized).
 *  - app_event_init()   MUST be called before this (Custom event loop ready).
 *
 * Connection results are notified via APP_EVENT_BASE events:
 *  - APP_EVENT_WIFI_STA_CONNECTED    Connection successful
 *  - APP_EVENT_WIFI_STA_DISCONNECTED Retries exhausted, connection failed
 *  - APP_EVENT_WIFI_STA_GOT_IP       Got IP, event_data is esp_netif_ip_info_t*
 *
 * @return ESP_OK Initialization successful, other values indicate failure
 */
esp_err_t app_network_init(const app_network_config_t *config);

/**
 * @brief Initialize WiFi AP mode alongside STA
 *
 * Must be called after app_network_init(). Enables APSTA mode.
 *
 * @return ESP_OK on success
 */
esp_err_t app_network_start_ap(const app_network_ap_config_t *config);

/**
 * @brief Stop WiFi AP mode
 *
 * Switches back to STA-only mode.
 *
 * @return ESP_OK on success
 */
esp_err_t app_network_stop_ap(void);

/**
 * @brief Deinitialize WiFi STA Network
 *
 * Stops WiFi, unregisters event handlers, releases timers, etc.
 * Can call app_network_init() again after calling this.
 *
 * @return
 *  - ESP_OK              Success
 *  - ESP_ERR_INVALID_STATE Not initialized
 */
esp_err_t app_network_deinit(void);

#ifdef __cplusplus
}
#endif