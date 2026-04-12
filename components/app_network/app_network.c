#include "app_network.h"
#include "app_event.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>

/* ───────────────────────── Constants Definition ───────────────────────── */

#define TAG "app_network"

/** Initial reconnect delay (ms), increases exponentially afterwards */
#define RETRY_BASE_DELAY_MS 1000

/** Maximum reconnect delay limit (ms) */
#define RETRY_MAX_DELAY_MS 30000

/** IEEE 802.11 SSID Maximum Length (bytes) */
#define WIFI_SSID_MAX_LEN 32

/** WPA2 Password Minimum Length */
#define WPA2_PASSWORD_MIN_LEN 8

/* ───────────────────────── Module Static Variables ───────────────────────── */

/** Module initialized flag */
static bool s_initialized = false;

/**
 * Current retry count (Atomic).
 * Read/Written in two different task contexts: WiFi Event Callback (Event Loop Task)
 * and Timer Callback (Timer Service Task), requiring thread safety.
 */
static atomic_int s_retry_num = 0;

/** WiFi Maximum Retry Count; 0 = Infinite Retry */
static uint8_t s_max_retry_count = 5;

/** WiFi Reconnect Timer Handle */
static TimerHandle_t s_retry_timer = NULL;

/**
 * Flag indicating if a connect is in progress (prevents duplicate connect triggers).
 * Accessed in two task contexts, using atomic variable for thread safety.
 */
static atomic_bool s_connect_in_progress = false;

/** WiFi Event Handler Registration Handle, used for unregistering during deinit */
static esp_event_handler_instance_t s_wifi_handler_instance = NULL;

/** IP Event Handler Registration Handle, used for unregistering during deinit */
static esp_event_handler_instance_t s_ip_handler_instance = NULL;

/** Default STA netif pointer, used for destruction during deinit */
static esp_netif_t *s_sta_netif = NULL;

/** Default AP netif pointer */
static esp_netif_t *s_ap_netif = NULL;

/** AP mode active flag */
static bool s_ap_active = false;

/* ───────────────────── Internal Helper Functions ───────────────────── */

/**
 * @brief Calculate backoff delay for the retry_num-th retry
 *
 * Uses exponential backoff strategy: delay = base * 2^n, capped at RETRY_MAX_DELAY_MS.
 * E.g., base=1000ms: 1s -> 2s -> 4s -> 8s -> 16s -> 30s(capped)
 *
 * @param retry_num Current retry index (starting from 0)
 * @return Delay in FreeRTOS ticks
 */
static TickType_t get_retry_delay_ticks(int retry_num)
{
    /* Limit shift count to prevent overflow */
    int shift = (retry_num < 5) ? retry_num : 4;
    uint32_t delay_ms = RETRY_BASE_DELAY_MS << shift;

    if (delay_ms > RETRY_MAX_DELAY_MS)
    {
        delay_ms = RETRY_MAX_DELAY_MS;
    }

    return pdMS_TO_TICKS(delay_ms);
}

/**
 * @brief Determine if disconnection reason is non-retriable
 *
 * The following reasons usually imply incorrect credentials or AP rejection,
 * retrying won't succeed:
 *  - AUTH_FAIL: Authentication failed (wrong password etc.)
 *  - 4WAY_HANDSHAKE_TIMEOUT: 4-way handshake timeout (password mismatch)
 *  - HANDSHAKE_TIMEOUT: Handshake timeout
 *  - MIC_FAILURE: Message Integrity Check failure (another form of wrong password)
 *
 * @param reason WiFi disconnect reason code
 * @return true means should not retry, false means can retry
 */
static bool is_non_retriable_reason(wifi_err_reason_t reason)
{
    switch (reason)
    {
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
        return true;
    default:
        return false;
    }
}

/**
 * @brief Schedule reconnect (asynchronously via one-shot timer)
 *
 * Checks if retry count is exhausted (when max_retry_count > 0).
 * If not exhausted, starts/resets timer. calling esp_wifi_connect() in callback upon expiration.
 */
