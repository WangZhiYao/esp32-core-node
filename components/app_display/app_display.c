#include "app_display.h"
#include "app_event.h"
#include "app_protocol.h"
#include "app_espnow.h"
#include "app_mqtt.h"
#include "app_weather.h"
#include "weather_icons.h"
#include "epd_4in0e.h"
#include "epd_paint.h"
#include "epd_fonts.h"

#include <esp_log.h>
#include <esp_system.h>
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <stddef.h>
#include <stdatomic.h>

static const char *TAG = "app_display";
#define DISPLAY_MUTEX_TIMEOUT_MS   50
#define DISPLAY_NODE_INDICATORS    3

static const char *DISPLAY_CMD_TOPIC   = "home/iot/display/cmd";
static const char *DISPLAY_IMAGE_TOPIC = "home/iot/display/image";

/* ───────────────────────── Display Mode ───────────────────────── */

typedef enum {
    DISPLAY_MODE_DASHBOARD = 0,
    DISPLAY_MODE_IMAGE,
} display_mode_t;

/* ───────────────────────── Sensor Data Caches ───────────────────────── */

typedef struct {
    app_protocol_env_data_t data;
    bool                    valid;
    uint8_t                 node_id;
    int                     rssi;
    uint8_t                 mac[6];
} env_cache_t;

typedef struct {
    app_protocol_iaq_data_t data;
    bool                    valid;
    uint8_t                 node_id;
    int                     rssi;
    uint8_t                 mac[6];
} iaq_cache_t;

typedef struct {
    app_protocol_presence_data_t data;
    bool                         valid;
    uint8_t                      node_id;
    int                          rssi;
    uint8_t                      mac[6];
} presence_cache_t;

/* ───────────────────────── Internal State ───────────────────────── */

static display_mode_t     s_mode = DISPLAY_MODE_DASHBOARD;
static env_cache_t        s_env_cache;
static iaq_cache_t        s_iaq_cache;
static presence_cache_t   s_presence_cache;
static bool               s_node_online[APP_ESPNOW_MAX_NODES];
static uint8_t           *s_image_buf = NULL;
static TaskHandle_t       s_display_task = NULL;
static SemaphoreHandle_t  s_mutex = NULL;
static atomic_bool        s_exit_requested = false;
static int                s_refresh_interval_s = 30;
static bool               s_presence_active = false;
static bool               s_force_refresh = false;
static bool               s_epd_active = false;
static uint8_t            s_presence_node_id = APP_ESPNOW_NODE_ID_INVALID;
static epd_pin_config_t   s_epd_pins;

typedef struct {
    env_cache_t      env;
    iaq_cache_t      iaq;
    presence_cache_t presence;
    bool             node_online[APP_ESPNOW_MAX_NODES];
} display_snapshot_t;

/* ───────────────────────── AQI Text Lookup ───────────────────────── */

static const char *aqi_to_str(uint8_t aqi)
{
    switch (aqi) {
    case 1:  return "Excellent";
    case 2:  return "Good";
    case 3:  return "Moderate";
    case 4:  return "Poor";
    case 5:  return "Unhealthy";
    default: return "Unknown";
    }
}

static void snapshot_display_data(display_snapshot_t *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(DISPLAY_MUTEX_TIMEOUT_MS)) == pdTRUE) {
        snapshot->env = s_env_cache;
        snapshot->iaq = s_iaq_cache;
        snapshot->presence = s_presence_cache;
        for (int i = 0; i < APP_ESPNOW_MAX_NODES; i++) {
            snapshot->node_online[i] = s_node_online[i];
        }
        xSemaphoreGive(s_mutex);
    } else {
        ESP_LOGW(TAG, "Snapshot lock timeout, drawing with empty cache");
    }
}

/* ───────────────────────── Dashboard Drawing ───────────────────────── */

/**
 * @brief Draw the sensor dashboard onto the framebuffer.
 *
 * The display is used in landscape orientation (ROTATE_90) giving
 * a usable area of 600 x 400 pixels.
 *
 * Layout zones:
 *   y=  0.. 29  Title bar (title + date/time)
 *   y= 31..105  Weather (icon + temp + condition + details)
 *   y=107..295  Sensor data (two columns: ENV | IAQ)
 *   y=297..370  3-day forecast (three sub-columns)
 *   y=372..399  Status bar (node indicators + heap)
 */

