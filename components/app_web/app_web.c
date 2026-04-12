#include "app_web.h"

#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>

#include <esp_log.h>
#include <esp_http_server.h>
#include <esp_system.h>
#include <esp_chip_info.h>
#include <esp_mac.h>
#include <esp_wifi.h>
#include <esp_timer.h>
#include <cJSON.h>

#include "app_event.h"
#include "app_espnow.h"
#include "app_protocol.h"
#include "app_weather.h"
#include "app_mqtt.h"
#include "app_storage.h"

#define TAG "app_web"

/* ───────────────────────── NVS Namespaces & Keys ───────────────────────── */

#define NVS_NS_WEB    "web_cfg"
#define NVS_NS_WIFI   "web_wifi"
#define NVS_NS_MQTT   "web_mqtt"
#define NVS_NS_ESPNOW "web_espnow"
#define NVS_NS_SNTP   "web_sntp"
#define NVS_NS_WEATHER "web_weather"
#define NVS_NS_DISPLAY "web_display"

#define NVS_KEY_DATA  "data"
#define NVS_KEY_USER  "username"
#define NVS_KEY_PASS  "password"

/* Max length for NVS config blobs */
#define NVS_CONFIG_MAX_LEN 512

/* ───────────────────────── Embedded Frontend ───────────────────────── */

extern const uint8_t index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[]   asm("_binary_index_html_gz_end");
extern const uint8_t style_css_gz_start[]  asm("_binary_style_css_gz_start");
extern const uint8_t style_css_gz_end[]    asm("_binary_style_css_gz_end");
extern const uint8_t app_js_gz_start[]     asm("_binary_app_js_gz_start");
extern const uint8_t app_js_gz_end[]       asm("_binary_app_js_gz_end");
extern const uint8_t weathericons_regular_webfont_ttf_start[] asm("_binary_weathericons_regular_webfont_ttf_start");
extern const uint8_t weathericons_regular_webfont_ttf_end[]   asm("_binary_weathericons_regular_webfont_ttf_end");

/* ───────────────────────── Sensor Data Cache ───────────────────────── */

typedef struct {
    uint8_t  sensor_type; /* app_protocol_sensor_type_t, 0 = no data */
    int64_t  timestamp;   /* esp_timer_get_time() when received */
    union {
        app_protocol_env_data_t      env;
        app_protocol_iaq_data_t      iaq;
        app_protocol_presence_data_t presence;
    };
} sensor_cache_entry_t;

static sensor_cache_entry_t s_sensor_cache[APP_ESPNOW_MAX_NODES]; /* indexed by node_id-1 */

/* ───────────────────────── State ───────────────────────── */

static httpd_handle_t s_server = NULL;
static int64_t s_start_time_us = 0;

/* ───────────────────────── Helpers ───────────────────────── */

static esp_err_t web_send_json(httpd_req_t *req, const char *json_str)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_sendstr(req, json_str);
}

static esp_err_t web_send_error(httpd_req_t *req, int status, const char *message)
{
    httpd_resp_set_status(req, status == 400 ? "400 Bad Request" :
                               status == 401 ? "401 Unauthorized" :
                               status == 404 ? "404 Not Found" :
                               status == 503 ? "503 Service Unavailable" :
                               "500 Internal Server Error");
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", message);
    return web_send_json(req, buf);
}

static esp_err_t web_send_ok(httpd_req_t *req, cJSON *root)
{
    char *json = cJSON_PrintUnformatted(root);
    if (!json) {
        cJSON_Delete(root);
        return web_send_error(req, 500, "json serialization failed");
    }
    esp_err_t ret = web_send_json(req, json);
    free(json);
    cJSON_Delete(root);
    return ret;
}

/**
 * @brief Read full POST body from request
 * @return allocated buffer (caller must free), NULL on failure
 */
static char *web_read_body(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 4096) {
        return NULL;
    }
    char *buf = malloc(total + 1);
    if (!buf) return NULL;

    int received = 0;
    while (received < total) {
        int ret = httpd_req_recv(req, buf + received, total - received);
        if (ret <= 0) {
            free(buf);
            return NULL;
        }
        received += ret;
    }
    buf[total] = '\0';
    return buf;
}

/* ───────────────────────── Basic Auth ───────────────────────── */

static bool web_check_auth(httpd_req_t *req)
{
    char auth_header[256] = {0};
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth_header, sizeof(auth_header)) != ESP_OK) {
        return false;
    }

    /* Expect "Basic <base64>" */
    if (strncmp(auth_header, "Basic ", 6) != 0) {
        return false;
    }

    const char *b64 = auth_header + 6;

    /* Load credentials: NVS first, then Kconfig defaults */
    char username[64] = CONFIG_WEB_ADMIN_USERNAME;
    char password[64] = CONFIG_WEB_ADMIN_PASSWORD;

    size_t len = sizeof(username);
    if (app_storage_get_blob(NVS_NS_WEB, NVS_KEY_USER, username, &len) == ESP_OK) {
        username[len] = '\0';
    }
    len = sizeof(password);
    if (app_storage_get_blob(NVS_NS_WEB, NVS_KEY_PASS, password, &len) == ESP_OK) {
        password[len] = '\0';
    }

    /* Build expected "user:pass" and base64 encode */
    char credentials[130];
    snprintf(credentials, sizeof(credentials), "%s:%s", username, password);

    /* Simple base64 encode */
    static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t cred_len = strlen(credentials);
    size_t encoded_len = 4 * ((cred_len + 2) / 3);
    char *expected = malloc(encoded_len + 1);
    if (!expected) return false;

    size_t i, j = 0;
    for (i = 0; i < cred_len; i += 3) {
        uint32_t octet_a = (uint8_t)credentials[i];
        uint32_t octet_b = (i + 1 < cred_len) ? (uint8_t)credentials[i + 1] : 0;
        uint32_t octet_c = (i + 2 < cred_len) ? (uint8_t)credentials[i + 2] : 0;
        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;

        expected[j++] = b64_table[(triple >> 18) & 0x3F];
        expected[j++] = b64_table[(triple >> 12) & 0x3F];
        expected[j++] = (i + 1 < cred_len) ? b64_table[(triple >> 6) & 0x3F] : '=';
        expected[j++] = (i + 2 < cred_len) ? b64_table[triple & 0x3F] : '=';
    }
    expected[j] = '\0';

    bool match = (strcmp(b64, expected) == 0);
    free(expected);
    return match;
}