static void schedule_reconnect(void)
{
    int current_retry = atomic_load(&s_retry_num);

    /* max_retry_count == 0 means infinite retry, never give up */
    if (s_max_retry_count > 0 && current_retry >= s_max_retry_count)
    {
        ESP_LOGE(TAG, "Max retry count reached (%d), giving up", s_max_retry_count);
        app_event_post_with_timeout(APP_EVENT_WIFI_STA_DISCONNECTED, NULL, 0, pdMS_TO_TICKS(100));
        return;
    }

    int next_retry = atomic_fetch_add(&s_retry_num, 1) + 1;
    TickType_t delay = get_retry_delay_ticks(next_retry - 1);

    if (s_max_retry_count > 0)
    {
        ESP_LOGI(TAG, "Reconnect attempt %d/%d, delay %lu ms",
                 next_retry, s_max_retry_count,
                 (unsigned long)(delay * portTICK_PERIOD_MS));
    }
    else
    {
        ESP_LOGI(TAG, "Reconnect attempt %d (unlimited), delay %lu ms",
                 next_retry,
                 (unsigned long)(delay * portTICK_PERIOD_MS));
    }

    if (s_retry_timer == NULL)
    {
        ESP_LOGE(TAG, "Retry timer is NULL, cannot schedule reconnect");
        return;
    }

    /*
     * Change timer period (implementing exponential backoff) and reset/start.
     * xTimerChangePeriod starts the timer, so no extra reset needed.
     */
    BaseType_t ok = xTimerChangePeriod(s_retry_timer, delay, 0);
    if (ok != pdPASS)
    {
        ESP_LOGE(TAG, "xTimerChangePeriod failed");
    }
}

/* ───────────────────── Timer Callback ───────────────────── */

/**
 * @brief Reconnect Timer Callback
 *
 * Executes in FreeRTOS Timer Service Task context.
 * Note: If esp_wifi_connect() fails synchronously, do not recursively call schedule_reconnect().
 * Instead, restart the timer to avoid stack overflow from infinite recursion in timer callback.
 *
 * @param xTimer Timer Handle
 */
static void retry_timer_callback(TimerHandle_t xTimer)
{
    /* If previous connect result not received yet, skip this one */
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_connect_in_progress, &expected, true))
    {
        ESP_LOGW(TAG, "Reconnect skipped: previous connect still in progress");
        return;
    }

    ESP_LOGI(TAG, "Attempting to reconnect...");

    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK)
    {
        atomic_store(&s_connect_in_progress, false);
        ESP_LOGE(TAG, "esp_wifi_connect() failed: %s", esp_err_to_name(err));

        /*
         * Do not call schedule_reconnect() to avoid recursion.
         * Check retry count then restart timer directly.
         */
        int current_retry = atomic_load(&s_retry_num);
        if (s_max_retry_count > 0 && current_retry >= s_max_retry_count)
        {
            ESP_LOGE(TAG, "Max retry count reached (%d), giving up", s_max_retry_count);
            app_event_post_with_timeout(APP_EVENT_WIFI_STA_DISCONNECTED, NULL, 0, pdMS_TO_TICKS(100));
        }
        else
        {
            /* Increment retry count and restart timer (with new backoff period) */
            int next = atomic_fetch_add(&s_retry_num, 1) + 1;
            TickType_t delay = get_retry_delay_ticks(next - 1);
            ESP_LOGI(TAG, "Will retry again (attempt %d) in %lu ms",
                     next, (unsigned long)(delay * portTICK_PERIOD_MS));
            xTimerChangePeriod(xTimer, delay, 0);
        }
    }
    else
    {
        ESP_LOGI(TAG, "esp_wifi_connect() initiated, waiting for result...");
    }
}

/* ───────────────────── Event Handlers ───────────────────── */

