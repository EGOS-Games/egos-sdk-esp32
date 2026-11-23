/**
 * @file egos_mqtt.c
 * @brief MQTT client with EGOS protocol support
 */

#include "egos_internal.h"
#include "mqtt_client.h"
#include "mdns.h"
#include "cJSON.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>   /* malloc/free for incoming payloads */

static const char *TAG = "egos_mqtt";

static esp_mqtt_client_handle_t s_client = NULL;
static egos_internal_status_cb_t s_status_cb = NULL;
static egos_internal_timeout_cb_t s_timeout_cb = NULL;
static volatile bool s_connected = false;
static esp_timer_handle_t s_timeout_timer = NULL;

/* Module secret (protocol v1, egos_module_auth.h). s_secret is what THIS
 * client sends as its password, loaded in egos_mqtt_init(). A secret that
 * arrives mid-connection is saved to NVS and picked up by the next
 * egos_mqtt_init(). If that save fails it waits in s_pending_secret (RAM):
 * egos_mqtt_init() uses it when NVS has none, and every connect retries the
 * save. NEVER log either. */
static char    s_secret[EGOS_MODULE_SECRET_BUF_SIZE];
static bool    s_using_secret = false;
static char    s_pending_secret[EGOS_MODULE_SECRET_BUF_SIZE];
static char    s_broker_uri[128];
static int64_t s_last_forget_log_us = 0;

/** How often the "press Forget" message repeats while the broker refuses us. */
#define FORGET_LOG_INTERVAL_US  (60LL * 1000 * 1000)

/* Backoff after CONNACK 5 (our id has a secret we do not hold). esp-mqtt's own
 * reconnect is a fixed 1 s, so on that refusal the client is stopped and then
 * restarted from s_backoff_timer after 1 s, 2 s, 4 s ... 60 s. The delay resets
 * on MQTT_EVENT_CONNECTED. The timer callback runs in the esp_timer task, never
 * in the MQTT task (which cannot stop its own client). s_client_lock keeps it
 * from racing egos_mqtt_stop(), which destroys the client. */
typedef enum {
    BACKOFF_IDLE,       /**< Not backing off */
    BACKOFF_STOPPING,   /**< Timer armed to stop the refused client */
    BACKOFF_WAITING,    /**< Client stopped; timer armed to restart it */
} backoff_phase_t;

static esp_timer_handle_t s_backoff_timer = NULL;
static SemaphoreHandle_t  s_client_lock = NULL;
static volatile backoff_phase_t s_backoff_phase = BACKOFF_IDLE;
static uint32_t s_backoff_ms = 0;
/** Set by a CONNACK 5 refusal, cleared on connect: keeps the auth-error LED. */
static volatile bool s_auth_refused = false;

/* Module-defined topics under "{moduleId}/". Small and fixed: a module
 * that needs more than a handful of side channels is describing devices,
 * and those belong in the device table where the controller can see them. */
typedef struct {
    char                suffix[24];
    egos_message_cb_t   cb;
} custom_sub_t;

static custom_sub_t s_custom_subs[EGOS_MAX_CUSTOM_TOPICS];
static uint8_t      s_custom_sub_count = 0;

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

    /* The controller answers module/register with our secret here when we
     * connected without one. Subscribed on every connect, before the caller
     * publishes module/register, so the answer cannot arrive first. */
    snprintf(topic, sizeof(topic), "%s/module/secret", egos_g_module_id);
    esp_mqtt_client_subscribe(s_client, topic, 1);

    /* Re-subscribed on every reconnect, not just the first connect, so a
     * module keeps its side channels across a broker restart. */
    for (uint8_t i = 0; i < s_custom_sub_count; i++) {
        snprintf(topic, sizeof(topic), "%s/%s", egos_g_module_id,
                 s_custom_subs[i].suffix);
        esp_mqtt_client_subscribe(s_client, topic, 1);
    }

    ESP_LOGI(TAG, "Subscribed to command topics (%u module-defined)",
             (unsigned)s_custom_sub_count);
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

