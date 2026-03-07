#include "app_espnow.h"
#include "app_event.h"
#include "app_storage.h"
#include "app_protocol.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include <string.h>
#include <stdatomic.h>
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

/** Default mutex timeout for timer task */
#define MUTEX_TIMEOUT_TIMER_MS 50

/** RX queue depth */
#define RX_QUEUE_DEPTH 8

/** RX processing task stack size */
#define RX_TASK_STACK_SIZE 4096

/** RX processing task priority */
#define RX_TASK_PRIORITY 5

/* ───────────────────────── NVS Persistence Structure ───────────────────────── */

/**
 * @brief Node record stored in NVS (Compact Layout)
 *
 * Only persists necessary fields. Runtime state (status, last_seen_ms, rssi) is not stored.
 */
typedef struct __attribute__((packed))
{
    uint8_t node_id;
    uint8_t mac[APP_ESPNOW_MAC_LEN];
    uint8_t device_type;
    uint8_t fw_version;
} nvs_node_record_t;

/* ───────────────────────── Internal Data Types ───────────────────────── */

/**
 * @brief RX queue item
 */
typedef struct
{
    uint8_t src_addr[APP_ESPNOW_MAC_LEN];
    int rssi;
    int data_len;
    uint8_t data[ESP_NOW_MAX_DATA_LEN];
} rx_item_t;

/**
 * @brief Node Slot
 *
 * used == true means this ID is assigned.
 * Array index = node_id - 1.
 */
typedef struct
{
    bool used;
    app_espnow_node_info_t info;
} node_slot_t;

/* ───────────────────────── Module Static Variables ───────────────────────── */

/** Module initialized flag (atomic for lock-free reads in public API) */
static atomic_bool s_initialized = false;

/** Mutex protecting the node table and ESP-NOW operations */
static SemaphoreHandle_t s_mutex = NULL;

/** RX queue for decoupling recv_cb from protocol handling */
static QueueHandle_t s_rx_queue = NULL;

/** RX processing task handle */
static TaskHandle_t s_rx_task = NULL;

/** Node Table (index = node_id - 1) */
static node_slot_t s_nodes[APP_ESPNOW_MAX_NODES] = {0};

/** Registered Node Count (Cached, matches the number of used==true in s_nodes) */
static uint8_t s_node_count = 0;

/** Heartbeat Check Timer */
static TimerHandle_t s_heartbeat_timer = NULL;

/** Heartbeat Timeout Threshold (ms) */
static uint32_t s_heartbeat_timeout_ms = 30000;

/** Send queue: frames to be sent by the dedicated send task */
static QueueHandle_t s_send_queue = NULL;

/** Dedicated send task handle */
static TaskHandle_t s_send_task = NULL;

/** Maximum frame size that can be queued for sending */
#define SEND_QUEUE_FRAME_MAX_LEN 250

/** Send queue depth */
#define SEND_QUEUE_DEPTH 8

/**
 * @brief Item stored in the send queue
 *
 * A special sentinel (len == 0) signals the send task to exit gracefully.
 */
typedef struct
{
    uint8_t dst_mac[APP_ESPNOW_MAC_LEN];
    uint16_t len; /**< 0 = shutdown sentinel */
    uint8_t data[SEND_QUEUE_FRAME_MAX_LEN];
} send_queue_item_t;

/* ───────────────────── NVS Persistence Operations ───────────────────── */

/**
 * @brief Construct NVS key for a node
 */
static void make_nvs_key(uint8_t node_id, char *key_buf, size_t buf_size)
{
    snprintf(key_buf, buf_size, "%s%02u", NVS_KEY_NODE_PREFIX, node_id);
}

/**
 * @brief Save a single node to NVS and update node count atomically
 *
 * Both the node blob and node count are written. If the blob write succeeds
 * but the count write fails, the next boot will re-scan and self-heal.
 *
 * @param node_count  Current node count (caller reads under lock before calling)
 */
static esp_err_t nvs_save_node(const app_espnow_node_info_t *info, uint8_t node_count)
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

    err = app_storage_set_u8(NVS_NAMESPACE, NVS_KEY_NODE_COUNT, node_count);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Save node count failed: %s", esp_err_to_name(err));
    }

    return err;
}

/**
 * @brief Delete a single node from NVS
 *
 * @param node_count  Current node count (caller reads under lock before calling)
 */
