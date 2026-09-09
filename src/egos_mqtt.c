/**
 * @file egos_mqtt.c
 * @brief MQTT client with EGOS protocol support
 */

#include "egos_internal.h"
#include "mqtt_client.h"
#include "mdns.h"
#include "cJSON.h"
#include "esp_timer.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "egos_mqtt";

static esp_mqtt_client_handle_t s_client = NULL;
static egos_internal_status_cb_t s_status_cb = NULL;
static egos_internal_timeout_cb_t s_timeout_cb = NULL;
static volatile bool s_connected = false;
static esp_timer_handle_t s_timeout_timer = NULL;

/* --------------------------------------------------------------------------
 * Topic Helpers
 * -------------------------------------------------------------------------- */

static void build_topic(char *buf, size_t len, const char *device_id, const char *suffix)
{
    snprintf(buf, len, "%s/device/%s/%s", egos_g_module_id, device_id, suffix);
}

/**
 * Extract device_id from a topic like "{moduleId}/device/{deviceId}/output"
 * Returns pointer into topic_str (not a copy).
 */
static bool parse_device_id_from_topic(const char *topic, int topic_len,
                                        char *device_id_buf, size_t buf_len)
{
    /* Find "/device/" in topic */
    const char *device_marker = "/device/";
    const char *start = NULL;

    for (int i = 0; i <= topic_len - 8; i++) {
        if (strncmp(topic + i, device_marker, 8) == 0) {
            start = topic + i + 8;
            break;
        }
    }
    if (!start) return false;

    /* Find next "/" after device_id */
    const char *end = start;
    const char *topic_end = topic + topic_len;
    while (end < topic_end && *end != '/') {
        end++;
    }

    size_t id_len = end - start;
    if (id_len == 0 || id_len >= buf_len) return false;

    memcpy(device_id_buf, start, id_len);
    device_id_buf[id_len] = '\0';
    return true;
}

/**
 * Check if topic ends with a given suffix
 */
static bool topic_ends_with(const char *topic, int topic_len, const char *suffix)
{
    int suffix_len = strlen(suffix);
    if (topic_len < suffix_len) return false;
    return strncmp(topic + topic_len - suffix_len, suffix, suffix_len) == 0;
}

/* --------------------------------------------------------------------------
 * Device Registration
 * -------------------------------------------------------------------------- */

static void register_devices(void)
{
    cJSON *arr = cJSON_CreateArray();
    if (!arr) {
        ESP_LOGE(TAG, "Failed to create registration JSON");
        return;
    }

    for (size_t i = 0; i < egos_g_config.device_count; i++) {
        const egos_device_t *dev = &egos_g_config.devices[i];
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "id", dev->id);
        cJSON_AddStringToObject(obj, "type", dev->type);
        cJSON_AddStringToObject(obj, "name", dev->name);

        if (dev->config_json) {
            cJSON *cfg = cJSON_Parse(dev->config_json);
            if (cfg) {
                cJSON_AddItemToObject(obj, "configuration", cfg);
            }
        }

        cJSON_AddItemToArray(arr, obj);
    }

    char *payload = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);

    if (payload) {
        esp_mqtt_client_publish(s_client, "module/register", payload, 0, 1, 0);
        ESP_LOGI(TAG, "Registered %d devices", (int)egos_g_config.device_count);
        free(payload);
    }
}

/* --------------------------------------------------------------------------
 * Subscription Management
 * -------------------------------------------------------------------------- */

static void subscribe_topics(void)
{
    char topic[128];

    snprintf(topic, sizeof(topic), "%s/device/+/output", egos_g_module_id);
    esp_mqtt_client_subscribe(s_client, topic, 1);

    snprintf(topic, sizeof(topic), "%s/device/+/configuration/set", egos_g_module_id);
    esp_mqtt_client_subscribe(s_client, topic, 1);

    snprintf(topic, sizeof(topic), "%s/credentials/set", egos_g_module_id);
    esp_mqtt_client_subscribe(s_client, topic, 1);

    snprintf(topic, sizeof(topic), "%s/credentials/reset", egos_g_module_id);
    esp_mqtt_client_subscribe(s_client, topic, 1);

    ESP_LOGI(TAG, "Subscribed to command topics");
}