/* Draw "°C" — small circle (degree symbol) + letter C */
static void draw_degree_c(uint16_t x, uint16_t y, sFONT *font, uint16_t fg, uint16_t bg)
{
    int r = font->Height / 8;
    if (r < 1) r = 1;
    Paint_DrawCircle(x + r + 1, y + r + 1, r, fg, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);
    Paint_DrawString_EN(x + 2 * r + 2, y, "C", font, fg, bg);
}

static void draw_dashboard(uint8_t *buf)
{
    /* Set up paint surface in landscape orientation */
    Paint_NewImage(buf, EPD_WIDTH, EPD_HEIGHT, ROTATE_90, EPD_COLOR_WHITE);
    Paint_SetScale(7);
    Paint_Clear(EPD_COLOR_WHITE);

    char text[64];
    display_snapshot_t snapshot;
    snapshot_display_data(&snapshot);

    /* Fetch weather data (thread-safe copy) */
    app_weather_data_t weather;
    bool has_weather = (app_weather_get(&weather) == ESP_OK);

    /* ========== Title Bar (y = 0..29) ========== */
    Paint_DrawString_EN(10, 5, "DASHBOARD", &Font20, EPD_COLOR_BLACK, EPD_COLOR_WHITE);

    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    snprintf(text, sizeof(text), "%02d-%02d %02d:%02d",
             timeinfo.tm_mon + 1, timeinfo.tm_mday,
             timeinfo.tm_hour, timeinfo.tm_min);
    Paint_DrawString_EN(470, 8, text, &Font16, EPD_COLOR_BLACK, EPD_COLOR_WHITE);

    Paint_DrawLine(0, 30, 599, 30, EPD_COLOR_BLACK, DOT_PIXEL_1X1, LINE_STYLE_SOLID);

    /* ========== Weather Section (y = 31..105) ========== */
    if (has_weather) {
        /* Weather icon (48x48) at (12, 38) */
        const uint8_t *icon_bmp = weather_icon_lookup(weather.now.icon_code);
        if (icon_bmp) {
            Paint_DrawColorBitmap(12, 38, WEATHER_ICON_WIDTH, WEATHER_ICON_HEIGHT,
                                 icon_bmp, 0xF);
        }

        /* Large temperature */
        snprintf(text, sizeof(text), "%d", weather.now.temp);
        Paint_DrawString_EN(70, 36, text, &Font24, EPD_COLOR_BLACK, EPD_COLOR_WHITE);
        /* Degree symbol + C after the number */
        int temp_width = (int)strlen(text) * 17; /* Font24 width=17 */
        draw_degree_c(75 + temp_width, 36, &Font24, EPD_COLOR_BLACK, EPD_COLOR_WHITE);

        /* Condition text */
        Paint_DrawString_EN(70, 64, weather.now.text, &Font16, EPD_COLOR_BLACK, EPD_COLOR_WHITE);

        /* Details row */
        snprintf(text, sizeof(text), "Feels %d", weather.now.feels_like);
        Paint_DrawString_EN(70, 86, text, &Font12, EPD_COLOR_BLACK, EPD_COLOR_WHITE);
        draw_degree_c(75 + (int)strlen(text) * 7, 86, &Font12, EPD_COLOR_BLACK, EPD_COLOR_WHITE);

        snprintf(text, sizeof(text), "Humi %u%%", weather.now.humidity);
        Paint_DrawString_EN(200, 86, text, &Font12, EPD_COLOR_BLACK, EPD_COLOR_WHITE);

        snprintf(text, sizeof(text), "Wind %s L%u", weather.now.wind_dir, weather.now.wind_scale);
        Paint_DrawString_EN(310, 86, text, &Font12, EPD_COLOR_BLACK, EPD_COLOR_WHITE);
    } else {
        Paint_DrawString_EN(70, 55, "Weather: --", &Font20, EPD_COLOR_BLACK, EPD_COLOR_WHITE);
    }

    Paint_DrawLine(0, 106, 599, 106, EPD_COLOR_BLACK, DOT_PIXEL_1X1, LINE_STYLE_SOLID);

    /* ========== Sensor Data Section (y = 107..295) ========== */

    /* --- Column 0 (x = 0..299): ENV Data --- */
    Paint_DrawRectangle(0, 109, 299, 129, EPD_COLOR_BLUE, DOT_PIXEL_1X1, DRAW_FILL_FULL);
    Paint_DrawString_EN(10, 111, "ENVIRONMENT", &Font16, EPD_COLOR_WHITE, EPD_COLOR_BLUE);

    int y = 137;
    if (snapshot.env.valid) {
        snprintf(text, sizeof(text), "Temp  %.1f", snapshot.env.data.temperature);
        Paint_DrawString_EN(10, y, text, &Font16, EPD_COLOR_BLACK, EPD_COLOR_WHITE);
        draw_degree_c(20 + (int)strlen(text) * 11, y, &Font16, EPD_COLOR_BLACK, EPD_COLOR_WHITE);
        y += 25;

        snprintf(text, sizeof(text), "Humi  %.1f %%", snapshot.env.data.humidity);
        Paint_DrawString_EN(10, y, text, &Font16, EPD_COLOR_BLACK, EPD_COLOR_WHITE);
        y += 25;

        snprintf(text, sizeof(text), "Press %.0f hPa", snapshot.env.data.pressure);
        Paint_DrawString_EN(10, y, text, &Font16, EPD_COLOR_BLACK, EPD_COLOR_WHITE);
        y += 25;

        snprintf(text, sizeof(text), "Lux   %.0f lx", snapshot.env.data.lux);
        Paint_DrawString_EN(10, y, text, &Font16, EPD_COLOR_BLACK, EPD_COLOR_WHITE);
    } else {
        Paint_DrawString_EN(10, y, "Temp  --", &Font16, EPD_COLOR_BLACK, EPD_COLOR_WHITE);
        y += 25;
        Paint_DrawString_EN(10, y, "Humi  --", &Font16, EPD_COLOR_BLACK, EPD_COLOR_WHITE);
        y += 25;
        Paint_DrawString_EN(10, y, "Press --", &Font16, EPD_COLOR_BLACK, EPD_COLOR_WHITE);
        y += 25;
        Paint_DrawString_EN(10, y, "Lux   --", &Font16, EPD_COLOR_BLACK, EPD_COLOR_WHITE);
    }

    /* Vertical separator between columns */
    Paint_DrawLine(300, 109, 300, 295, EPD_COLOR_BLACK, DOT_PIXEL_1X1, LINE_STYLE_SOLID);

    /* --- Column 1 (x = 300..599): IAQ Data --- */
    Paint_DrawRectangle(301, 109, 599, 129, EPD_COLOR_GREEN, DOT_PIXEL_1X1, DRAW_FILL_FULL);
    Paint_DrawString_EN(310, 111, "AIR QUALITY", &Font16, EPD_COLOR_WHITE, EPD_COLOR_GREEN);

    y = 137;
    if (snapshot.iaq.valid) {
        snprintf(text, sizeof(text), "Temp  %.1f", snapshot.iaq.data.temperature);
        Paint_DrawString_EN(310, y, text, &Font16, EPD_COLOR_BLACK, EPD_COLOR_WHITE);
        draw_degree_c(320 + (int)strlen(text) * 11, y, &Font16, EPD_COLOR_BLACK, EPD_COLOR_WHITE);
        y += 25;

        snprintf(text, sizeof(text), "Humi  %.1f %%", snapshot.iaq.data.humidity);
        Paint_DrawString_EN(310, y, text, &Font16, EPD_COLOR_BLACK, EPD_COLOR_WHITE);
        y += 25;

        snprintf(text, sizeof(text), "eCO2  %u ppm", snapshot.iaq.data.eco2);
        Paint_DrawString_EN(310, y, text, &Font16, EPD_COLOR_BLACK, EPD_COLOR_WHITE);
        y += 25;

        snprintf(text, sizeof(text), "TVOC  %u ppb", snapshot.iaq.data.tvoc);
        Paint_DrawString_EN(310, y, text, &Font16, EPD_COLOR_BLACK, EPD_COLOR_WHITE);
        y += 25;

        snprintf(text, sizeof(text), "AQI   %s", aqi_to_str(snapshot.iaq.data.aqi));
        Paint_DrawString_EN(310, y, text, &Font16, EPD_COLOR_BLACK, EPD_COLOR_WHITE);
    } else {
        Paint_DrawString_EN(310, y, "Temp  --", &Font16, EPD_COLOR_BLACK, EPD_COLOR_WHITE);
        y += 25;
        Paint_DrawString_EN(310, y, "Humi  --", &Font16, EPD_COLOR_BLACK, EPD_COLOR_WHITE);
        y += 25;
        Paint_DrawString_EN(310, y, "eCO2  --", &Font16, EPD_COLOR_BLACK, EPD_COLOR_WHITE);
        y += 25;
        Paint_DrawString_EN(310, y, "TVOC  --", &Font16, EPD_COLOR_BLACK, EPD_COLOR_WHITE);
        y += 25;
        Paint_DrawString_EN(310, y, "AQI   --", &Font16, EPD_COLOR_BLACK, EPD_COLOR_WHITE);
    }

    Paint_DrawLine(0, 296, 599, 296, EPD_COLOR_BLACK, DOT_PIXEL_1X1, LINE_STYLE_SOLID);

    /* ========== 3-Day Forecast (y = 297..370) ========== */
    if (has_weather) {
        int col_w = 200; /* 600 / 3 */
        for (int i = 0; i < 3; i++) {
            int cx = i * col_w;
            const app_weather_daily_t *d = &weather.daily[i];

            /* Small icon (48x48) */
            const uint8_t *fc_icon = weather_icon_lookup(d->icon_day);
            if (fc_icon) {
                Paint_DrawColorBitmap(cx + 10, 306, WEATHER_ICON_WIDTH, WEATHER_ICON_HEIGHT,
                                     fc_icon, 0xF);
            }

            /* Date (MM-DD) — extract from "YYYY-MM-DD" */
            if (strlen(d->date) >= 10) {
                snprintf(text, sizeof(text), "%s", d->date + 5);
            } else {
                snprintf(text, sizeof(text), "--");
            }
            Paint_DrawString_EN(cx + 65, 312, text, &Font12, EPD_COLOR_BLACK, EPD_COLOR_WHITE);

            /* Condition text */
            Paint_DrawString_EN(cx + 65, 328, d->text_day, &Font12, EPD_COLOR_BLACK, EPD_COLOR_WHITE);

            /* Temp range */
            snprintf(text, sizeof(text), "%d~%d", d->temp_min, d->temp_max);
            Paint_DrawString_EN(cx + 65, 344, text, &Font12, EPD_COLOR_BLACK, EPD_COLOR_WHITE);
            draw_degree_c(cx + 70 + (int)strlen(text) * 7, 344, &Font12, EPD_COLOR_BLACK, EPD_COLOR_WHITE);

            /* Vertical separator between forecast columns */
            if (i < 2) {
                Paint_DrawLine(cx + col_w, 297, cx + col_w, 370,
                               EPD_COLOR_BLACK, DOT_PIXEL_1X1, LINE_STYLE_DOTTED);
            }
        }
    } else {
        Paint_DrawString_EN(10, 325, "Forecast: --", &Font16, EPD_COLOR_BLACK, EPD_COLOR_WHITE);
    }

    /* ========== Bottom Status Bar (y = 371..399) ========== */
    Paint_DrawLine(0, 371, 599, 371, EPD_COLOR_BLACK, DOT_PIXEL_1X1, LINE_STYLE_SOLID);

    /* Node online status indicators mapped to dashboard columns */
    const char *slot_name[DISPLAY_NODE_INDICATORS] = {"ENV", "IAQ", "PRES"};
    bool slot_valid[DISPLAY_NODE_INDICATORS] = {
        snapshot.env.valid,
        snapshot.iaq.valid,
        snapshot.presence.valid,
    };
    uint8_t slot_node_id[DISPLAY_NODE_INDICATORS] = {
        snapshot.env.node_id,
        snapshot.iaq.node_id,
        snapshot.presence.node_id,
    };

    int x_pos = 10;
    for (int i = 0; i < DISPLAY_NODE_INDICATORS; i++) {
        bool online = false;
        if (slot_valid[i] &&
            slot_node_id[i] >= 1 &&
            slot_node_id[i] <= APP_ESPNOW_MAX_NODES) {
            online = snapshot.node_online[slot_node_id[i] - 1];
        }

        if (online) {
            Paint_DrawCircle(x_pos + 5, 390, 5, EPD_COLOR_GREEN, DOT_PIXEL_1X1, DRAW_FILL_FULL);
        } else {
            Paint_DrawCircle(x_pos + 5, 390, 5, EPD_COLOR_BLACK, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);
        }

        if (slot_valid[i] &&
            slot_node_id[i] >= 1 &&
            slot_node_id[i] <= APP_ESPNOW_MAX_NODES) {
            snprintf(text, sizeof(text), "%s N%u", slot_name[i], slot_node_id[i]);
        } else {
            snprintf(text, sizeof(text), "%s --", slot_name[i]);
        }

        Paint_DrawString_EN(x_pos + 16, 384, text, &Font12, EPD_COLOR_BLACK, EPD_COLOR_WHITE);
        x_pos += 130;
    }

    /* Free heap display */
    snprintf(text, sizeof(text), "Heap:%lu", (unsigned long)esp_get_free_heap_size());
    Paint_DrawString_EN(480, 384, text, &Font12, EPD_COLOR_BLACK, EPD_COLOR_WHITE);
}