static esp_err_t web_require_auth(httpd_req_t *req)
{
    if (web_check_auth(req)) {
        return ESP_OK;
    }
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ESP32 Gateway\"");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return web_send_error(req, 401, "unauthorized");
}

/* ───────────────────────── CORS ───────────────────────── */

static esp_err_t handle_options(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type, Authorization");
    httpd_resp_set_hdr(req, "Access-Control-Max-Age", "86400");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* ───────────────────────── Static File Handlers ───────────────────────── */

static esp_err_t handle_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, (const char *)index_html_gz_start,
                           index_html_gz_end - index_html_gz_start);
}

static esp_err_t handle_style_css(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/css");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=3600");
    return httpd_resp_send(req, (const char *)style_css_gz_start,
                           style_css_gz_end - style_css_gz_start);
}

static esp_err_t handle_app_js(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=3600");
    return httpd_resp_send(req, (const char *)app_js_gz_start,
                           app_js_gz_end - app_js_gz_start);
}

static esp_err_t handle_font(httpd_req_t *req)
{
    httpd_resp_set_type(req, "font/ttf");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=86400");
    return httpd_resp_send(req, (const char *)weathericons_regular_webfont_ttf_start,
                           weathericons_regular_webfont_ttf_end - weathericons_regular_webfont_ttf_start);
}

/* ───────────────────────── Status API ───────────────────────── */

static void sensor_cache_to_json(cJSON *obj, const sensor_cache_entry_t *sc)
{
    int64_t age_s = (esp_timer_get_time() - sc->timestamp) / 1000000;
    cJSON_AddNumberToObject(obj, "age_s", (double)age_s);

    switch (sc->sensor_type) {
    case APP_PROTOCOL_SENSOR_ENV:
        cJSON_AddStringToObject(obj, "type", "env");
        cJSON_AddNumberToObject(obj, "temperature", sc->env.temperature);
        cJSON_AddNumberToObject(obj, "pressure", sc->env.pressure);
        cJSON_AddNumberToObject(obj, "humidity", sc->env.humidity);
        cJSON_AddNumberToObject(obj, "lux", sc->env.lux);
        break;
    case APP_PROTOCOL_SENSOR_IAQ:
        cJSON_AddStringToObject(obj, "type", "iaq");
        cJSON_AddNumberToObject(obj, "temperature", sc->iaq.temperature);
        cJSON_AddNumberToObject(obj, "humidity", sc->iaq.humidity);
        cJSON_AddNumberToObject(obj, "eco2", sc->iaq.eco2);
        cJSON_AddNumberToObject(obj, "tvoc", sc->iaq.tvoc);
        cJSON_AddNumberToObject(obj, "aqi", sc->iaq.aqi);
        break;
    case APP_PROTOCOL_SENSOR_PRESENCE:
        cJSON_AddStringToObject(obj, "type", "presence");
        cJSON_AddNumberToObject(obj, "target_state", sc->presence.target_state);
        cJSON_AddNumberToObject(obj, "moving_distance", sc->presence.moving_distance);
        cJSON_AddNumberToObject(obj, "moving_energy", sc->presence.moving_energy);
        cJSON_AddNumberToObject(obj, "static_distance", sc->presence.static_distance);
        cJSON_AddNumberToObject(obj, "static_energy", sc->presence.static_energy);
        break;
    default:
        cJSON_AddStringToObject(obj, "type", "unknown");
        break;
    }
}

static esp_err_t handle_status_system(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();

    /* Heap */
    cJSON_AddNumberToObject(root, "heap_free", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "heap_min", esp_get_minimum_free_heap_size());

    /* Uptime */
    int64_t uptime_us = esp_timer_get_time() - s_start_time_us;
    cJSON_AddNumberToObject(root, "uptime_s", (double)(uptime_us / 1000000));

    /* Chip info */
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    cJSON_AddStringToObject(root, "chip_model", CONFIG_IDF_TARGET);
    cJSON_AddNumberToObject(root, "chip_revision", chip.revision);

    /* MAC */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    cJSON_AddStringToObject(root, "mac", mac_str);

    /* IP & WiFi */
    esp_netif_ip_info_t ip_info;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        char ip_str[16];
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
        cJSON_AddStringToObject(root, "ip", ip_str);
    } else {
        cJSON_AddStringToObject(root, "ip", "0.0.0.0");
    }

    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        cJSON_AddStringToObject(root, "wifi_ssid", (char *)ap.ssid);
        cJSON_AddNumberToObject(root, "wifi_rssi", ap.rssi);
    } else {
        cJSON_AddStringToObject(root, "wifi_ssid", "");
        cJSON_AddNumberToObject(root, "wifi_rssi", 0);
    }

    /* MQTT */
    cJSON_AddBoolToObject(root, "mqtt_connected", app_mqtt_is_connected());

    /* Time */
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    char time_str[32];
    strftime(time_str, sizeof(time_str), "%Y-%m-%dT%H:%M:%S", &timeinfo);
    cJSON_AddStringToObject(root, "time", time_str);

    return web_send_ok(req, root);
}