/* --------------------------------------------------------------------------
 * Incoming Message Handling
 * -------------------------------------------------------------------------- */

static void handle_credentials_set(const char *data, int data_len)
{
    cJSON *json = cJSON_ParseWithLength(data, data_len);
    if (!json) {
        ESP_LOGE(TAG, "Failed to parse credentials JSON");
        return;
    }

    cJSON *ssid = cJSON_GetObjectItem(json, "ssid");
    cJSON *password = cJSON_GetObjectItem(json, "password");

    if (cJSON_IsString(ssid) && cJSON_IsString(password)) {
        /* The controller re-publishes its current credentials on EVERY module
         * registration, so this arrives on every connect - not just when the
         * credentials have actually changed. Saving and rebooting
         * unconditionally therefore produces an endless reboot loop:
         *
         *   boot -> connect -> register -> controller pushes -> save ->
         *   reboot -> ...
         *
         * and the module never becomes usable. Only act when something differs. */
        char cur_ssid[33] = {0};
        char cur_pass[65] = {0};
        bool unchanged = false;

        if (egos_nvs_load_wifi_creds(cur_ssid, sizeof(cur_ssid),
                                     cur_pass, sizeof(cur_pass)) == ESP_OK) {
            unchanged = (strcmp(cur_ssid, ssid->valuestring) == 0) &&
                        (strcmp(cur_pass, password->valuestring) == 0);
        }

        if (unchanged) {
            ESP_LOGI(TAG, "Received credentials are identical to stored credentials - ignoring");
            cJSON_Delete(json);
            return;
        }

        ESP_LOGI(TAG, "Received new WiFi credentials (SSID: %s)", ssid->valuestring);
        egos_nvs_save_wifi_creds(ssid->valuestring, password->valuestring);
        cJSON_Delete(json);

        egos_led_update(EGOS_LED_CRED_PROVISIONING);
        ESP_LOGI(TAG, "Rebooting to apply new credentials...");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
        return;  /* unreachable, but prevents double-free if esp_restart changes */
    }

    if (!cJSON_IsString(ssid) || !cJSON_IsString(password)) {
        ESP_LOGW(TAG, "Invalid credentials format");
    }

    cJSON_Delete(json);
}

