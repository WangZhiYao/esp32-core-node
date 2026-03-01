#include "app_espnow.h"
#include "app_event.h"
#include "app_storage.h"
#include "app_protocol.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include <string.h>
#include <inttypes.h>

/* ───────────────────────── Constants & Macros ───────────────────────── */

#define TAG "app_espnow"

/** NVS Namespace */
#define NVS_NAMESPACE "espnow_gw"

/** NVS Key: Node Count */
#define NVS_KEY_NODE_COUNT "node_cnt"

/**
 * NVS Key Prefix: Node Info
 * Actual key is "node_XX", where XX is the two-digit decimal node ID
 */
#define NVS_KEY_NODE_PREFIX "node_"

/** Broadcast Address */
static const uint8_t BROADCAST_MAC[APP_ESPNOW_MAC_LEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

/* ───────────────────────── NVS Persistence Structure ───────────────────────── */

/**
 * @brief Node record stored in NVS (Compact Layout)
 *
 * Only persists necessary fields. Runtime state (status, last_seen_ms, rssi) is not stored.
 */
typedef struct __attribute__((packed)) {
    uint8_t node_id;
    uint8_t mac[APP_ESPNOW_MAC_LEN];
    uint8_t device_type;
    uint8_t fw_version;
} nvs_node_record_t;

/* ───────────────────────── Internal Data Types ───────────────────────── */

/**
 * @brief Node Slot
 *
 * used == true means this ID is assigned.
 * Array index = node_id - 1.
 */
typedef struct {
    bool                   used;
    app_espnow_node_info_t info;
} node_slot_t;

/* ───────────────────────── Module Static Variables ───────────────────────── */

/** Module initialized flag */
static bool s_initialized = false;

/** Mutex protecting the node table and ESP-NOW operations */
static SemaphoreHandle_t s_mutex = NULL;

/** Node Table (index = node_id - 1) */
static node_slot_t s_nodes[APP_ESPNOW_MAX_NODES] = {0};

/** Registered Node Count (Cached, matches the number of used==true in s_nodes) */
static uint8_t s_node_count = 0;

/** Heartbeat Check Timer */
static TimerHandle_t s_heartbeat_timer = NULL;

/** Heartbeat Timeout Threshold (ms) */
static uint32_t s_heartbeat_timeout_ms = 60000;

/* ───────────────────── NVS Persistence Operations ───────────────────── */

/**
 * @brief Construct NVS key for a node
 */
static void make_nvs_key(uint8_t node_id, char *key_buf, size_t buf_size)
{
    snprintf(key_buf, buf_size, "%s%02u", NVS_KEY_NODE_PREFIX, node_id);
}

/**
 * @brief Save a single node to NVS
 */
static esp_err_t nvs_save_node(const app_espnow_node_info_t *info)
{
    nvs_node_record_t record = {
        .node_id = info->node_id,
        .device_type = info->device_type,
        .fw_version = info->fw_version,
    };
    memcpy(record.mac, info->mac, APP_ESPNOW_MAC_LEN);

    char key[16];
    make_nvs_key(info->node_id, key, sizeof(key));

    esp_err_t err = app_storage_set_blob(NVS_NAMESPACE, key, &record, sizeof(record));
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Save node %u failed: %s", info->node_id, esp_err_to_name(err));
        return err;
    }

    /* Update node count */
    err = app_storage_set_u8(NVS_NAMESPACE, NVS_KEY_NODE_COUNT, s_node_count);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Save node count failed: %s", esp_err_to_name(err));
    }

    return err;
}

/**
 * @brief Delete a single node from NVS
 */
static esp_err_t nvs_delete_node(uint8_t node_id)
{
    char key[16];
    make_nvs_key(node_id, key, sizeof(key));

    esp_err_t err = app_storage_erase_key(NVS_NAMESPACE, key);
    if (err == ESP_OK)
    {
        app_storage_set_u8(NVS_NAMESPACE, NVS_KEY_NODE_COUNT, s_node_count);
    }

    return err;
}

/**
 * @brief Restore all registered nodes from NVS
 *
 * Caller must hold s_mutex.
 */
