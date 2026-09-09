/**
 * @file egos_discovery.c
 * @brief MQTT broker discovery with reachability probing, NVS caching and
 *        subnet scanning.
 *
 * Ported from the motor-master module, which had a considerably more robust
 * discovery than the SDK. Two ideas make the difference:
 *
 *   - Every candidate address is PROVED reachable with a TCP connect before it
 *     is used. Resolving a name that no longer points at a live broker, or
 *     assuming the gateway is the controller, otherwise wastes a full MQTT
 *     connect timeout before anything notices.
 *   - A candidate that works is cached in NVS, so the next boot reconnects
 *     without waiting for mDNS at all. mDNS is the slowest step and the one
 *     most likely to fail on a busy or unusual network.
 *
 * Cascade, first reachable candidate wins:
 *   1. Broker IP configured by the caller  (trusted, not probed)
 *   2. Broker IP cached in NVS from a previous success
 *   3. Gateway IP, when joined to the default EGOS network - the controller
 *      is the gateway there
 *   4. mDNS resolution of the configured hostname
 *   5. Subnet scan of the local /24, common addresses first
 *   6. The hostname itself, handed to the MQTT client unresolved
 */

#include "sdkconfig.h"
#include "egos_internal.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "mdns.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <errno.h>

static const char *TAG = "egos_disc";

#define NVS_NAMESPACE       "broker_cfg"
#define NVS_KEY_IP          "broker_ip"

/* Probing a specific candidate we have reason to believe in. Generous, because
 * a slow answer still beats falling through to a subnet scan. */
#define PROBE_TIMEOUT_MS    3000

/* Probing addresses we are merely guessing at during a scan. Short, because
 * most will not answer and there may be 254 of them. */
#define SCAN_TIMEOUT_MS     500

#define MDNS_TIMEOUT_MS     5000

bool egos_discovery_load_stored_ip(char *ip_buf, size_t buf_len)
{
    if (ip_buf == NULL || buf_len < 16) {
        return false;
    }

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;   /* namespace absent on first ever boot - not an error */
    }

    size_t len = buf_len;
    esp_err_t err = nvs_get_str(h, NVS_KEY_IP, ip_buf, &len);
    nvs_close(h);

    if (err != ESP_OK || ip_buf[0] == '\0') {
        return false;
    }
    return true;
}

bool egos_discovery_save_ip(const char *ip)
{
    if (ip == NULL || ip[0] == '\0') {
        return false;
    }

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }

    esp_err_t err = nvs_set_str(h, NVS_KEY_IP, ip);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err == ESP_OK;
}

bool egos_discovery_clear_ip(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_erase_key(h, NVS_KEY_IP);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err == ESP_OK;
}

bool egos_discovery_check_port(const char *ip, uint16_t port, int timeout_ms)
{
    if (ip == NULL || ip[0] == '\0') {
        return false;
    }

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        return false;
    }

    /* Non-blocking connect plus select, so a host that silently drops packets
     * costs us timeout_ms rather than the stack's own much longer default. */
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port   = htons(port),
    };
    if (inet_pton(AF_INET, ip, &dest.sin_addr) != 1) {
        close(sock);
        return false;
    }

    bool reachable = false;
    int rc = connect(sock, (struct sockaddr *)&dest, sizeof(dest));

    if (rc == 0) {
        reachable = true;               /* connected immediately */
    } else if (errno == EINPROGRESS) {
        fd_set wset;
        FD_ZERO(&wset);
        FD_SET(sock, &wset);
        struct timeval tv = {
            .tv_sec  = timeout_ms / 1000,
            .tv_usec = (timeout_ms % 1000) * 1000,
        };
        if (select(sock + 1, NULL, &wset, NULL, &tv) > 0) {
            int err = 0;
            socklen_t err_len = sizeof(err);
            if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &err_len) == 0 && err == 0) {
                reachable = true;
            }
        }
    }

    close(sock);
    return reachable;
}

