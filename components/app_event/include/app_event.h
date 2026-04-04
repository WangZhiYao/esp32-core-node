#pragma once

#include <esp_event.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Logging tag for the custom event loop */
#define APP_EVENT_TAG "app_event"

/** Declare the custom event base */
ESP_EVENT_DECLARE_BASE(APP_EVENT_BASE);

/**
 * @brief Application Layer Event ID Enumeration
 */
typedef enum
{
    /* ── WiFi Events ── */
    APP_EVENT_WIFI_STA_CONNECTED,    /*!< WiFi STA connected to AP */
    APP_EVENT_WIFI_STA_DISCONNECTED, /*!< WiFi STA disconnected and retries exhausted */
    APP_EVENT_WIFI_STA_GOT_IP,       /*!< WiFi STA got IP, event_data: esp_netif_ip_info_t* */
    /* ── MQTT Events ── */
    APP_EVENT_MQTT_CONNECTED,    /*!< Connected to MQTT Broker */
    APP_EVENT_MQTT_DISCONNECTED, /*!< Disconnected from MQTT Broker */
    APP_EVENT_MQTT_DATA,         /*!< Received MQTT message, event_data: app_mqtt_data_t* */
    APP_EVENT_MQTT_PUBLISHED,    /*!< Message published, event_data: int*(msg_id) */
    APP_EVENT_MQTT_ERROR,        /*!< MQTT error occurred, event_data: app_mqtt_error_t* */
    /* ── ESP-NOW Events ── */
    APP_EVENT_ESPNOW_NODE_ONLINE,  /*!< Child node online (new or reconnect), event_data: app_espnow_node_online_t* */
    APP_EVENT_ESPNOW_NODE_OFFLINE, /*!< Child node offline (heartbeat timeout), event_data: app_espnow_node_offline_t* */
    APP_EVENT_ESPNOW_NODE_DATA,    /*!< Child node data report, event_data: app_espnow_node_data_t* */
    /* ── SNTP Events ── */
    APP_EVENT_SNTP_RESYNC,         /*!< Periodic SNTP re-sync request (no event_data) */
} app_event_id_t;

/**
 * @brief Initialize the custom event loop
 * @return ESP_OK on success, other values indicate failure
 */
esp_err_t app_event_init(void);

/**
 * @brief Post an event to the custom event loop
 * @param event_id        Event ID
 * @param event_data      Pointer to event data (can be NULL), data is copied internally
 * @param event_data_size Size of event data (bytes)
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if event loop is not initialized
 */
esp_err_t app_event_post(app_event_id_t event_id, void *event_data, size_t event_data_size);

/**
 * @brief Post an event to the custom event loop with timeout
 * @param event_id        Event ID
 * @param event_data      Pointer to event data (can be NULL), data is copied internally
 * @param event_data_size Size of event data (bytes)
 * @param timeout         Timeout to wait for the event queue to be available
 * @return ESP_OK on success, ESP_ERR_TIMEOUT if queue is full, ESP_ERR_INVALID_STATE if event loop is not initialized
 */
esp_err_t app_event_post_with_timeout(app_event_id_t event_id, void *event_data, size_t event_data_size, TickType_t timeout);

/**
 * @brief Register an event handler to the specified event loop (for APP_EVENT_BASE)
 *
 * This function is a wrapper for esp_event_handler_register_with, fixing APP_EVENT_BASE as the event base.
 *
 * @param event_id          Event ID to listen for (app_event_id_t). Use ESP_EVENT_ANY_ID to listen to all events.
 * @param event_handler     Event handling callback function.
 * @param event_handler_arg User custom argument passed to the callback function (can be NULL).
 * @return
 *      - ESP_OK: Registration successful
 *      - ESP_ERR_INVALID_STATE: If event_loop is NULL and app_event is not initialized
 *      - ESP_ERR_INVALID_ARG: Invalid arguments (e.g., event_handler is NULL)
 *      - Other error codes from esp_event_handler_register_with
 */
esp_err_t app_event_handler_register(int32_t event_id, esp_event_handler_t event_handler, void *event_handler_arg);

/**
 * @brief Unregister an event handler from the specified event loop (for APP_EVENT_BASE)
 *
 * This function is a wrapper for esp_event_handler_unregister_with, fixing APP_EVENT_BASE as the event base.
 *
 * @param event_id          Event ID used during registration (app_event_id_t or ESP_EVENT_ANY_ID).
 * @param event_handler     Event handling callback function registered previously.
 * @return
 *      - ESP_OK: Unregistration successful
 *      - ESP_ERR_INVALID_STATE: If event_loop is NULL and app_event is not initialized
 *      - ESP_ERR_INVALID_ARG: Invalid arguments
 *      - Other error codes from esp_event_handler_unregister_with
 */
esp_err_t app_event_handler_unregister(int32_t event_id, esp_event_handler_t event_handler);

#ifdef __cplusplus
}
#endif