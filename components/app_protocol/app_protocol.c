#include "app_protocol.h"

#include "esp_log.h"
#include "esp_now.h"

#include <stddef.h>
#include <string.h>

static const char *TAG = "app_protocol";

int app_protocol_min_frame_size(app_protocol_msg_type_t type)
{
    switch (type)
    {
    case APP_PROTOCOL_MSG_REGISTER_REQ:
        return (int)sizeof(app_protocol_register_req_t);
    case APP_PROTOCOL_MSG_REGISTER_RESP:
        return (int)sizeof(app_protocol_register_resp_t);
    case APP_PROTOCOL_MSG_HEARTBEAT:
        return (int)sizeof(app_protocol_heartbeat_t);
    case APP_PROTOCOL_MSG_HEARTBEAT_ACK:
        return (int)sizeof(app_protocol_heartbeat_ack_t);
    case APP_PROTOCOL_MSG_DATA_REPORT:
        return (int)offsetof(app_protocol_data_report_t, data);
    default:
        return -1;
    }
}

esp_err_t app_protocol_validate(const void *data, int data_len,
                                app_protocol_msg_type_t *out_type)
{
    if (data == NULL || data_len < (int)sizeof(app_protocol_header_t))
    {
        return ESP_ERR_INVALID_SIZE;
    }

    const app_protocol_header_t *header = (const app_protocol_header_t *)data;
    app_protocol_msg_type_t type = (app_protocol_msg_type_t)header->type;

    int min_size = app_protocol_min_frame_size(type);
    if (min_size < 0)
    {
        ESP_LOGD(TAG, "Unknown message type 0x%02X", header->type);
        return ESP_ERR_INVALID_ARG;
    }

    if (data_len < min_size)
    {
        ESP_LOGD(TAG, "Frame too short for type 0x%02X: %d < %d",
                 header->type, data_len, min_size);
        return ESP_ERR_INVALID_SIZE;
    }

    /* Additional validation for DATA_REPORT */
    if (type == APP_PROTOCOL_MSG_DATA_REPORT)
    {
        const app_protocol_data_report_t *report =
            (const app_protocol_data_report_t *)data;

        if (report->data_len > APP_PROTOCOL_USER_DATA_MAX_LEN)
        {
            ESP_LOGD(TAG, "DATA_REPORT data_len too large: %u", report->data_len);
            return ESP_ERR_INVALID_SIZE;
        }

        int expected_min =
            (int)(offsetof(app_protocol_data_report_t, data) + report->data_len);
        if (data_len < expected_min)
        {
            ESP_LOGD(TAG, "DATA_REPORT truncated: %d < %d", data_len, expected_min);
            return ESP_ERR_INVALID_SIZE;
        }
    }

    if (out_type != NULL)
    {
        *out_type = type;
    }

    return ESP_OK;
}

size_t app_protocol_build_register_resp(void *buf, size_t buf_size,
                                        uint8_t assigned_id, uint8_t channel,
                                        uint16_t seq)
{
    if (buf == NULL || buf_size < sizeof(app_protocol_register_resp_t))
    {
        return 0;
    }

    app_protocol_register_resp_t *resp = (app_protocol_register_resp_t *)buf;
    resp->header.type = APP_PROTOCOL_MSG_REGISTER_RESP;
    resp->header.node_id = 0;
    resp->header.seq = seq;
    resp->assigned_id = assigned_id;
    resp->channel = channel;

    return sizeof(app_protocol_register_resp_t);
}

size_t app_protocol_build_heartbeat_ack(void *buf, size_t buf_size,
                                        uint8_t node_id, uint16_t seq)
{
    if (buf == NULL || buf_size < sizeof(app_protocol_heartbeat_ack_t))
    {
        return 0;
    }

    app_protocol_heartbeat_ack_t *ack = (app_protocol_heartbeat_ack_t *)buf;
    ack->header.type = APP_PROTOCOL_MSG_HEARTBEAT_ACK;
    ack->header.node_id = node_id;
    ack->header.seq = seq;

    return sizeof(app_protocol_heartbeat_ack_t);
}

esp_err_t app_protocol_parse_register_req(const void *data, int data_len,
                                          const app_protocol_register_req_t **out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    app_protocol_msg_type_t type;
    esp_err_t err = app_protocol_validate(data, data_len, &type);
    if (err != ESP_OK)
    {
        return err;
    }

    if (type != APP_PROTOCOL_MSG_REGISTER_REQ)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *out = (const app_protocol_register_req_t *)data;
    return ESP_OK;
}

esp_err_t app_protocol_parse_heartbeat(const void *data, int data_len,
                                       const app_protocol_heartbeat_t **out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    app_protocol_msg_type_t type;
    esp_err_t err = app_protocol_validate(data, data_len, &type);
    if (err != ESP_OK)
    {
        return err;
    }

    if (type != APP_PROTOCOL_MSG_HEARTBEAT)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *out = (const app_protocol_heartbeat_t *)data;
    return ESP_OK;
}

esp_err_t app_protocol_parse_data_report(const void *data, int data_len,
                                         const app_protocol_data_report_t **out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    app_protocol_msg_type_t type;
    esp_err_t err = app_protocol_validate(data, data_len, &type);
    if (err != ESP_OK)
    {
        return err;
    }

    if (type != APP_PROTOCOL_MSG_DATA_REPORT)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *out = (const app_protocol_data_report_t *)data;
    return ESP_OK;
}