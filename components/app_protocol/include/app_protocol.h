#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_now.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ───────────────────────── Constants Definition ───────────────────────── */

/** ESP-NOW Maximum Data Length (ESP-NOW protocol limit is 250 bytes) */
#define APP_PROTOCOL_DATA_MAX_LEN ESP_NOW_MAX_DATA_LEN

/** Maximum User Data Payload Length (excluding protocol overhead) */
#define APP_PROTOCOL_USER_DATA_MAX_LEN 194

/* ───────────────────────── Sensor Type Definition ───────────────────────── */

/**
 * @brief Sensor type identifiers carried in DATA_REPORT frames.
 *
 * The gateway uses this field to interpret the binary payload without
 * needing to parse the raw data bytes.
 */
typedef enum
{
    APP_PROTOCOL_SENSOR_ENV = 0x01, /**< BME280 + BH1750: temperature, pressure, humidity, lux */
    APP_PROTOCOL_SENSOR_IAQ = 0x02, /**< ENS160 + AHT21: temperature, humidity, eCO2, TVOC, AQI */
} app_protocol_sensor_type_t;

/**
 * @brief Binary payload for APP_PROTOCOL_SENSOR_ENV reports.
 *
 * All fields are IEEE-754 single-precision floats in little-endian byte order.
 */
typedef struct __attribute__((packed))
{
    float temperature; /**< °C  */
    float pressure;    /**< hPa */
    float humidity;    /**< %RH */
    float lux;         /**< lux */
} app_protocol_env_data_t;

/**
 * @brief Binary payload for APP_PROTOCOL_SENSOR_IAQ reports.
 */
typedef struct __attribute__((packed))
{
    float    temperature; /**< °C  */
    float    humidity;    /**< %RH */
    uint16_t eco2;        /**< Equivalent CO2 in ppm */
    uint16_t tvoc;        /**< Total VOC in ppb */
    uint8_t  aqi;         /**< AQI-UBA (1=Excellent … 5=Unhealthy) */
} app_protocol_iaq_data_t;

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
typedef enum
{
    APP_PROTOCOL_MSG_REGISTER_REQ  = 0x01, /**< Node -> Gateway: Register Request */
    APP_PROTOCOL_MSG_REGISTER_RESP = 0x02, /**< Gateway -> Node: Register Response */
    APP_PROTOCOL_MSG_HEARTBEAT     = 0x03, /**< Node -> Gateway: Heartbeat */
    APP_PROTOCOL_MSG_HEARTBEAT_ACK = 0x04, /**< Gateway -> Node: Heartbeat Acknowledge */
    APP_PROTOCOL_MSG_DATA_REPORT   = 0x05, /**< Node -> Gateway: Data Report */
} app_protocol_msg_type_t;

/**
 * @brief Protocol Frame Header (Common for all messages)
 */
typedef struct __attribute__((packed))
{
    uint8_t  type;    /**< Message Type (app_protocol_msg_type_t) */
    uint8_t  node_id; /**< Node ID (0 for Register Request) */
    uint16_t seq;     /**< Sequence Number */
} app_protocol_header_t;

/**
 * @brief Register Request (Node -> Gateway)
 */
typedef struct __attribute__((packed))
{
    app_protocol_header_t header;
    uint8_t             device_type; /**< Device Type Identifier (Defined by Node) */
    uint8_t             fw_version;  /**< Firmware Version */
} app_protocol_register_req_t;

/**
 * @brief Register Response (Gateway -> Node)
 */
typedef struct __attribute__((packed))
{
    app_protocol_header_t header;
    uint8_t             assigned_id; /**< Assigned Node ID, 0 means refused */
    uint8_t             channel;     /**< WiFi channel for communication */
} app_protocol_register_resp_t;

/**
 * @brief Heartbeat (Node -> Gateway)
 */
typedef struct __attribute__((packed))
{
    app_protocol_header_t header;
} app_protocol_heartbeat_t;

/**
 * @brief Heartbeat Acknowledge (Gateway -> Node)
 */
typedef struct __attribute__((packed))
{
    app_protocol_header_t header;
} app_protocol_heartbeat_ack_t;

/**
 * @brief Data Report (Node -> Gateway)
 */
typedef struct __attribute__((packed))
{
    app_protocol_header_t header;
    uint8_t  sensor_type;                          /**< Sensor type (app_protocol_sensor_type_t) */
    uint16_t data_len;                             /**< Payload length in bytes */
    uint8_t  data[APP_PROTOCOL_USER_DATA_MAX_LEN]; /**< Binary sensor payload */
} app_protocol_data_report_t;

#ifdef __cplusplus
}
#endif