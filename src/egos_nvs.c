/**
 * @file egos_nvs.c
 * @brief NVS-based WiFi credential and module secret storage
 */

#include "egos_internal.h"
#include "egos_module_auth.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>

static const char *TAG = "egos_nvs";

#define NVS_NAMESPACE   "wifi_creds"
#define NVS_KEY_SSID    "ssid"
#define NVS_KEY_PASS    "password"

/* The module secret lives in its own namespace on purpose: credentials/reset
 * runs nvs_erase_all() on "wifi_creds", and must not take the secret with it.
 * NEVER log the secret's value. */
#define NVS_AUTH_NAMESPACE      "egos_auth"
#define NVS_KEY_MODULE_SECRET   "module_secret"

esp_err_t egos_nvs_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* This erases the whole partition, the module secret included. The
         * module then connects without a secret and, in enforce mode, is
         * refused (CONNACK 5) until someone presses Forget for it in the
         * controller's Settings > Paired devices. */
        ESP_LOGW(TAG, "NVS partition needs erase, reinitializing");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "NVS initialized");
    } else {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t egos_nvs_save_wifi_creds(const char *ssid, const char *password)
{
    if (!ssid || !password) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = nvs_set_str(handle, NVS_KEY_SSID, ssid);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save SSID: %s", esp_err_to_name(ret));
        nvs_close(handle);
        return ret;
    }

    ret = nvs_set_str(handle, NVS_KEY_PASS, password);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save password: %s", esp_err_to_name(ret));
        nvs_close(handle);
        return ret;
    }

    ret = nvs_commit(handle);
    nvs_close(handle);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "WiFi credentials saved (SSID: %s)", ssid);
    }
    return ret;
}

esp_err_t egos_nvs_load_wifi_creds(char *ssid_buf, size_t ssid_len,
                                    char *pass_buf, size_t pass_len)
{
    if (!ssid_buf || !pass_buf) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }

    size_t required_len = ssid_len;
    ret = nvs_get_str(handle, NVS_KEY_SSID, ssid_buf, &required_len);
    if (ret != ESP_OK) {
        nvs_close(handle);
        return ESP_ERR_NOT_FOUND;
    }

    required_len = pass_len;
    ret = nvs_get_str(handle, NVS_KEY_PASS, pass_buf, &required_len);
    if (ret != ESP_OK) {
        nvs_close(handle);
        return ESP_ERR_NOT_FOUND;
    }

    nvs_close(handle);
    ESP_LOGI(TAG, "WiFi credentials loaded (SSID: %s)", ssid_buf);
    return ESP_OK;
}

esp_err_t egos_nvs_clear_wifi_creds(void)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_erase_all(handle);
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }

    nvs_close(handle);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "WiFi credentials cleared");
    }
    return ret;
}

bool egos_nvs_has_wifi_creds(void)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        return false;
    }

    size_t required_len = 0;
    ret = nvs_get_str(handle, NVS_KEY_SSID, NULL, &required_len);
    nvs_close(handle);

    return (ret == ESP_OK && required_len > 0);
}

/* --------------------------------------------------------------------------
 * Module secret (protocol v1)
 * -------------------------------------------------------------------------- */

esp_err_t egos_nvs_save_module_secret(const char *secret)
{
    if (!egos_module_secret_is_valid(secret)) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_AUTH_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for the module secret: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = nvs_set_str(handle, NVS_KEY_MODULE_SECRET, secret);
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Module secret stored");
    } else {
        ESP_LOGE(TAG, "Failed to store the module secret: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t egos_nvs_load_module_secret(char *buf, size_t buf_len)
{
    if (!buf || buf_len < EGOS_MODULE_SECRET_BUF_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }
    buf[0] = '\0';

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_AUTH_NAMESPACE, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        /* The namespace does not exist until a secret is first stored. */
        return ESP_ERR_NOT_FOUND;
    }

    size_t required_len = buf_len;
    ret = nvs_get_str(handle, NVS_KEY_MODULE_SECRET, buf, &required_len);
    nvs_close(handle);

    if (ret != ESP_OK) {
        buf[0] = '\0';
        return ESP_ERR_NOT_FOUND;
    }
    if (!egos_module_secret_is_valid(buf)) {
        /* Never true for anything this SDK stored; treat it as absent. */
        buf[0] = '\0';
        ESP_LOGW(TAG, "Stored module secret is malformed - ignoring it");
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t egos_nvs_clear_module_secret(void)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_AUTH_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_erase_key(handle, NVS_KEY_MODULE_SECRET);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ret = ESP_OK;   /* nothing stored: already clear */
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Module secret cleared");
    } else {
        ESP_LOGE(TAG, "Failed to clear the module secret: %s", esp_err_to_name(ret));
    }
    return ret;
}
