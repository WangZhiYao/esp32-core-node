#include "app_weather.h"
#include "app_event.h"

#include <string.h>
#include <sys/time.h>

#include <esp_log.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <esp_timer.h>
#include <cJSON.h>
#include <miniz.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <stdatomic.h>

#define TAG "app_weather"

/* ─── Kconfig ─── */
#define WEATHER_API_KEY      CONFIG_WEATHER_API_KEY
#define WEATHER_LOCATION     CONFIG_WEATHER_LOCATION
#define WEATHER_API_HOST     CONFIG_WEATHER_API_HOST
#define WEATHER_REFRESH_MIN  CONFIG_WEATHER_REFRESH_MIN

/* ─── Constants ─── */
#define HTTP_BUF_SIZE        4096
#define URL_BUF_SIZE         256
#define AUTH_BUF_SIZE        64

/* ─── Internal State ─── */
static app_weather_data_t s_cache;
static SemaphoreHandle_t  s_mutex;
static esp_timer_handle_t s_timer;
static TaskHandle_t       s_task;
static atomic_bool        s_wifi_ready;

/* ─── Gzip decompression ─── */

/**
 * @brief Try to decompress gzip data. Returns decompressed buffer on success
 *        (caller must free), NULL if data is not gzip or decompression fails.
 */
