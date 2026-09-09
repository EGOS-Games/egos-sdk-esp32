/**
 * @file egos_settings.c
 * @brief Per-device persisted settings.
 *
 * Modules frequently need to remember something per device: a motor's
 * direction inversion and ramp time, a servo's pulse limits and rail voltage,
 * a sensor's calibration. That has to survive reboots and firmware updates,
 * and it has to be addressable by device id because that is what the
 * controller talks in.
 *
 * The SDK deliberately stores these as OPAQUE BLOBS rather than defining the
 * shapes itself. A motor_cfg_t belongs to the module that has motors; teaching
 * the SDK about it would mean every new device type needing an SDK change, and
 * would put hardware semantics in a layer that should not have them. So the
 * split is: the SDK owns persistence and keying, the module owns the struct and
 * its schema version.
 *
 * Typical use:
 *
 *     motor_cfg_t cfg;
 *     if (egos_settings_load("motor1", &cfg, sizeof(cfg)) != ESP_OK ||
 *         cfg.version != MOTOR_CFG_VERSION) {
 *         cfg = motor_cfg_defaults();          // never configured, or upgraded
 *         egos_settings_save("motor1", &cfg, sizeof(cfg));
 *     }
 *
 * Note the version check is the module's job. The SDK cannot know whether a
 * blob it stored last week still matches this firmware's struct, so it returns
 * what it has and lets the caller decide.
 */

#include "sdkconfig.h"
#include "egos_internal.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>

static const char *TAG = "egos_settings";

/* Kept separate from the SDK's other namespaces so that clearing WiFi
 * credentials or the cached broker IP does not disturb device settings. */
#define NVS_NAMESPACE "egos_devcfg"

/**
 * NVS keys are limited to 15 characters, and device ids are not. Rather than
 * silently truncating - which would make "servo_position_1" and
 * "servo_position_2" collide and quietly share settings - long ids are hashed.
 *
 * FNV-1a, rendered as 8 hex characters with a "h" prefix. Short ids are used
 * verbatim so the common case stays readable with nvs_tool.
 */
static void make_key(const char *device_id, char *key_buf, size_t key_len)
{
    size_t id_len = strlen(device_id);

    if (id_len <= 15) {
        strncpy(key_buf, device_id, key_len - 1);
        key_buf[key_len - 1] = '\0';
        return;
    }

    uint32_t hash = 2166136261u;            /* FNV-1a offset basis */
    for (size_t i = 0; i < id_len; i++) {
        hash ^= (uint8_t)device_id[i];
        hash *= 16777619u;                  /* FNV-1a prime */
    }
    snprintf(key_buf, key_len, "h%08lx", (unsigned long)hash);
}

esp_err_t egos_settings_save(const char *device_id, const void *data, size_t len)
{
    if (device_id == NULL || device_id[0] == '\0' || data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    char key[16];
    make_key(device_id, key, sizeof(key));

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot open settings namespace: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_blob(h, key, data, len);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save settings for %s: %s", device_id, esp_err_to_name(err));
    }
    return err;
}

esp_err_t egos_settings_load(const char *device_id, void *data, size_t len)
{
    if (device_id == NULL || device_id[0] == '\0' || data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    char key[16];
    make_key(device_id, key, sizeof(key));

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        /* Namespace absent means nothing has ever been saved. That is the
         * normal first-boot case, not a failure worth logging loudly. */
        return ESP_ERR_NVS_NOT_FOUND;
    }

    size_t stored_len = 0;
    err = nvs_get_blob(h, key, NULL, &stored_len);
    if (err != ESP_OK) {
        nvs_close(h);
        return err;
    }

    /* A size mismatch means the struct changed shape since this was written.
     * Report it rather than filling a smaller struct from a larger blob, which
     * would read plausible-looking rubbish into the tail fields. */
    if (stored_len != len) {
        ESP_LOGW(TAG, "Settings for %s are %u bytes, caller expects %u - treating as absent",
                 device_id, (unsigned)stored_len, (unsigned)len);
        nvs_close(h);
        return ESP_ERR_INVALID_SIZE;
    }

    err = nvs_get_blob(h, key, data, &stored_len);
    nvs_close(h);
    return err;
}

esp_err_t egos_settings_erase(const char *device_id)
{
    if (device_id == NULL || device_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    char key[16];
    make_key(device_id, key, sizeof(key));

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_erase_key(h, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;               /* already absent - the desired end state */
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t egos_settings_erase_all(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
    }

    err = nvs_erase_all(h);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "All device settings erased");
    }
    return err;
}