static esp_err_t nvs_delete_node(uint8_t node_id, uint8_t node_count)
{
    char key[16];
    make_nvs_key(node_id, key, sizeof(key));

    esp_err_t err = app_storage_erase_key(NVS_NAMESPACE, key);
    if (err == ESP_OK)
    {
        app_storage_set_u8(NVS_NAMESPACE, NVS_KEY_NODE_COUNT, node_count);
    }

    return err;
}

/**
 * @brief Restore all registered nodes from NVS
 *
 * Caller must hold s_mutex. Self-heals count mismatches by scanning all
 * possible IDs and using the actual loaded count.
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

    ESP_LOGI(TAG, "NVS reports %u nodes, scanning...", count);

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

        /* Validate record consistency */
        if (record.node_id != id)
        {
            ESP_LOGW(TAG, "NVS node key %u has mismatched node_id %u, skipping", id, record.node_id);
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

    /* Self-heal: if NVS count was wrong, correct it */
    if (loaded != count)
    {
        ESP_LOGW(TAG, "NVS count mismatch (stored=%u, actual=%u), correcting", count, loaded);
        app_storage_set_u8(NVS_NAMESPACE, NVS_KEY_NODE_COUNT, loaded);
    }

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
 * Caller must hold lock. s_node_count increments on success (new node only).
 *
 * @param[out] out_is_new  Set to true if this is a brand new registration
 * @param[out] out_info_changed  Set to true if an existing node's info (type/fw) was updated
 * @return Assigned Node ID, 0 on failure
 */
static uint8_t register_node_locked(const uint8_t *mac, uint8_t device_type,
                                    uint8_t fw_version, int rssi,
                                    bool *out_is_new, bool *out_info_changed)
{
    *out_is_new = false;
    *out_info_changed = false;

    /* Check if already registered (duplicate registration) */
    uint8_t existing_id = find_node_by_mac_locked(mac);
    if (existing_id != APP_ESPNOW_NODE_ID_INVALID)
    {
        /* Already registered, check if info changed */
        uint8_t idx = existing_id - 1;

        if (s_nodes[idx].info.device_type != device_type ||
            s_nodes[idx].info.fw_version != fw_version)
        {
            s_nodes[idx].info.device_type = device_type;
            s_nodes[idx].info.fw_version = fw_version;
            *out_info_changed = true;
        }

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
    *out_is_new = true;
    return new_id;
}

/**
 * @brief Update node last seen time (Caller holds lock)
 * @return true if the node was previously offline, false otherwise
 */
static bool touch_node_locked(uint8_t node_id, int rssi)
{
    if (node_id < APP_ESPNOW_NODE_ID_MIN || node_id > APP_ESPNOW_NODE_ID_MAX)
    {
        return false;
    }

    uint8_t idx = node_id - 1;
    if (!s_nodes[idx].used)
    {
        return false;
    }

    int64_t now_ms = esp_timer_get_time() / 1000;
    bool was_offline = (s_nodes[idx].info.status == APP_ESPNOW_NODE_OFFLINE);

    s_nodes[idx].info.last_seen_ms = now_ms;
    s_nodes[idx].info.rssi = rssi;
    s_nodes[idx].info.status = APP_ESPNOW_NODE_ONLINE;

    return was_offline;
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

/* ─────────────────────────────────────────── Send Task ─────────────────────────────────────────── */

/**
 * @brief Dedicated ESP-NOW send task
 *
 * esp_now_send() MUST NOT be called from the WiFi Task (recv_cb context).
 * This task drains the send queue and performs the actual transmission.
 *
 * Exits gracefully when it receives a sentinel item (len == 0).
 */
static void espnow_send_task(void *arg)
{
    (void)arg;
    send_queue_item_t item;

    while (true)
    {
        if (xQueueReceive(s_send_queue, &item, portMAX_DELAY) == pdTRUE)
        {
            /* Shutdown sentinel: len == 0 means exit */
            if (item.len == 0)
            {
                ESP_LOGI(TAG, "Send task received shutdown sentinel, exiting");
                break;
            }

            ensure_unicast_peer(item.dst_mac);
            esp_err_t err = esp_now_send(item.dst_mac, item.data, item.len);
            if (err != ESP_OK)
            {
                ESP_LOGW(TAG, "esp_now_send to " MACSTR " failed: %s",
                         MAC2STR(item.dst_mac), esp_err_to_name(err));
            }
        }
    }

    /* Notify deinit that we have exited cleanly */
    s_send_task = NULL;
    vTaskDelete(NULL);
}

/**
 * @brief Enqueue a frame for sending via the dedicated send task
 *
 * Safe to call from any context including the WiFi Task recv_cb.
 *
 * @param ticks_to_wait  Queue wait ticks (0 for non-blocking from ISR/callback context)
 * @return true if enqueued successfully, false otherwise
 */
static bool enqueue_send(const uint8_t *dst_mac, const void *frame, uint16_t len,
                         TickType_t ticks_to_wait)
{
    if (len > SEND_QUEUE_FRAME_MAX_LEN)
    {
        ESP_LOGE(TAG, "Frame too large to enqueue (%u bytes)", len);
        return false;
    }

    if (s_send_queue == NULL)
    {
        ESP_LOGE(TAG, "Send queue not available");
        return false;
    }

    send_queue_item_t item;
    memcpy(item.dst_mac, dst_mac, APP_ESPNOW_MAC_LEN);
    item.len = len;
    memcpy(item.data, frame, len);

    if (xQueueSend(s_send_queue, &item, ticks_to_wait) != pdTRUE)
    {
        ESP_LOGW(TAG, "Send queue full, dropping frame to " MACSTR, MAC2STR(dst_mac));
        return false;
    }
    return true;
}

/* ─────────────────────────────────────────── Protocol Frame Sending ─────────────────────────────────────────── */

/**
 * @brief Enqueue Register Response to Child Node
 *
 * Critical frame: uses a short timeout to increase delivery chance.
 */
static void send_register_resp(const uint8_t *dst_mac, uint8_t assigned_id, uint16_t seq)
{
    uint8_t primary_ch = 0;
    wifi_second_chan_t second_ch = WIFI_SECOND_CHAN_NONE;
    esp_wifi_get_channel(&primary_ch, &second_ch);

    app_protocol_register_resp_t resp = {
        .header = {
            .type = APP_PROTOCOL_MSG_REGISTER_RESP,
            .node_id = 0,
            .seq = seq,
        },
        .assigned_id = assigned_id,
        .channel = primary_ch,
    };

    ESP_LOGI(TAG, "Queuing REGISTER_RESP to " MACSTR " assigned_id=%u channel=%u",
             MAC2STR(dst_mac), assigned_id, primary_ch);
    enqueue_send(dst_mac, &resp, sizeof(resp), pdMS_TO_TICKS(50));
}

/**
 * @brief Enqueue Heartbeat Ack to Child Node
 */
static void send_heartbeat_ack(const uint8_t *dst_mac, uint8_t node_id, uint16_t seq)
{
    app_protocol_heartbeat_ack_t ack = {
        .header = {
            .type = APP_PROTOCOL_MSG_HEARTBEAT_ACK,
            .node_id = node_id,
            .seq = seq,
        },
    };

    ESP_LOGD(TAG, "Queuing HEARTBEAT_ACK to " MACSTR " node_id=%u",
             MAC2STR(dst_mac), node_id);
    enqueue_send(dst_mac, &ack, sizeof(ack), 0);
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

    /* Runs in RX task context — safe to block */
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE)
    {
        ESP_LOGW(TAG, "Failed to take mutex in handle_register_req, dropping packet");
        return;
    }

    bool is_new = false;
    bool info_changed = false;

    uint8_t assigned_id = register_node_locked(src_mac, req->device_type,
                                               req->fw_version, rssi,
                                               &is_new, &info_changed);

    if (assigned_id != APP_ESPNOW_NODE_ID_INVALID)
    {
        uint8_t idx = assigned_id - 1;

        /* Copy node info and count for use outside lock */
        app_espnow_node_online_t evt = {
            .node = s_nodes[idx].info,
            .is_new = is_new,
            .info_changed = info_changed,
            .node_count = s_node_count,
        };

        xSemaphoreGive(s_mutex);

        /* Send Register Response */
        send_register_resp(src_mac, assigned_id, req->header.seq);

        /* NVS persistence is handled uniformly in app_espnow_event_handler */
        if (app_event_post_with_timeout(APP_EVENT_ESPNOW_NODE_ONLINE, &evt, sizeof(evt), pdMS_TO_TICKS(10)) != ESP_OK)
        {
            ESP_LOGW(TAG, "Failed to post NODE_ONLINE event (queue full)");
        }
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

    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE)
    {
        ESP_LOGW(TAG, "Failed to take mutex in handle_heartbeat, dropping packet");
        return;
    }

    /* Verify Node ID and MAC match */
    if (node_id >= APP_ESPNOW_NODE_ID_MIN && node_id <= APP_ESPNOW_NODE_ID_MAX)
    {
        uint8_t idx = node_id - 1;
        if (s_nodes[idx].used &&
            memcmp(s_nodes[idx].info.mac, src_mac, APP_ESPNOW_MAC_LEN) == 0)
        {
            bool was_offline = touch_node_locked(node_id, rssi);
            app_espnow_node_info_t node_info_copy = s_nodes[idx].info;
            xSemaphoreGive(s_mutex);

            send_heartbeat_ack(src_mac, node_id, hb->header.seq);

            if (was_offline)
            {
                app_espnow_node_online_t evt = {
                    .node = node_info_copy,
                    .is_new = false,
                };
                if (app_event_post_with_timeout(APP_EVENT_ESPNOW_NODE_ONLINE, &evt, sizeof(evt), 0) != ESP_OK)
                {
                    ESP_LOGW(TAG, "Failed to post NODE_ONLINE event (queue full)");
                }
            }
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
    if (data_len < (int)(sizeof(app_protocol_header_t) + sizeof(uint16_t)))
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

    /* Validate total frame length matches declared payload */
    int expected_min_len = (int)(sizeof(app_protocol_header_t) + sizeof(uint16_t) + report->data_len);
    if (data_len < expected_min_len)
    {
        ESP_LOGW(TAG, "DATA_REPORT truncated: data_len=%d, expected>=%d", data_len, expected_min_len);
        return;
    }

    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE)
    {
        ESP_LOGW(TAG, "Failed to take mutex in handle_data_report, dropping packet");
        return;
    }

    /* Verify Node */
    if (node_id >= APP_ESPNOW_NODE_ID_MIN && node_id <= APP_ESPNOW_NODE_ID_MAX)
    {
        uint8_t idx = node_id - 1;
        if (s_nodes[idx].used &&
            memcmp(s_nodes[idx].info.mac, src_mac, APP_ESPNOW_MAC_LEN) == 0)
        {
            bool was_offline = touch_node_locked(node_id, rssi);
            app_espnow_node_info_t node_info_copy = s_nodes[idx].info;
            xSemaphoreGive(s_mutex);

            /* Post node online event if it just recovered */
            if (was_offline)
            {
                app_espnow_node_online_t online_evt = {
                    .node = node_info_copy,
                    .is_new = false,
                };
                if (app_event_post_with_timeout(APP_EVENT_ESPNOW_NODE_ONLINE, &online_evt, sizeof(online_evt), pdMS_TO_TICKS(10)) != ESP_OK)
                {
                    ESP_LOGW(TAG, "Failed to post NODE_ONLINE event (queue full)");
                }
            }

            /* Construct and post data event */
            app_espnow_node_data_t data_evt = {0};
            data_evt.node_id = node_id;
            memcpy(data_evt.src_addr, src_mac, APP_ESPNOW_MAC_LEN);
            data_evt.rssi = rssi;
            data_evt.data_len = report->data_len;
            memcpy(data_evt.data, report->data, report->data_len);

            if (app_event_post_with_timeout(APP_EVENT_ESPNOW_NODE_DATA, &data_evt, sizeof(data_evt), pdMS_TO_TICKS(10)) != ESP_OK)
            {
                ESP_LOGW(TAG, "Failed to post NODE_DATA event (queue full)");
            }
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
 * Executes in WiFi Task Context — only enqueue, no blocking operations.
 */
static void app_espnow_recv_cb(const esp_now_recv_info_t *recv_info,
                               const uint8_t *data,
                               int data_len)
{
    if (recv_info == NULL || data == NULL || data_len < (int)sizeof(app_protocol_header_t))
    {
        ESP_LOGW(TAG, "Invalid recv: data_len=%d", data_len);
        return;
    }

    if (s_rx_queue == NULL)
        return;

    rx_item_t item;
    memcpy(item.src_addr, recv_info->src_addr, APP_ESPNOW_MAC_LEN);
    item.rssi = recv_info->rx_ctrl->rssi;
    item.data_len = (data_len > (int)sizeof(item.data)) ? (int)sizeof(item.data) : data_len;
    memcpy(item.data, data, item.data_len);

    if (xQueueSend(s_rx_queue, &item, 0) != pdTRUE)
    {
        ESP_LOGW(TAG, "RX queue full, dropping packet from " MACSTR,
                 MAC2STR(recv_info->src_addr));
    }
}

/**
 * @brief Dedicated RX processing task
 *
 * Drains the RX queue and dispatches to protocol handlers.
 * Runs in its own task context so handlers can safely block on mutex.
 */
static void espnow_rx_task(void *arg)
{
    (void)arg;
    rx_item_t item;

    while (true)
    {
        if (xQueueReceive(s_rx_queue, &item, portMAX_DELAY) != pdTRUE)
            continue;

        const app_protocol_header_t *header = (const app_protocol_header_t *)item.data;

        switch ((app_protocol_msg_type_t)header->type)
        {
        case APP_PROTOCOL_MSG_REGISTER_REQ:
            handle_register_req(item.src_addr, item.data, item.data_len, item.rssi);
            break;

        case APP_PROTOCOL_MSG_HEARTBEAT:
            handle_heartbeat(item.src_addr, item.data, item.data_len, item.rssi);
            break;

        case APP_PROTOCOL_MSG_DATA_REPORT:
            handle_data_report(item.src_addr, item.data, item.data_len, item.rssi);
            break;

        default:
            ESP_LOGW(TAG, "Unknown msg type 0x%02X from " MACSTR,
                     header->type, MAC2STR(item.src_addr));
            break;
        }
    }
}

/**
 * @brief ESP-NOW Send Callback
 */
static void app_espnow_send_cb(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)
{
    if (tx_info == NULL)
    {
        return;
    }

    if (status == ESP_NOW_SEND_SUCCESS)
    {
        ESP_LOGI(TAG, "Send to " MACSTR " OK", MAC2STR(tx_info->des_addr));
    }
    else
    {
        ESP_LOGW(TAG, "Send to " MACSTR " failed", MAC2STR(tx_info->des_addr));
    }
}

/* ───────────────────── Heartbeat Check Timer ───────────────────── */

/**
 * @brief Heartbeat Timeout Check Callback
 *
 * Executes in Timer Service Task Context.
 * Iterates through node table, marking online nodes as offline if heartbeat timed out.
 *
 * Uses a static buffer to avoid malloc in timer context. Protected by the timer
 * being single-threaded (only one instance of this callback runs at a time).
 */
static void heartbeat_check_callback(TimerHandle_t timer)
{
    (void)timer;

    int64_t now_ms = esp_timer_get_time() / 1000;

    /* Static buffer — safe because Timer Service Task is single-threaded
     * and this callback is only registered once */
    static app_espnow_node_offline_t offline_evts[APP_ESPNOW_MAX_NODES];
    uint8_t offline_count = 0;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_TIMER_MS)) != pdTRUE)
    {
        ESP_LOGW(TAG, "Heartbeat check: failed to take mutex, skipping this cycle");
        return;
    }

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
            offline_evts[offline_count].node = s_nodes[i].info;
            offline_count++;
        }
    }

    xSemaphoreGive(s_mutex);

    /* Post events after releasing the lock to prevent deadlock */
    for (uint8_t i = 0; i < offline_count; i++)
    {
        ESP_LOGW(TAG, "Node %u (" MACSTR ") heartbeat timeout",
                 offline_evts[i].node.node_id, MAC2STR(offline_evts[i].node.mac));

        if (app_event_post_with_timeout(APP_EVENT_ESPNOW_NODE_OFFLINE, &offline_evts[i],
                                        sizeof(app_espnow_node_offline_t), pdMS_TO_TICKS(10)) != ESP_OK)
        {
            ESP_LOGW(TAG, "Failed to post NODE_OFFLINE event (queue full)");
        }
    }
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

        /* Persist to NVS for new registrations or info changes.
         * node_count is pre-captured under lock in handle_register_req,
         * so no mutex needed here. */
        if (evt->is_new || evt->info_changed)
        {
            nvs_save_node(&evt->node, evt->node_count);
            ESP_LOGI(TAG, "Node %u persisted to NVS (is_new=%d info_changed=%d)",
                     evt->node.node_id, evt->is_new, evt->info_changed);
        }
    }
}