static void nvs_load_all_nodes(void)
{
    uint8_t count = 0;
    esp_err_t err = app_storage_get_u8(NVS_NAMESPACE, NVS_KEY_NODE_COUNT, &count);
    if (err != ESP_OK || count == 0)
    {
        ESP_LOGI(TAG, "No saved nodes in NVS (first boot?)");
        return;
    }

    ESP_LOGI(TAG, "Loading %u nodes from NVS...", count);

    uint8_t loaded = 0;
    for (uint8_t id = APP_ESPNOW_NODE_ID_MIN; id <= APP_ESPNOW_NODE_ID_MAX; id++)
    {
        char key[16];
        make_nvs_key(id, key, sizeof(key));

        nvs_node_record_t record;
        size_t len = sizeof(record);
        err = app_storage_get_blob(NVS_NAMESPACE, key, &record, &len);
        if (err != ESP_OK)
        {
            continue;
        }

        uint8_t idx = id - 1;
        s_nodes[idx].used = true;
        s_nodes[idx].info.node_id = record.node_id;
        memcpy(s_nodes[idx].info.mac, record.mac, APP_ESPNOW_MAC_LEN);
        s_nodes[idx].info.device_type = record.device_type;
        s_nodes[idx].info.fw_version = record.fw_version;
        s_nodes[idx].info.status = APP_ESPNOW_NODE_OFFLINE;
        s_nodes[idx].info.last_seen_ms = 0;
        s_nodes[idx].info.rssi = 0;

        loaded++;
        ESP_LOGI(TAG, "  Loaded node %u: " MACSTR " type=%u fw=%u",
                 id, MAC2STR(record.mac), record.device_type, record.fw_version);
    }

    s_node_count = loaded;
    ESP_LOGI(TAG, "Loaded %u nodes from NVS", loaded);
}

/* ───────────────────── Node Table Operations (Caller holds lock) ───────────────────── */

/**
 * @brief Find registered node by MAC address
 * @return Node ID (1~MAX), 0 if not found
 */
static uint8_t find_node_by_mac_locked(const uint8_t *mac)
{
    for (uint8_t i = 0; i < APP_ESPNOW_MAX_NODES; i++)
    {
        if (s_nodes[i].used &&
            memcmp(s_nodes[i].info.mac, mac, APP_ESPNOW_MAC_LEN) == 0)
        {
            return s_nodes[i].info.node_id;
        }
    }
    return APP_ESPNOW_NODE_ID_INVALID;
}

/**
 * @brief Allocate a free node ID
 * @return Node ID (1~MAX), 0 if no free ID
 */
static uint8_t alloc_node_id_locked(void)
{
    for (uint8_t i = 0; i < APP_ESPNOW_MAX_NODES; i++)
    {
        if (!s_nodes[i].used)
        {
            return (uint8_t)(i + 1);
        }
    }
    return APP_ESPNOW_NODE_ID_INVALID;
}

/**
 * @brief Register new node to the table
 *
 * Caller must hold lock. s_node_count increments on success.
 *
 * @return Assigned Node ID, 0 on failure
 */
static uint8_t register_node_locked(const uint8_t *mac, uint8_t device_type,
                                     uint8_t fw_version, int rssi)
{
    /* Check if already registered (duplicate registration) */
    uint8_t existing_id = find_node_by_mac_locked(mac);
    if (existing_id != APP_ESPNOW_NODE_ID_INVALID)
    {
        /* Already registered, update info and return original ID */
        uint8_t idx = existing_id - 1;
        s_nodes[idx].info.device_type = device_type;
        s_nodes[idx].info.fw_version = fw_version;
        s_nodes[idx].info.status = APP_ESPNOW_NODE_ONLINE;
        s_nodes[idx].info.last_seen_ms = esp_timer_get_time() / 1000;
        s_nodes[idx].info.rssi = rssi;
        return existing_id;
    }

    /* Allocate new ID */
    uint8_t new_id = alloc_node_id_locked();
    if (new_id == APP_ESPNOW_NODE_ID_INVALID)
    {
        ESP_LOGW(TAG, "Node table full, cannot register " MACSTR, MAC2STR(mac));
        return APP_ESPNOW_NODE_ID_INVALID;
    }

    uint8_t idx = new_id - 1;
    s_nodes[idx].used = true;
    s_nodes[idx].info.node_id = new_id;
    memcpy(s_nodes[idx].info.mac, mac, APP_ESPNOW_MAC_LEN);
    s_nodes[idx].info.device_type = device_type;
    s_nodes[idx].info.fw_version = fw_version;
    s_nodes[idx].info.status = APP_ESPNOW_NODE_ONLINE;
    s_nodes[idx].info.last_seen_ms = esp_timer_get_time() / 1000;
    s_nodes[idx].info.rssi = rssi;

    s_node_count++;
    return new_id;
}