static void node_info_to_json(cJSON *obj, const app_espnow_node_info_t *info)
{
    cJSON_AddNumberToObject(obj, "node_id", info->node_id);
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             info->mac[0], info->mac[1], info->mac[2],
             info->mac[3], info->mac[4], info->mac[5]);
    cJSON_AddStringToObject(obj, "mac", mac_str);
    cJSON_AddNumberToObject(obj, "device_type", info->device_type);
    cJSON_AddNumberToObject(obj, "fw_version", info->fw_version);
    cJSON_AddBoolToObject(obj, "online", info->status == APP_ESPNOW_NODE_ONLINE);
    cJSON_AddNumberToObject(obj, "last_seen", (double)info->last_seen_ms);
    cJSON_AddNumberToObject(obj, "rssi", info->rssi);

    /* Attach latest sensor data if available */
    uint8_t idx = info->node_id - 1;
    if (idx < APP_ESPNOW_MAX_NODES) {
        const sensor_cache_entry_t *sc = &s_sensor_cache[idx];
        if (sc->sensor_type != 0) {
            cJSON *sensor = cJSON_AddObjectToObject(obj, "sensor");
            sensor_cache_to_json(sensor, sc);
        }
    }
}

static esp_err_t handle_status_nodes(httpd_req_t *req)
{
    app_espnow_node_info_t nodes[APP_ESPNOW_MAX_NODES];
    uint8_t count = 0;
    app_espnow_get_all_nodes(nodes, APP_ESPNOW_MAX_NODES, &count);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "count", count);
    cJSON *arr = cJSON_AddArrayToObject(root, "nodes");

    for (uint8_t i = 0; i < count; i++) {
        cJSON *node = cJSON_CreateObject();
        node_info_to_json(node, &nodes[i]);
        cJSON_AddItemToArray(arr, node);
    }

    return web_send_ok(req, root);
}

static esp_err_t handle_status_node(httpd_req_t *req)
{
    /* Parse node ID from URI: /api/v1/status/nodes/XX */
    const char *uri = req->uri;
    const char *id_str = strrchr(uri, '/');
    if (!id_str || *(id_str + 1) == '\0') {
        return web_send_error(req, 400, "missing node id");
    }
    id_str++;

    char *endptr;
    long node_id = strtol(id_str, &endptr, 10);
    if (*endptr != '\0' && *endptr != '?') {
        return web_send_error(req, 400, "invalid node id");
    }

    app_espnow_node_info_t info;
    if (app_espnow_get_node_info((uint8_t)node_id, &info) != ESP_OK) {
        return web_send_error(req, 404, "node not found");
    }

    cJSON *root = cJSON_CreateObject();
    node_info_to_json(root, &info);
    return web_send_ok(req, root);
}

static esp_err_t handle_status_weather(httpd_req_t *req)
{
    app_weather_data_t data;
    cJSON *root = cJSON_CreateObject();

    if (app_weather_get(&data) != ESP_OK || !data.valid) {
        cJSON_AddBoolToObject(root, "valid", false);
        cJSON_AddNullToObject(root, "now");
        cJSON *arr = cJSON_AddArrayToObject(root, "forecast");
        (void)arr;
        return web_send_ok(req, root);
    }

    cJSON_AddBoolToObject(root, "valid", true);
    cJSON_AddNumberToObject(root, "updated_at", (double)data.timestamp);

    /* Current weather */
    cJSON *now = cJSON_AddObjectToObject(root, "now");
    cJSON_AddNumberToObject(now, "temp", data.now.temp);
    cJSON_AddNumberToObject(now, "feels_like", data.now.feels_like);
    cJSON_AddNumberToObject(now, "icon_code", data.now.icon_code);
    cJSON_AddStringToObject(now, "text", data.now.text);
    cJSON_AddNumberToObject(now, "humidity", data.now.humidity);
    cJSON_AddNumberToObject(now, "wind_scale", data.now.wind_scale);
    cJSON_AddStringToObject(now, "wind_dir", data.now.wind_dir);

    /* 3-day forecast */
    cJSON *forecast = cJSON_AddArrayToObject(root, "forecast");
    for (int i = 0; i < 3; i++) {
        cJSON *day = cJSON_CreateObject();
        cJSON_AddStringToObject(day, "date", data.daily[i].date);
        cJSON_AddNumberToObject(day, "temp_max", data.daily[i].temp_max);
        cJSON_AddNumberToObject(day, "temp_min", data.daily[i].temp_min);
        cJSON_AddNumberToObject(day, "icon_day", data.daily[i].icon_day);
        cJSON_AddStringToObject(day, "text_day", data.daily[i].text_day);
        cJSON_AddItemToArray(forecast, day);
    }

    return web_send_ok(req, root);
}

/* ───────────────────────── Config Helpers ───────────────────────── */

static void mask_string(cJSON *obj, const char *key, const char *value)
{
    if (value && value[0] != '\0') {
        cJSON_AddStringToObject(obj, key, "****");
    } else {
        cJSON_AddStringToObject(obj, key, "");
    }
}

static cJSON *get_wifi_config_json(bool mask)
{
    cJSON *obj = cJSON_CreateObject();
    char buf[NVS_CONFIG_MAX_LEN];
    size_t len;

    /* SSID */
    len = sizeof(buf);
    if (app_storage_get_blob(NVS_NS_WIFI, "ssid", buf, &len) == ESP_OK) {
        buf[len] = '\0';
        cJSON_AddStringToObject(obj, "ssid", buf);
    } else {
        cJSON_AddStringToObject(obj, "ssid", CONFIG_WIFI_SSID);
    }

    /* Password */
    len = sizeof(buf);
    if (app_storage_get_blob(NVS_NS_WIFI, "password", buf, &len) == ESP_OK) {
        buf[len] = '\0';
        if (mask) mask_string(obj, "password", buf);
        else cJSON_AddStringToObject(obj, "password", buf);
    } else {
        if (mask) mask_string(obj, "password", CONFIG_WIFI_PASSWORD);
        else cJSON_AddStringToObject(obj, "password", CONFIG_WIFI_PASSWORD);
    }

    /* Max retry */
    uint8_t retry_val;
    if (app_storage_get_u8(NVS_NS_WIFI, "max_retry", &retry_val) == ESP_OK) {
        cJSON_AddNumberToObject(obj, "max_retry", retry_val);
    } else {
        cJSON_AddNumberToObject(obj, "max_retry", CONFIG_WIFI_MAX_RETRY);
    }

    return obj;
}

