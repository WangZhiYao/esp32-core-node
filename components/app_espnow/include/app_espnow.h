#pragma once

#include "esp_err.h"
#include "esp_now.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ───────────────────────── Constants Definition ───────────────────────── */

/** ESP-NOW Maximum Data Length (ESP-NOW protocol limit is 250 bytes) */
#define APP_ESPNOW_DATA_MAX_LEN ESP_NOW_MAX_DATA_LEN

/** MAC Address Length */
#define APP_ESPNOW_MAC_LEN 6

/** Maximum Number of Child Nodes */
#define APP_ESPNOW_MAX_NODES 20

/** Invalid Node ID Value */
#define APP_ESPNOW_NODE_ID_INVALID 0

/** Minimum Node ID Value */
#define APP_ESPNOW_NODE_ID_MIN 1

/** Maximum Node ID Value */
#define APP_ESPNOW_NODE_ID_MAX APP_ESPNOW_MAX_NODES

/** Maximum User Data Payload Length (excluding protocol overhead) */
#define APP_ESPNOW_USER_DATA_MAX_LEN 200

/* ───────────────────────── Protocol Frame Definition ───────────────────────── */

/**
 * @brief ESP-NOW Protocol Message Types
 *
 * Communication Protocol between Gateway and Node:
 *  1. Node broadcasts REGISTER_REQ after power up.
 *  2. Gateway assigns ID and replies with REGISTER_RESP.
 *  3. Node sends HEARTBEAT periodically, Gateway replies with HEARTBEAT_ACK.
 *  4. Node reports data using DATA_REPORT.
 */
typedef enum {
    APP_ESPNOW_MSG_REGISTER_REQ  = 0x01, /**< Node -> Gateway: Register Request */
    APP_ESPNOW_MSG_REGISTER_RESP = 0x02, /**< Gateway -> Node: Register Response */
    APP_ESPNOW_MSG_HEARTBEAT     = 0x03, /**< Node -> Gateway: Heartbeat */
    APP_ESPNOW_MSG_HEARTBEAT_ACK = 0x04, /**< Gateway -> Node: Heartbeat Acknowledge */
    APP_ESPNOW_MSG_DATA_REPORT   = 0x05, /**< Node -> Gateway: Data Report */
} app_espnow_msg_type_t;

/**
 * @brief Protocol Frame Header (Common for all messages)
 */
typedef struct __attribute__((packed)) {
    uint8_t  type;    /**< Message Type (app_espnow_msg_type_t) */
    uint8_t  node_id; /**< Node ID (0 for Register Request) */
    uint16_t seq;     /**< Sequence Number */
} app_espnow_header_t;

/**
 * @brief Register Request (Node -> Gateway)
 */
typedef struct __attribute__((packed)) {
    app_espnow_header_t header;
    uint8_t             device_type; /**< Device Type Identifier (Defined by Node) */
    uint8_t             fw_version;  /**< Firmware Version */
} app_espnow_register_req_t;

/**
 * @brief Register Response (Gateway -> Node)
 */
typedef struct __attribute__((packed)) {
    app_espnow_header_t header;
    uint8_t             assigned_id; /**< Assigned Node ID, 0 means refused */
} app_espnow_register_resp_t;

/**
 * @brief Heartbeat (Node -> Gateway)
 */
typedef struct __attribute__((packed)) {
    app_espnow_header_t header;
} app_espnow_heartbeat_t;

/**
 * @brief Heartbeat Acknowledge (Gateway -> Node)
 */
typedef struct __attribute__((packed)) {
    app_espnow_header_t header;
} app_espnow_heartbeat_ack_t;

/**
 * @brief Data Report (Node -> Gateway)
 */
typedef struct __attribute__((packed)) {
    app_espnow_header_t header;
    uint16_t            data_len;                            /**< User Data Length */
    uint8_t             data[APP_ESPNOW_USER_DATA_MAX_LEN]; /**< User Data */
} app_espnow_data_report_t;

/* ───────────────────────── Node Information ───────────────────────── */

/**
 * @brief Node Online Status
 */
typedef enum {
    APP_ESPNOW_NODE_OFFLINE = 0, /**< Offline */
    APP_ESPNOW_NODE_ONLINE  = 1, /**< Online */
} app_espnow_node_status_t;

/**
 * @brief Registered Node Information
 */
typedef struct {
    uint8_t                  node_id;                    /**< Node ID (1~MAX) */
    uint8_t                  mac[APP_ESPNOW_MAC_LEN];   /**< MAC Address */
    uint8_t                  device_type;                /**< Device Type */
    uint8_t                  fw_version;                 /**< Firmware Version */
    app_espnow_node_status_t status;                     /**< Online Status */
    int64_t                  last_seen_ms;               /**< Timestamp of last received message (ms) */
    int                      rssi;                       /**< Last RSSI */
} app_espnow_node_info_t;

/* ───────────────────────── Event Data Types ───────────────────────── */