/**
 * @brief Update node last seen time (Caller holds lock)
 */
static void touch_node_locked(uint8_t node_id, int rssi)
{
    if (node_id < APP_ESPNOW_NODE_ID_MIN || node_id > APP_ESPNOW_NODE_ID_MAX)
    {
        return;
    }

    uint8_t idx = node_id - 1;
    if (!s_nodes[idx].used)
    {
        return;
    }

    int64_t now_ms = esp_timer_get_time() / 1000;
    bool was_offline = (s_nodes[idx].info.status == APP_ESPNOW_NODE_OFFLINE);

    s_nodes[idx].info.last_seen_ms = now_ms;
    s_nodes[idx].info.rssi = rssi;
    s_nodes[idx].info.status = APP_ESPNOW_NODE_ONLINE;

    /* Node recovered from offline, send online event */
    if (was_offline)
    {
        app_espnow_node_online_t evt = {
            .node = s_nodes[idx].info,
            .is_new = false,
        };
        app_event_post(APP_EVENT_ESPNOW_NODE_ONLINE, &evt, sizeof(evt));
    }
}

/* ───────────────────── ESP-NOW Peer Management Helpers ───────────────────── */

/**
 * @brief Ensure broadcast peer is added
 */
static esp_err_t ensure_broadcast_peer(void)
{
    if (!esp_now_is_peer_exist(BROADCAST_MAC))
    {
        esp_now_peer_info_t peer = {0};
        memcpy(peer.peer_addr, BROADCAST_MAC, APP_ESPNOW_MAC_LEN);
        peer.channel = 0;
        peer.ifidx = WIFI_IF_STA;
        peer.encrypt = false;
        return esp_now_add_peer(&peer);
    }
    return ESP_OK;
}

/**
 * @brief Ensure unicast peer is added for specific MAC
 */
static esp_err_t ensure_unicast_peer(const uint8_t *mac)
{
    if (!esp_now_is_peer_exist(mac))
    {
        esp_now_peer_info_t peer = {0};
        memcpy(peer.peer_addr, mac, APP_ESPNOW_MAC_LEN);
        peer.channel = 0;
        peer.ifidx = WIFI_IF_STA;
        peer.encrypt = false;
        return esp_now_add_peer(&peer);
    }
    return ESP_OK;
}

/* ───────────────────── Protocol Frame Sending ───────────────────── */

/**
 * @brief Send Register Response to Child Node
 */
static void send_register_resp(const uint8_t *dst_mac, uint8_t assigned_id, uint16_t seq)
{
    ensure_unicast_peer(dst_mac);

    app_protocol_register_resp_t resp = {
        .header = {
            .type = APP_PROTOCOL_MSG_REGISTER_RESP,
            .node_id = 0,
            .seq = seq,
        },
        .assigned_id = assigned_id,
    };

    esp_err_t err = esp_now_send(dst_mac, (const uint8_t *)&resp, sizeof(resp));
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Send REGISTER_RESP to " MACSTR " failed: %s",
                 MAC2STR(dst_mac), esp_err_to_name(err));
    }
    else
    {
        ESP_LOGI(TAG, "Sent REGISTER_RESP to " MACSTR " assigned_id=%u",
                 MAC2STR(dst_mac), assigned_id);
    }
}

/**
 * @brief Send Heartbeat Ack to Child Node
 */
static void send_heartbeat_ack(const uint8_t *dst_mac, uint8_t node_id, uint16_t seq)
{
    ensure_unicast_peer(dst_mac);

    app_protocol_heartbeat_ack_t ack = {
        .header = {
            .type = APP_PROTOCOL_MSG_HEARTBEAT_ACK,
            .node_id = node_id,
            .seq = seq,
        },
    };

    esp_err_t err = esp_now_send(dst_mac, (const uint8_t *)&ack, sizeof(ack));
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Send HEARTBEAT_ACK to " MACSTR " failed: %s",
                 MAC2STR(dst_mac), esp_err_to_name(err));
    }
}

/* ───────────────────── Protocol Frame Handling ───────────────────── */

/**
 * @brief Handle Register Request
 */
