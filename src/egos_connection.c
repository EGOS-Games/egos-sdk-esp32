/**
 * @file egos_connection.c
 * @brief Connection manager state machine
 *
 * Orchestrates WiFi/Ethernet/MQTT lifecycle with automatic retry and fallback.
 */

#include "egos_internal.h"
#include <string.h>

static const char *TAG = "egos_conn";

#define ETHERNET_TIMEOUT_MS     5000
#define MQTT_FALLBACK_THRESHOLD 5
#define TASK_STACK_SIZE         4096
#define TASK_PRIORITY           5
#define TICK_MS                 100

/* --------------------------------------------------------------------------
 * State
 * -------------------------------------------------------------------------- */

static volatile egos_conn_state_t s_conn_state = EGOS_CONN_INIT;
static volatile bool s_wifi_connected = false;
static volatile bool s_mqtt_connected = false;
/* Whether MQTT has ever completed a connection since the current attempt was
 * started. Without this, EGOS_CONN_CONNECTED reads "not connected yet" as
 * "connection lost" on its very first tick and fires a second connect that
 * races the first - producing two brokers connections, two device
 * registrations and two on_connected callbacks per boot. */
static volatile bool s_mqtt_established = false;
static volatile bool s_mqtt_switch_requested = false;
static volatile uint8_t s_mqtt_timeouts = 0;
static egos_cred_source_t s_cred_source = EGOS_CRED_DEFAULT;
static bool s_mqtt_initialized = false;
static TaskHandle_t s_task_handle = NULL;

/* Which credential source to try next.
 *
 * Sticky: starts at STORED so a provisioned module always tries its own network
 * first, is updated to whatever actually connects, and alternates to the other
 * source after repeated failures. Because it is updated on success, a module
 * that has fallen back to the default network keeps preferring it across later
 * drops rather than reverting to a stored network that is no longer there.
 * Because it still alternates on failure, the module can never end up pinned to
 * a network that has gone away.
 *
 * RAM only: a power cycle, or the reboot that follows new credentials arriving,
 * deliberately re-tries the stored network first. */
static egos_cred_source_t s_preferred_cred_source = EGOS_CRED_STORED;

/* Consecutive failed connect attempts against s_preferred_cred_source */
static uint8_t s_cred_source_failures = 0;

/* Backoff between WiFi retries, to avoid hammering a busy router. Starts at the
 * minimum rather than 0 so a retry can never fire on the very next tick. */
#define EGOS_WIFI_RETRY_BACKOFF_MIN_MS 2000
#define EGOS_WIFI_RETRY_BACKOFF_MAX_STEPS 3
static uint32_t s_wifi_retry_backoff_ms = EGOS_WIFI_RETRY_BACKOFF_MIN_MS;
static uint8_t s_wifi_retry_count = 0;

static const char *cred_source_name(egos_cred_source_t source)
{
    return (source == EGOS_CRED_STORED) ? "stored (NVS)" : "default";
}

#ifdef CONFIG_EGOS_ETHERNET_ENABLED
static volatile bool s_ethernet_connected = false;

typedef enum {
    EGOS_NET_ETHERNET,
    EGOS_NET_WIFI_DEFAULT,
    EGOS_NET_WIFI_STORED,
} egos_active_network_t;

static egos_active_network_t s_active_network = EGOS_NET_ETHERNET;
#endif

/* --------------------------------------------------------------------------
 * Internal Callbacks
 * -------------------------------------------------------------------------- */

static void wifi_status_cb(bool connected)
{
    s_wifi_connected = connected;
    if (!connected) {
        ESP_LOGW(TAG, "WiFi disconnected");
    }
}

