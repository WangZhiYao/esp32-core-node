#pragma once

#include "esp_err.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ───────────────────────── Data Types ───────────────────────── */

/**
 * @brief SNTP Initialization Configuration
 */
typedef struct {
    const char *ntp_server;      /**< NTP server hostname, e.g., "pool.ntp.org" */
    const char *timezone;        /**< POSIX timezone string, e.g., "CST-8" for UTC+8 */
    int         sync_interval;   /**< Periodic re-sync interval in minutes */
} app_sntp_config_t;

/* ───────────────────────── API ───────────────────────── */

/**
 * @brief Initialize SNTP Module
 *
 * Stores configuration but does NOT start time synchronization.
 * Call app_sntp_start() after the network is ready.
 *
 * @param[in] config Pointer to configuration, cannot be NULL
 * @return
 *  - ESP_OK              Success
 *  - ESP_ERR_INVALID_ARG Invalid argument
 *  - ESP_ERR_INVALID_STATE Already initialized
 */
esp_err_t app_sntp_init(const app_sntp_config_t *config);

/**
 * @brief Start SNTP Synchronization
 *
 * Configures and starts the SNTP client, creates periodic re-sync timer.
 * Should be called when the network has obtained an IP address.
 *
 * @return
 *  - ESP_OK              Success
 *  - ESP_ERR_INVALID_STATE Not initialized or already started
 */
esp_err_t app_sntp_start(void);

/**
 * @brief Stop SNTP Synchronization
 *
 * Stops the SNTP client and deletes the re-sync timer.
 *
 * @return
 *  - ESP_OK              Success
 *  - ESP_ERR_INVALID_STATE Not initialized or not started
 */
esp_err_t app_sntp_stop(void);

/**
 * @brief Trigger an SNTP re-sync
 *
 * Restarts the SNTP client to perform a time re-synchronization.
 * Must be called from a task context (not from a timer callback).
 */
void app_sntp_resync(void);

#ifdef __cplusplus
}
#endif