static void handle_register_req(const uint8_t *src_mac, const uint8_t *data,
                                 int data_len, int rssi)
{
    if (data_len < (int)sizeof(app_protocol_register_req_t))
    {
        ESP_LOGW(TAG, "REGISTER_REQ too short (%d)", data_len);
        return;
    }

    const app_protocol_register_req_t *req = (const app_protocol_register_req_t *)data;

    ESP_LOGI(TAG, "REGISTER_REQ from " MACSTR " type=%u fw=%u",
             MAC2STR(src_mac), req->device_type, req->fw_version);

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    /* Check if already registered */
    uint8_t existing_id = find_node_by_mac_locked(src_mac);
    bool is_new = (existing_id == APP_ESPNOW_NODE_ID_INVALID);

    uint8_t assigned_id = register_node_locked(src_mac, req->device_type,
                                                req->fw_version, rssi);

    if (assigned_id != APP_ESPNOW_NODE_ID_INVALID)
    {
        /* Move NVS persistence to event handler to execute asynchronously, avoiding WiFi task blockage */
        uint8_t idx = assigned_id - 1;
        
        /* Copy node info for event posting (use after releasing lock) */
        app_espnow_node_online_t evt = {
            .node = s_nodes[idx].info,
            .is_new = is_new,
        };

        xSemaphoreGive(s_mutex);

        /* Send Register Response */
        send_register_resp(src_mac, assigned_id, req->header.seq);

        /* Post Node Online Event */
        app_event_post(APP_EVENT_ESPNOW_NODE_ONLINE, &evt, sizeof(evt));
    }
    else
    {
        xSemaphoreGive(s_mutex);

        /* Node table full, reply ID=0 to reject */
        send_register_resp(src_mac, APP_ESPNOW_NODE_ID_INVALID, req->header.seq);
    }
}

/**
 * @brief Handle Heartbeat
 */
static void handle_heartbeat(const uint8_t *src_mac, const uint8_t *data,
                              int data_len, int rssi)
{
    if (data_len < (int)sizeof(app_protocol_heartbeat_t))
    {
        ESP_LOGW(TAG, "HEARTBEAT too short (%d)", data_len);
        return;
    }

    const app_protocol_heartbeat_t *hb = (const app_protocol_heartbeat_t *)data;
    uint8_t node_id = hb->header.node_id;

    ESP_LOGD(TAG, "HEARTBEAT from " MACSTR " node_id=%u", MAC2STR(src_mac), node_id);

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    /* Verify Node ID and MAC match */
    if (node_id >= APP_ESPNOW_NODE_ID_MIN && node_id <= APP_ESPNOW_NODE_ID_MAX)
    {
        uint8_t idx = node_id - 1;
        if (s_nodes[idx].used &&
            memcmp(s_nodes[idx].info.mac, src_mac, APP_ESPNOW_MAC_LEN) == 0)
        {
            touch_node_locked(node_id, rssi);
            xSemaphoreGive(s_mutex);

            send_heartbeat_ack(src_mac, node_id, hb->header.seq);
            return;
        }
    }

    xSemaphoreGive(s_mutex);
    ESP_LOGW(TAG, "HEARTBEAT from unknown/mismatched node_id=%u " MACSTR,
             node_id, MAC2STR(src_mac));
}

/**
 * @brief Handle Data Report
 */