/**
 * @brief Node Online Event Data (Passed via APP_EVENT_ESPNOW_NODE_ONLINE)
 */
typedef struct {
    app_espnow_node_info_t node;  /**< Node Information */
    bool                   is_new; /**< Is new registration (false means reconnect) */
} app_espnow_node_online_t;

/**
 * @brief Node Offline Event Data (Passed via APP_EVENT_ESPNOW_NODE_OFFLINE)
 */
typedef struct {
    app_espnow_node_info_t node; /**< Node Information */
} app_espnow_node_offline_t;

/**
 * @brief Node Data Report Event Data (Passed via APP_EVENT_ESPNOW_NODE_DATA)
 */
typedef struct {
    uint8_t  node_id;                                /**< Node ID */
    uint8_t  src_addr[APP_ESPNOW_MAC_LEN];          /**< Sender MAC */
    int      rssi;                                   /**< RSSI */
    uint16_t data_len;                               /**< User Data Length */
    uint8_t  data[APP_ESPNOW_USER_DATA_MAX_LEN];    /**< User Data */
} app_espnow_node_data_t;

/* ───────────────────────── Configuration ───────────────────────── */

/**
 * @brief ESP-NOW Gateway Initialization Configuration
 */
typedef struct {
    const uint8_t *pmk;                /**< Primary Master Key (16 bytes), NULL for no encryption */
    uint32_t       heartbeat_timeout_s; /**< Heartbeat timeout (seconds), considered offline if exceeded */
    uint32_t       heartbeat_check_s;   /**< Heartbeat check period (seconds) */
} app_espnow_config_t;

/* ───────────────────────── API ───────────────────────── */

/**
 * @brief Initialize ESP-NOW Gateway Module
 *
 * Initializes ESP-NOW protocol, registers callbacks, restores registered nodes from NVS,
 * and starts heartbeat check timer.
 *
 * Dependencies:
 *  - app_storage_init() MUST be called before this (NVS initialized).
 *  - app_event_init()   MUST be called before this (Custom event loop ready).
 *  - WiFi driver MUST be initialized (app_network_init called).
 *
 * @param[in] config Pointer to configuration, cannot be NULL
 * @return
 *  - ESP_OK              Success
 *  - ESP_ERR_INVALID_ARG Invalid argument
 *  - ESP_ERR_INVALID_STATE Already initialized
 *  - Other ESP-IDF error codes
 */
esp_err_t app_espnow_init(const app_espnow_config_t *config);

/**
 * @brief Deinitialize ESP-NOW Gateway Module
 *
 * Stops heartbeat check, unregisters callbacks, deinitializes ESP-NOW, and releases all resources.
 *
 * @return
 *  - ESP_OK              Success
 *  - ESP_ERR_INVALID_STATE Not initialized
 */
esp_err_t app_espnow_deinit(void);

/**
 * @brief Get the number of registered nodes
 *
 * Thread-safe.
 *
 * @return Number of registered nodes
 */
uint8_t app_espnow_get_node_count(void);

/**
 * @brief Get node information by ID
 *
 * Thread-safe.
 *
 * @param[in]  node_id Node ID (1~MAX)
 * @param[out] info    Output node information, cannot be NULL
 * @return
 *  - ESP_OK              Success
 *  - ESP_ERR_NOT_FOUND   ID not registered
 *  - ESP_ERR_INVALID_ARG Invalid argument
 */
esp_err_t app_espnow_get_node_info(uint8_t node_id, app_espnow_node_info_t *info);

/**
 * @brief Get all registered node information
 *
 * Thread-safe.
 *
 * @param[out] nodes     Output array, cannot be NULL
 * @param[in]  max_count Maximum capacity of the array
 * @param[out] count     Actual filled count, cannot be NULL
 * @return ESP_OK Success
 */
esp_err_t app_espnow_get_all_nodes(app_espnow_node_info_t *nodes, uint8_t max_count, uint8_t *count);

/**
 * @brief Remove a registered node
 *
 * Removes the specified node from the node table and NVS. Thread-safe.
 *
 * @param[in] node_id Node ID (1~MAX)
 * @return
 *  - ESP_OK            Success
 *  - ESP_ERR_NOT_FOUND ID not registered
 */
esp_err_t app_espnow_remove_node(uint8_t node_id);

/**
 * @brief Send raw data to a specific node
 *
 * Thread-safe.
 *
 * @param[in] peer_addr Target MAC address (6 bytes), NULL for broadcast
 * @param[in] data      Data pointer, cannot be NULL
 * @param[in] len       Data length (not exceeding APP_ESPNOW_DATA_MAX_LEN)
 * @return
 *  - ESP_OK Success
 *  - Other ESP-IDF error codes
 */
esp_err_t app_espnow_send(const uint8_t *peer_addr, const uint8_t *data, uint16_t len);

#ifdef __cplusplus
}
#endif