static cJSON *get_mqtt_config_json(bool mask)
{
    cJSON *obj = cJSON_CreateObject();
    char buf[NVS_CONFIG_MAX_LEN];
    size_t len;

    /* Broker URI */
    len = sizeof(buf);
    if (app_storage_get_blob(NVS_NS_MQTT, "broker_uri", buf, &len) == ESP_OK) {
        buf[len] = '\0';
        cJSON_AddStringToObject(obj, "broker_uri", buf);
    } else {
        cJSON_AddStringToObject(obj, "broker_uri", CONFIG_MQTT_BROKER_URI);
    }

    /* Client ID */
    len = sizeof(buf);
    if (app_storage_get_blob(NVS_NS_MQTT, "client_id", buf, &len) == ESP_OK) {
        buf[len] = '\0';
        cJSON_AddStringToObject(obj, "client_id", buf);
    } else {
        cJSON_AddStringToObject(obj, "client_id", CONFIG_MQTT_CLIENT_ID);
    }

    /* Username */
    len = sizeof(buf);
    if (app_storage_get_blob(NVS_NS_MQTT, "username", buf, &len) == ESP_OK) {
        buf[len] = '\0';
        cJSON_AddStringToObject(obj, "username", buf);
    } else {
        cJSON_AddStringToObject(obj, "username", CONFIG_MQTT_USERNAME);
    }

    /* Password */
    len = sizeof(buf);
    if (app_storage_get_blob(NVS_NS_MQTT, "password", buf, &len) == ESP_OK) {
        buf[len] = '\0';
        if (mask) mask_string(obj, "password", buf);
        else cJSON_AddStringToObject(obj, "password", buf);
    } else {
        if (mask) mask_string(obj, "password", CONFIG_MQTT_PASSWORD);
        else cJSON_AddStringToObject(obj, "password", CONFIG_MQTT_PASSWORD);
    }

    /* Keepalive */
    uint8_t keepalive;
    if (app_storage_get_u8(NVS_NS_MQTT, "keepalive", &keepalive) == ESP_OK) {
        cJSON_AddNumberToObject(obj, "keepalive", keepalive);
    } else {
        cJSON_AddNumberToObject(obj, "keepalive", CONFIG_MQTT_KEEPALIVE);
    }

    return obj;
}

static cJSON *get_espnow_config_json(bool mask)
{
    cJSON *obj = cJSON_CreateObject();
    char buf[NVS_CONFIG_MAX_LEN];
    size_t len;

    /* PMK */
    len = sizeof(buf);
    if (app_storage_get_blob(NVS_NS_ESPNOW, "pmk", buf, &len) == ESP_OK) {
        buf[len] = '\0';
        if (mask) mask_string(obj, "pmk", buf);
        else cJSON_AddStringToObject(obj, "pmk", buf);
    } else {
        if (mask) mask_string(obj, "pmk", CONFIG_ESPNOW_PMK);
        else cJSON_AddStringToObject(obj, "pmk", CONFIG_ESPNOW_PMK);
    }

    /* Heartbeat timeout */
    uint8_t val;
    if (app_storage_get_u8(NVS_NS_ESPNOW, "hb_timeout", &val) == ESP_OK) {
        cJSON_AddNumberToObject(obj, "heartbeat_timeout_s", val);
    } else {
        cJSON_AddNumberToObject(obj, "heartbeat_timeout_s", CONFIG_ESPNOW_HEARTBEAT_TIMEOUT_S);
    }

    /* Heartbeat check */
    if (app_storage_get_u8(NVS_NS_ESPNOW, "hb_check", &val) == ESP_OK) {
        cJSON_AddNumberToObject(obj, "heartbeat_check_s", val);
    } else {
        cJSON_AddNumberToObject(obj, "heartbeat_check_s", CONFIG_ESPNOW_HEARTBEAT_CHECK_S);
    }

    return obj;
}

static cJSON *get_sntp_config_json(void)
{
    cJSON *obj = cJSON_CreateObject();
    char buf[NVS_CONFIG_MAX_LEN];
    size_t len;

    len = sizeof(buf);
    if (app_storage_get_blob(NVS_NS_SNTP, "ntp_server", buf, &len) == ESP_OK) {
        buf[len] = '\0';
        cJSON_AddStringToObject(obj, "ntp_server", buf);
    } else {
        cJSON_AddStringToObject(obj, "ntp_server", CONFIG_SNTP_SERVER);
    }

    len = sizeof(buf);
    if (app_storage_get_blob(NVS_NS_SNTP, "timezone", buf, &len) == ESP_OK) {
        buf[len] = '\0';
        cJSON_AddStringToObject(obj, "timezone", buf);
    } else {
        cJSON_AddStringToObject(obj, "timezone", CONFIG_SNTP_TIMEZONE);
    }

    uint8_t val;
    if (app_storage_get_u8(NVS_NS_SNTP, "sync_min", &val) == ESP_OK) {
        cJSON_AddNumberToObject(obj, "sync_interval", val);
    } else {
        cJSON_AddNumberToObject(obj, "sync_interval", CONFIG_SNTP_SYNC_INTERVAL);
    }

    return obj;
}

static cJSON *get_weather_config_json(bool mask)
{
    cJSON *obj = cJSON_CreateObject();
    char buf[NVS_CONFIG_MAX_LEN];
    size_t len;

    /* API Key */
    len = sizeof(buf);
    if (app_storage_get_blob(NVS_NS_WEATHER, "api_key", buf, &len) == ESP_OK) {
        buf[len] = '\0';
        if (mask) mask_string(obj, "api_key", buf);
        else cJSON_AddStringToObject(obj, "api_key", buf);
    } else {
        if (mask) mask_string(obj, "api_key", CONFIG_WEATHER_API_KEY);
        else cJSON_AddStringToObject(obj, "api_key", CONFIG_WEATHER_API_KEY);
    }

    /* Location */
    len = sizeof(buf);
    if (app_storage_get_blob(NVS_NS_WEATHER, "location", buf, &len) == ESP_OK) {
        buf[len] = '\0';
        cJSON_AddStringToObject(obj, "location", buf);
    } else {
        cJSON_AddStringToObject(obj, "location", CONFIG_WEATHER_LOCATION);
    }

    /* API Host */
    len = sizeof(buf);
    if (app_storage_get_blob(NVS_NS_WEATHER, "api_host", buf, &len) == ESP_OK) {
        buf[len] = '\0';
        cJSON_AddStringToObject(obj, "api_host", buf);
    } else {
        cJSON_AddStringToObject(obj, "api_host", CONFIG_WEATHER_API_HOST);
    }

    /* Refresh interval */
    uint8_t val;
    if (app_storage_get_u8(NVS_NS_WEATHER, "refresh_min", &val) == ESP_OK) {
        cJSON_AddNumberToObject(obj, "refresh_min", val);
    } else {
        cJSON_AddNumberToObject(obj, "refresh_min", CONFIG_WEATHER_REFRESH_MIN);
    }

    return obj;
}

