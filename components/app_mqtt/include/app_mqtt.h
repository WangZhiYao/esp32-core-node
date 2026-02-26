#pragma once

#include "esp_err.h"
#include "mqtt_client.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ───────────────────────── Constants Definition ───────────────────────── */

/** MQTT Topic Maximum Length (including '\0') */
#define APP_MQTT_TOPIC_MAX_LEN   128

/** MQTT Payload Maximum Length (including '\0') */
#define APP_MQTT_PAYLOAD_MAX_LEN 1024

/* ───────────────────────── Data Types ───────────────────────── */

/**
 * @brief MQTT Initialization Configuration
 */
typedef struct {
    const char *broker_uri; /**< Broker Address, e.g., "mqtt://host:1883", cannot be NULL */
    const char *client_id;  /**< Client ID, cannot be NULL */
    const char *username;   /**< Username, can be NULL if authentication not required */
    const char *password;   /**< Password, can be NULL if authentication not required */
} app_mqtt_config_t;

/**
 * @brief MQTT Received Data (passed to upper layer via APP_EVENT_MQTT_DATA event)
 */
typedef struct {
    int     msg_id;                            /**< Message ID */
    uint8_t qos;                               /**< QoS Level */
    uint8_t retain;                            /**< Retain Flag */
    char    topic[APP_MQTT_TOPIC_MAX_LEN];     /**< Topic (null-terminated) */
    char    payload[APP_MQTT_PAYLOAD_MAX_LEN]; /**< Payload (null-terminated) */
} app_mqtt_data_t;

/**
 * @brief MQTT Error Information (passed to upper layer via APP_EVENT_MQTT_ERROR event)
 */
typedef struct {
    esp_mqtt_error_type_t type;       /**< Error Type */
    esp_err_t             esp_err;    /**< ESP-TLS underlying error code */
    int                   sock_errno; /**< Socket error code */
} app_mqtt_error_t;

/* ───────────────────────── API ───────────────────────── */

/**
 * @brief Initialize MQTT Module
 *
 * Deep copies configuration parameters, creates mutex, registers WiFi event listeners.
 * MQTT client is not created at this time, but waits for WiFi to get IP before automatically starting.
 *
 * @param[in] config Pointer to configuration parameters, cannot be NULL
 * @return
 *  - ESP_OK              Success
 *  - ESP_ERR_INVALID_ARG Invalid argument
 *  - ESP_ERR_INVALID_STATE Already initialized
 *  - ESP_ERR_NO_MEM      Insufficient memory
 */
esp_err_t app_mqtt_init(const app_mqtt_config_t *config);

/**
 * @brief Deinitialize MQTT Module
 *
 * Stops and destroys MQTT client, unregisters event handlers, releases all resources.
 *
 * @return
 *  - ESP_OK              Success
 *  - ESP_ERR_INVALID_STATE Not initialized
 */
esp_err_t app_mqtt_deinit(void);

/**
 * @brief Query if MQTT is connected
 *
 * Thread-safe.
 *
 * @return true Connected, false Not connected or not initialized
 */
bool app_mqtt_is_connected(void);

/**
 * @brief Publish message
 *
 * Thread-safe. Performs publication while holding internal mutex to ensure consistency between connection state and operation.
 *
 * @param[in] topic   Topic string, cannot be NULL
 * @param[in] payload Payload string, cannot be NULL
 * @param[in] qos     QoS Level (0, 1, 2)
 * @param[in] retain  Retain Flag (0 or 1)
 * @return >= 0 Message ID (Returns 0 for QoS 0), < 0 Failure
 */
int app_mqtt_publish(const char *topic, const char *payload, uint8_t qos, uint8_t retain);

/**
 * @brief Subscribe to topic
 *
 * Thread-safe.
 *
 * @param[in] topic Topic string, cannot be NULL
 * @param[in] qos   QoS Level
 * @return >= 0 Message ID, < 0 Failure
 */
int app_mqtt_subscribe(const char *topic, uint8_t qos);

/**
 * @brief Unsubscribe from topic
 *
 * Thread-safe.
 *
 * @param[in] topic Topic string, cannot be NULL
 * @return >= 0 Message ID, < 0 Failure
 */
int app_mqtt_unsubscribe(const char *topic);

#ifdef __cplusplus
}
#endif