static void handle_module_secret(const char *data, int data_len)
{
    /* NEVER log the secret or the payload: only that one was stored or not. */
    cJSON *json = cJSON_ParseWithLength(data, data_len);
    if (!json) {
        ESP_LOGW(TAG, "Module secret message is not valid JSON - ignoring");
        return;
    }

    cJSON *secret = cJSON_GetObjectItem(json, "secret");
    cJSON *version = cJSON_GetObjectItem(json, "version");
    const char *secret_str = cJSON_IsString(secret) ? secret->valuestring : NULL;
    bool version_is_number = cJSON_IsNumber(version);

    if (!egos_module_secret_message_is_valid(secret_str, version_is_number,
                                             version_is_number ? version->valuedouble : 0)) {
        ESP_LOGW(TAG, "Module secret message is malformed or an unknown version - ignoring");
        cJSON_Delete(json);
        return;
    }

    /* The controller issues a secret only to a connection that has none, so
     * one arriving while we hold one is not from the protocol: ignore it,
     * rather than let it replace a secret that works. */
    if (s_using_secret || s_pending_secret[0] != '\0') {
        ESP_LOGW(TAG, "Module secret message while a secret is already held - ignoring");
        cJSON_Delete(json);
        return;
    }

    if (egos_nvs_save_module_secret(secret_str) == ESP_OK) {
        /* This connection carries on as it is; the next one sends it. */
        ESP_LOGI(TAG, "Module secret received; it is used from the next connection");
    } else {
        /* The controller now holds this secret, so losing it would leave the
         * module refused until someone presses Forget. Keep it in RAM:
         * egos_mqtt_init() uses it and every connect retries the save. */
        strncpy(s_pending_secret, secret_str, sizeof(s_pending_secret) - 1);
        s_pending_secret[sizeof(s_pending_secret) - 1] = '\0';
        ESP_LOGW(TAG, "Module secret kept in RAM until it can be saved");
    }

    cJSON_Delete(json);
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
    } else if (egos_module_auth_is_secret_topic(event->topic, (size_t)event->topic_len,
                                                egos_g_module_id)) {
        /* Exactly "{our id}/module/secret", matched on the untruncated topic. */
        handle_module_secret(event->data, event->data_len);
    } else if (topic_ends_with(topic_buf, topic_len, "/configuration/set")) {
        handle_configuration_set(topic_buf, topic_len, event->data, event->data_len);
    } else if (topic_ends_with(topic_buf, topic_len, "/output")) {
        handle_output_command(topic_buf, topic_len, event->data, event->data_len);
    } else {
        /* Module-defined topic. Checked last so a module can never shadow
         * the protocol topics above by registering a colliding suffix. */
        for (uint8_t i = 0; i < s_custom_sub_count; i++) {
            char want[32];
            snprintf(want, sizeof(want), "/%s", s_custom_subs[i].suffix);
            if (!topic_ends_with(topic_buf, topic_len, want)) {
                continue;
            }
            /* event->data is not NUL terminated and may be a partial
             * fragment of a large message; copy the part we have. */
            char *payload = malloc(event->data_len + 1);
            if (payload == NULL) {
                ESP_LOGE(TAG, "Out of memory handling %s", s_custom_subs[i].suffix);
                return;
            }
            memcpy(payload, event->data, event->data_len);
            payload[event->data_len] = '\0';
            s_custom_subs[i].cb(s_custom_subs[i].suffix, payload,
                                egos_g_config.user_data);
            free(payload);
            return;
        }
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
 * Refused Connections (module secrets, protocol v1)
 * -------------------------------------------------------------------------- */

/** How soon after a CONNACK 5 the client is stopped: well inside esp-mqtt's
 *  1 s reconnect_timeout_ms, so its own retry never runs. */
#define BACKOFF_STOP_DELAY_US   (10 * 1000)

/**
 * s_backoff_timer callback (esp_timer task). Two steps on one timer:
 * STOPPING stops the refused client and re-arms for s_backoff_ms; WAITING
 * starts it again. egos_mqtt_stop() resets the phase to IDLE under the same
 * lock, so a timer that fires after the client is gone does nothing.
 */
static void backoff_timer_cb(void *arg)
{
    if (!s_client_lock || xSemaphoreTake(s_client_lock, portMAX_DELAY) != pdTRUE) {
        return;
    }

    if (s_client == NULL) {
        s_backoff_phase = BACKOFF_IDLE;
    } else if (s_backoff_phase == BACKOFF_STOPPING) {
        esp_mqtt_client_stop(s_client);
        s_backoff_phase = BACKOFF_WAITING;
        ESP_LOGI(TAG, "Retrying the MQTT connection in %lu ms",
                 (unsigned long)s_backoff_ms);
        if (esp_timer_start_once(s_backoff_timer, (uint64_t)s_backoff_ms * 1000) != ESP_OK) {
            /* Never leave the client stopped for good. */
            s_backoff_phase = BACKOFF_IDLE;
            esp_mqtt_client_start(s_client);
        }
    } else if (s_backoff_phase == BACKOFF_WAITING) {
        s_backoff_phase = BACKOFF_IDLE;
        if (esp_mqtt_client_start(s_client) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to restart the MQTT client after backoff");
        }
    }

    xSemaphoreGive(s_client_lock);
}

/**
 * Called from the MQTT task when the broker refuses a CONNECT. esp-mqtt then
 * waits reconnect_timeout_ms and reconnects with the client's current
 * credentials, so anything that must change for the next attempt changes here.
 */
static void handle_connection_refused(int connack_code)
{
    switch (egos_module_auth_refusal_action(connack_code, s_using_secret)) {
        case EGOS_AUTH_REFUSAL_DROP_SECRET: {
            /* The controller no longer knows our secret: someone pressed
             * Forget. Drop it and come back without one, so the controller
             * issues a new one after module/register. */
            ESP_LOGW(TAG, "Controller refused the module secret (CONNACK %d); "
                     "clearing it and reconnecting without one", connack_code);
            egos_nvs_clear_module_secret();
            memset(s_secret, 0, sizeof(s_secret));
            memset(s_pending_secret, 0, sizeof(s_pending_secret));
            s_using_secret = false;

            /* esp_mqtt_client_set_uri() is the light way to change the
             * username and password of a live client: it takes the client's
             * recursive API lock (we already hold it on this task) and, unlike
             * esp_mqtt_set_config(), does not rebuild the message buffers in
             * the middle of the client's own event dispatch. The URI carries
             * username "module" and an EMPTY password, never a secret, so its
             * debug log line is harmless. */
            char uri[sizeof(s_broker_uri) + sizeof(EGOS_MODULE_AUTH_USERNAME) + 2];
            if (!egos_module_auth_anonymous_uri(s_broker_uri, uri, sizeof(uri)) ||
                esp_mqtt_client_set_uri(s_client, uri) != ESP_OK) {
                /* The client keeps sending the old secret and keeps being
                 * refused; the next egos_mqtt_init() (network switch or
                 * reboot) loads the now-empty store and recovers. */
                ESP_LOGE(TAG, "Could not drop the secret from the live MQTT client");
            }
            break;
        }

        case EGOS_AUTH_REFUSAL_NEEDS_FORGET:
            /* Our id already has a secret on this controller that this module
             * does not hold (reflashed, NVS erased). Retry with backoff, and
             * say what to do at most once a minute. */
            s_auth_refused = true;
            s_backoff_ms = egos_module_auth_next_backoff_ms(s_backoff_ms);
            if (egos_module_auth_log_due(esp_timer_get_time(), &s_last_forget_log_us,
                                         FORGET_LOG_INTERVAL_US)) {
                ESP_LOGE(TAG, "Controller refused module %s (CONNACK %d): it already has a "
                         "secret for this module. Press \"Forget\" for this module in the "
                         "controller's Settings > Paired devices.",
                         egos_g_module_id, connack_code);
            }
            egos_led_update(EGOS_LED_AUTH_ERROR);

            /* esp-mqtt would retry in reconnect_timeout_ms (1 s). Stop the
             * client from the esp_timer task well before that; the same timer
             * then restarts it after s_backoff_ms. */
            if (s_backoff_timer && s_backoff_phase == BACKOFF_IDLE) {
                s_backoff_phase = BACKOFF_STOPPING;
                if (esp_timer_start_once(s_backoff_timer, BACKOFF_STOP_DELAY_US) != ESP_OK) {
                    s_backoff_phase = BACKOFF_IDLE;   /* falls back to the 1 s retry */
                }
            }
            break;

        case EGOS_AUTH_REFUSAL_OTHER:
        default:
            ESP_LOGW(TAG, "Broker refused the connection (CONNACK %d)", connack_code);
            break;
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
            s_last_forget_log_us = 0;
            s_auth_refused = false;
            s_backoff_ms = 0;
            /* A secret that could not be saved when it arrived: try again. */
            if (s_pending_secret[0] != '\0' &&
                egos_nvs_save_module_secret(s_pending_secret) == ESP_OK) {
                memset(s_pending_secret, 0, sizeof(s_pending_secret));
            }
            /* Subscribe first: the controller answers module/register on
             * {id}/module/secret (and re-sends {id}/credentials/set), and
             * esp-mqtt sends these packets in the order they are queued. */
            subscribe_topics();
            register_devices();
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
            } else if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
                handle_connection_refused((int)event->error_handle->connect_return_code);
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

    if (broker_uri == NULL || strlen(broker_uri) >= sizeof(s_broker_uri)) {
        /* s_broker_uri is what a CONNACK 4 rebuilds the credentials from;
         * a truncated copy would point the retry at the wrong broker. */
        ESP_LOGE(TAG, "Broker URI is missing or longer than %u characters",
                 (unsigned)(sizeof(s_broker_uri) - 1));
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_client_lock) {
        s_client_lock = xSemaphoreCreateMutex();
        if (!s_client_lock) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s_backoff_timer) {
        /* Created once and never deleted, so a callback in flight can
         * never find it gone. */
        esp_timer_create_args_t backoff_args = {
            .callback = backoff_timer_cb,
            .name = "mqtt_backoff",
        };
        if (esp_timer_create(&backoff_args, &s_backoff_timer) != ESP_OK) {
            s_backoff_timer = NULL;   /* CONNACK 5 then keeps the 1 s retry */
        }
    }

    s_status_cb = status_cb;
    s_timeout_cb = timeout_cb;

    /* Build last will payload */
    char lwt_payload[128];
    snprintf(lwt_payload, sizeof(lwt_payload),
             "{\"id\":\"%s\",\"status\":\"disconnected\"}", egos_g_module_id);

    /* Module secret (protocol v1): always username "module"; the password is
     * the stored secret, or empty (NULL, so esp-mqtt sends no password field)
     * to ask the controller for one. The client id stays the module id. */
    s_using_secret = (egos_nvs_load_module_secret(s_secret, sizeof(s_secret)) == ESP_OK);
    if (!s_using_secret && egos_module_secret_is_valid(s_pending_secret)) {
        /* Received but never saved (NVS write failed): still ours. */
        memcpy(s_secret, s_pending_secret, sizeof(s_secret));
        s_using_secret = true;
    }
    if (!s_using_secret) {
        memset(s_secret, 0, sizeof(s_secret));
    }
    memcpy(s_broker_uri, broker_uri, strlen(broker_uri) + 1);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = broker_uri,
        .credentials.client_id = egos_g_module_id,
        .credentials.username = EGOS_MODULE_AUTH_USERNAME,
        .credentials.authentication.password = s_using_secret ? s_secret : NULL,
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

    ESP_LOGI(TAG, "MQTT client initialized (broker: %s, client_id: %s, %s)",
             broker_uri, egos_g_module_id,
             s_using_secret ? "with module secret" : "no module secret yet");
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

    /* Under s_client_lock so a backoff callback is either finished or will
     * see s_client == NULL; never inside it. The backoff delay (s_backoff_ms)
     * is kept: only a successful connect resets it. */
    bool locked = s_client_lock && xSemaphoreTake(s_client_lock, portMAX_DELAY) == pdTRUE;
    if (s_backoff_timer) {
        esp_timer_stop(s_backoff_timer);
    }
    s_backoff_phase = BACKOFF_IDLE;

    if (s_client) {
        /* Fails harmlessly (ESP_FAIL) when backoff already stopped it. */
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }
    if (locked) {
        xSemaphoreGive(s_client_lock);
    }

    s_connected = false;
    ESP_LOGI(TAG, "MQTT client stopped");
    return ESP_OK;
}

bool egos_mqtt_is_connected(void)
{
    return s_connected;
}

bool egos_mqtt_auth_refused(void)
{
    return s_auth_refused;
}

bool egos_mqtt_is_backing_off(void)
{
    return s_backoff_phase != BACKOFF_IDLE;
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

esp_err_t egos_mqtt_publish_system(const char *state_json)
{
    if (!s_connected || !s_client) {
        return ESP_ERR_INVALID_STATE;
    }

    char topic[128];
    snprintf(topic, sizeof(topic), "%s/system", egos_g_module_id);

    int msg_id = esp_mqtt_client_publish(s_client, topic, state_json, 0, 0, 0);
    return (msg_id >= 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t egos_mqtt_subscribe_custom(const char *suffix, egos_message_cb_t cb)
{
    if (suffix == NULL || suffix[0] == '\0' || cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(suffix) >= sizeof(s_custom_subs[0].suffix)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (s_custom_sub_count >= EGOS_MAX_CUSTOM_TOPICS) {
        return ESP_ERR_NO_MEM;
    }

    strncpy(s_custom_subs[s_custom_sub_count].suffix, suffix,
            sizeof(s_custom_subs[0].suffix) - 1);
    s_custom_subs[s_custom_sub_count].suffix[sizeof(s_custom_subs[0].suffix) - 1] = '\0';
    s_custom_subs[s_custom_sub_count].cb = cb;
    s_custom_sub_count++;

    /* If MQTT is already up, subscribe now; otherwise subscribe_topics()
     * will pick it up on connect. Registering before egos_start() is the
     * expected case, but a late registration must not be silently inert. */
    if (s_connected && s_client) {
        char topic[128];
        snprintf(topic, sizeof(topic), "%s/%s", egos_g_module_id, suffix);
        esp_mqtt_client_subscribe(s_client, topic, 1);
    }

    ESP_LOGI(TAG, "Registered module topic %s/%s", egos_g_module_id, suffix);
    return ESP_OK;
}

esp_err_t egos_mqtt_resolve_broker(egos_cred_source_t cred_source,
                                    char *uri_buf, size_t uri_len)
{
    /* Discovery lives in egos_discovery.c: cached IP, gateway, mDNS and a
     * subnet sweep, each proved reachable with a TCP probe before use, and the
     * winner cached in NVS for the next boot. */
    char ip[16];
    egos_discovery_source_t source = EGOS_DISCOVERY_NONE;

    if (egos_discovery_resolve(cred_source, ip, sizeof(ip), &source)) {
        snprintf(uri_buf, uri_len, "mqtt://%s:%d", ip,
                 egos_g_config.mqtt.broker_port);

        static const char *const source_name[] = {
            [EGOS_DISCOVERY_NONE]       = "none",
            [EGOS_DISCOVERY_CONFIGURED] = "configured",
            [EGOS_DISCOVERY_CACHED]     = "cached",
            [EGOS_DISCOVERY_GATEWAY]    = "gateway",
            [EGOS_DISCOVERY_MDNS]       = "mDNS",
            [EGOS_DISCOVERY_SCAN]       = "subnet scan",
        };
        ESP_LOGI(TAG, "Broker %s (via %s)", uri_buf, source_name[source]);
        return ESP_OK;
    }

    /* Nothing answered. Hand the hostname to the MQTT client unresolved and
     * let it try - better than refusing to attempt a connection at all. */
    snprintf(uri_buf, uri_len, "mqtt://%s:%d",
             egos_g_config.mqtt.broker_hostname,
             egos_g_config.mqtt.broker_port);
    ESP_LOGW(TAG, "Discovery failed, falling back to hostname: %s", uri_buf);
    return ESP_OK;
}