static cJSON *get_display_config_json(void)
{
    cJSON *obj = cJSON_CreateObject();

    cJSON_AddNumberToObject(obj, "pin_mosi", CONFIG_DISPLAY_PIN_MOSI);
    cJSON_AddNumberToObject(obj, "pin_clk", CONFIG_DISPLAY_PIN_CLK);
    cJSON_AddNumberToObject(obj, "pin_cs", CONFIG_DISPLAY_PIN_CS);
    cJSON_AddNumberToObject(obj, "pin_dc", CONFIG_DISPLAY_PIN_DC);
    cJSON_AddNumberToObject(obj, "pin_rst", CONFIG_DISPLAY_PIN_RST);
    cJSON_AddNumberToObject(obj, "pin_busy", CONFIG_DISPLAY_PIN_BUSY);
    cJSON_AddNumberToObject(obj, "pin_pwr", CONFIG_DISPLAY_PIN_PWR);

    uint8_t val;
    if (app_storage_get_u8(NVS_NS_DISPLAY, "refresh_s", &val) == ESP_OK) {
        cJSON_AddNumberToObject(obj, "refresh_interval_s", val);
    } else {
        cJSON_AddNumberToObject(obj, "refresh_interval_s", CONFIG_DISPLAY_REFRESH_INTERVAL);
    }

    return obj;
}

/* ───────────────────────── Forward Declarations ───────────────────────── */

static esp_err_t handle_config_reset(httpd_req_t *req);

/* ───────────────────────── Config API Handlers ───────────────────────── */

static const char *extract_module_name(const char *uri, const char *prefix)
{
    const char *p = uri + strlen(prefix);
    /* Skip leading slash if present */
    if (*p == '/') p++;
    return p;
}

static esp_err_t handle_config_get_all(httpd_req_t *req)
{
    if (web_require_auth(req) != ESP_OK) return ESP_OK;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "wifi", get_wifi_config_json(true));
    cJSON_AddItemToObject(root, "mqtt", get_mqtt_config_json(true));
    cJSON_AddItemToObject(root, "espnow", get_espnow_config_json(true));
    cJSON_AddItemToObject(root, "sntp", get_sntp_config_json());
    cJSON_AddItemToObject(root, "weather", get_weather_config_json(true));
    cJSON_AddItemToObject(root, "display", get_display_config_json());

    return web_send_ok(req, root);
}

static esp_err_t handle_config_get_module(httpd_req_t *req)
{
    if (web_require_auth(req) != ESP_OK) return ESP_OK;

    const char *module = extract_module_name(req->uri, "/api/v1/config");
    /* Remove trailing slash or query string */
    char mod_name[32];
    size_t i;
    for (i = 0; i < sizeof(mod_name) - 1 && module[i] && module[i] != '/' && module[i] != '?'; i++) {
        mod_name[i] = module[i];
    }
    mod_name[i] = '\0';

    cJSON *obj = NULL;
    if (strcmp(mod_name, "wifi") == 0) obj = get_wifi_config_json(true);
    else if (strcmp(mod_name, "mqtt") == 0) obj = get_mqtt_config_json(true);
    else if (strcmp(mod_name, "espnow") == 0) obj = get_espnow_config_json(true);
    else if (strcmp(mod_name, "sntp") == 0) obj = get_sntp_config_json();
    else if (strcmp(mod_name, "weather") == 0) obj = get_weather_config_json(true);
    else if (strcmp(mod_name, "display") == 0) obj = get_display_config_json();
    else return web_send_error(req, 404, "unknown module");

    return web_send_ok(req, obj);
}

/* ───────── Config POST (update) ───────── */

static esp_err_t save_wifi_config(cJSON *json, bool *restart_required)
{
    cJSON *item;
    *restart_required = false;

    item = cJSON_GetObjectItem(json, "ssid");
    if (item && cJSON_IsString(item)) {
        app_storage_set_blob(NVS_NS_WIFI, "ssid", item->valuestring, strlen(item->valuestring));
        *restart_required = true;
    }
    item = cJSON_GetObjectItem(json, "password");
    if (item && cJSON_IsString(item)) {
        app_storage_set_blob(NVS_NS_WIFI, "password", item->valuestring, strlen(item->valuestring));
        *restart_required = true;
    }
    item = cJSON_GetObjectItem(json, "max_retry");
    if (item && cJSON_IsNumber(item)) {
        int val = item->valueint;
        if (val < 1 || val > 100) return ESP_ERR_INVALID_ARG;
        app_storage_set_u8(NVS_NS_WIFI, "max_retry", (uint8_t)val);
        *restart_required = true;
    }
    return ESP_OK;
}