/**
 * @brief WiFi Event Handler
 *
 * Handles following events:
 *  - STA_START:        WiFi STA started, initiate first connection
 *  - STA_CONNECTED:    L2 association successful, reset retry counter
 *  - STA_DISCONNECTED: Disconnected, decide whether to reconnect based on reason
 */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;

    switch (event_id)
    {
    case WIFI_EVENT_STA_START:
    {
        ESP_LOGI(TAG, "WiFi STA started, initiating first connection...");
        atomic_store(&s_retry_num, 0);
        atomic_store(&s_connect_in_progress, true);

        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK)
        {
            atomic_store(&s_connect_in_progress, false);
            ESP_LOGE(TAG, "Initial esp_wifi_connect() failed: %s", esp_err_to_name(err));
            schedule_reconnect();
        }
        break;
    }

    case WIFI_EVENT_STA_CONNECTED:
    {
        /*
         * L2 (802.11) association successful. No IP address yet.
         * Usable network connection waits for IP_EVENT_STA_GOT_IP.
         * Reset reconnect state and notify upper layer "Associated".
         */
        ESP_LOGI(TAG, "Associated with AP (L2 connected)");
        atomic_store(&s_connect_in_progress, false);
        atomic_store(&s_retry_num, 0);

        /* If timer is still active (e.g. timer reset before connect success), stop it */
        if (s_retry_timer != NULL && xTimerIsTimerActive(s_retry_timer))
        {
            xTimerStop(s_retry_timer, 0);
        }

        app_event_post_with_timeout(APP_EVENT_WIFI_STA_CONNECTED, NULL, 0, pdMS_TO_TICKS(100));
        break;
    }

    case WIFI_EVENT_STA_DISCONNECTED:
    {
        atomic_store(&s_connect_in_progress, false);

        wifi_event_sta_disconnected_t *disconn =
            (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "Disconnected from AP, reason: %d", disconn->reason);

        /* For auth failures, retry won't succeed, give up immediately */
        if (is_non_retriable_reason(disconn->reason))
        {
            ESP_LOGE(TAG, "Non-retriable reason (%d), will not retry", disconn->reason);
            app_event_post_with_timeout(APP_EVENT_WIFI_STA_DISCONNECTED, NULL, 0, pdMS_TO_TICKS(100));
            break;
        }

        /* Retriable reason, schedule asynchronous reconnect */
        schedule_reconnect();
        break;
    }

    default:
        break;
    }
}

/**
 * @brief IP Event Handler
 *
 * When STA gets IP address, publish GOT_IP event to upper layer,
 * carrying full IP info (ip, netmask, gw).
 */
static void ip_event_handler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;

    if (event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));

        app_event_post_with_timeout(APP_EVENT_WIFI_STA_GOT_IP,
                       &event->ip_info,
                       sizeof(esp_netif_ip_info_t),
                       pdMS_TO_TICKS(100));
    }
}

/* ───────────────────── Public API ───────────────────── */