static void handle_data_report(const uint8_t *src_mac, const uint8_t *data,
                                int data_len, int rssi)
{
    if (data_len < (int)sizeof(app_protocol_header_t) + sizeof(uint16_t))
    {
        ESP_LOGW(TAG, "DATA_REPORT too short (%d)", data_len);
        return;
    }

    const app_protocol_data_report_t *report = (const app_protocol_data_report_t *)data;
    uint8_t node_id = report->header.node_id;

    if (report->data_len > APP_PROTOCOL_USER_DATA_MAX_LEN)
    {
        ESP_LOGW(TAG, "DATA_REPORT data_len too large (%u)", report->data_len);
        return;
    }

    int expected_len = (int)(sizeof(app_protocol_header_t) + sizeof(uint16_t) + report->data_len);
    if (data_len < expected_len)
    {
        ESP_LOGW(TAG, "DATA_REPORT truncated: got %d, expected %d", data_len, expected_len);
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    /* Verify Node */
    if (node_id >= APP_ESPNOW_NODE_ID_MIN && node_id <= APP_ESPNOW_NODE_ID_MAX)
    {
        uint8_t idx = node_id - 1;
        if (s_nodes[idx].used &&
            memcmp(s_nodes[idx].info.mac, src_mac, APP_ESPNOW_MAC_LEN) == 0)
        {
            touch_node_locked(node_id, rssi);
            xSemaphoreGive(s_mutex);

            /* Construct event data and post */
            app_espnow_node_data_t evt = {0};
            evt.node_id = node_id;
            memcpy(evt.src_addr, src_mac, APP_ESPNOW_MAC_LEN);
            evt.rssi = rssi;
            evt.data_len = report->data_len;
            memcpy(evt.data, report->data, report->data_len);

            app_event_post(APP_EVENT_ESPNOW_NODE_DATA, &evt, sizeof(evt));
            return;
        }
    }

    xSemaphoreGive(s_mutex);
    ESP_LOGW(TAG, "DATA_REPORT from unregistered node_id=%u " MACSTR,
             node_id, MAC2STR(src_mac));
}

/* ───────────────────── ESP-NOW Callbacks ───────────────────── */

/**
 * @brief ESP-NOW Receive Callback
 *
 * Executes in WiFi Task Context, DO NOT perform blocking operations.
 * Dispatches to handlers based on protocol frame type.
 */
static void on_espnow_recv(const esp_now_recv_info_t *recv_info,
                            const uint8_t *data,
                            int data_len)
{
    if (recv_info == NULL || data == NULL || data_len < (int)sizeof(app_protocol_header_t))
    {
        ESP_LOGW(TAG, "Invalid recv: data_len=%d", data_len);
        return;
    }

    const app_protocol_header_t *header = (const app_protocol_header_t *)data;
    int rssi = recv_info->rx_ctrl->rssi;

    switch ((app_protocol_msg_type_t)header->type)
    {
    case APP_PROTOCOL_MSG_REGISTER_REQ:
        handle_register_req(recv_info->src_addr, data, data_len, rssi);
        break;

    case APP_PROTOCOL_MSG_HEARTBEAT:
        handle_heartbeat(recv_info->src_addr, data, data_len, rssi);
        break;

    case APP_PROTOCOL_MSG_DATA_REPORT:
        handle_data_report(recv_info->src_addr, data, data_len, rssi);
        break;

    default:
        ESP_LOGW(TAG, "Unknown msg type 0x%02X from " MACSTR,
                 header->type, MAC2STR(recv_info->src_addr));
        break;
    }
}

/**
 * @brief ESP-NOW Send Callback
 */
static void on_espnow_send(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)
{
    if (tx_info == NULL)
    {
        return;
    }

    const uint8_t *mac_addr = tx_info->des_addr;

    if (status != ESP_NOW_SEND_SUCCESS)
    {
        ESP_LOGW(TAG, "Send to " MACSTR " failed", MAC2STR(mac_addr));
    }
}

/* ───────────────────── Heartbeat Check Timer ───────────────────── */

/**
 * @brief Heartbeat Timeout Check Callback
 *
 * Executes in Timer Service Task Context.
 * Iterates through node table, marking online nodes as offline if heartbeat timed out.
 */
static void heartbeat_check_callback(TimerHandle_t timer)
{
    (void)timer;

    int64_t now_ms = esp_timer_get_time() / 1000;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    for (uint8_t i = 0; i < APP_ESPNOW_MAX_NODES; i++)
    {
        if (!s_nodes[i].used || s_nodes[i].info.status != APP_ESPNOW_NODE_ONLINE)
        {
            continue;
        }

        int64_t elapsed_ms = now_ms - s_nodes[i].info.last_seen_ms;
        if (elapsed_ms > (int64_t)s_heartbeat_timeout_ms)
        {
            s_nodes[i].info.status = APP_ESPNOW_NODE_OFFLINE;

            ESP_LOGW(TAG, "Node %u (" MACSTR ") heartbeat timeout (%" PRId64 " ms)",
                     s_nodes[i].info.node_id, MAC2STR(s_nodes[i].info.mac), elapsed_ms);

            /* Copy info for event posting */
            app_espnow_node_offline_t evt = {
                .node = s_nodes[i].info,
            };

            /* Post event inside lock (app_event_post copies data, won't block long) */
            app_event_post(APP_EVENT_ESPNOW_NODE_OFFLINE, &evt, sizeof(evt));
        }
    }

    xSemaphoreGive(s_mutex);
}

/* ───────────────────── Internal Event Handler ───────────────────── */

/**
 * @brief Internal event handler for logic requiring asynchronous execution (e.g. NVS operations)
 */
static void app_espnow_event_handler(void *arg, esp_event_base_t event_base,
                                     int32_t event_id, void *event_data)
{
    if (event_base == APP_EVENT_BASE && event_id == APP_EVENT_ESPNOW_NODE_ONLINE)
    {
        app_espnow_node_online_t *evt = (app_espnow_node_online_t *)event_data;
        /* When node goes online (register or reconnect), save node info to NVS */
        /* Note: This function executes in Event Loop task, will not block WiFi task */
        nvs_save_node(&evt->node);
    }
}

/* ───────────────────── Public API ───────────────────── */

esp_err_t app_espnow_init(const app_espnow_config_t *config)
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

    esp_err_t err;

    /* ── Save Configuration ── */

    s_heartbeat_timeout_ms = config->heartbeat_timeout_s * 1000;
    uint32_t check_period_ms = config->heartbeat_check_s * 1000;

    /* ── Create Mutex ── */

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL)
    {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    /* ── Initialize Node Table ── */

    memset(s_nodes, 0, sizeof(s_nodes));
    s_node_count = 0;

    /* ── Restore Node Table from NVS ── */

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    nvs_load_all_nodes();
    xSemaphoreGive(s_mutex);

    /* ── Register Internal Event Handler ── */
    err = app_event_handler_register(APP_EVENT_ESPNOW_NODE_ONLINE, app_espnow_event_handler, NULL);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to register internal event handler: %s", esp_err_to_name(err));
        goto cleanup_mutex;
    }

    /* ── Initialize ESP-NOW ── */

    err = esp_now_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_now_init failed: %s", esp_err_to_name(err));
        goto cleanup_handler;
    }

    /* ── Set Primary Master Key (PMK) ── */

    if (config->pmk != NULL)
    {
        err = esp_now_set_pmk(config->pmk);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_now_set_pmk failed: %s", esp_err_to_name(err));
            goto cleanup_espnow;
        }
        ESP_LOGI(TAG, "PMK configured");
    }

    /* ── Register Callbacks ── */

    err = esp_now_register_recv_cb(on_espnow_recv);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Register recv callback failed: %s", esp_err_to_name(err));
        goto cleanup_espnow;
    }

    err = esp_now_register_send_cb(on_espnow_send);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Register send callback failed: %s", esp_err_to_name(err));
        goto cleanup_recv_cb;
    }

    /* ── Add Broadcast Peer ── */

    err = ensure_broadcast_peer();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Add broadcast peer failed: %s", esp_err_to_name(err));
        goto cleanup_send_cb;
    }

    /* ── Add ESP-NOW peers for restored nodes ── */

    for (uint8_t i = 0; i < APP_ESPNOW_MAX_NODES; i++)
    {
        if (s_nodes[i].used)
        {
            ensure_unicast_peer(s_nodes[i].info.mac);
        }
    }

    /* ── Create Heartbeat Check Timer ── */

    s_heartbeat_timer = xTimerCreate(
        "espnow_hb",
        pdMS_TO_TICKS(check_period_ms),
        pdTRUE, /* Auto-reload */
        NULL,
        heartbeat_check_callback);

    if (s_heartbeat_timer == NULL)
    {
        ESP_LOGE(TAG, "Failed to create heartbeat timer");
        err = ESP_ERR_NO_MEM;
        goto cleanup_send_cb;
    }

    if (xTimerStart(s_heartbeat_timer, pdMS_TO_TICKS(1000)) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to start heartbeat timer");
        err = ESP_FAIL;
        goto cleanup_timer;
    }

    /* Print STA MAC Address */
    uint8_t mac[APP_ESPNOW_MAC_LEN];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    ESP_LOGI(TAG, "ESP-NOW Gateway initialized, STA MAC: " MACSTR, MAC2STR(mac));
    ESP_LOGI(TAG, "  Registered nodes: %u, HB timeout: %" PRIu32 "s, HB check: %" PRIu32 "s",
             s_node_count, config->heartbeat_timeout_s, config->heartbeat_check_s);

    s_initialized = true;
    return ESP_OK;

    /* ── Error Rollback ── */