static esp_err_t save_mqtt_config(cJSON *json, bool *restart_required)
{
    cJSON *item;
    *restart_required = false;

    item = cJSON_GetObjectItem(json, "broker_uri");
    if (item && cJSON_IsString(item)) {
        app_storage_set_blob(NVS_NS_MQTT, "broker_uri", item->valuestring, strlen(item->valuestring));
        *restart_required = true;
    }
    item = cJSON_GetObjectItem(json, "client_id");
    if (item && cJSON_IsString(item)) {
        app_storage_set_blob(NVS_NS_MQTT, "client_id", item->valuestring, strlen(item->valuestring));
        *restart_required = true;
    }
    item = cJSON_GetObjectItem(json, "username");
    if (item && cJSON_IsString(item)) {
        app_storage_set_blob(NVS_NS_MQTT, "username", item->valuestring, strlen(item->valuestring));
        *restart_required = true;
    }
    item = cJSON_GetObjectItem(json, "password");
    if (item && cJSON_IsString(item)) {
        app_storage_set_blob(NVS_NS_MQTT, "password", item->valuestring, strlen(item->valuestring));
        *restart_required = true;
    }
    item = cJSON_GetObjectItem(json, "keepalive");
    if (item && cJSON_IsNumber(item)) {
        int val = item->valueint;
        if (val < 10 || val > 255) return ESP_ERR_INVALID_ARG;
        app_storage_set_u8(NVS_NS_MQTT, "keepalive", (uint8_t)val);
        *restart_required = true;
    }
    return ESP_OK;
}

static esp_err_t save_espnow_config(cJSON *json, bool *restart_required)
{
    cJSON *item;
    *restart_required = false;

    item = cJSON_GetObjectItem(json, "pmk");
    if (item && cJSON_IsString(item)) {
        if (strlen(item->valuestring) != 0 && strlen(item->valuestring) != 16) {
            return ESP_ERR_INVALID_ARG;
        }
        app_storage_set_blob(NVS_NS_ESPNOW, "pmk", item->valuestring, strlen(item->valuestring));
        *restart_required = true;
    }
    item = cJSON_GetObjectItem(json, "heartbeat_timeout_s");
    if (item && cJSON_IsNumber(item)) {
        int val = item->valueint;
        if (val < 10 || val > 255) return ESP_ERR_INVALID_ARG;
        app_storage_set_u8(NVS_NS_ESPNOW, "hb_timeout", (uint8_t)val);
        *restart_required = true;
    }
    item = cJSON_GetObjectItem(json, "heartbeat_check_s");
    if (item && cJSON_IsNumber(item)) {
        int val = item->valueint;
        if (val < 5 || val > 120) return ESP_ERR_INVALID_ARG;
        app_storage_set_u8(NVS_NS_ESPNOW, "hb_check", (uint8_t)val);
        *restart_required = true;
    }
    return ESP_OK;
}

static esp_err_t save_sntp_config(cJSON *json, bool *restart_required)
{
    cJSON *item;
    *restart_required = false;

    item = cJSON_GetObjectItem(json, "ntp_server");
    if (item && cJSON_IsString(item)) {
        app_storage_set_blob(NVS_NS_SNTP, "ntp_server", item->valuestring, strlen(item->valuestring));
        *restart_required = true;
    }
    item = cJSON_GetObjectItem(json, "timezone");
    if (item && cJSON_IsString(item)) {
        app_storage_set_blob(NVS_NS_SNTP, "timezone", item->valuestring, strlen(item->valuestring));
        *restart_required = true;
    }
    item = cJSON_GetObjectItem(json, "sync_interval");
    if (item && cJSON_IsNumber(item)) {
        int val = item->valueint;
        if (val < 1 || val > 255) return ESP_ERR_INVALID_ARG;
        app_storage_set_u8(NVS_NS_SNTP, "sync_min", (uint8_t)val);
        *restart_required = true;
    }
    return ESP_OK;
}

static esp_err_t save_weather_config(cJSON *json, bool *restart_required)
{
    cJSON *item;
    *restart_required = false;

    item = cJSON_GetObjectItem(json, "api_key");
    if (item && cJSON_IsString(item)) {
        app_storage_set_blob(NVS_NS_WEATHER, "api_key", item->valuestring, strlen(item->valuestring));
        *restart_required = true;
    }
    item = cJSON_GetObjectItem(json, "location");
    if (item && cJSON_IsString(item)) {
        app_storage_set_blob(NVS_NS_WEATHER, "location", item->valuestring, strlen(item->valuestring));
        *restart_required = true;
    }
    item = cJSON_GetObjectItem(json, "api_host");
    if (item && cJSON_IsString(item)) {
        app_storage_set_blob(NVS_NS_WEATHER, "api_host", item->valuestring, strlen(item->valuestring));
        *restart_required = true;
    }
    item = cJSON_GetObjectItem(json, "refresh_min");
    if (item && cJSON_IsNumber(item)) {
        int val = item->valueint;
        if (val < 10 || val > 255) return ESP_ERR_INVALID_ARG;
        app_storage_set_u8(NVS_NS_WEATHER, "refresh_min", (uint8_t)val);
        *restart_required = true;
    }
    return ESP_OK;
}

static esp_err_t save_display_config(cJSON *json, bool *restart_required)
{
    cJSON *item;
    *restart_required = false;

    item = cJSON_GetObjectItem(json, "refresh_interval_s");
    if (item && cJSON_IsNumber(item)) {
        int val = item->valueint;
        if (val < 30 || val > 255) return ESP_ERR_INVALID_ARG;
        app_storage_set_u8(NVS_NS_DISPLAY, "refresh_s", (uint8_t)val);
        *restart_required = true;
    }
    return ESP_OK;
}