esp_err_t app_network_init(const app_network_config_t *config)
{
    /* ── Parameter Validation ── */

    if (config == NULL)
    {
        ESP_LOGE(TAG, "NULL config");
        return ESP_ERR_INVALID_ARG;
    }

    if (s_initialized)
    {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    /* SSID cannot be empty */
    if (config->ssid == NULL || strlen(config->ssid) == 0)
    {
        ESP_LOGE(TAG, "SSID is required");
        return ESP_ERR_INVALID_ARG;
    }

    /* SSID length cannot exceed IEEE 802.11 limit of 32 bytes */
    if (strlen(config->ssid) >= WIFI_SSID_MAX_LEN)
    {
        ESP_LOGE(TAG, "SSID too long (max %d bytes)", WIFI_SSID_MAX_LEN - 1);
        return ESP_ERR_INVALID_ARG;
    }

    /* WPA2 Password at least 8 chars */
    bool has_password = (config->password != NULL && strlen(config->password) > 0);
    if (has_password && strlen(config->password) < WPA2_PASSWORD_MIN_LEN)
    {
        ESP_LOGE(TAG, "Password too short for WPA2 (min %d chars)", WPA2_PASSWORD_MIN_LEN);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err;

    /* ── Step 1: Initialize TCP/IP Stack ── */

    err = esp_netif_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* ── Step 2: Create Default System Event Loop ──
     * WiFi and IP events rely on this loop.
     * ESP_ERR_INVALID_STATE means already created, which is normal.
     */
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(err));
        return err;
    }

    /* ── Step 3: Create Reconnect Timer ──
     * One-shot timer (pdFALSE), manually restart after trigger.
     * Initial period uses base delay, subsequent uses xTimerChangePeriod for exponential backoff.
     */
    s_retry_timer = xTimerCreate("wifi_retry",
                                 pdMS_TO_TICKS(RETRY_BASE_DELAY_MS),
                                 pdFALSE, /* One-shot mode */
                                 NULL,
                                 retry_timer_callback);
    if (s_retry_timer == NULL)
    {
        ESP_LOGE(TAG, "Failed to create retry timer");
        return ESP_FAIL;
    }

    /* ── Step 4: Create Default STA Network Interface ── */
    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_sta_netif == NULL)
    {
        ESP_LOGE(TAG, "Failed to create default STA netif");
        err = ESP_FAIL;
        goto cleanup_timer;
    }

    /* ── Step 5: Initialize WiFi Driver ── */
    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wifi_init_cfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        goto cleanup_netif;
    }

    /* ── Step 6: Register Event Handlers ──
     * Save instance handle for unregistering during deinit.
     */
    err = esp_event_handler_instance_register(WIFI_EVENT,
                                              ESP_EVENT_ANY_ID,
                                              &wifi_event_handler,
                                              NULL,
                                              &s_wifi_handler_instance);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Register WiFi handler failed: %s", esp_err_to_name(err));
        goto cleanup_wifi;
    }

    err = esp_event_handler_instance_register(IP_EVENT,
                                              IP_EVENT_STA_GOT_IP,
                                              &ip_event_handler,
                                              NULL,
                                              &s_ip_handler_instance);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Register IP handler failed: %s", esp_err_to_name(err));
        goto cleanup_wifi_handler;
    }

    /* ── Step 7: Configure WiFi STA Parameters ── */
    wifi_config_t wifi_config;
    memset(&wifi_config, 0, sizeof(wifi_config));

    /* Copy SSID */
    strncpy((char *)wifi_config.sta.ssid,
            config->ssid,
            sizeof(wifi_config.sta.ssid) - 1);

    /* Copy Password (if any) */
    if (has_password)
    {
        strncpy((char *)wifi_config.sta.password,
                config->password,
                sizeof(wifi_config.sta.password) - 1);
    }

    /*
     * Automatically select auth mode based on password presence:
     *  - With password: Min WPA2-PSK
     *  - Without password: Allow OPEN
     */
    wifi_config.sta.threshold.authmode =
        has_password ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    /* Enable PMF (Protected Management Frames) for enhanced security */
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    /* Save max retry count */
    s_max_retry_count = config->max_retry;

    /* ── Step 8: Apply Config and Start WiFi ── */

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(err));
        goto cleanup_ip_handler;
    }

    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(err));
        goto cleanup_ip_handler;
    }

    /*
     * Success of esp_wifi_start() triggers WIFI_EVENT_STA_START,
     * which automatically calls esp_wifi_connect() in event handler.
     */
    err = esp_wifi_start();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        goto cleanup_ip_handler;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "WiFi STA initialization complete (SSID: %s, max_retry: %s)",
             config->ssid,
             (s_max_retry_count > 0) ? "limited" : "unlimited");

    return ESP_OK;

    /* ── Error Rollback (Release resources in reverse order) ── */

cleanup_ip_handler:
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                          s_ip_handler_instance);
    s_ip_handler_instance = NULL;

cleanup_wifi_handler:
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                          s_wifi_handler_instance);
    s_wifi_handler_instance = NULL;

cleanup_wifi:
    esp_wifi_deinit();

cleanup_netif:
    esp_netif_destroy_default_wifi(s_sta_netif);
    s_sta_netif = NULL;

cleanup_timer:
    xTimerDelete(s_retry_timer, portMAX_DELAY);
    s_retry_timer = NULL;

    return err;
}

