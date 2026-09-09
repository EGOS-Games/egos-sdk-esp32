/**
 * @file egos_sdk.c
 * @brief Top-level EGOS SDK implementation
 */

#include "egos_internal.h"
#include "esp_mac.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "egos_sdk";

/* Global state accessible to internal modules */
egos_config_t egos_g_config = {0};
char egos_g_module_id[64] = {0};

static bool s_initialized = false;

esp_err_t egos_init(const egos_config_t *config)
{
    if (!config) {
        ESP_LOGE(TAG, "Config is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (!config->module_prefix) {
        ESP_LOGE(TAG, "module_prefix is required");
        return ESP_ERR_INVALID_ARG;
    }

    if (!config->devices || config->device_count == 0) {
        ESP_LOGE(TAG, "At least one device is required");
        return ESP_ERR_INVALID_ARG;
    }

#ifdef CONFIG_EGOS_ETHERNET_ENABLED
    if (config->ethernet.cs_pin == 0 && config->ethernet.sclk_pin == 0) {
        ESP_LOGW(TAG, "Ethernet enabled but SPI pins appear unconfigured (all zero)");
    }
#endif

#ifdef CONFIG_EGOS_STATUS_LED_ENABLED
    if (config->status_led.red_pin == 0 && config->status_led.green_pin == 0
        && config->status_led.blue_pin == 0) {
        ESP_LOGW(TAG, "Status LED enabled but GPIO pins appear unconfigured (all zero)");
    }
#endif

    /* Copy configuration */
    memcpy(&egos_g_config, config, sizeof(egos_config_t));

    /* Generate module ID from MAC address */
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    snprintf(egos_g_module_id, sizeof(egos_g_module_id),
             "%s-%02X%02X%02X%02X%02X%02X",
             config->module_prefix,
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    ESP_LOGI(TAG, "EGOS SDK initialized");
    ESP_LOGI(TAG, "Module ID: %s", egos_g_module_id);
    ESP_LOGI(TAG, "Devices: %d", (int)egos_g_config.device_count);

    s_initialized = true;
    return ESP_OK;
}

esp_err_t egos_start(void)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "SDK not initialized, call egos_init() first");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Starting EGOS SDK...");
    return egos_connection_start();
}

esp_err_t egos_stop(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Stopping EGOS SDK...");
    return egos_connection_stop();
}

esp_err_t egos_publish_input(const char *device_id, const char *command,
                              const char *params_json)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!device_id || !command || !params_json) {
        return ESP_ERR_INVALID_ARG;
    }

    return egos_mqtt_publish_input(device_id, command, params_json);
}

esp_err_t egos_publish_state(const char *device_id, const char *state_json)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!device_id || !state_json) {
        return ESP_ERR_INVALID_ARG;
    }

    return egos_mqtt_publish_state(device_id, state_json);
}

const char *egos_get_module_id(void)
{
    if (!s_initialized) {
        return NULL;
    }
    return egos_g_module_id;
}

bool egos_is_connected(void)
{
    if (!s_initialized) {
        return false;
    }
    return egos_mqtt_is_connected();
}

void egos_indicate_input(void)
{
#ifdef CONFIG_EGOS_STATUS_LED_ENABLED
    egos_led_flash_input();
#endif
}