static esp_err_t handle_config_post_module(httpd_req_t *req)
{
    if (web_require_auth(req) != ESP_OK) return ESP_OK;

    /* Check if this is a reset request: /api/v1/config/{module}/reset */
    if (strstr(req->uri, "/reset") != NULL) {
        return handle_config_reset(req);
    }

    const char *module = extract_module_name(req->uri, "/api/v1/config");
    char mod_name[32];
    size_t i;
    for (i = 0; i < sizeof(mod_name) - 1 && module[i] && module[i] != '/' && module[i] != '?'; i++) {
        mod_name[i] = module[i];
    }
    mod_name[i] = '\0';

    char *body = web_read_body(req);
    if (!body) return web_send_error(req, 400, "invalid body");

    cJSON *json = cJSON_Parse(body);
    free(body);
    if (!json) return web_send_error(req, 400, "invalid json");

    bool restart_required = false;
    esp_err_t err;

    if (strcmp(mod_name, "wifi") == 0) err = save_wifi_config(json, &restart_required);
    else if (strcmp(mod_name, "mqtt") == 0) err = save_mqtt_config(json, &restart_required);
    else if (strcmp(mod_name, "espnow") == 0) err = save_espnow_config(json, &restart_required);
    else if (strcmp(mod_name, "sntp") == 0) err = save_sntp_config(json, &restart_required);
    else if (strcmp(mod_name, "weather") == 0) err = save_weather_config(json, &restart_required);
    else if (strcmp(mod_name, "display") == 0) err = save_display_config(json, &restart_required);
    else {
        cJSON_Delete(json);
        return web_send_error(req, 404, "unknown module");
    }

    cJSON_Delete(json);

    if (err == ESP_ERR_INVALID_ARG) {
        return web_send_error(req, 400, "invalid field value");
    }
    if (err != ESP_OK) {
        return web_send_error(req, 500, "failed to save config");
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "ok");
    cJSON_AddBoolToObject(resp, "restart_required", restart_required);
    return web_send_ok(req, resp);
}

/* ───────── Config Reset ───────── */

static const char *module_nvs_keys[][8] = {
    /* wifi */    {"ssid", "password", "max_retry", NULL},
    /* mqtt */    {"broker_uri", "client_id", "username", "password", "keepalive", NULL},
    /* espnow */  {"pmk", "hb_timeout", "hb_check", NULL},
    /* sntp */    {"ntp_server", "timezone", "sync_min", NULL},
    /* weather */ {"api_key", "location", "api_host", "refresh_min", NULL},
    /* display */ {"refresh_s", NULL},
};

static const char *module_nvs_ns[] = {
    NVS_NS_WIFI, NVS_NS_MQTT, NVS_NS_ESPNOW, NVS_NS_SNTP, NVS_NS_WEATHER, NVS_NS_DISPLAY
};

static const char *module_names[] = {
    "wifi", "mqtt", "espnow", "sntp", "weather", "display"
};

static int find_module_index(const char *name)
{
    for (int i = 0; i < 6; i++) {
        if (strcmp(module_names[i], name) == 0) return i;
    }
    return -1;
}

static esp_err_t handle_config_reset(httpd_req_t *req)
{
    if (web_require_auth(req) != ESP_OK) return ESP_OK;

    /* URI: /api/v1/config/{module}/reset */
    const char *uri = req->uri;
    const char *p = uri + strlen("/api/v1/config/");
    char mod_name[32];
    size_t i;
    for (i = 0; i < sizeof(mod_name) - 1 && p[i] && p[i] != '/'; i++) {
        mod_name[i] = p[i];
    }
    mod_name[i] = '\0';

    int idx = find_module_index(mod_name);
    if (idx < 0) return web_send_error(req, 404, "unknown module");

    for (int k = 0; module_nvs_keys[idx][k] != NULL; k++) {
        app_storage_erase_key(module_nvs_ns[idx], module_nvs_keys[idx][k]);
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "ok");
    cJSON_AddBoolToObject(resp, "restart_required", true);
    return web_send_ok(req, resp);
}

/* ───────── System Endpoints ───────── */

static esp_err_t handle_system_restart(httpd_req_t *req)
{
    if (web_require_auth(req) != ESP_OK) return ESP_OK;

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "restarting");
    web_send_ok(req, resp);

    ESP_LOGI(TAG, "Restart requested via web interface");
    /* Delay to let response be sent */
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK; /* unreachable */
}

static esp_err_t handle_system_credentials(httpd_req_t *req)
{
    if (web_require_auth(req) != ESP_OK) return ESP_OK;

    char *body = web_read_body(req);
    if (!body) return web_send_error(req, 400, "invalid body");

    cJSON *json = cJSON_Parse(body);
    free(body);
    if (!json) return web_send_error(req, 400, "invalid json");

    cJSON *user = cJSON_GetObjectItem(json, "username");
    cJSON *pass = cJSON_GetObjectItem(json, "password");

    if (pass && cJSON_IsString(pass) && strlen(pass->valuestring) == 0) {
        cJSON_Delete(json);
        return web_send_error(req, 400, "password must not be empty");
    }

    if (user && cJSON_IsString(user) && strlen(user->valuestring) > 0) {
        app_storage_set_blob(NVS_NS_WEB, NVS_KEY_USER, user->valuestring, strlen(user->valuestring));
    }
    if (pass && cJSON_IsString(pass) && strlen(pass->valuestring) > 0) {
        app_storage_set_blob(NVS_NS_WEB, NVS_KEY_PASS, pass->valuestring, strlen(pass->valuestring));
    }

    cJSON_Delete(json);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "ok");
    return web_send_ok(req, resp);
}

/* ───────────────────────── Sensor Data Event Handler ───────────────────────── */

static void web_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base != APP_EVENT_BASE || event_id != APP_EVENT_ESPNOW_NODE_DATA) {
        return;
    }

    const app_espnow_node_data_t *evt = (const app_espnow_node_data_t *)event_data;
    if (evt->node_id < APP_ESPNOW_NODE_ID_MIN || evt->node_id > APP_ESPNOW_NODE_ID_MAX) {
        return;
    }

    const app_protocol_data_report_t *report;
    if (app_protocol_parse_data_report(evt->data, evt->data_len, &report) != ESP_OK) {
        return;
    }

    uint8_t idx = evt->node_id - 1;
    sensor_cache_entry_t *sc = &s_sensor_cache[idx];
    sc->timestamp = esp_timer_get_time();
    sc->sensor_type = report->sensor_type;

    switch (report->sensor_type) {
    case APP_PROTOCOL_SENSOR_ENV:
        if (report->data_len >= sizeof(app_protocol_env_data_t)) {
            memcpy(&sc->env, report->data, sizeof(app_protocol_env_data_t));
        }
        break;
    case APP_PROTOCOL_SENSOR_IAQ:
        if (report->data_len >= sizeof(app_protocol_iaq_data_t)) {
            memcpy(&sc->iaq, report->data, sizeof(app_protocol_iaq_data_t));
        }
        break;
    case APP_PROTOCOL_SENSOR_PRESENCE:
        if (report->data_len >= sizeof(app_protocol_presence_data_t)) {
            memcpy(&sc->presence, report->data, sizeof(app_protocol_presence_data_t));
        }
        break;
    default:
        sc->sensor_type = 0; /* unknown, don't cache */
        break;
    }
}

