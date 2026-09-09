/**
 * @file egos_ethernet.c
 * @brief W5500 SPI Ethernet manager (optional)
 *
 * Only compiled when CONFIG_EGOS_ETHERNET_ENABLED is set in Kconfig.
 */

/* sdkconfig.h must be included BEFORE the guard below, otherwise
 * CONFIG_EGOS_ETHERNET_ENABLED is undefined when the #ifdef is evaluated and
 * this file compiles to nothing whatever the configuration says - which is
 * exactly what used to happen, leaving egos_ethernet_init/egos_ethernet_start
 * undefined at link time. */
#include "sdkconfig.h"

#ifdef CONFIG_EGOS_ETHERNET_ENABLED

#include "egos_internal.h"
#include "esp_eth.h"
#include "esp_eth_mac_spi.h"
#include "esp_mac.h"      /* esp_read_mac / ESP_MAC_ETH */
#include "esp_eth_phy.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "mdns.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include <string.h>

static const char *TAG = "egos_eth";

static esp_eth_handle_t s_eth_handle = NULL;
static esp_netif_t *s_eth_netif = NULL;
static egos_internal_status_cb_t s_status_cb = NULL;
static volatile bool s_connected = false;
static bool s_mdns_initialized = false;

static void eth_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    switch (event_id) {
        case ETHERNET_EVENT_CONNECTED: {
            uint8_t mac[6];
            esp_eth_handle_t handle = *(esp_eth_handle_t *)event_data;
            esp_eth_ioctl(handle, ETH_CMD_G_MAC_ADDR, mac);
            ESP_LOGI(TAG, "Ethernet Link Up (MAC: %02x:%02x:%02x:%02x:%02x:%02x)",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            break;
        }
        case ETHERNET_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "Ethernet Link Down");
            s_connected = false;
            if (s_status_cb) s_status_cb(false);
            break;
        case ETHERNET_EVENT_STOP:
            s_connected = false;
            if (s_status_cb) s_status_cb(false);
            break;
        default:
            break;
    }
}

static void eth_got_ip_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "Ethernet Got IP: " IPSTR, IP2STR(&event->ip_info.ip));

    if (!s_mdns_initialized) {
        if (mdns_init() == ESP_OK) {
            s_mdns_initialized = true;
        }
    }

    s_connected = true;
    if (s_status_cb) s_status_cb(true);
}

esp_err_t egos_ethernet_init(egos_internal_status_cb_t status_cb)
{
    s_status_cb = status_cb;

    const egos_ethernet_config_t *cfg = &egos_g_config.ethernet;

    ESP_LOGI(TAG, "Initializing W5500 Ethernet (MOSI=%d, MISO=%d, SCLK=%d, CS=%d, INT=%d)",
             cfg->mosi_pin, cfg->miso_pin, cfg->sclk_pin, cfg->cs_pin, cfg->int_pin);

    /* Initialize SPI bus */
    spi_bus_config_t buscfg = {
        .mosi_io_num = cfg->mosi_pin,
        .miso_io_num = cfg->miso_pin,
        .sclk_io_num = cfg->sclk_pin,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };

    int spi_host = cfg->spi_host ? cfg->spi_host : SPI2_HOST;
    esp_err_t ret = spi_bus_initialize(spi_host, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Create Ethernet netif */
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    s_eth_netif = esp_netif_new(&netif_cfg);

    /* Configure W5500 SPI device */
    int clock_hz = (cfg->spi_clock_mhz > 0 ? cfg->spi_clock_mhz : 5) * 1000 * 1000;

    spi_device_interface_config_t devcfg = {
        .mode = 0,
        .clock_speed_hz = clock_hz,
        .queue_size = 20,
        .spics_io_num = cfg->cs_pin,
    };

    eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(spi_host, &devcfg);
    w5500_config.int_gpio_num = cfg->int_pin;
    w5500_config.poll_period_ms = (cfg->int_pin < 0) ? 100 : 0;

    /* Create MAC and PHY */
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    mac_config.rx_task_stack_size = 4096;
    mac_config.rx_task_prio = 15;

    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);
    if (!mac) {
        ESP_LOGE(TAG, "Failed to create W5500 MAC");
        return ESP_FAIL;
    }

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.reset_gpio_num = -1;

    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_config);
    if (!phy) {
        ESP_LOGE(TAG, "Failed to create W5500 PHY");
        return ESP_FAIL;
    }

    /* Install Ethernet driver */
    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    ret = esp_eth_driver_install(&eth_config, &s_eth_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet driver install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* The W5500 has no MAC address of its own, so one must be programmed before
     * the interface is attached. Without this the netif comes up as
     * 00:00:00:00:00:00, which no switch will forward and no DHCP server will
     * answer - Ethernet appears to link up but never obtains an IP. Deriving it
     * from the chip's efuse base MAC keeps it stable across reboots and unique
     * per board. */
    uint8_t eth_mac[6] = {0};
    ret = esp_read_mac(eth_mac, ESP_MAC_ETH);
    if (ret == ESP_OK) {
        ret = esp_eth_ioctl(s_eth_handle, ETH_CMD_S_MAC_ADDR, eth_mac);
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set Ethernet MAC address: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "Ethernet MAC address set to %02x:%02x:%02x:%02x:%02x:%02x",
             eth_mac[0], eth_mac[1], eth_mac[2], eth_mac[3], eth_mac[4], eth_mac[5]);

    /* Attach to TCP/IP stack */
    ret = esp_netif_attach(s_eth_netif, esp_eth_new_netif_glue(s_eth_handle));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet netif attach failed");
        return ret;
    }

    /* Register event handlers */
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                                &eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                                &eth_got_ip_handler, NULL));

    ESP_LOGI(TAG, "Ethernet initialized");
    return ESP_OK;
}

esp_err_t egos_ethernet_start(void)
{
    if (!s_eth_handle) return ESP_ERR_INVALID_STATE;
    return esp_eth_start(s_eth_handle);
}

esp_err_t egos_ethernet_stop(void)
{
    if (!s_eth_handle) return ESP_ERR_INVALID_STATE;
    s_connected = false;
    return esp_eth_stop(s_eth_handle);
}

bool egos_ethernet_is_connected(void)
{
    return s_connected;
}

bool egos_ethernet_get_ip(char *ip_str, size_t len)
{
    if (!s_connected || !s_eth_netif || len < 16) return false;

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(s_eth_netif, &ip_info) == ESP_OK) {
        snprintf(ip_str, len, IPSTR, IP2STR(&ip_info.ip));
        return true;
    }
    return false;
}

void egos_ethernet_cleanup(void)
{
    if (s_mdns_initialized) {
        mdns_free();
        s_mdns_initialized = false;
    }

    if (s_eth_handle) {
        esp_eth_stop(s_eth_handle);
        esp_eth_driver_uninstall(s_eth_handle);
        s_eth_handle = NULL;
    }

    if (s_eth_netif) {
        esp_netif_destroy(s_eth_netif);
        s_eth_netif = NULL;
    }

    s_connected = false;
    ESP_LOGI(TAG, "Ethernet cleaned up");
}

#endif /* CONFIG_EGOS_ETHERNET_ENABLED */
