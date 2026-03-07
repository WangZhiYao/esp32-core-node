#include "app_sntp.h"

#include <string.h>
#include <time.h>
#include <sys/time.h>

#include <esp_log.h>
#include <esp_sntp.h>
#include <freertos/FreeRTOS.h>
#include <freertos/timers.h>

#define TAG "app_sntp"

/* ───────────────────────── Internal State ───────────────────────── */

static char           s_ntp_server[64];
static char           s_timezone[32];
static int            s_sync_interval_h;
static TimerHandle_t  s_resync_timer;
static bool           s_initialized;
static bool           s_started;

/* ───────────────────────── Internal Functions ───────────────────── */

static void sntp_sync_notification_cb(struct timeval *tv)
{
    time_t now = tv->tv_sec;
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    ESP_LOGI(TAG, "Time synchronized: %s", buf);
}

static void resync_timer_cb(TimerHandle_t timer)
{
    (void)timer;
    ESP_LOGI(TAG, "Periodic re-sync triggered");
    esp_sntp_restart();
}

/* ───────────────────────── Public API ────────────────────────────── */

esp_err_t app_sntp_init(const app_sntp_config_t *config)
{
    if (config == NULL || config->ntp_server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    strlcpy(s_ntp_server, config->ntp_server, sizeof(s_ntp_server));
    strlcpy(s_timezone,
            (config->timezone && config->timezone[0]) ? config->timezone : "UTC0",
            sizeof(s_timezone));
    s_sync_interval_h = config->sync_interval_h;
    if (s_sync_interval_h < 1) {
        s_sync_interval_h = 1;
    }

    /* Set timezone */
    setenv("TZ", s_timezone, 1);
    tzset();

    s_initialized = true;
    ESP_LOGI(TAG, "Initialized (server=%s, interval=%dh)", s_ntp_server, s_sync_interval_h);
    return ESP_OK;
}

esp_err_t app_sntp_start(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_started) {
        ESP_LOGW(TAG, "Already started");
        return ESP_OK;
    }

    /* Configure and start SNTP */
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, s_ntp_server);
    sntp_set_time_sync_notification_cb(sntp_sync_notification_cb);
    esp_sntp_init();

    /* Create periodic re-sync timer.
     * Cap at 24h to prevent pdMS_TO_TICKS overflow on high tick-rate configs. */
    int capped_h = (s_sync_interval_h > 24) ? 24 : s_sync_interval_h;
    TickType_t period = pdMS_TO_TICKS((uint32_t)capped_h * 3600U * 1000U);
    s_resync_timer = xTimerCreate("sntp_resync", period, pdTRUE, NULL, resync_timer_cb);
    if (s_resync_timer != NULL) {
        xTimerStart(s_resync_timer, 0);
    } else {
        ESP_LOGW(TAG, "Failed to create re-sync timer");
    }

    s_started = true;
    ESP_LOGI(TAG, "SNTP sync started");
    return ESP_OK;
}

esp_err_t app_sntp_stop(void)
{
    if (!s_initialized || !s_started) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_resync_timer != NULL) {
        xTimerStop(s_resync_timer, 0);
        xTimerDelete(s_resync_timer, 0);
        s_resync_timer = NULL;
    }

    esp_sntp_stop();
    s_started = false;
    ESP_LOGI(TAG, "SNTP sync stopped");
    return ESP_OK;
}
