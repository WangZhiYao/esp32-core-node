#include "app_mqtt.h"
#include "app_event.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "mqtt_client.h"

#include <string.h>
#include <stdio.h>
#include <inttypes.h>

/* ───────────────────────── Constants & Macros ───────────────────────── */

#define TAG "app_mqtt"

/** Configuration string buffer size */
#define MQTT_URI_MAX_LEN 128
#define MQTT_USERNAME_MAX_LEN 64
#define MQTT_PASSWORD_MAX_LEN 64
#define MQTT_CLIENT_ID_MAX_LEN 64

/* ───────────────────────── Internal Data Types ───────────────────────── */

/**
 * @brief Module Configuration Copy
 *
 * Deep copy of strings passed by caller in app_mqtt_init(),
 * so module does not rely on caller pointers during its lifecycle.
 */
typedef struct
{
    char broker_uri[MQTT_URI_MAX_LEN];
    char username[MQTT_USERNAME_MAX_LEN];
    char password[MQTT_PASSWORD_MAX_LEN];
    char client_id[MQTT_CLIENT_ID_MAX_LEN];
} mqtt_config_t;

/**
 * @brief Module Runtime State
 *
 * All fields protected by s_mutex.
 * client == NULL means not created yet, is_connected indicates MQTT layer connection status.
 */
typedef struct
{
    esp_mqtt_client_handle_t client;
    bool is_connected;
} mqtt_runtime_t;

/* ───────────────────────── Module Static Variables ───────────────────────── */

/** Module initialized flag (only modified in init/deinit, no extra sync needed) */
static bool s_initialized = false;

/** Configuration copy (Read-only after init, no lock needed) */
static mqtt_config_t s_config = {0};

/** Runtime state (Protected by s_mutex) */
static mqtt_runtime_t s_runtime = {0};

/** Mutex protecting s_runtime */
static SemaphoreHandle_t s_mutex = NULL;

/* ───────────────────────── Internal Helper Functions ───────────────────────── */

/**
 * @brief Safely set connected state
 *
 * Caller might already hold lock (e.g. mqtt_client_stop_locked),
 * so providing _locked and _unlocked versions.
 */
static void set_connected_locked(bool connected)
{
    s_runtime.is_connected = connected;
}

static void set_connected(bool connected)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    set_connected_locked(connected);
    xSemaphoreGive(s_mutex);
}

/**
 * @brief Safe String Copy
 *
 * Encapsulates strncpy and ensures '\0' termination, eliminating duplicate code.
 *
 * @param[out] dst  Destination buffer
 * @param[in]  src  Source string, can be NULL (no copy in that case)
 * @param[in]  size Destination buffer size
 */
static void safe_strncpy(char *dst, const char *src, size_t size)
{
    if (src != NULL)
    {
        strncpy(dst, src, size - 1);
        dst[size - 1] = '\0';
    }
}

/* ───────────────────── MQTT Client Lifecycle Management ───────────────────── */

/**
 * @brief Create and start MQTT client (First time)
 *
 * Caller must hold s_mutex.
 *
 * @return ESP_OK Success, other values Failure
 */
static esp_err_t mqtt_client_create_and_start_locked(void);

/**
 * @brief Handle ESP-IDF Native MQTT Client Events
 *
 * Executes in MQTT internal task context (not caller task),
 * forwards events to upper layer via app_event bus.
 *
 * Note: Do not perform time-consuming operations in this callback,
 * and do not hold s_mutex for too long.
 * For CONNECTED/DISCONNECTED events, only modify is_connected flag and post event.
 */