static char *try_gzip_decompress(const char *data, int data_len, int *out_len)
{
    /* Check gzip magic bytes */
    if (data_len < 18 || (uint8_t)data[0] != 0x1F || (uint8_t)data[1] != 0x8B) {
        return NULL; /* Not gzip */
    }
    if ((uint8_t)data[2] != 8) {
        ESP_LOGW(TAG, "Gzip: unsupported method %u", (uint8_t)data[2]);
        return NULL;
    }

    /* Read original size from last 4 bytes (little-endian, mod 2^32) */
    uint32_t orig_size = (uint8_t)data[data_len - 4] |
                         ((uint8_t)data[data_len - 3] << 8) |
                         ((uint8_t)data[data_len - 2] << 16) |
                         ((uint8_t)data[data_len - 1] << 24);

    if (orig_size == 0 || orig_size > HTTP_BUF_SIZE * 4) {
        ESP_LOGW(TAG, "Gzip: unreasonable original size %lu", (unsigned long)orig_size);
        return NULL;
    }

    /* Skip gzip header: fixed 10 bytes + optional fields */
    int pos = 10;
    uint8_t flags = (uint8_t)data[3];

    if (flags & 0x04) { /* FEXTRA */
        if (pos + 2 > data_len) return NULL;
        int xlen = (uint8_t)data[pos] | ((uint8_t)data[pos + 1] << 8);
        pos += 2 + xlen;
    }
    if (flags & 0x08) { /* FNAME */
        while (pos < data_len && data[pos]) pos++;
        pos++;
    }
    if (flags & 0x10) { /* FCOMMENT */
        while (pos < data_len && data[pos]) pos++;
        pos++;
    }
    if (flags & 0x02) { /* FHCRC */
        pos += 2;
    }

    /* Deflate data: from pos to (data_len - 8), last 8 bytes are CRC32+ISIZE */
    int deflate_len = data_len - pos - 8;
    if (deflate_len <= 0 || pos >= data_len) {
        ESP_LOGW(TAG, "Gzip: invalid header");
        return NULL;
    }

    char *out = heap_caps_malloc(orig_size + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out) out = malloc(orig_size + 1);
    if (!out) {
        ESP_LOGE(TAG, "Gzip: alloc failed for %lu bytes", (unsigned long)orig_size);
        return NULL;
    }

    /* Heap-allocate tinfl_decompressor (~11KB) to avoid stack overflow */
    tinfl_decompressor *decomp = malloc(sizeof(tinfl_decompressor));
    if (!decomp) {
        ESP_LOGE(TAG, "Gzip: alloc failed for decompressor");
        free(out);
        return NULL;
    }
    tinfl_init(decomp);

    size_t in_bytes = (size_t)deflate_len;
    size_t out_bytes = (size_t)orig_size;

    /* Single-shot decompress: raw deflate (not zlib), full output buffer available */
    tinfl_status status = tinfl_decompress(decomp,
        (const mz_uint8 *)(data + pos), &in_bytes,
        (mz_uint8 *)out, (mz_uint8 *)out, &out_bytes,
        TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    free(decomp);

    if (status != TINFL_STATUS_DONE) {
        ESP_LOGE(TAG, "Gzip: decompression failed (status %d)", status);
        free(out);
        return NULL;
    }

    out[out_bytes] = '\0';
    *out_len = (int)out_bytes;
    ESP_LOGI(TAG, "Gzip: %d -> %d bytes", data_len, (int)out_bytes);
    return out;
}

/* ─── HTTP helpers ─── */

typedef struct {
    char  *buf;
    int    len;
    int    cap;
} http_recv_ctx_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_recv_ctx_t *ctx = (http_recv_ctx_t *)evt->user_data;
    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (ctx->len + evt->data_len < ctx->cap) {
            memcpy(ctx->buf + ctx->len, evt->data, evt->data_len);
            ctx->len += evt->data_len;
            ctx->buf[ctx->len] = '\0';
        } else {
            ESP_LOGW(TAG, "HTTP response too large, truncating");
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

/**
 * @brief Perform a GET request and return the parsed cJSON root.
 *        Caller must call cJSON_Delete() on the returned object.
 */
static cJSON *weather_http_get(const char *path)
{
    char url[URL_BUF_SIZE];
    snprintf(url, sizeof(url), "https://%s%slocation=%s&lang=en",
             WEATHER_API_HOST, path, WEATHER_LOCATION);

    char auth[AUTH_BUF_SIZE];
    snprintf(auth, sizeof(auth), "%s", WEATHER_API_KEY);

    char *recv_buf = heap_caps_malloc(HTTP_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!recv_buf) {
        recv_buf = malloc(HTTP_BUF_SIZE);
    }
    if (!recv_buf) {
        ESP_LOGE(TAG, "Failed to allocate HTTP buffer");
        return NULL;
    }

    http_recv_ctx_t recv_ctx = { .buf = recv_buf, .len = 0, .cap = HTTP_BUF_SIZE };

    esp_http_client_config_t config = {
        .url            = url,
        .method         = HTTP_METHOD_GET,
        .event_handler  = http_event_handler,
        .user_data      = &recv_ctx,
        .timeout_ms     = 10000,
        .buffer_size    = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(recv_buf);
        return NULL;
    }

    esp_http_client_set_header(client, "X-QW-Api-Key", auth);

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        free(recv_buf);
        return NULL;
    }

    ESP_LOGI(TAG, "HTTP %d, recv %d bytes for %s", status, recv_ctx.len, path);

    if (status != 200) {
        ESP_LOGW(TAG, "HTTP status %d, body: %.128s", status, recv_buf);
        free(recv_buf);
        return NULL;
    }

    /* Try gzip decompression */
    char *json_str = recv_buf;
    bool json_allocated = false;
    int decomp_len = 0;
    char *decompressed = try_gzip_decompress(recv_buf, recv_ctx.len, &decomp_len);
    if (decompressed) {
        json_str = decompressed;
        json_allocated = true;
    } else {
        ESP_LOGD(TAG, "Response (first 128): %.128s", recv_buf);
    }

    cJSON *root = cJSON_Parse(json_str);
    if (json_allocated) free(json_str);
    free(recv_buf);

    if (!root) {
        ESP_LOGE(TAG, "JSON parse error (recv %d bytes, gzip=%s)",
                 recv_ctx.len, json_allocated ? "yes" : "no");
        return NULL;
    }

    /* QWeather returns "code":"200" on success */
    cJSON *code = cJSON_GetObjectItem(root, "code");
    if (!code || !cJSON_IsString(code) || strcmp(code->valuestring, "200") != 0) {
        ESP_LOGW(TAG, "API error code: %s",
                 (code && cJSON_IsString(code)) ? code->valuestring : "missing");
        cJSON_Delete(root);
        return NULL;
    }

    return root;
}

/* ─── Parsers ─── */

static void parse_now(cJSON *root, app_weather_now_t *out)
{
    cJSON *now = cJSON_GetObjectItem(root, "now");
    if (!now) return;

    cJSON *item;
    if ((item = cJSON_GetObjectItem(now, "temp")) && cJSON_IsString(item))
        out->temp = (int8_t)atoi(item->valuestring);
    if ((item = cJSON_GetObjectItem(now, "feelsLike")) && cJSON_IsString(item))
        out->feels_like = (int8_t)atoi(item->valuestring);
    if ((item = cJSON_GetObjectItem(now, "icon")) && cJSON_IsString(item))
        out->icon_code = (uint16_t)atoi(item->valuestring);
    if ((item = cJSON_GetObjectItem(now, "text")) && cJSON_IsString(item))
        strlcpy(out->text, item->valuestring, sizeof(out->text));
    if ((item = cJSON_GetObjectItem(now, "humidity")) && cJSON_IsString(item))
        out->humidity = (uint8_t)atoi(item->valuestring);
    if ((item = cJSON_GetObjectItem(now, "windScale")) && cJSON_IsString(item))
        out->wind_scale = (uint8_t)atoi(item->valuestring);
    if ((item = cJSON_GetObjectItem(now, "windDir")) && cJSON_IsString(item))
        strlcpy(out->wind_dir, item->valuestring, sizeof(out->wind_dir));
}

static void parse_daily(cJSON *root, app_weather_daily_t *out, int max_days)
{
    cJSON *daily = cJSON_GetObjectItem(root, "daily");
    if (!daily || !cJSON_IsArray(daily)) return;

    int count = cJSON_GetArraySize(daily);
    if (count > max_days) count = max_days;

    for (int i = 0; i < count; i++) {
        cJSON *day = cJSON_GetArrayItem(daily, i);
        if (!day) continue;

        cJSON *item;
        if ((item = cJSON_GetObjectItem(day, "fxDate")) && cJSON_IsString(item))
            strlcpy(out[i].date, item->valuestring, sizeof(out[i].date));
        if ((item = cJSON_GetObjectItem(day, "tempMax")) && cJSON_IsString(item))
            out[i].temp_max = (int8_t)atoi(item->valuestring);
        if ((item = cJSON_GetObjectItem(day, "tempMin")) && cJSON_IsString(item))
            out[i].temp_min = (int8_t)atoi(item->valuestring);
        if ((item = cJSON_GetObjectItem(day, "iconDay")) && cJSON_IsString(item))
            out[i].icon_day = (uint16_t)atoi(item->valuestring);
        if ((item = cJSON_GetObjectItem(day, "textDay")) && cJSON_IsString(item))
            strlcpy(out[i].text_day, item->valuestring, sizeof(out[i].text_day));
    }
}

/* ─── Fetch ─── */

static void weather_fetch(void)
{
    if (!s_wifi_ready) return;

    ESP_LOGI(TAG, "Fetching weather data...");

    app_weather_data_t temp;
    memset(&temp, 0, sizeof(temp));

    /* 1. Current weather */
    cJSON *now_root = weather_http_get("/v7/weather/now?");
    if (now_root) {
        parse_now(now_root, &temp.now);
        cJSON_Delete(now_root);
    } else {
        ESP_LOGW(TAG, "Failed to fetch current weather");
        return;
    }

    /* 2. 3-day forecast */
    cJSON *daily_root = weather_http_get("/v7/weather/3d?");
    if (daily_root) {
        parse_daily(daily_root, temp.daily, 3);
        cJSON_Delete(daily_root);
    } else {
        ESP_LOGW(TAG, "Failed to fetch forecast");
        return;
    }

    /* Get current epoch time */
    struct timeval tv;
    gettimeofday(&tv, NULL);
    temp.timestamp = tv.tv_sec;
    temp.valid = true;

    /* Update cache under lock */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(&s_cache, &temp, sizeof(s_cache));
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Weather updated: %s %d°C (feels %d°C), humidity %u%%",
             temp.now.text, temp.now.temp, temp.now.feels_like, temp.now.humidity);

    app_event_post_with_timeout(APP_EVENT_WEATHER_UPDATED, NULL, 0, pdMS_TO_TICKS(100));

    /* After first successful fetch, switch to periodic timer */
    esp_timer_stop(s_timer);
    esp_timer_start_periodic(s_timer, (uint64_t)WEATHER_REFRESH_MIN * 60ULL * 1000000ULL);
}

