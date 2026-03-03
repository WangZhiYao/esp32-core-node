#include <esp_log.h>
#include "app_event.h"

/** Define the custom event base */
ESP_EVENT_DEFINE_BASE(APP_EVENT_BASE);

/** Custom event loop handle, NULL before initialization */
static esp_event_loop_handle_t s_app_event_loop_handle = NULL;

esp_err_t app_event_init(void)
{
    /* Prevent duplicate initialization */
    if (s_app_event_loop_handle != NULL)
    {
        ESP_LOGW(APP_EVENT_TAG, "Event loop already initialized");
        return ESP_OK;
    }

    esp_event_loop_args_t event_loop_args = {
        .queue_size = 32, /* Event queue depth, adjust as needed */
        .task_name = "app_event_task",
        .task_priority = 5,
        .task_stack_size = 4096,       /* Appropriately increased stack size to prevent overflow */
        .task_core_id = tskNO_AFFINITY /* No core binding, decided by scheduler */
    };

    esp_err_t err = esp_event_loop_create(&event_loop_args, &s_app_event_loop_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(APP_EVENT_TAG, "Failed to create event loop: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t app_event_post_with_timeout(app_event_id_t event_id, void *event_data, size_t event_data_size, TickType_t timeout)
{
    /* Check if event loop is initialized */
    if (s_app_event_loop_handle == NULL)
    {
        ESP_LOGE(APP_EVENT_TAG, "Event loop not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    return esp_event_post_to(
        s_app_event_loop_handle,
        APP_EVENT_BASE,
        (int32_t)event_id,
        event_data,
        event_data_size,
        timeout);
}

esp_err_t app_event_post(app_event_id_t event_id, void *event_data, size_t event_data_size)
{
    return app_event_post_with_timeout(event_id, event_data, event_data_size, portMAX_DELAY);
}

esp_err_t app_event_handler_register(int32_t event_id, esp_event_handler_t event_handler, void *event_handler_arg)
{
    /* Check if event loop is initialized */
    if (s_app_event_loop_handle == NULL)
    {
        ESP_LOGE(APP_EVENT_TAG, "Event loop not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (event_handler == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    return esp_event_handler_register_with(
        s_app_event_loop_handle,
        APP_EVENT_BASE,
        event_id,
        event_handler,
        event_handler_arg);
}

esp_err_t app_event_handler_unregister(int32_t event_id, esp_event_handler_t event_handler)
{
    /* Check if event loop is initialized */
    if (s_app_event_loop_handle == NULL)
    {
        ESP_LOGE(APP_EVENT_TAG, "Event loop not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (event_handler == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    return esp_event_handler_unregister_with(
        s_app_event_loop_handle,
        APP_EVENT_BASE,
        event_id,
        event_handler);
}