/* ───────────────────────── Display Task ───────────────────────── */

static void display_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Display task started");

    for (;;) {
        if (atomic_load(&s_exit_requested)) {
            ESP_LOGI(TAG, "Display task exiting");
            s_display_task = NULL;
            vTaskDelete(NULL);
            return;
        }

        display_mode_t mode = DISPLAY_MODE_DASHBOARD;
        int refresh_interval_s = 30;
        bool presence_active = false;
        bool force_refresh = false;
        bool epd_active = false;

        if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(DISPLAY_MUTEX_TIMEOUT_MS)) == pdTRUE) {
            mode = s_mode;
            refresh_interval_s = s_refresh_interval_s;
            presence_active = s_presence_active;
            force_refresh = s_force_refresh;
            s_force_refresh = false;
            epd_active = s_epd_active;
            xSemaphoreGive(s_mutex);
        }

        if (mode == DISPLAY_MODE_DASHBOARD) {
            if (!presence_active) {
                if (epd_active) {
                    ESP_LOGI(TAG, "No presence, clear screen and power off EPD");
                    epd_clear(EPD_COLOR_WHITE);
                    epd_sleep();
                    epd_deinit();
                    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(DISPLAY_MUTEX_TIMEOUT_MS)) == pdTRUE) {
                        s_epd_active = false;
                        xSemaphoreGive(s_mutex);
                    }
                }
                ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
                continue;
            }

            if (!epd_active) {
                ESP_LOGI(TAG, "Presence detected, power on EPD");
                if (epd_init(&s_epd_pins) != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to reinitialize EPD");
                    vTaskDelay(pdMS_TO_TICKS(2000));
                    continue;
                }
                if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(DISPLAY_MUTEX_TIMEOUT_MS)) == pdTRUE) {
                    s_epd_active = true;
                    xSemaphoreGive(s_mutex);
                }
                force_refresh = true;
            }

            if (!force_refresh) {
                /* Wait for notification or timeout at the refresh interval */
                ulTaskNotifyTake(pdTRUE,
                                 pdMS_TO_TICKS(refresh_interval_s * 1000));
            }

            if (s_image_buf == NULL) {
                ESP_LOGE(TAG, "Framebuffer not allocated");
                vTaskDelay(pdMS_TO_TICKS(5000));
                continue;
            }

            draw_dashboard(s_image_buf);
            epd_display(s_image_buf);

            ESP_LOGI(TAG, "Dashboard refreshed, free heap: %lu",
                     (unsigned long)esp_get_free_heap_size());

        } else {
            /* IMAGE mode: just wait for notification, no periodic timeout */
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        }
    }
}