/* ───────────────────────── Sensors API ───────────────────────── */

static esp_err_t handle_status_sensors(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "sensors");

    for (int i = 0; i < APP_ESPNOW_MAX_NODES; i++) {
        const sensor_cache_entry_t *sc = &s_sensor_cache[i];
        if (sc->sensor_type == 0) continue;

        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "node_id", i + 1);
        sensor_cache_to_json(item, sc);
        cJSON_AddItemToArray(arr, item);
    }

    return web_send_ok(req, root);
}

/* ───────────────────────── 404 Handler ───────────────────────── */

static esp_err_t handle_404(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    return web_send_error(req, 404, "not found");
}

/* ───────────────────────── Route Registration ───────────────────────── */

static void register_routes(httpd_handle_t server)
{
    /* ── Static files ── */
    httpd_uri_t uri_index = {
        .uri = "/", .method = HTTP_GET, .handler = handle_index
    };
    httpd_uri_t uri_css = {
        .uri = "/style.css", .method = HTTP_GET, .handler = handle_style_css
    };
    httpd_uri_t uri_js = {
        .uri = "/app.js", .method = HTTP_GET, .handler = handle_app_js
    };
    httpd_uri_t uri_font = {
        .uri = "/weathericons.ttf", .method = HTTP_GET, .handler = handle_font
    };

    /* ── CORS preflight (wildcard) ── */
    httpd_uri_t uri_options = {
        .uri = "/api/*", .method = HTTP_OPTIONS, .handler = handle_options
    };

    /* ── Status API (public) ── */
    httpd_uri_t uri_status_system = {
        .uri = "/api/v1/status/system", .method = HTTP_GET, .handler = handle_status_system
    };
    httpd_uri_t uri_status_nodes = {
        .uri = "/api/v1/status/nodes", .method = HTTP_GET, .handler = handle_status_nodes
    };
    httpd_uri_t uri_status_node = {
        .uri = "/api/v1/status/nodes/*", .method = HTTP_GET, .handler = handle_status_node
    };
    httpd_uri_t uri_status_weather = {
        .uri = "/api/v1/status/weather", .method = HTTP_GET, .handler = handle_status_weather
    };
    httpd_uri_t uri_status_sensors = {
        .uri = "/api/v1/status/sensors", .method = HTTP_GET, .handler = handle_status_sensors
    };

    /* ── Config API (auth required) ── */
    httpd_uri_t uri_config_all = {
        .uri = "/api/v1/config", .method = HTTP_GET, .handler = handle_config_get_all
    };
    httpd_uri_t uri_config_module = {
        .uri = "/api/v1/config/*", .method = HTTP_GET, .handler = handle_config_get_module
    };
    httpd_uri_t uri_config_post = {
        .uri = "/api/v1/config/*", .method = HTTP_POST, .handler = handle_config_post_module
    };

    /* Config reset is handled inside handle_config_post_module */

    /* ── System API (auth required) ── */
    httpd_uri_t uri_restart = {
        .uri = "/api/v1/system/restart", .method = HTTP_POST, .handler = handle_system_restart
    };
    httpd_uri_t uri_credentials = {
        .uri = "/api/v1/system/credentials", .method = HTTP_POST, .handler = handle_system_credentials
    };

    httpd_register_uri_handler(server, &uri_index);
    httpd_register_uri_handler(server, &uri_css);
    httpd_register_uri_handler(server, &uri_js);
    httpd_register_uri_handler(server, &uri_font);
    httpd_register_uri_handler(server, &uri_options);
    httpd_register_uri_handler(server, &uri_status_system);
    httpd_register_uri_handler(server, &uri_status_nodes);
    httpd_register_uri_handler(server, &uri_status_node);
    httpd_register_uri_handler(server, &uri_status_weather);
    httpd_register_uri_handler(server, &uri_status_sensors);
    httpd_register_uri_handler(server, &uri_config_all);
    httpd_register_uri_handler(server, &uri_config_module);
    httpd_register_uri_handler(server, &uri_config_post);
    httpd_register_uri_handler(server, &uri_restart);
    httpd_register_uri_handler(server, &uri_credentials);
}

/* ───────────────────────── Public API ───────────────────────── */

esp_err_t app_web_init(void)
{
    if (s_server) {
        ESP_LOGW(TAG, "Web server already running");
        return ESP_ERR_INVALID_STATE;
    }

    s_start_time_us = esp_timer_get_time();
    memset(s_sensor_cache, 0, sizeof(s_sensor_cache));

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = CONFIG_WEB_SERVER_PORT;
    config.max_open_sockets = CONFIG_WEB_SERVER_MAX_CONN;
    config.max_uri_handlers = 16;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.lru_purge_enable = true;
    config.stack_size = 8192;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
        return err;
    }

    /* Register event handler only after server is successfully started */
    app_event_handler_register(APP_EVENT_ESPNOW_NODE_DATA, web_event_handler, NULL);

    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, handle_404);
    register_routes(s_server);

    ESP_LOGI(TAG, "Web server started on port %d", CONFIG_WEB_SERVER_PORT);
    return ESP_OK;
}

esp_err_t app_web_stop(void)
{
    if (!s_server) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Unregister event handler before stopping the server */
    app_event_handler_unregister(APP_EVENT_ESPNOW_NODE_DATA, web_event_handler);

    esp_err_t err = httpd_stop(s_server);
    s_server = NULL;
    ESP_LOGI(TAG, "Web server stopped");
    return err;
}