static void handle_credentials_reset(void)
{
    ESP_LOGI(TAG, "Resetting WiFi credentials to defaults");
    egos_nvs_clear_wifi_creds();

    ESP_LOGI(TAG, "Rebooting to apply default credentials...");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static void handle_output_command(const char *topic, int topic_len,
                                   const char *data, int data_len)
{
    if (!egos_g_config.on_output_command) return;

    char device_id[32];
    if (!parse_device_id_from_topic(topic, topic_len, device_id, sizeof(device_id))) {
        ESP_LOGW(TAG, "Could not parse device_id from output topic");
        return;
    }

    cJSON *json = cJSON_ParseWithLength(data, data_len);
    if (!json) {
        ESP_LOGW(TAG, "Failed to parse output command JSON");
        return;
    }

    cJSON *cmd = cJSON_GetObjectItem(json, "command");
    cJSON *params = cJSON_GetObjectItem(json, "parameters");

    if (cJSON_IsString(cmd)) {
        egos_g_config.on_output_command(device_id, cmd->valuestring, params,
                                         egos_g_config.user_data);
    }

    cJSON_Delete(json);
}

static void handle_configuration_set(const char *topic, int topic_len,
                                      const char *data, int data_len)
{
    if (!egos_g_config.on_configuration) return;

    char device_id[32];
    if (!parse_device_id_from_topic(topic, topic_len, device_id, sizeof(device_id))) {
        ESP_LOGW(TAG, "Could not parse device_id from configuration topic");
        return;
    }

    cJSON *json = cJSON_ParseWithLength(data, data_len);
    if (!json) {
        ESP_LOGW(TAG, "Failed to parse configuration JSON");
        return;
    }

    egos_g_config.on_configuration(device_id, json, egos_g_config.user_data);
    cJSON_Delete(json);
}

static void handle_mqtt_data(esp_mqtt_event_handle_t event)
{
    /* Null-terminate topic for string operations */
    char topic_buf[256];
    int topic_len = event->topic_len < (int)(sizeof(topic_buf) - 1)
                    ? event->topic_len : (int)(sizeof(topic_buf) - 1);
    memcpy(topic_buf, event->topic, topic_len);
    topic_buf[topic_len] = '\0';

    if (topic_ends_with(topic_buf, topic_len, "/credentials/set")) {
        handle_credentials_set(event->data, event->data_len);
    } else if (topic_ends_with(topic_buf, topic_len, "/credentials/reset")) {
        handle_credentials_reset();
    } else if (topic_ends_with(topic_buf, topic_len, "/configuration/set")) {
        handle_configuration_set(topic_buf, topic_len, event->data, event->data_len);
    } else if (topic_ends_with(topic_buf, topic_len, "/output")) {
        handle_output_command(topic_buf, topic_len, event->data, event->data_len);
    }
}

/* --------------------------------------------------------------------------
 * Timeout Timer
 * -------------------------------------------------------------------------- */

static void timeout_timer_cb(void *arg)
{
    ESP_LOGW(TAG, "MQTT connection timed out");
    if (s_timeout_cb) {
        s_timeout_cb();
    }
}

/* --------------------------------------------------------------------------
 * MQTT Event Handler
 * -------------------------------------------------------------------------- */

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Connected to MQTT broker");
            if (s_timeout_timer) {
                esp_timer_stop(s_timeout_timer);
            }
            s_connected = true;
            register_devices();
            subscribe_topics();
            if (s_status_cb) {
                s_status_cb(true);
            }
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "Disconnected from MQTT broker");
            s_connected = false;
            if (s_status_cb) {
                s_status_cb(false);
            }
            break;

        case MQTT_EVENT_DATA:
            handle_mqtt_data(event);
            break;

        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT error event");
            if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
                ESP_LOGE(TAG, "Transport error: %s",
                         esp_err_to_name(event->error_handle->esp_transport_sock_errno));
            }
            break;

        default:
            break;
    }
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

esp_err_t egos_mqtt_init(const char *broker_uri,
                          egos_internal_status_cb_t status_cb,
                          egos_internal_timeout_cb_t timeout_cb)
{
    if (s_client) {
        ESP_LOGW(TAG, "MQTT client already initialized, stopping first");
        egos_mqtt_stop();
    }

    s_status_cb = status_cb;
    s_timeout_cb = timeout_cb;

    /* Build last will payload */
    char lwt_payload[128];
    snprintf(lwt_payload, sizeof(lwt_payload),
             "{\"id\":\"%s\",\"status\":\"disconnected\"}", egos_g_module_id);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = broker_uri,
        .credentials.client_id = egos_g_module_id,
        .session.keepalive = egos_g_config.mqtt.keepalive_sec,
        .session.last_will = {
            .topic = "client/disconnect",
            .msg = lwt_payload,
            .msg_len = strlen(lwt_payload),
            .qos = 1,
            .retain = 0,
        },
        .buffer.size = 2048,
        .buffer.out_size = 4096,
        .network.disable_auto_reconnect = false,
        .network.reconnect_timeout_ms = 1000,
    };

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "Failed to create MQTT client");
        return ESP_FAIL;
    }

    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);

    /* Create timeout timer */
    if (!s_timeout_timer) {
        esp_timer_create_args_t timer_args = {
            .callback = timeout_timer_cb,
            .name = "mqtt_timeout",
        };
        esp_timer_create(&timer_args, &s_timeout_timer);
    }

    ESP_LOGI(TAG, "MQTT client initialized (broker: %s, client_id: %s)",
             broker_uri, egos_g_module_id);
    return ESP_OK;
}