static void on_mqtt_client_event(void *arg,
                                 esp_event_base_t event_base,
                                 int32_t event_id,
                                 void *event_data)
{
    (void)arg;
    (void)event_base;

    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id)
    {
    case MQTT_EVENT_BEFORE_CONNECT:
        ESP_LOGI(TAG, "Connecting to broker: %s", s_config.broker_uri);
        break;

    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Connected to broker");
        set_connected(true);
        app_event_post(APP_EVENT_MQTT_CONNECTED, NULL, 0);
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Disconnected from broker");
        set_connected(false);
        app_event_post(APP_EVENT_MQTT_DISCONNECTED, NULL, 0);
        break;

    case MQTT_EVENT_DATA:
    {
        /*
         * ESP-IDF MQTT client might split large messages into multiple MQTT_EVENT_DATA callbacks:
         *   - First packet: current_data_offset == 0, data_len < total_data_len
         *   - Subsequent packets: current_data_offset > 0
         *
         * Currently only handling single packet complete messages (offset==0 and data_len==total_data_len).
         * Fragmented messages are dropped with a warning.
         */
        if (event->current_data_offset != 0 ||
            event->data_len != event->total_data_len)
        {
            ESP_LOGW(TAG,
                     "Fragmented message dropped: topic=%.*s "
                     "offset=%d data_len=%d total=%d",
                     event->topic_len, event->topic,
                     event->current_data_offset,
                     event->data_len,
                     event->total_data_len);
            break;
        }

        /* Check length to prevent buffer overflow */
        if (event->topic_len >= APP_MQTT_TOPIC_MAX_LEN)
        {
            ESP_LOGW(TAG, "Topic too long (%d >= %d), dropped",
                     event->topic_len, APP_MQTT_TOPIC_MAX_LEN);
            break;
        }
        if (event->data_len >= APP_MQTT_PAYLOAD_MAX_LEN)
        {
            ESP_LOGW(TAG, "Payload too long (%d >= %d), dropped",
                     event->data_len, APP_MQTT_PAYLOAD_MAX_LEN);
            break;
        }

        /*
         * Construct application layer data structure.
         * Using {0} initialization ensures all fields (including string end) are zero,
         * memcpy only copies effective length, '\0' termination guaranteed by initialization.
         */
        app_mqtt_data_t mqtt_data = {0};
        mqtt_data.msg_id = event->msg_id;
        mqtt_data.qos = (uint8_t)event->qos;
        mqtt_data.retain = (uint8_t)event->retain;

        memcpy(mqtt_data.topic, event->topic, event->topic_len);
        memcpy(mqtt_data.payload, event->data, event->data_len);

        ESP_LOGD(TAG, "Received [%s]: %.*s",
                 mqtt_data.topic, event->data_len, mqtt_data.payload);
        app_event_post(APP_EVENT_MQTT_DATA, &mqtt_data, sizeof(app_mqtt_data_t));
        break;
    }

    case MQTT_EVENT_PUBLISHED:
        ESP_LOGD(TAG, "Message published, msg_id=%d", event->msg_id);
        app_event_post(APP_EVENT_MQTT_PUBLISHED,
                       &event->msg_id, sizeof(event->msg_id));
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGD(TAG, "Subscribed, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGD(TAG, "Unsubscribed, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_ERROR:
    {
        /*
         * Extract key error info and report via app event bus,
         * Upper layer can use this for UI display or diagnostics.
         */
        app_mqtt_error_t err_data = {
            .type = event->error_handle->error_type,
            .esp_err = event->error_handle->esp_tls_last_esp_err,
            .sock_errno = event->error_handle->esp_transport_sock_errno,
        };
        ESP_LOGE(TAG, "MQTT error: type=%d esp_err=0x%x sock_errno=%d",
                 err_data.type, err_data.esp_err, err_data.sock_errno);
        app_event_post(APP_EVENT_MQTT_ERROR, &err_data, sizeof(app_mqtt_error_t));
        break;
    }

    default:
        ESP_LOGD(TAG, "Unhandled MQTT event id: %" PRId32, event_id);
        break;
    }
}

/**
 * @brief Create and start MQTT client (Caller must hold s_mutex)
 */