/* ─── Weather task ─── */

static void weather_task(void *arg)
{
    (void)arg;

    for (;;) {
        /* Block until notified by timer or event handler */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        weather_fetch();
    }
}

/* ─── Timer callback ─── */

static void weather_timer_cb(void *arg)
{
    (void)arg;
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

/* ─── Event handler ─── */

static void weather_event_handler(void *arg, esp_event_base_t base,
                                  int32_t event_id, void *data)
{
    if (base != APP_EVENT_BASE) return;

    if (event_id == APP_EVENT_WIFI_STA_GOT_IP) {
        ESP_LOGI(TAG, "WiFi connected, scheduling initial fetch");
        s_wifi_ready = true;
        /* Fetch after a short delay so network stack is fully ready */
        esp_timer_start_once(s_timer, 3 * 1000 * 1000); /* 3 seconds */
    } else if (event_id == APP_EVENT_WIFI_STA_DISCONNECTED) {
        s_wifi_ready = false;
    }
}

/* ─── Public API ─── */

esp_err_t app_weather_init(void)
{
    if (strlen(WEATHER_API_KEY) == 0) {
        ESP_LOGW(TAG, "No API key configured, weather disabled");
        return ESP_ERR_INVALID_STATE;
    }
    if (strlen(WEATHER_LOCATION) == 0) {
        ESP_LOGW(TAG, "No location configured, weather disabled");
        return ESP_ERR_INVALID_STATE;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;

    memset(&s_cache, 0, sizeof(s_cache));

    /* Create a one-shot timer (will be restarted as periodic after first fetch) */
    esp_timer_create_args_t timer_args = {
        .callback = weather_timer_cb,
        .name     = "weather",
    };
    esp_err_t err = esp_timer_create(&timer_args, &s_timer);
    if (err != ESP_OK) return err;

    /* Create weather fetch task (needs large stack for HTTP + TLS + gzip) */
    BaseType_t xret = xTaskCreate(weather_task, "weather", 12288, NULL, 4, &s_task);
    if (xret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create weather task");
        esp_timer_delete(s_timer);
        s_timer = NULL;
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* Listen for WiFi events on the app event bus */
    err = app_event_handler_register(ESP_EVENT_ANY_ID, weather_event_handler, NULL);
    if (err != ESP_OK) {
        vTaskDelete(s_task);
        s_task = NULL;
        esp_timer_delete(s_timer);
        s_timer = NULL;
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return err;
    }

    ESP_LOGI(TAG, "Initialized (location=%s, refresh=%dmin)", WEATHER_LOCATION, WEATHER_REFRESH_MIN);
    return ESP_OK;
}

esp_err_t app_weather_get(app_weather_data_t *data)
{
    if (!data) return ESP_ERR_INVALID_ARG;
    if (!s_mutex) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool valid = s_cache.valid;
    if (valid) {
        memcpy(data, &s_cache, sizeof(*data));
    }
    xSemaphoreGive(s_mutex);

    return valid ? ESP_OK : ESP_ERR_NOT_FOUND;
}