/* ───────────────────────── Event Handler ───────────────────────── */

static void display_event_handler(void *arg, esp_event_base_t event_base,
                                  int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base != APP_EVENT_BASE) {
        return;
    }

    switch ((app_event_id_t)event_id) {

    case APP_EVENT_ESPNOW_NODE_DATA: {
        const app_espnow_node_data_t *evt = (const app_espnow_node_data_t *)event_data;
        bool notify_refresh = false;

        const app_protocol_data_report_t *report;
        if (app_protocol_parse_data_report(evt->data, evt->data_len, &report) != ESP_OK) {
            break;
        }

        if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(DISPLAY_MUTEX_TIMEOUT_MS)) != pdTRUE) {
            ESP_LOGW(TAG, "Mutex timeout in event handler");
            break;
        }

        if (evt->node_id >= 1 && evt->node_id <= APP_ESPNOW_MAX_NODES) {
            s_node_online[evt->node_id - 1] = true;
        }

        switch (report->sensor_type) {
        case APP_PROTOCOL_SENSOR_ENV:
            if (report->data_len >= sizeof(app_protocol_env_data_t)) {
                memcpy(&s_env_cache.data, report->data, sizeof(app_protocol_env_data_t));
                s_env_cache.valid   = true;
                s_env_cache.node_id = evt->node_id;
                s_env_cache.rssi    = evt->rssi;
                memcpy(s_env_cache.mac, evt->src_addr, 6);
            }
            break;

        case APP_PROTOCOL_SENSOR_IAQ:
            if (report->data_len >= sizeof(app_protocol_iaq_data_t)) {
                memcpy(&s_iaq_cache.data, report->data, sizeof(app_protocol_iaq_data_t));
                s_iaq_cache.valid   = true;
                s_iaq_cache.node_id = evt->node_id;
                s_iaq_cache.rssi    = evt->rssi;
                memcpy(s_iaq_cache.mac, evt->src_addr, 6);
            }
            break;

        case APP_PROTOCOL_SENSOR_PRESENCE:
            if (report->data_len >= sizeof(app_protocol_presence_data_t)) {
                const app_protocol_presence_data_t *presence =
                    (const app_protocol_presence_data_t *)report->data;
                bool is_present = (presence->target_state != 0);

                memcpy(&s_presence_cache.data, report->data,
                       sizeof(app_protocol_presence_data_t));
                s_presence_cache.valid   = true;
                s_presence_cache.node_id = evt->node_id;
                s_presence_cache.rssi    = evt->rssi;
                memcpy(s_presence_cache.mac, evt->src_addr, 6);

                s_presence_node_id = evt->node_id;
                if (s_presence_active != is_present) {
                    ESP_LOGI(TAG, "Presence state changed: %s", is_present ? "present" : "absent");
                    s_presence_active = is_present;
                    s_force_refresh = true;
                    notify_refresh = true;
                }
            }
            break;

        default:
            break;
        }

        xSemaphoreGive(s_mutex);

        if (notify_refresh && s_display_task != NULL) {
            xTaskNotifyGive(s_display_task);
        }

        break;
    }

    case APP_EVENT_MQTT_CONNECTED: {
        int msg_id = app_mqtt_subscribe(DISPLAY_CMD_TOPIC, 0);
        if (msg_id < 0) {
            ESP_LOGW(TAG, "Subscribe failed: %s", DISPLAY_CMD_TOPIC);
        } else {
            ESP_LOGI(TAG, "Subscribed %s (msg_id=%d)", DISPLAY_CMD_TOPIC, msg_id);
        }

        msg_id = app_mqtt_subscribe(DISPLAY_IMAGE_TOPIC, 0);
        if (msg_id < 0) {
            ESP_LOGW(TAG, "Subscribe failed: %s", DISPLAY_IMAGE_TOPIC);
        } else {
            ESP_LOGI(TAG, "Subscribed %s (msg_id=%d)", DISPLAY_IMAGE_TOPIC, msg_id);
        }
        break;
    }

    case APP_EVENT_ESPNOW_NODE_ONLINE: {
        const app_espnow_node_online_t *evt = (const app_espnow_node_online_t *)event_data;
        uint8_t idx = evt->node.node_id;
        bool notify = false;
        if (idx >= 1 && idx <= APP_ESPNOW_MAX_NODES) {
            if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(DISPLAY_MUTEX_TIMEOUT_MS)) == pdTRUE) {
                s_node_online[idx - 1] = true;
                notify = (s_mode == DISPLAY_MODE_DASHBOARD);
                xSemaphoreGive(s_mutex);
            }
        }
        if (notify && s_display_task != NULL) {
            xTaskNotifyGive(s_display_task);
        }
        break;
    }

    case APP_EVENT_ESPNOW_NODE_OFFLINE: {
        const app_espnow_node_offline_t *evt = (const app_espnow_node_offline_t *)event_data;
        uint8_t idx = evt->node.node_id;
        bool notify = false;
        if (idx >= 1 && idx <= APP_ESPNOW_MAX_NODES) {
            if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(DISPLAY_MUTEX_TIMEOUT_MS)) == pdTRUE) {
                s_node_online[idx - 1] = false;

                if (s_env_cache.valid && s_env_cache.node_id == idx) {
                    s_env_cache.valid = false;
                }
                if (s_iaq_cache.valid && s_iaq_cache.node_id == idx) {
                    s_iaq_cache.valid = false;
                }
                if (s_presence_cache.valid && s_presence_cache.node_id == idx) {
                    s_presence_cache.valid = false;
                }
                if (s_presence_node_id == idx) {
                    s_presence_active = false;
                }

                notify = (s_mode == DISPLAY_MODE_DASHBOARD);
                xSemaphoreGive(s_mutex);
            }
        }
        if (notify && s_display_task != NULL) {
            xTaskNotifyGive(s_display_task);
        }
        break;
    }

    case APP_EVENT_WEATHER_UPDATED: {
        /* Weather data refreshed — trigger display update */
        if (s_display_task != NULL) {
            xTaskNotifyGive(s_display_task);
        }
        break;
    }

    case APP_EVENT_MQTT_DATA: {
        const app_mqtt_data_t *mqtt = (const app_mqtt_data_t *)event_data;

        if (strcmp(mqtt->topic, DISPLAY_CMD_TOPIC) == 0) {
            /* Parse command JSON payload using cJSON */
            bool refresh_now = false;
            cJSON *root = cJSON_Parse(mqtt->payload);
            if (root == NULL) {
                ESP_LOGW(TAG, "Invalid JSON in display command");
                break;
            }

            cJSON *mode_item = cJSON_GetObjectItem(root, "mode");
            if (cJSON_IsString(mode_item) && mode_item->valuestring != NULL) {
                if (strcmp(mode_item->valuestring, "dashboard") == 0) {
                    ESP_LOGI(TAG, "Switching to DASHBOARD mode");
                    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(DISPLAY_MUTEX_TIMEOUT_MS)) == pdTRUE) {
                        s_mode = DISPLAY_MODE_DASHBOARD;
                        xSemaphoreGive(s_mutex);
                        refresh_now = true;
                    }
                } else if (strcmp(mode_item->valuestring, "image") == 0) {
                    ESP_LOGI(TAG, "Switching to IMAGE mode");
                    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(DISPLAY_MUTEX_TIMEOUT_MS)) == pdTRUE) {
                        s_mode = DISPLAY_MODE_IMAGE;
                        xSemaphoreGive(s_mutex);
                    }
                }
            }

            cJSON *refresh_item = cJSON_GetObjectItem(root, "refresh");
            if (cJSON_IsNumber(refresh_item)) {
                int val = refresh_item->valueint;
                if (val >= 30 && val <= 3600) {
                    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(DISPLAY_MUTEX_TIMEOUT_MS)) == pdTRUE) {
                        s_refresh_interval_s = val;
                        refresh_now = (s_mode == DISPLAY_MODE_DASHBOARD);
                        xSemaphoreGive(s_mutex);
                        ESP_LOGI(TAG, "Refresh interval set to %d seconds", val);
                    }
                } else {
                    ESP_LOGW(TAG, "Refresh interval out of range (30-3600): %d", val);
                }
            }

            cJSON_Delete(root);

            if (refresh_now && s_display_task != NULL) {
                xTaskNotifyGive(s_display_task);
            }
        } else if (strcmp(mqtt->topic, DISPLAY_IMAGE_TOPIC) == 0) {
            /*
             * NOTE: For now just log that an image was received.
             * The actual 120KB image transfer requires chunked MQTT
             * or a larger MQTT buffer, which the current setup does
             * not support. This will be implemented later.
             */
            ESP_LOGI(TAG, "Image data received on display/image topic "
                     "(payload len=%u, handling deferred)",
                     (unsigned)strlen(mqtt->payload));
        }
        break;
    }

    default:
        break;
    }
}

