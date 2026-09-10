/**
 * @file egos_wifi.c
 * @brief WiFi STA connection management
 */

#include "egos_internal.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "mdns.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "egos_wifi";

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

static EventGroupHandle_t s_wifi_event_group = NULL;
static esp_netif_t *s_sta_netif = NULL;
static egos_internal_status_cb_t s_status_cb = NULL;
static volatile bool s_connected = false;
static int s_retry_count = 0;
static bool s_mdns_initialized = false;
static bool s_initialized = false;
/* Reason from the most recent STA_DISCONNECTED, exposed so the connection
 * manager can tell "SSID not in range" from "SSID there but rejecting us" */
static uint8_t s_last_disconnect_reason = 0;
/* Consecutive reason-201 disconnects within the current connect attempt */
static uint8_t s_no_ap_found_count = 0;
/* Set while the diagnostic scan runs. esp_wifi_start() fires STA_START,
 * whose handler calls esp_wifi_connect(); a scan cannot start while a
 * connect is in flight, so without this the diagnostic silently fails and
 * reports an empty AP list - which reads exactly like a dead radio. */
static volatile bool s_diag_scan_active = false;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (s_diag_scan_active) {
            return;             /* scanning, do not auto-connect */
        }
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disconn = (wifi_event_sta_disconnected_t *)event_data;
        s_connected = false;
        s_last_disconnect_reason = disconn->reason;

        if (disconn->reason == EGOS_WIFI_REASON_NO_AP_FOUND) {
            s_no_ap_found_count++;
        }

        if (s_no_ap_found_count >= EGOS_WIFI_NO_AP_FOUND_ABORT_COUNT) {
            /* Each attempt runs a full scan, so repeated "no AP found" means the
             * SSID genuinely is not in range. Abandon the attempt now rather
             * than burning the remaining retries and the caller's timeout, so
             * the connection manager can try the other credential source. */
            ESP_LOGE(TAG, "SSID '%s' not found after %d scans - abandoning attempt so a fallback network can be tried",
                     (const char *)disconn->ssid, s_no_ap_found_count);
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        } else if (s_retry_count < egos_g_config.wifi.max_retry) {
            s_retry_count++;
            ESP_LOGW(TAG, "WiFi disconnected (reason: %d), retry %d/%d",
                     disconn->reason, s_retry_count, egos_g_config.wifi.max_retry);
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "WiFi connection failed after %d retries", s_retry_count);
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }

        if (s_status_cb) {
            s_status_cb(false);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));

        s_retry_count = 0;
        s_connected = true;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

        /* Initialize mDNS for .local hostname resolution */
        if (!s_mdns_initialized) {
            if (mdns_init() == ESP_OK) {
                s_mdns_initialized = true;
                ESP_LOGI(TAG, "mDNS initialized");
            } else {
                ESP_LOGW(TAG, "mDNS init failed");
            }
        }

        if (s_status_cb) {
            s_status_cb(true);
        }
    }
}

esp_err_t egos_wifi_init(egos_internal_status_cb_t status_cb)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "WiFi already initialized");
        return ESP_OK;
    }

    s_status_cb = status_cb;
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    /* Create default event loop if not already created */
    esp_err_t ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to create event loop: %s", esp_err_to_name(ret));
        return ret;
    }

    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    s_initialized = true;
    ESP_LOGI(TAG, "WiFi initialized");
    return ESP_OK;
}


/**
 * Log every AP the radio can actually hear.
 *
 * "SSID not found" on its own is a dead end: it cannot distinguish an AP that
 * is switched off from one that is simply out of reach of THIS board, and the
 * two need completely different fixes. Reading the radio at the moment it gives
 * up settles that, and the RSSI says whether a marginal signal is the problem.
 *
 * Must run in task context, not the event handler - esp_wifi_scan_start() with
 * block=true cannot be called from the event loop.
 */
static void log_visible_aps(const char *wanted_ssid)
{
    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,               /* all channels */
        .show_hidden = true,
    };

    esp_err_t serr = esp_wifi_scan_start(&scan_cfg, true);
    if (serr != ESP_OK) {
        ESP_LOGW(TAG, "diagnostic scan did not run (%s) - no conclusion can be"
                      " drawn about what the radio can hear", esp_err_to_name(serr));
        return;
    }

    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    if (count == 0) {
        ESP_LOGE(TAG, "Scan ran but found NOTHING - if other devices nearby can "
                      "see networks, suspect this board's antenna");
        return;
    }

    uint16_t max = count > 20 ? 20 : count;
    wifi_ap_record_t *recs = calloc(max, sizeof(wifi_ap_record_t));
    if (recs == NULL) {
        esp_wifi_clear_ap_list();
        return;
    }

    esp_wifi_scan_get_ap_records(&max, recs);

    ESP_LOGW(TAG, "--- radio can hear %u network(s); '%s' is not among them ---",
             (unsigned)count, wanted_ssid ? wanted_ssid : "?");
    for (uint16_t i = 0; i < max; i++) {
        ESP_LOGW(TAG, "    %-32s ch %2d  %4d dBm  auth %d",
                 (const char *)recs[i].ssid, recs[i].primary,
                 recs[i].rssi, recs[i].authmode);
    }

    free(recs);
    esp_wifi_clear_ap_list();
}