static esp_err_t mqtt_client_create_and_start_locked(void)
{
    /*
     * Construct ESP-IDF MQTT configuration.
     * s_config is read-only after init, safe to reference its internal buffer here,
     * esp_mqtt_client_init() will copy required fields internally.
     */
    const esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = s_config.broker_uri,
        .credentials.username = (s_config.username[0] != '\0') ? s_config.username : NULL,
        .credentials.authentication.password = (s_config.password[0] != '\0') ? s_config.password : NULL,
        .credentials.client_id = s_config.client_id,
    };

    s_runtime.client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_runtime.client == NULL)
    {
        ESP_LOGE(TAG, "esp_mqtt_client_init failed");
        return ESP_FAIL;
    }

    /* Register unified callback for all MQTT events */
    esp_err_t err = esp_mqtt_client_register_event(
        s_runtime.client, MQTT_EVENT_ANY, on_mqtt_client_event, NULL);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Register MQTT event handler failed: %s", esp_err_to_name(err));
        goto cleanup;
    }

    err = esp_mqtt_client_start(s_runtime.client);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_mqtt_client_start failed: %s", esp_err_to_name(err));
        goto cleanup;
    }

    ESP_LOGI(TAG, "MQTT client created and started");
    return ESP_OK;

cleanup:
    esp_mqtt_client_destroy(s_runtime.client);
    s_runtime.client = NULL;
    return err;
}

/**
 * @brief Start or Resume MQTT Client Connection
 *
 * Called after WiFi gets IP:
 *  - First time: Create client and start
 *  - Subsequent times (After WiFi reconnect): Call esp_mqtt_client_start to resume
 *
 * Note: Cannot use esp_mqtt_client_reconnect after esp_mqtt_client_stop,
 * must use esp_mqtt_client_start to restart.
 */
static void mqtt_client_start(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (s_runtime.client != NULL)
    {
        /* Existing client instance, means it was stopped before, restart now */
        ESP_LOGI(TAG, "Restarting existing MQTT client");
        esp_err_t err = esp_mqtt_client_start(s_runtime.client);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to restart MQTT client: %s", esp_err_to_name(err));
            /*
             * Restart failed, destroy old instance, will recreate next time WiFi recovers.
             * This avoids leaving an unusable client instance residue.
             */
            esp_mqtt_client_destroy(s_runtime.client);
            s_runtime.client = NULL;
            set_connected_locked(false);
        }
    }
    else
    {
        /* Create for the first time */
        esp_err_t err = mqtt_client_create_and_start_locked();
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to create MQTT client");
        }
    }

    xSemaphoreGive(s_mutex);
}

/**
 * @brief Stop MQTT Client
 *
 * Called when WiFi completely disconnected (retries exhausted),
 * avoids MQTT layer doing invalid reconnects consuming resources when no network.
 *
 * Note: Only stop, do not destroy, so it can quickly restart after WiFi recovers.
 */
static void mqtt_client_stop(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (s_runtime.client != NULL)
    {
        ESP_LOGW(TAG, "Stopping MQTT client (WiFi lost)");
        esp_mqtt_client_stop(s_runtime.client);
        set_connected_locked(false);
    }

    xSemaphoreGive(s_mutex);
}

/* ───────────────────── Application Event Handling ───────────────────── */

/**
 * @brief Respond to WiFi state changes from App Event Bus
 *
 * Executes in app_event_task context:
 *  - GOT_IP:       WiFi ready, start MQTT
 *  - DISCONNECTED: WiFi completely disconnected (retries exhausted), stop MQTT
 *
 * Note: WIFI_STA_CONNECTED (L2 association) does not mean network available, do not start MQTT here.
 */