esp_err_t egos_mqtt_connect(void)
{
    if (!s_client) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = esp_mqtt_client_start(s_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Start connection timeout timer */
    if (s_timeout_timer) {
        esp_timer_start_once(s_timeout_timer,
                              egos_g_config.mqtt.connect_timeout_ms * 1000);
    }

    ESP_LOGI(TAG, "MQTT connection started (timeout: %lums)",
             (unsigned long)egos_g_config.mqtt.connect_timeout_ms);
    return ESP_OK;
}

esp_err_t egos_mqtt_stop(void)
{
    if (s_timeout_timer) {
        esp_timer_stop(s_timeout_timer);
        esp_timer_delete(s_timeout_timer);
        s_timeout_timer = NULL;
    }

    if (s_client) {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }

    s_connected = false;
    ESP_LOGI(TAG, "MQTT client stopped");
    return ESP_OK;
}

bool egos_mqtt_is_connected(void)
{
    return s_connected;
}

esp_err_t egos_mqtt_publish_input(const char *device_id, const char *command,
                                   const char *params_json)
{
    if (!s_connected || !s_client) {
        return ESP_ERR_INVALID_STATE;
    }

    char topic[128];
    build_topic(topic, sizeof(topic), device_id, "input");

    cJSON *json = cJSON_CreateObject();
    if (!json) return ESP_ERR_NO_MEM;

    cJSON_AddStringToObject(json, "command", command);
    cJSON *params = cJSON_Parse(params_json);
    if (params) {
        cJSON_AddItemToObject(json, "parameters", params);
    } else {
        cJSON_AddRawToObject(json, "parameters", params_json);
    }

    char *payload = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!payload) return ESP_ERR_NO_MEM;

    int msg_id = esp_mqtt_client_publish(s_client, topic, payload, 0, 0, 0);
    free(payload);
    return (msg_id >= 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t egos_mqtt_publish_state(const char *device_id, const char *state_json)
{
    if (!s_connected || !s_client) {
        return ESP_ERR_INVALID_STATE;
    }

    char topic[128];
    build_topic(topic, sizeof(topic), device_id, "state");

    int msg_id = esp_mqtt_client_publish(s_client, topic, state_json, 0, 0, 0);
    return (msg_id >= 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t egos_mqtt_resolve_broker(egos_cred_source_t cred_source,
                                    char *uri_buf, size_t uri_len)
{
    /* 1. Direct IP from config */
    if (egos_g_config.mqtt.broker_ip) {
        snprintf(uri_buf, uri_len, "mqtt://%s:%d",
                 egos_g_config.mqtt.broker_ip, egos_g_config.mqtt.broker_port);
        ESP_LOGI(TAG, "Using configured broker IP: %s", uri_buf);
        return ESP_OK;
    }

    /* 2. Gateway IP when on default EGOS WiFi (controller is the gateway) */
    if (cred_source == EGOS_CRED_DEFAULT) {
        char gw_ip[16];
        if (egos_wifi_get_gateway_ip(gw_ip, sizeof(gw_ip))) {
            snprintf(uri_buf, uri_len, "mqtt://%s:%d", gw_ip,
                     egos_g_config.mqtt.broker_port);
            ESP_LOGI(TAG, "Using gateway IP as broker (EGOS network): %s", uri_buf);
            return ESP_OK;
        }
    }

    /* 3. mDNS resolution */
    const char *hostname = egos_g_config.mqtt.broker_hostname;
    /* Strip .local suffix for mdns_query_a (it adds .local internally) */
    char mdns_host[64];
    strncpy(mdns_host, hostname, sizeof(mdns_host) - 1);
    mdns_host[sizeof(mdns_host) - 1] = '\0';
    char *local_suffix = strstr(mdns_host, ".local");
    if (local_suffix) {
        *local_suffix = '\0';
    }

    esp_ip4_addr_t addr;
    esp_err_t ret = mdns_query_a(mdns_host, 5000, &addr);
    if (ret == ESP_OK) {
        snprintf(uri_buf, uri_len, "mqtt://" IPSTR ":%d",
                 IP2STR(&addr), egos_g_config.mqtt.broker_port);
        ESP_LOGI(TAG, "Resolved broker via mDNS: %s", uri_buf);
        return ESP_OK;
    }

    /* 4. Fallback to hostname as-is */
    snprintf(uri_buf, uri_len, "mqtt://%s:%d", hostname,
             egos_g_config.mqtt.broker_port);
    ESP_LOGW(TAG, "mDNS failed, using hostname fallback: %s", uri_buf);
    return ESP_OK;
}