static void mqtt_status_cb(bool connected)
{
    s_mqtt_connected = connected;
    if (connected) {
        s_mqtt_established = true;
        s_mqtt_timeouts = 0;
        s_mqtt_switch_requested = false;
        ESP_LOGI(TAG, "MQTT connected");

        egos_led_state_t led = (s_cred_source == EGOS_CRED_STORED)
                               ? EGOS_LED_CONNECTED_STORED
                               : EGOS_LED_CONNECTED_DEFAULT;
        egos_led_update(led);

        if (egos_g_config.on_connected) {
            egos_g_config.on_connected(egos_g_config.user_data);
        }
    } else {
        ESP_LOGW(TAG, "MQTT disconnected");
        egos_led_update(EGOS_LED_MQTT_CONNECTING);

        if (egos_g_config.on_disconnected) {
            egos_g_config.on_disconnected(egos_g_config.user_data);
        }
    }
}

static void mqtt_timeout_cb(void)
{
    s_mqtt_timeouts++;
    ESP_LOGW(TAG, "MQTT timeout (%d/%d)", s_mqtt_timeouts, MQTT_FALLBACK_THRESHOLD);
    if (s_mqtt_timeouts >= MQTT_FALLBACK_THRESHOLD) {
        s_mqtt_switch_requested = true;
    }
}

#ifdef CONFIG_EGOS_ETHERNET_ENABLED
static void ethernet_status_cb(bool connected)
{
    s_ethernet_connected = connected;
    if (!connected) {
        ESP_LOGW(TAG, "Ethernet disconnected");
    }
}
#endif

/* --------------------------------------------------------------------------
 * WiFi Connect Helper
 * -------------------------------------------------------------------------- */

/**
 * Attempt a WiFi connection using the sticky preferred credential source, and
 * update that preference from the outcome.
 *
 * On success the preference becomes whatever actually connected, so later
 * reconnects resume on the network that works. On failure the preference
 * alternates once the source has failed enough times - immediately when the
 * SSID was not found (conclusive), after more attempts for any other reason so
 * a router that is slow to come back does not strand the module elsewhere.
 */
static esp_err_t wifi_connect_preferred(void)
{
    egos_cred_source_t actual = s_preferred_cred_source;

    ESP_LOGI(TAG, "Connecting WiFi (%s credentials)...",
             cred_source_name(s_preferred_cred_source));
    egos_led_update(s_preferred_cred_source == EGOS_CRED_STORED
                    ? EGOS_LED_WIFI_CONNECTING_STORED
                    : EGOS_LED_WIFI_CONNECTING_DEFAULT);

    esp_err_t ret = egos_wifi_connect(s_preferred_cred_source, &actual);

    if (ret == ESP_OK) {
        /* Remember what actually worked. egos_wifi_connect() downgrades STORED
         * to DEFAULT when NVS is empty, so read back what it reports. */
        s_preferred_cred_source = actual;
        s_cred_source = actual;
        s_cred_source_failures = 0;
        s_wifi_retry_count = 0;
        s_wifi_retry_backoff_ms = EGOS_WIFI_RETRY_BACKOFF_MIN_MS;
        return ESP_OK;
    }

    s_cred_source_failures++;

    uint8_t reason = egos_wifi_get_last_disconnect_reason();
    uint8_t threshold = (reason == EGOS_WIFI_REASON_NO_AP_FOUND)
                        ? EGOS_WIFI_SWITCH_AFTER_NO_AP
                        : EGOS_WIFI_SWITCH_AFTER_OTHER;

    if (s_cred_source_failures >= threshold) {
        s_preferred_cred_source = (s_preferred_cred_source == EGOS_CRED_STORED)
                                  ? EGOS_CRED_DEFAULT
                                  : EGOS_CRED_STORED;
        s_cred_source_failures = 0;
        ESP_LOGW(TAG, "Falling back to %s credentials (last disconnect reason: %d)",
                 cred_source_name(s_preferred_cred_source), reason);
    } else {
        ESP_LOGI(TAG, "Retrying %s credentials (%d/%d before switching, reason: %d)",
                 cred_source_name(s_preferred_cred_source),
                 s_cred_source_failures, threshold, reason);
    }

    /* Exponential backoff, capped, to reduce load on a busy router */
    s_wifi_retry_count++;
    s_wifi_retry_backoff_ms = EGOS_WIFI_RETRY_BACKOFF_MIN_MS
        * (s_wifi_retry_count > EGOS_WIFI_RETRY_BACKOFF_MAX_STEPS
           ? EGOS_WIFI_RETRY_BACKOFF_MAX_STEPS : s_wifi_retry_count);

    return ret;
}