static void on_app_wifi_event(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base != APP_EVENT_BASE)
    {
        return;
    }

    switch ((app_event_id_t)event_id)
    {
    case APP_EVENT_WIFI_STA_GOT_IP:
        ESP_LOGI(TAG, "WiFi ready (got IP), starting MQTT");
        mqtt_client_start();
        break;

    case APP_EVENT_WIFI_STA_DISCONNECTED:
        ESP_LOGW(TAG, "WiFi lost, stopping MQTT");
        mqtt_client_stop();
        break;

    default:
        break;
    }
}

/* ───────────────────── Public API ───────────────────── */

esp_err_t app_mqtt_init(const app_mqtt_config_t *config)
{
    /* ── Parameter Validation ── */

    if (config == NULL)
    {
        ESP_LOGE(TAG, "NULL config");
        return ESP_ERR_INVALID_ARG;
    }

    if (config->broker_uri == NULL || strlen(config->broker_uri) == 0)
    {
        ESP_LOGE(TAG, "broker_uri is required");
        return ESP_ERR_INVALID_ARG;
    }

    if (config->client_id == NULL || strlen(config->client_id) == 0)
    {
        ESP_LOGE(TAG, "client_id is required");
        return ESP_ERR_INVALID_ARG;
    }

    /* URI Length Check */
    if (strlen(config->broker_uri) >= MQTT_URI_MAX_LEN)
    {
        ESP_LOGE(TAG, "broker_uri too long (max %d)", MQTT_URI_MAX_LEN - 1);
        return ESP_ERR_INVALID_ARG;
    }

    if (strlen(config->client_id) >= MQTT_CLIENT_ID_MAX_LEN)
    {
        ESP_LOGE(TAG, "client_id too long (max %d)", MQTT_CLIENT_ID_MAX_LEN - 1);
        return ESP_ERR_INVALID_ARG;
    }

    if (s_initialized)
    {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    /* ── Deep Copy Configuration Strings ── */

    memset(&s_config, 0, sizeof(s_config));
    safe_strncpy(s_config.broker_uri, config->broker_uri, sizeof(s_config.broker_uri));
    safe_strncpy(s_config.client_id, config->client_id, sizeof(s_config.client_id));
    safe_strncpy(s_config.username, config->username, sizeof(s_config.username));
    safe_strncpy(s_config.password, config->password, sizeof(s_config.password));

    /* ── Create Mutex ── */

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL)
    {
        ESP_LOGE(TAG, "Failed to create mutex");
        memset(&s_config, 0, sizeof(s_config));
        return ESP_ERR_NO_MEM;
    }

    /* ── Register App Event Listeners ──
     * Wait for WiFi to get IP before automatically starting MQTT client.
     */
    esp_err_t err = app_event_handler_register(ESP_EVENT_ANY_ID,
                                               on_app_wifi_event, NULL);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to register app event handler: %s", esp_err_to_name(err));
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        memset(&s_config, 0, sizeof(s_config));
        return err;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "MQTT module initialized (broker=%s, client_id=%s)",
             s_config.broker_uri, s_config.client_id);
    return ESP_OK;
}