esp_err_t app_network_start_ap(const app_network_ap_config_t *config)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Network not initialized, call app_network_init() first");
        return ESP_ERR_INVALID_STATE;
    }

    if (config == NULL || config->ssid == NULL || strlen(config->ssid) == 0) {
        ESP_LOGE(TAG, "AP SSID is required");
        return ESP_ERR_INVALID_ARG;
    }

    /* Reject password that is provided but too short for WPA2 */
    if (config->password && strlen(config->password) > 0 &&
        strlen(config->password) < WPA2_PASSWORD_MIN_LEN) {
        ESP_LOGE(TAG, "AP password too short for WPA2 (min %d chars), "
                 "use NULL or empty string for open AP", WPA2_PASSWORD_MIN_LEN);
        return ESP_ERR_INVALID_ARG;
    }

    if (s_ap_active) {
        ESP_LOGW(TAG, "AP already active");
        return ESP_OK;
    }

    /* Create AP netif */
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (s_ap_netif == NULL) {
        ESP_LOGE(TAG, "Failed to create AP netif");
        return ESP_FAIL;
    }

    /* Switch to APSTA mode */
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set APSTA mode: %s", esp_err_to_name(err));
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = NULL;
        return err;
    }

    /* Configure AP */
    wifi_config_t ap_config;
    memset(&ap_config, 0, sizeof(ap_config));
    strncpy((char *)ap_config.ap.ssid, config->ssid, sizeof(ap_config.ap.ssid) - 1);
    ap_config.ap.ssid_len = strlen(config->ssid);
    /*
     * Channel 0 = follow STA home channel (official doc: in APSTA mode
     * the SoftAP channel is forced to match the STA channel).
     * Only set an explicit channel when the caller really knows it.
     */
    ap_config.ap.channel = config->channel;
    ap_config.ap.max_connection = config->max_conn > 0 ? config->max_conn : 4;

    if (config->password && strlen(config->password) >= WPA2_PASSWORD_MIN_LEN) {
        strncpy((char *)ap_config.ap.password, config->password, sizeof(ap_config.ap.password) - 1);
        ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else if (config->password == NULL || strlen(config->password) == 0) {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }
    ap_config.ap.pmf_cfg.required = false;

    err = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set AP config: %s", esp_err_to_name(err));
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = NULL;
        return err;
    }

    s_ap_active = true;
    ESP_LOGI(TAG, "AP started (SSID: %s)", config->ssid);
    return ESP_OK;
}

esp_err_t app_network_stop_ap(void)
{
    if (!s_ap_active) {
        return ESP_OK;
    }

    esp_wifi_set_mode(WIFI_MODE_STA);

    if (s_ap_netif) {
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = NULL;
    }

    s_ap_active = false;
    ESP_LOGI(TAG, "AP stopped");
    return ESP_OK;
}

esp_err_t app_network_deinit(void)
{
    if (!s_initialized)
    {
        ESP_LOGW(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Deinitializing WiFi STA...");

    /* Stop Reconnect Timer (if running) */
    if (s_retry_timer != NULL)
    {
        xTimerStop(s_retry_timer, portMAX_DELAY);
        xTimerDelete(s_retry_timer, portMAX_DELAY);
        s_retry_timer = NULL;
    }

    /* Stop and deinit WiFi Driver */
    esp_wifi_stop();
    esp_wifi_deinit();

    /* Unregister Event Handlers */
    if (s_wifi_handler_instance != NULL)
    {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              s_wifi_handler_instance);
        s_wifi_handler_instance = NULL;
    }

    if (s_ip_handler_instance != NULL)
    {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              s_ip_handler_instance);
        s_ip_handler_instance = NULL;
    }

    /* Destroy Network Interface */
    if (s_sta_netif != NULL)
    {
        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = NULL;
    }

    /* Reset State Variables */
    atomic_store(&s_retry_num, 0);
    atomic_store(&s_connect_in_progress, false);
    s_max_retry_count = 5;
    s_ap_active = false;

    /* Destroy AP Network Interface if active */
    if (s_ap_netif != NULL)
    {
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = NULL;
    }

    s_initialized = false;
    ESP_LOGI(TAG, "WiFi STA deinitialized");

    return ESP_OK;
}