/* --------------------------------------------------------------------------
 * MQTT Connect Helper
 * -------------------------------------------------------------------------- */

static esp_err_t try_mqtt_connect(void)
{
    char broker_uri[128];
    esp_err_t ret = egos_mqtt_resolve_broker(s_cred_source, broker_uri, sizeof(broker_uri));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to resolve broker");
        return ret;
    }

    if (s_mqtt_initialized) {
        egos_mqtt_stop();
        s_mqtt_initialized = false;
    }

    ret = egos_mqtt_init(broker_uri, mqtt_status_cb, mqtt_timeout_cb);
    if (ret != ESP_OK) {
        return ret;
    }

    s_mqtt_initialized = true;
    return egos_mqtt_connect();
}

/* --------------------------------------------------------------------------
 * Connection Manager Task
 * -------------------------------------------------------------------------- */

static void connection_manager_task(void *pvParameters)
{
    uint32_t state_timer = 0;

    ESP_LOGI(TAG, "Connection manager started");
    egos_led_update(EGOS_LED_INIT);

    while (1) {
        switch (s_conn_state) {

#ifndef CONFIG_EGOS_ETHERNET_ENABLED
        /* EGOS_CONN_TRYING_ETHERNET is declared unconditionally in the state
         * enum, but its case body is compiled out when Ethernet support is
         * disabled. Without this the build fails under -Werror=switch, which
         * means the component would not compile in its own default
         * configuration (CONFIG_EGOS_ETHERNET_ENABLED defaults to n).
         * The state is unreachable here: nothing sets it when Ethernet is off. */
        case EGOS_CONN_TRYING_ETHERNET:
            break;
#endif

        case EGOS_CONN_INIT:
            state_timer = 0;

#ifdef CONFIG_EGOS_ETHERNET_ENABLED
            ESP_LOGI(TAG, "Trying Ethernet first...");
            egos_led_update(EGOS_LED_ETHERNET_CONNECTING);
            egos_ethernet_start();
            s_conn_state = EGOS_CONN_TRYING_ETHERNET;
#else
            /* Uses the sticky preferred source, which starts at STORED. A
             * failure here is handled by EGOS_CONN_TRYING_WIFI, which retries
             * and alternates rather than leaving the module stuck. */
            wifi_connect_preferred();
            s_conn_state = EGOS_CONN_TRYING_WIFI;
#endif
            break;

#ifdef CONFIG_EGOS_ETHERNET_ENABLED
        case EGOS_CONN_TRYING_ETHERNET:
            state_timer += TICK_MS;

            if (s_ethernet_connected) {
                ESP_LOGI(TAG, "Ethernet connected");
                s_active_network = EGOS_NET_ETHERNET;
                s_conn_state = EGOS_CONN_NETWORK_READY;
                break;
            }

            if (state_timer >= ETHERNET_TIMEOUT_MS) {
                ESP_LOGW(TAG, "Ethernet timeout, falling back to WiFi");
                egos_ethernet_stop();

                wifi_connect_preferred();
                s_active_network = (s_cred_source == EGOS_CRED_STORED)
                                   ? EGOS_NET_WIFI_STORED
                                   : EGOS_NET_WIFI_DEFAULT;

                s_conn_state = EGOS_CONN_TRYING_WIFI;
                state_timer = 0;
            }
            break;
#endif

        case EGOS_CONN_TRYING_WIFI:
            /* egos_wifi_is_connected() is checked as well as the callback flag:
             * the WiFi module sets its own state before signalling the event
             * group, so a successful connect can return here before
             * wifi_status_cb() has run. Without this we could briefly see
             * "not connected" and start a needless retry. */
            if (s_wifi_connected || egos_wifi_is_connected()) {
                char ip[16];
                egos_wifi_get_ip(ip, sizeof(ip));
                ESP_LOGI(TAG, "WiFi connected (IP: %s)", ip);
                state_timer = 0;
                s_conn_state = EGOS_CONN_NETWORK_READY;
                break;
            }

            /* Not connected. Retry after the backoff, alternating credential
             * source as wifi_connect_preferred() sees fit. Without this the
             * state machine would wait here forever for a connection that is
             * never coming. egos_wifi_connect() blocks until it succeeds or
             * times out, so this does not spin. */
            state_timer += TICK_MS;
            if (state_timer >= s_wifi_retry_backoff_ms) {
                state_timer = 0;
                if (wifi_connect_preferred() == ESP_OK) {
#ifdef CONFIG_EGOS_ETHERNET_ENABLED
                    s_active_network = (s_cred_source == EGOS_CRED_STORED)
                                       ? EGOS_NET_WIFI_STORED
                                       : EGOS_NET_WIFI_DEFAULT;
#endif
                }
            }
            break;

        case EGOS_CONN_NETWORK_READY:
            ESP_LOGI(TAG, "Network ready, connecting MQTT...");
            egos_led_update(EGOS_LED_MQTT_CONNECTING);

            /* Fresh attempt: nothing established yet, so CONNECTED must not
             * mistake "no reply yet" for "connection lost". */
            s_mqtt_established = false;

            if (try_mqtt_connect() != ESP_OK) {
                ESP_LOGE(TAG, "Failed to start MQTT connection");
                egos_led_update(EGOS_LED_ERROR);
                vTaskDelay(pdMS_TO_TICKS(2000));
                /* Retry */
                break;
            }

            s_conn_state = EGOS_CONN_CONNECTED;
            break;

        case EGOS_CONN_CONNECTED:
            /* Check for MQTT network switch request */
            if (s_mqtt_switch_requested) {
                ESP_LOGW(TAG, "MQTT fallback threshold reached, switching network");
                s_mqtt_switch_requested = false;
                s_mqtt_timeouts = 0;
                s_conn_state = EGOS_CONN_SWITCHING_NETWORK;
                break;
            }

            /* If MQTT dropped after having been connected, and the network is
             * still up, reconnect.
             *
             * s_mqtt_established is what stops this firing on the very first
             * tick after NETWORK_READY started a connection: at that point the
             * broker simply has not replied yet, which is not the same as a
             * lost connection. Without it a second connect raced the first and
             * the module registered its devices twice per boot, ~300ms apart,
             * and called on_connected twice. egos_mqtt has its own connect
             * timeout, so waiting here costs nothing. */
            if (!s_mqtt_connected && s_mqtt_established && (s_wifi_connected
#ifdef CONFIG_EGOS_ETHERNET_ENABLED
                || s_ethernet_connected
#endif
            )) {
                ESP_LOGI(TAG, "MQTT lost, reconnecting...");
                s_mqtt_established = false;
                egos_led_update(EGOS_LED_MQTT_CONNECTING);
                try_mqtt_connect();
            }
            break;

        case EGOS_CONN_SWITCHING_NETWORK:
            egos_led_update(EGOS_LED_NETWORK_SWITCHING);

            /* Stop current MQTT */
            if (s_mqtt_initialized) {
                egos_mqtt_stop();
                s_mqtt_initialized = false;
            }

#ifdef CONFIG_EGOS_ETHERNET_ENABLED
            /* Cycle: Ethernet → WiFi(default) → WiFi(stored) → Ethernet */
            switch (s_active_network) {
                case EGOS_NET_ETHERNET:
                    egos_ethernet_stop();
                    /* Drive the preference so the TRYING_WIFI retry path
                     * resumes on the network we are switching to */
                    s_preferred_cred_source = EGOS_CRED_DEFAULT;
                    s_cred_source_failures = 0;
                    s_active_network = EGOS_NET_WIFI_DEFAULT;
                    wifi_connect_preferred();
                    s_conn_state = EGOS_CONN_TRYING_WIFI;
                    break;

                case EGOS_NET_WIFI_DEFAULT:
                    egos_wifi_disconnect();
                    if (egos_nvs_has_wifi_creds()) {
                        s_preferred_cred_source = EGOS_CRED_STORED;
                        s_cred_source_failures = 0;
                        s_active_network = EGOS_NET_WIFI_STORED;
                        wifi_connect_preferred();
                        s_conn_state = EGOS_CONN_TRYING_WIFI;
                    } else {
                        s_active_network = EGOS_NET_ETHERNET;
                        egos_led_update(EGOS_LED_ETHERNET_CONNECTING);
                        egos_ethernet_start();
                        s_conn_state = EGOS_CONN_TRYING_ETHERNET;
                        state_timer = 0;
                    }
                    break;

                case EGOS_NET_WIFI_STORED:
                    egos_wifi_disconnect();
                    s_active_network = EGOS_NET_ETHERNET;
                    egos_led_update(EGOS_LED_ETHERNET_CONNECTING);
                    egos_ethernet_start();
                    s_conn_state = EGOS_CONN_TRYING_ETHERNET;
                    state_timer = 0;
                    break;
            }
#else
            /* WiFi only: toggle between default and stored credentials */
            egos_wifi_disconnect();

            if (s_cred_source == EGOS_CRED_DEFAULT && egos_nvs_has_wifi_creds()) {
                s_preferred_cred_source = EGOS_CRED_STORED;
            } else {
                s_preferred_cred_source = EGOS_CRED_DEFAULT;
            }
            s_cred_source_failures = 0;

            ESP_LOGI(TAG, "Switching to %s WiFi credentials",
                     cred_source_name(s_preferred_cred_source));
            wifi_connect_preferred();
            s_conn_state = EGOS_CONN_TRYING_WIFI;
#endif
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

esp_err_t egos_connection_start(void)
{
    /* Initialize NVS */
    esp_err_t ret = egos_nvs_init();
    if (ret != ESP_OK) {
        return ret;
    }

    /* Initialize status LED (optional) */
#ifdef CONFIG_EGOS_STATUS_LED_ENABLED
    egos_led_init();
#endif

    /* Initialize WiFi */
    ret = egos_wifi_init(wifi_status_cb);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi init failed");
        return ret;
    }

    /* Initialize Ethernet (optional) */
#ifdef CONFIG_EGOS_ETHERNET_ENABLED
    ret = egos_ethernet_init(ethernet_status_cb);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Ethernet init failed, will use WiFi only");
    }
#endif

    /* Create connection manager task */
    BaseType_t task_ret = xTaskCreate(connection_manager_task, "egos_conn",
                                       TASK_STACK_SIZE, NULL, TASK_PRIORITY,
                                       &s_task_handle);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create connection manager task");
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t egos_connection_stop(void)
{
    /* Delete task */
    if (s_task_handle) {
        vTaskDelete(s_task_handle);
        s_task_handle = NULL;
    }

    /* Stop MQTT */
    if (s_mqtt_initialized) {
        egos_mqtt_stop();
        s_mqtt_initialized = false;
    }

    /* Disconnect WiFi */
    egos_wifi_disconnect();

    /* Stop Ethernet */
#ifdef CONFIG_EGOS_ETHERNET_ENABLED
    egos_ethernet_stop();
    egos_ethernet_cleanup();
#endif

    /* Cleanup LED */
#ifdef CONFIG_EGOS_STATUS_LED_ENABLED
    egos_led_cleanup();
#endif

    s_conn_state = EGOS_CONN_INIT;
    ESP_LOGI(TAG, "Connection manager stopped");
    return ESP_OK;
}