/* ───────────────────────── Public API ───────────────────────── */

esp_err_t app_display_init(const app_display_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_display_task != NULL) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    /* Create mutex */
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    /* Clear caches */
    memset(&s_env_cache, 0, sizeof(s_env_cache));
    memset(&s_iaq_cache, 0, sizeof(s_iaq_cache));
    memset(&s_presence_cache, 0, sizeof(s_presence_cache));
    memset(s_node_online, 0, sizeof(s_node_online));
    s_mode = DISPLAY_MODE_DASHBOARD;
    s_refresh_interval_s = config->refresh_interval_s > 0
                               ? config->refresh_interval_s
                               : 30;
    s_presence_active = false;
    s_force_refresh = false;
    s_epd_active = false;
    s_presence_node_id = APP_ESPNOW_NODE_ID_INVALID;

    s_image_buf = malloc(EPD_BUF_SIZE);
    if (s_image_buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate framebuffer (%d bytes)", EPD_BUF_SIZE);
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* Initialize EPD hardware */
    s_epd_pins = (epd_pin_config_t){
        .spi_host = config->spi_host,
        .pin_mosi = config->pin_mosi,
        .pin_clk  = config->pin_clk,
        .pin_cs   = config->pin_cs,
        .pin_dc   = config->pin_dc,
        .pin_rst  = config->pin_rst,
        .pin_busy = config->pin_busy,
        .pin_pwr  = config->pin_pwr,
    };

    esp_err_t ret = epd_init(&s_epd_pins);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "EPD init failed: %s", esp_err_to_name(ret));
        free(s_image_buf);
        s_image_buf = NULL;
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return ret;
    }
    s_epd_active = true;

    /* Register event handler for all app events */
    ret = app_event_handler_register(ESP_EVENT_ANY_ID, display_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register event handler: %s", esp_err_to_name(ret));
        epd_deinit();
        s_epd_active = false;
        free(s_image_buf);
        s_image_buf = NULL;
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return ret;
    }

    /* Create display task */
    BaseType_t xret = xTaskCreate(display_task, "display_task", 8192, NULL, 3,
                                  &s_display_task);
    if (xret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create display task");
        app_event_handler_unregister(ESP_EVENT_ANY_ID, display_event_handler);
        epd_deinit();
        s_epd_active = false;
        free(s_image_buf);
        s_image_buf = NULL;
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Display initialized (refresh interval: %ds)", s_refresh_interval_s);
    return ESP_OK;
}