/* ───────────────────── Public API ───────────────────── */

static void app_espnow_init_cleanup(bool stop_send_task, bool unregister_handler,
                                    bool deinit_espnow, bool unregister_recv_cb,
                                    bool unregister_send_cb, bool delete_timer)
{
    if (delete_timer && s_heartbeat_timer != NULL)
    {
        xTimerDelete(s_heartbeat_timer, portMAX_DELAY);
        s_heartbeat_timer = NULL;
    }
    if (unregister_send_cb)
        esp_now_unregister_send_cb();
    if (unregister_recv_cb)
        esp_now_unregister_recv_cb();
    if (deinit_espnow)
        esp_now_deinit();
    if (unregister_handler)
        app_event_handler_unregister(APP_EVENT_ESPNOW_NODE_ONLINE, app_espnow_event_handler);
    if (s_rx_task != NULL)
    {
        vTaskDelete(s_rx_task);
        s_rx_task = NULL;
    }
    if (stop_send_task && s_send_task != NULL)
    {
        send_queue_item_t sentinel = {0};
        xQueueSend(s_send_queue, &sentinel, portMAX_DELAY);
        for (int i = 0; i < 50 && s_send_task != NULL; i++)
            vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_send_queue != NULL)
    {
        vQueueDelete(s_send_queue);
        s_send_queue = NULL;
    }
    if (s_rx_queue != NULL)
    {
        vQueueDelete(s_rx_queue);
        s_rx_queue = NULL;
    }
    if (s_mutex != NULL)
    {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }
}