esp_err_t app_mqtt_deinit(void)
{
    if (!s_initialized)
    {
        ESP_LOGW(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Deinitializing MQTT module...");

    /* Unregister app event listeners (prevent receiving new WiFi events during deinit) */
    app_event_handler_unregister(ESP_EVENT_ANY_ID, on_app_wifi_event);

    /*
     * Hold lock to destroy client.
     * Use portMAX_DELAY to wait for lock, ensuring no conflict with ongoing
     * publish/subscribe operations.
     */
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (s_runtime.client != NULL)
    {
        esp_mqtt_client_stop(s_runtime.client);
        esp_mqtt_client_unregister_event(
            s_runtime.client, MQTT_EVENT_ANY, on_mqtt_client_event);
        esp_mqtt_client_destroy(s_runtime.client);
        s_runtime.client = NULL;
    }
    s_runtime.is_connected = false;

    xSemaphoreGive(s_mutex);

    /* Release Mutex */
    vSemaphoreDelete(s_mutex);
    s_mutex = NULL;

    /* Clear sensitive info (passwords etc) */
    memset(&s_config, 0, sizeof(s_config));
    memset(&s_runtime, 0, sizeof(s_runtime));

    s_initialized = false;
    ESP_LOGI(TAG, "MQTT module deinitialized");
    return ESP_OK;
}

bool app_mqtt_is_connected(void)
{
    if (!s_initialized || s_mutex == NULL)
    {
        return false;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool connected = s_runtime.is_connected;
    xSemaphoreGive(s_mutex);

    return connected;
}

int app_mqtt_publish(const char *topic, const char *payload, uint8_t qos, uint8_t retain)
{
    if (topic == NULL || payload == NULL)
    {
        ESP_LOGE(TAG, "publish: topic or payload is NULL");
        return -1;
    }

    if (!s_initialized)
    {
        ESP_LOGW(TAG, "publish: module not initialized");
        return -1;
    }

    int msg_id = -1;

    /*
     * Hold lock for "Check Connection + Publish", ensuring atomicity.
     * Prevents connection loss between is_connected() check and publish() execution.
     */
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (!s_runtime.is_connected || s_runtime.client == NULL)
    {
        ESP_LOGW(TAG, "publish: MQTT not connected");
        xSemaphoreGive(s_mutex);
        return -1;
    }

    msg_id = esp_mqtt_client_publish(
        s_runtime.client, topic, payload, (int)strlen(payload), qos, retain);

    xSemaphoreGive(s_mutex);

    if (msg_id < 0)
    {
        ESP_LOGE(TAG, "Publish failed: topic=%s", topic);
    }
    else
    {
        ESP_LOGD(TAG, "Published [%s] msg_id=%d", topic, msg_id);
    }

    return msg_id;
}

int app_mqtt_subscribe(const char *topic, uint8_t qos)
{
    if (topic == NULL)
    {
        ESP_LOGE(TAG, "subscribe: topic is NULL");
        return -1;
    }

    if (!s_initialized)
    {
        ESP_LOGW(TAG, "subscribe: module not initialized");
        return -1;
    }

    int msg_id = -1;

    /* Hold lock to ensure "Check Connection + Subscribe" atomicity */
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (!s_runtime.is_connected || s_runtime.client == NULL)
    {
        ESP_LOGW(TAG, "subscribe: MQTT not connected");
        xSemaphoreGive(s_mutex);
        return -1;
    }

    msg_id = esp_mqtt_client_subscribe(s_runtime.client, topic, qos);

    xSemaphoreGive(s_mutex);

    if (msg_id < 0)
    {
        ESP_LOGE(TAG, "Subscribe failed: topic=%s", topic);
    }
    else
    {
        ESP_LOGI(TAG, "Subscribed [%s] qos=%d msg_id=%d", topic, qos, msg_id);
    }

    return msg_id;
}

int app_mqtt_unsubscribe(const char *topic)
{
    if (topic == NULL)
    {
        ESP_LOGE(TAG, "unsubscribe: topic is NULL");
        return -1;
    }

    if (!s_initialized)
    {
        ESP_LOGW(TAG, "unsubscribe: module not initialized");
        return -1;
    }

    int msg_id = -1;

    /* Hold lock to ensure "Check Connection + Unsubscribe" atomicity */
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (!s_runtime.is_connected || s_runtime.client == NULL)
    {
        ESP_LOGW(TAG, "unsubscribe: MQTT not connected");
        xSemaphoreGive(s_mutex);
        return -1;
    }

    msg_id = esp_mqtt_client_unsubscribe(s_runtime.client, topic);

    xSemaphoreGive(s_mutex);

    if (msg_id < 0)
    {
        ESP_LOGE(TAG, "Unsubscribe failed: topic=%s", topic);
    }
    else
    {
        ESP_LOGI(TAG, "Unsubscribed [%s] msg_id=%d", topic, msg_id);
    }

    return msg_id;
}