esp_err_t app_display_stop(void)
{
    bool epd_active = false;

    if (s_display_task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Unregister event handler */
    app_event_handler_unregister(ESP_EVENT_ANY_ID, display_event_handler);

    /* Request graceful exit and wake the task */
    atomic_store(&s_exit_requested, true);
    xTaskNotifyGive(s_display_task);

    /* Poll-wait for task to self-delete (500ms intervals, 5s max) */
    for (int i = 0; i < 10 && s_display_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    if (s_display_task != NULL) {
        ESP_LOGW(TAG, "Display task did not exit gracefully, force deleting");
        vTaskDelete(s_display_task);
        s_display_task = NULL;
    }

    atomic_store(&s_exit_requested, false);

    /* Free image buffer if allocated */
    if (s_image_buf != NULL) {
        free(s_image_buf);
        s_image_buf = NULL;
    }

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(DISPLAY_MUTEX_TIMEOUT_MS)) == pdTRUE) {
        epd_active = s_epd_active;
        s_epd_active = false;
        xSemaphoreGive(s_mutex);
    }

    /* Put EPD to sleep and deinitialize if currently active */
    if (epd_active) {
        epd_sleep();
        epd_deinit();
    }

    /* Delete mutex */
    if (s_mutex != NULL) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }

    ESP_LOGI(TAG, "Display stopped");
    return ESP_OK;
}