esp_err_t app_espnow_init(const app_espnow_config_t *config)
{
    /* ── Parameter Validation ── */

    if (config == NULL)
    {
        ESP_LOGE(TAG, "NULL config");
        return ESP_ERR_INVALID_ARG;
    }

    if (atomic_load(&s_initialized))
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

    /* ── Create Send Queue ── */

    s_send_queue = xQueueCreate(SEND_QUEUE_DEPTH, sizeof(send_queue_item_t));
    if (s_send_queue == NULL)
    {
        ESP_LOGE(TAG, "Failed to create send queue");
        app_espnow_init_cleanup(false, false, false, false, false, false);
        return ESP_ERR_NO_MEM;
    }

    /* ── Create RX Queue ── */

    s_rx_queue = xQueueCreate(RX_QUEUE_DEPTH, sizeof(rx_item_t));
    if (s_rx_queue == NULL)
    {
        ESP_LOGE(TAG, "Failed to create RX queue");
        app_espnow_init_cleanup(false, false, false, false, false, false);
        return ESP_ERR_NO_MEM;
    }

    /* ── Create Send Task ── */

    if (xTaskCreate(espnow_send_task, "espnow_send", 3072, NULL, 5, &s_send_task) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create send task");
        app_espnow_init_cleanup(false, false, false, false, false, false);
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(espnow_rx_task, "espnow_rx", RX_TASK_STACK_SIZE, NULL,
                    RX_TASK_PRIORITY, &s_rx_task) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create RX task");
        app_espnow_init_cleanup(true, false, false, false, false, false);
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
        app_espnow_init_cleanup(true, false, false, false, false, false);
        return err;
    }

    /* ── Initialize ESP-NOW ── */

    err = esp_now_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_now_init failed: %s", esp_err_to_name(err));
        app_espnow_init_cleanup(true, true, false, false, false, false);
        return err;
    }

    /* ── Set Primary Master Key (PMK) ── */

    if (config->pmk != NULL)
    {
        err = esp_now_set_pmk(config->pmk);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_now_set_pmk failed: %s", esp_err_to_name(err));
            app_espnow_init_cleanup(true, true, true, false, false, false);
            return err;
        }
        ESP_LOGI(TAG, "PMK configured");
    }

    /* ── Register Callbacks ── */

    err = esp_now_register_recv_cb(app_espnow_recv_cb);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Register recv callback failed: %s", esp_err_to_name(err));
        app_espnow_init_cleanup(true, true, true, false, false, false);
        return err;
    }

    err = esp_now_register_send_cb(app_espnow_send_cb);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Register send callback failed: %s", esp_err_to_name(err));
        app_espnow_init_cleanup(true, true, true, true, false, false);
        return err;
    }

    /* ── Add Broadcast Peer ── */

    err = ensure_broadcast_peer();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Add broadcast peer failed: %s", esp_err_to_name(err));
        app_espnow_init_cleanup(true, true, true, true, true, false);
        return err;
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
        app_espnow_init_cleanup(true, true, true, true, true, false);
        return ESP_ERR_NO_MEM;
    }

    if (xTimerStart(s_heartbeat_timer, pdMS_TO_TICKS(1000)) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to start heartbeat timer");
        app_espnow_init_cleanup(true, true, true, true, true, true);
        return ESP_FAIL;
    }

    /* Print STA MAC Address */
    uint8_t mac[APP_ESPNOW_MAC_LEN];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    ESP_LOGI(TAG, "ESP-NOW Gateway initialized, STA MAC: " MACSTR, MAC2STR(mac));
    ESP_LOGI(TAG, "  Registered nodes: %u, HB timeout: %" PRIu32 "s, HB check: %" PRIu32 "s",
             s_node_count, config->heartbeat_timeout_s, config->heartbeat_check_s);

    atomic_store(&s_initialized, true);
    return ESP_OK;
}