esp_err_t egos_wifi_connect(egos_cred_source_t source, egos_cred_source_t *actual_source)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    s_retry_count = 0;
    /* Fresh attempt - clear the per-attempt disconnect diagnostics so the
     * caller reads the reason for THIS attempt, not the previous one */
    s_last_disconnect_reason = 0;
    s_no_ap_found_count = 0;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .sae_pwe_h2e = WPA3_SAE_PWE_UNSPECIFIED,
            .pmf_cfg = {
                .capable = false,
                .required = false,
            },
            .scan_method = WIFI_FAST_SCAN,
            .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
        },
    };

    if (source == EGOS_CRED_STORED) {
        char ssid[33] = {0};
        char pass[65] = {0};
        esp_err_t ret = egos_nvs_load_wifi_creds(ssid, sizeof(ssid), pass, sizeof(pass));
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "No stored credentials, falling back to default");
            source = EGOS_CRED_DEFAULT;
        } else {
            memcpy(wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
            memcpy(wifi_config.sta.password, pass, sizeof(wifi_config.sta.password));
            ESP_LOGI(TAG, "Connecting with stored credentials (SSID: %s)", ssid);
        }
    }

    if (source == EGOS_CRED_DEFAULT) {
        strncpy((char *)wifi_config.sta.ssid, egos_g_config.wifi.ssid,
                sizeof(wifi_config.sta.ssid) - 1);
        strncpy((char *)wifi_config.sta.password, egos_g_config.wifi.password,
                sizeof(wifi_config.sta.password) - 1);
        ESP_LOGI(TAG, "Connecting with default credentials (SSID: %s)",
                 egos_g_config.wifi.ssid);
    }

    /* Report what was actually used - "source" may have been downgraded above
     * when STORED was requested but NVS held nothing */
    if (actual_source != NULL) {
        *actual_source = source;
    }

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Set max TX power and disable power save for low latency */
    esp_wifi_set_max_tx_power(78);  /* 19.5 dBm */
    esp_wifi_set_ps(WIFI_PS_NONE);

    /* Wait for connection or failure */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                            pdFALSE, pdFALSE,
                                            pdMS_TO_TICKS(egos_g_config.wifi.connect_timeout_ms));

    if (bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    }

    /* Failed. Stop the driver before returning.
     *
     * This is essential, not tidiness. esp_wifi_connect() is only ever called
     * from the WIFI_EVENT_STA_START handler, and STA_START only fires when the
     * driver transitions from stopped to started. If a failed attempt leaves
     * the driver running, the next esp_wifi_start() is a no-op, STA_START never
     * fires, esp_wifi_connect() is never called - and the attempt silently does
     * nothing until it times out, reporting no disconnect reason at all.
     *
     * Observed on hardware: only the very first connection attempt after boot
     * did anything. Every retry and every credential-source fallback timed out
     * with reason 0, including for an SSID that was definitely absent and had
     * correctly reported 201 on the first try. That made the whole retry and
     * fallback mechanism inert. */
    esp_wifi_stop();
    s_retry_count = 0;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    if (bits & WIFI_FAIL_BIT) {
        /* Only for the "not in range" case. A wrong password or a
         * rejecting AP is a different failure and the scan would say
         * nothing useful about it. */
        if (s_last_disconnect_reason == EGOS_WIFI_REASON_NO_AP_FOUND) {
            s_diag_scan_active = true;
            esp_wifi_start();
            /* Let the PHY settle before scanning. Scanning the instant the
             * driver starts can return an empty list on its own, which
             * would make this diagnostic lie in exactly the direction that
             * matters - reporting a dead radio when the radio is fine. */
            vTaskDelay(pdMS_TO_TICKS(500));
            log_visible_aps((const char *)wifi_config.sta.ssid);
            esp_wifi_stop();
            s_diag_scan_active = false;
        }
        return ESP_FAIL;
    }

    ESP_LOGW(TAG, "WiFi connection timed out");
    return ESP_ERR_TIMEOUT;
}

esp_err_t egos_wifi_disconnect(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    s_connected = false;
    esp_wifi_disconnect();
    esp_wifi_stop();
    ESP_LOGI(TAG, "WiFi disconnected");
    return ESP_OK;
}

bool egos_wifi_is_connected(void)
{
    return s_connected;
}

uint8_t egos_wifi_get_last_disconnect_reason(void)
{
    return s_last_disconnect_reason;
}

bool egos_wifi_get_ip(char *ip_str, size_t len)
{
    if (!s_connected || !s_sta_netif || len < 16) {
        return false;
    }

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(s_sta_netif, &ip_info) == ESP_OK) {
        snprintf(ip_str, len, IPSTR, IP2STR(&ip_info.ip));
        return true;
    }
    return false;
}

bool egos_wifi_get_gateway_ip(char *ip_str, size_t len)
{
    if (!s_connected || !s_sta_netif || len < 16) {
        return false;
    }

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(s_sta_netif, &ip_info) == ESP_OK) {
        snprintf(ip_str, len, IPSTR, IP2STR(&ip_info.gw));
        return true;
    }
    return false;
}
