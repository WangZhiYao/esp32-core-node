#pragma once

#include <esp_err.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize NVS Flash
 *
 * Automatically erases and re-initializes if the partition has no free pages or version mismatch.
 *
 * @return ESP_OK on success, other values indicate initialization failure
 */
esp_err_t app_storage_init(void);

/**
 * @brief Write uint8_t value to specified namespace
 *
 * Internally handles open -> set -> commit -> close flow.
 *
 * @param[in] ns  NVS Namespace (max 15 chars)
 * @param[in] key Key name (max 15 chars)
 * @param[in] value Value to write
 * @return ESP_OK on success, other values indicate failure
 */
esp_err_t app_storage_set_u8(const char *ns, const char *key, uint8_t value);

/**
 * @brief Read uint8_t value from specified namespace
 *
 * @param[in]  ns    NVS Namespace
 * @param[in]  key   Key name
 * @param[out] value Read value
 * @return ESP_OK on success, ESP_ERR_NVS_NOT_FOUND if key does not exist
 */
esp_err_t app_storage_get_u8(const char *ns, const char *key, uint8_t *value);

/**
 * @brief Write binary data (blob) to specified namespace
 *
 * Internally handles open -> set -> commit -> close flow.
 *
 * @param[in] ns   NVS Namespace
 * @param[in] key  Key name
 * @param[in] data Data pointer
 * @param[in] len  Data length (bytes)
 * @return ESP_OK on success, other values indicate failure
 */
esp_err_t app_storage_set_blob(const char *ns, const char *key, const void *data, size_t len);

/**
 * @brief Read binary data (blob) from specified namespace
 *
 * @param[in]     ns   NVS Namespace
 * @param[in]     key  Key name
 * @param[out]    data Output buffer
 * @param[in,out] len  Input: Buffer size; Output: Actual read length
 * @return ESP_OK on success, ESP_ERR_NVS_NOT_FOUND if key does not exist
 */
esp_err_t app_storage_get_blob(const char *ns, const char *key, void *data, size_t *len);

/**
 * @brief Delete key from specified namespace
 *
 * Internally handles open -> erase -> commit -> close flow.
 *
 * @param[in] ns  NVS Namespace
 * @param[in] key Key name
 * @return ESP_OK on success, ESP_ERR_NVS_NOT_FOUND if key does not exist
 */
esp_err_t app_storage_erase_key(const char *ns, const char *key);

#ifdef __cplusplus
}
#endif