cleanup_timer:
    xTimerDelete(s_heartbeat_timer, portMAX_DELAY);
    s_heartbeat_timer = NULL;

cleanup_send_cb:
    esp_now_unregister_send_cb();

cleanup_recv_cb:
    esp_now_unregister_recv_cb();

cleanup_espnow:
    esp_now_deinit();

cleanup_handler:
    app_event_handler_unregister(APP_EVENT_ESPNOW_NODE_ONLINE, app_espnow_event_handler);

cleanup_mutex:
    vSemaphoreDelete(s_mutex);
    s_mutex = NULL;

    return err;
}

esp_err_t app_espnow_deinit(void)
{
    if (!s_initialized)
    {
        ESP_LOGW(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Deinitializing ESP-NOW Gateway...");

    /* Stop Heartbeat Timer */
    if (s_heartbeat_timer != NULL)
    {
        xTimerStop(s_heartbeat_timer, portMAX_DELAY);
        xTimerDelete(s_heartbeat_timer, portMAX_DELAY);
        s_heartbeat_timer = NULL;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    esp_now_unregister_recv_cb();
    esp_now_unregister_send_cb();
    esp_now_deinit();
    
    app_event_handler_unregister(APP_EVENT_ESPNOW_NODE_ONLINE, app_espnow_event_handler);

    memset(s_nodes, 0, sizeof(s_nodes));
    s_node_count = 0;

    xSemaphoreGive(s_mutex);

    vSemaphoreDelete(s_mutex);
    s_mutex = NULL;

    s_initialized = false;
    ESP_LOGI(TAG, "ESP-NOW Gateway deinitialized");
    return ESP_OK;
}

uint8_t app_espnow_get_node_count(void)
{
    if (!s_initialized)
    {
        return 0;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint8_t count = s_node_count;
    xSemaphoreGive(s_mutex);

    return count;
}

esp_err_t app_espnow_get_node_info(uint8_t node_id, app_espnow_node_info_t *info)
{
    if (info == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (node_id < APP_ESPNOW_NODE_ID_MIN || node_id > APP_ESPNOW_NODE_ID_MAX)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    uint8_t idx = node_id - 1;
    if (!s_nodes[idx].used)
    {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    *info = s_nodes[idx].info;

    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t app_espnow_get_all_nodes(app_espnow_node_info_t *nodes, uint8_t max_count, uint8_t *count)
{
    if (nodes == NULL || count == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_initialized)
    {
        *count = 0;
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    uint8_t filled = 0;
    for (uint8_t i = 0; i < APP_ESPNOW_MAX_NODES && filled < max_count; i++)
    {
        if (s_nodes[i].used)
        {
            nodes[filled] = s_nodes[i].info;
            filled++;
        }
    }

    xSemaphoreGive(s_mutex);

    *count = filled;
    return ESP_OK;
}

esp_err_t app_espnow_remove_node(uint8_t node_id)
{
    if (node_id < APP_ESPNOW_NODE_ID_MIN || node_id > APP_ESPNOW_NODE_ID_MAX)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    uint8_t idx = node_id - 1;
    if (!s_nodes[idx].used)
    {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t mac[APP_ESPNOW_MAC_LEN];
    memcpy(mac, s_nodes[idx].info.mac, APP_ESPNOW_MAC_LEN);

    /* Clear node slot */
    memset(&s_nodes[idx], 0, sizeof(node_slot_t));
    s_node_count--;

    xSemaphoreGive(s_mutex);

    /* Delete from NVS */
    nvs_delete_node(node_id);

    /* Delete from ESP-NOW peer list */
    if (esp_now_is_peer_exist(mac))
    {
        esp_now_del_peer(mac);
    }

    ESP_LOGI(TAG, "Node %u (" MACSTR ") removed", node_id, MAC2STR(mac));
    return ESP_OK;
}

esp_err_t app_espnow_send(const uint8_t *peer_addr, const uint8_t *data, uint16_t len)
{
    if (data == NULL)
    {
        ESP_LOGE(TAG, "send: data is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (len == 0 || len > APP_PROTOCOL_DATA_MAX_LEN)
    {
        ESP_LOGE(TAG, "send: invalid data length (%d)", len);
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_initialized)
    {
        ESP_LOGW(TAG, "send: module not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (peer_addr != NULL)
    {
        ensure_unicast_peer(peer_addr);
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_err_t err = esp_now_send(peer_addr, data, len);
    xSemaphoreGive(s_mutex);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Send failed: %s", esp_err_to_name(err));
    }

    return err;
}