esp_err_t app_espnow_deinit(void)
{
    if (!atomic_load(&s_initialized))
    {
        ESP_LOGW(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Deinitializing ESP-NOW Gateway...");

    /* Mark as not initialized first to prevent new API calls */
    atomic_store(&s_initialized, false);

    /* Stop Heartbeat Timer */
    if (s_heartbeat_timer != NULL)
    {
        xTimerStop(s_heartbeat_timer, portMAX_DELAY);
        xTimerDelete(s_heartbeat_timer, portMAX_DELAY);
        s_heartbeat_timer = NULL;
    }

    /* Gracefully stop send task by sending a shutdown sentinel */
    if (s_send_task != NULL && s_send_queue != NULL)
    {
        send_queue_item_t sentinel = {0}; /* len == 0 */
        xQueueSend(s_send_queue, &sentinel, portMAX_DELAY);

        /* Wait for task to exit (it sets s_send_task = NULL before deleting itself) */
        for (int i = 0; i < 50 && s_send_task != NULL; i++)
        {
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        if (s_send_task != NULL)
        {
            ESP_LOGW(TAG, "Send task did not exit gracefully, force deleting");
            vTaskDelete(s_send_task);
            s_send_task = NULL;
        }
    }

    /* Stop RX task */
    if (s_rx_task != NULL)
    {
        vTaskDelete(s_rx_task);
        s_rx_task = NULL;
    }

    /* Unregister callbacks and deinit ESP-NOW before deleting queues,
     * so recv_cb cannot fire and access freed queue handles. */
    esp_now_unregister_recv_cb();
    esp_now_unregister_send_cb();
    esp_now_deinit();

    if (s_send_queue != NULL)
    {
        vQueueDelete(s_send_queue);
        s_send_queue = NULL;
    }

    if (s_rx_queue != NULL)
    {
        vQueueDelete(s_rx_queue);
        s_rx_queue = NULL;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memset(s_nodes, 0, sizeof(s_nodes));
    s_node_count = 0;
    xSemaphoreGive(s_mutex);

    app_event_handler_unregister(APP_EVENT_ESPNOW_NODE_ONLINE, app_espnow_event_handler);

    vSemaphoreDelete(s_mutex);
    s_mutex = NULL;

    ESP_LOGI(TAG, "ESP-NOW Gateway deinitialized");
    return ESP_OK;
}

uint8_t app_espnow_get_node_count(void)
{
    if (!atomic_load(&s_initialized))
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

    if (!atomic_load(&s_initialized))
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

    if (!atomic_load(&s_initialized))
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

    if (!atomic_load(&s_initialized))
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
    uint8_t count_after = s_node_count;

    xSemaphoreGive(s_mutex);

    /* Delete from NVS (outside lock — NVS operations may block) */
    nvs_delete_node(node_id, count_after);

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
    if (peer_addr == NULL || data == NULL)
    {
        ESP_LOGE(TAG, "send: peer_addr or data is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (len == 0 || len > SEND_QUEUE_FRAME_MAX_LEN)
    {
        ESP_LOGE(TAG, "send: invalid data length (%u)", len);
        return ESP_ERR_INVALID_ARG;
    }

    if (!atomic_load(&s_initialized))
    {
        ESP_LOGW(TAG, "send: module not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    /* Enqueue for safe sending from the dedicated task */
    if (!enqueue_send(peer_addr, data, len, pdMS_TO_TICKS(100)))
    {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}