#include <stdio.h>
#include <inttypes.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_mac.h>
#include "app_storage.h"
#include "app_event.h"
#include "app_network.h"
#include "app_mqtt.h"
#include "app_espnow.h"
#include "app_sntp.h"

#define TAG "app_main"

#define WIFI_SSID CONFIG_WIFI_SSID
#define WIFI_PASSWORD CONFIG_WIFI_PASSWORD
#define WIFI_MAX_RETRY CONFIG_WIFI_MAX_RETRY

#define MQTT_BROKER_URI CONFIG_MQTT_BROKER_URI
#define MQTT_USERNAME CONFIG_MQTT_USERNAME
#define MQTT_PASSWORD CONFIG_MQTT_PASSWORD
#define MQTT_CLIENT_ID CONFIG_MQTT_CLIENT_ID

#define ESPNOW_HEARTBEAT_TIMEOUT_S CONFIG_ESPNOW_HEARTBEAT_TIMEOUT_S
#define ESPNOW_HEARTBEAT_CHECK_S CONFIG_ESPNOW_HEARTBEAT_CHECK_S

/**
 * @brief Application Layer Event Handler
 *
 * Unifies handling of all application events from the custom event bus.
 * Note: This function executes in the context of the app_event_task.
 *
 * @param arg        User argument passed during registration (NULL here)
 * @param event_base Event base type (should be APP_EVENT_BASE)
 * @param event_id   Event ID
 * @param event_data Pointer to event data
 */
static void main_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    /* Only process APP_EVENT_BASE events, filter others */
    if (event_base != APP_EVENT_BASE)
    {
        return;
    }

    switch ((app_event_id_t)event_id)
    {
    case APP_EVENT_WIFI_STA_CONNECTED:
        ESP_LOGI(TAG, "WiFi station connected to AP");
        break;

    case APP_EVENT_WIFI_STA_DISCONNECTED:
        ESP_LOGE(TAG, "WiFi station disconnected (retries exhausted)");
        /* TODO: Trigger reboot or enter low power mode here */
        break;

    case APP_EVENT_WIFI_STA_GOT_IP:
    {
        /* event_data is esp_netif_ip_info_t pointer */
        esp_netif_ip_info_t *ip_info = (esp_netif_ip_info_t *)event_data;
        ESP_LOGI(TAG, "WiFi station got IP: " IPSTR, IP2STR(&ip_info->ip));
        app_sntp_start();
        break;
    }

    case APP_EVENT_ESPNOW_NODE_ONLINE:
    {
        app_espnow_node_online_t *evt = (app_espnow_node_online_t *)event_data;
        ESP_LOGI(TAG, "Node %u (" MACSTR ") online [%s], type=%u fw=%u",
                 evt->node.node_id, MAC2STR(evt->node.mac),
                 evt->is_new ? "new" : "reconnect",
                 evt->node.device_type, evt->node.fw_version);
        break;
    }

    case APP_EVENT_ESPNOW_NODE_OFFLINE:
    {
        app_espnow_node_offline_t *evt = (app_espnow_node_offline_t *)event_data;
        ESP_LOGW(TAG, "Node %u (" MACSTR ") offline (heartbeat timeout)",
                 evt->node.node_id, MAC2STR(evt->node.mac));
        break;
    }

    case APP_EVENT_ESPNOW_NODE_DATA:
    {
        app_espnow_node_data_t *evt = (app_espnow_node_data_t *)event_data;
        ESP_LOGI(TAG, "Node %u data: %u bytes, RSSI: %d",
                 evt->node_id, evt->data_len, evt->rssi);
        /* TODO: Process data reported by child node */
        break;
    }

    default:
        ESP_LOGW(TAG, "Unknown event ID: %" PRId32, event_id);
        break;
    }
}

void app_main(void)
{
    esp_err_t err;

    /* 1. Initialize NVS (WiFi driver needs NVS to store calibration data) */
    err = app_storage_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize storage: %s", esp_err_to_name(err));
        return;
    }

    /* 2. Initialize Custom Event Bus */
    err = app_event_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize event bus: %s", esp_err_to_name(err));
        return;
    }

    /* 3. Register Application Layer Event Handler (Listen to all APP_EVENT_BASE events) */
    err = app_event_handler_register(ESP_EVENT_ANY_ID, main_event_handler, NULL);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to register event handler: %s", esp_err_to_name(err));
        return;
    }

    app_network_config_t network_config = {
        .ssid = WIFI_SSID,
        .password = WIFI_PASSWORD,
        .max_retry = WIFI_MAX_RETRY,
    };

    /* 4. Initialize Network (WiFi STA Mode) */
    err = app_network_init(&network_config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize network: %s", esp_err_to_name(err));
        return;
    }

    app_mqtt_config_t mqtt_config = {
        .broker_uri = MQTT_BROKER_URI,
        .username = MQTT_USERNAME,
        .password = MQTT_PASSWORD,
        .client_id = MQTT_CLIENT_ID,
    };

    /* 5. Initialize MQTT Module */
    err = app_mqtt_init(&mqtt_config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize MQTT: %s", esp_err_to_name(err));
        return;
    }

    /* 6. Initialize SNTP Module */
    app_sntp_config_t sntp_config = {
        .ntp_server = CONFIG_SNTP_SERVER,
        .timezone = CONFIG_SNTP_TIMEZONE,
        .sync_interval_h = CONFIG_SNTP_SYNC_INTERVAL_H,
    };

    err = app_sntp_init(&sntp_config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize SNTP: %s", esp_err_to_name(err));
        return;
    }

    /* 7. Initialize ESP-NOW Gateway Module */
    const char *pmk_str = CONFIG_ESPNOW_PMK;
    app_espnow_config_t espnow_config = {
        .pmk = (pmk_str[0] != '\0') ? (const uint8_t *)pmk_str : NULL,
        .heartbeat_timeout_s = ESPNOW_HEARTBEAT_TIMEOUT_S,
        .heartbeat_check_s = ESPNOW_HEARTBEAT_CHECK_S,
    };

    err = app_espnow_init(&espnow_config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize ESP-NOW: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "Application started successfully");
}