bool egos_discovery_scan_subnet(uint16_t port, char *ip_buf, size_t buf_len)
{
    if (ip_buf == NULL || buf_len < 16) {
        return false;
    }

    /* Work out our own /24 from whichever interface currently has an address. */
    esp_netif_ip_info_t ip_info;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == NULL || esp_netif_get_ip_info(netif, &ip_info) != ESP_OK ||
        ip_info.ip.addr == 0) {
        netif = esp_netif_get_handle_from_ifkey("ETH_DEF");
        if (netif == NULL || esp_netif_get_ip_info(netif, &ip_info) != ESP_OK ||
            ip_info.ip.addr == 0) {
            ESP_LOGW(TAG, "No usable interface address - cannot scan");
            return false;
        }
    }

    uint32_t host_ip = ntohl(ip_info.ip.addr);
    uint32_t subnet  = host_ip & 0xFFFFFF00u;
    uint8_t  self    = host_ip & 0xFFu;

    char candidate[16];

    /* Controllers overwhelmingly sit on one of these, so try them before
     * walking the whole range. */
    static const uint8_t common[] = { 1, 100, 254 };
    for (size_t i = 0; i < sizeof(common) / sizeof(common[0]); i++) {
        if (common[i] == self) {
            continue;
        }
        uint32_t addr = subnet | common[i];
        snprintf(candidate, sizeof(candidate), "%lu.%lu.%lu.%lu",
                 (unsigned long)((addr >> 24) & 0xFF), (unsigned long)((addr >> 16) & 0xFF),
                 (unsigned long)((addr >> 8) & 0xFF),  (unsigned long)(addr & 0xFF));
        if (egos_discovery_check_port(candidate, port, SCAN_TIMEOUT_MS)) {
            strncpy(ip_buf, candidate, buf_len - 1);
            ip_buf[buf_len - 1] = '\0';
            ESP_LOGI(TAG, "Found broker at common address %s", ip_buf);
            return true;
        }
    }

    ESP_LOGI(TAG, "Scanning the rest of the subnet for port %u...", (unsigned)port);
    for (uint32_t hostpart = 1; hostpart < 255; hostpart++) {
        if (hostpart == self || hostpart == 1 || hostpart == 100 || hostpart == 254) {
            continue;   /* self, and the ones already tried */
        }
        uint32_t addr = subnet | hostpart;
        snprintf(candidate, sizeof(candidate), "%lu.%lu.%lu.%lu",
                 (unsigned long)((addr >> 24) & 0xFF), (unsigned long)((addr >> 16) & 0xFF),
                 (unsigned long)((addr >> 8) & 0xFF),  (unsigned long)(addr & 0xFF));
        if (egos_discovery_check_port(candidate, port, SCAN_TIMEOUT_MS)) {
            strncpy(ip_buf, candidate, buf_len - 1);
            ip_buf[buf_len - 1] = '\0';
            ESP_LOGI(TAG, "Found broker at %s", ip_buf);
            return true;
        }
        /* Yield regularly - a full sweep is 250 probes and must not starve
         * the WiFi or MQTT tasks. */
        if ((hostpart & 0x0F) == 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    ESP_LOGW(TAG, "Subnet scan found no host with port %u open", (unsigned)port);
    return false;
}

bool egos_discovery_resolve(egos_cred_source_t cred_source,
                            char *ip_buf, size_t buf_len,
                            egos_discovery_source_t *source)
{
    if (ip_buf == NULL || buf_len < 16) {
        return false;
    }
    ip_buf[0] = '\0';

    const uint16_t port = egos_g_config.mqtt.broker_port;

    /* 1. Explicitly configured - the caller has told us where the broker is,
     *    so take them at their word rather than probing. */
    if (egos_g_config.mqtt.broker_ip) {
        strncpy(ip_buf, egos_g_config.mqtt.broker_ip, buf_len - 1);
        ip_buf[buf_len - 1] = '\0';
        if (source) *source = EGOS_DISCOVERY_CONFIGURED;
        ESP_LOGI(TAG, "Using configured broker IP: %s", ip_buf);
        return true;
    }

    /* 2. Cached from a previous success - by far the fastest path. */
    char candidate[16];
    if (egos_discovery_load_stored_ip(candidate, sizeof(candidate))) {
        ESP_LOGI(TAG, "Trying cached broker IP %s:%u", candidate, (unsigned)port);
        if (egos_discovery_check_port(candidate, port, PROBE_TIMEOUT_MS)) {
            strncpy(ip_buf, candidate, buf_len - 1);
            ip_buf[buf_len - 1] = '\0';
            if (source) *source = EGOS_DISCOVERY_CACHED;
            ESP_LOGI(TAG, "Cached broker IP reachable: %s", ip_buf);
            return true;
        }
        ESP_LOGW(TAG, "Cached broker IP %s not reachable, continuing", candidate);
    }

    /* 3. On the default EGOS network the controller is the gateway. Still
     *    probed, because that is only true when it really is our controller. */
    if (cred_source == EGOS_CRED_DEFAULT) {
        char gw[16];
        if (egos_wifi_get_gateway_ip(gw, sizeof(gw)) &&
            egos_discovery_check_port(gw, port, PROBE_TIMEOUT_MS)) {
            strncpy(ip_buf, gw, buf_len - 1);
            ip_buf[buf_len - 1] = '\0';
            egos_discovery_save_ip(gw);
            if (source) *source = EGOS_DISCOVERY_GATEWAY;
            ESP_LOGI(TAG, "Using gateway as broker (EGOS network): %s", ip_buf);
            return true;
        }
    }

    /* 4. mDNS. Verified before use, because a stale record resolves happily. */
    const char *hostname = egos_g_config.mqtt.broker_hostname;
    if (hostname && hostname[0]) {
        char host[64];
        strncpy(host, hostname, sizeof(host) - 1);
        host[sizeof(host) - 1] = '\0';
        char *suffix = strstr(host, ".local");
        if (suffix) *suffix = '\0';     /* mdns_query_a appends .local itself */

        esp_ip4_addr_t addr;
        if (mdns_query_a(host, MDNS_TIMEOUT_MS, &addr) == ESP_OK) {
            snprintf(candidate, sizeof(candidate), IPSTR, IP2STR(&addr));
            ESP_LOGI(TAG, "mDNS resolved %s to %s, verifying", host, candidate);
            if (egos_discovery_check_port(candidate, port, PROBE_TIMEOUT_MS)) {
                strncpy(ip_buf, candidate, buf_len - 1);
                ip_buf[buf_len - 1] = '\0';
                egos_discovery_save_ip(candidate);
                if (source) *source = EGOS_DISCOVERY_MDNS;
                ESP_LOGI(TAG, "Broker found via mDNS: %s", ip_buf);
                return true;
            }
            ESP_LOGW(TAG, "mDNS gave %s but port %u is closed", candidate, (unsigned)port);
        }
    }

    /* 5. Last resort before giving up on an address: sweep the subnet. */
    if (egos_discovery_scan_subnet(port, candidate, sizeof(candidate))) {
        strncpy(ip_buf, candidate, buf_len - 1);
        ip_buf[buf_len - 1] = '\0';
        egos_discovery_save_ip(candidate);
        if (source) *source = EGOS_DISCOVERY_SCAN;
        return true;
    }

    if (source) *source = EGOS_DISCOVERY_NONE;
    return false;
}
