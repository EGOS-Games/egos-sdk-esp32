/**
 * @file egos_sdk.h
 * @brief EGOS SDK for ESP32 — single public header
 *
 * Provides WiFi/MQTT connectivity, device registration, and the EGOS
 * module protocol for building hardware modules that integrate with
 * the EGOS controller and cloud platform.
 *
 * Usage:
 *   1. Define devices with EGOS_DEVICE()
 *   2. Create config with EGOS_CONFIG_DEFAULT() and customize
 *   3. Call egos_init(&config) then egos_start()
 *   4. Publish input events with egos_publish_input()
 *   5. Handle output commands via on_output_command callback
 */

#ifndef EGOS_SDK_H
#define EGOS_SDK_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "cJSON.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Device Definition
 * -------------------------------------------------------------------------- */

/**
 * Defines a single device within an EGOS module.
 * Devices are registered with the controller on MQTT connect.
 */
typedef struct {
    const char *id;             /**< Unique device ID within module (e.g., "button1") */
    const char *type;           /**< Device type (e.g., "button", "sensor", "led", "motor") */
    const char *name;           /**< Human-readable name (e.g., "Button 1") */
    const char *config_json;    /**< Optional JSON string for device-specific configuration, or NULL */
} egos_device_t;

/**
 * Convenience macro for defining a device in a static array.
 *
 * Example:
 *   static const egos_device_t devices[] = {
 *       EGOS_DEVICE("button1", "button", "Button 1", NULL),
 *       EGOS_DEVICE("led1", "led", "Status LED", "{\"color\":\"red\"}"),
 *   };
 */
#define EGOS_DEVICE(_id, _type, _name, _config) \
    { .id = (_id), .type = (_type), .name = (_name), .config_json = (_config) }

/* --------------------------------------------------------------------------
 * Callbacks
 * -------------------------------------------------------------------------- */

/**
 * Called when an output command is received from the controller.
 *
 * @param device_id  Target device ID (e.g., "led1")
 * @param command    Command name (e.g., "setState")
 * @param params     Parsed JSON parameters object, or NULL if none
 * @param user_data  User-provided context pointer from egos_config_t
 */
typedef void (*egos_output_command_cb_t)(const char *device_id, const char *command,
                                         const cJSON *params, void *user_data);

/**
 * Called when a configuration update is received for a device.
 *
 * @param device_id  Target device ID
 * @param config     Parsed JSON configuration object
 * @param user_data  User-provided context pointer from egos_config_t
 */
typedef void (*egos_configuration_cb_t)(const char *device_id, const cJSON *config,
                                        void *user_data);

/** Maximum module-defined topics registerable with egos_subscribe(). */
#define EGOS_MAX_CUSTOM_TOPICS 4

/**
 * Called when a message arrives on a module-defined topic registered with
 * egos_subscribe().
 *
 * @param topic_suffix  The suffix registered, e.g. "bringup"
 * @param payload       NUL-terminated message body (not parsed by the SDK)
 * @param user_data     User-provided context pointer from egos_config_t
 */
typedef void (*egos_message_cb_t)(const char *topic_suffix, const char *payload,
                                  void *user_data);

/**
 * Called when the module is fully connected (MQTT broker connected, devices registered).
 *
 * @param user_data  User-provided context pointer from egos_config_t
 */
typedef void (*egos_connected_cb_t)(void *user_data);

/**
 * Called when the module loses its MQTT connection.
 *
 * @param user_data  User-provided context pointer from egos_config_t
 */
typedef void (*egos_disconnected_cb_t)(void *user_data);

/* --------------------------------------------------------------------------
 * Configuration
 * -------------------------------------------------------------------------- */

/** WiFi station configuration */
typedef struct {
    const char *ssid;               /**< Default WiFi SSID */
    const char *password;           /**< Default WiFi password */
    uint8_t max_retry;              /**< Max connection retries before giving up */
    uint32_t connect_timeout_ms;    /**< Per-attempt connection timeout in ms */
} egos_wifi_config_t;

/** MQTT broker configuration */
typedef struct {
    const char *broker_hostname;    /**< Broker hostname for mDNS resolution */
    const char *broker_ip;          /**< Direct broker IP (skips discovery if set) */
    uint16_t broker_port;           /**< Broker port number */
    uint8_t keepalive_sec;          /**< MQTT keepalive interval in seconds */
    uint32_t connect_timeout_ms;    /**< MQTT connection timeout in ms */
} egos_mqtt_config_t;

#ifdef CONFIG_EGOS_ETHERNET_ENABLED
/** W5500 SPI Ethernet configuration */
typedef struct {
    int mosi_pin;                   /**< SPI MOSI GPIO */
    int miso_pin;                   /**< SPI MISO GPIO */
    int sclk_pin;                   /**< SPI SCLK GPIO */
    int cs_pin;                     /**< SPI CS GPIO */
    int int_pin;                    /**< W5500 interrupt GPIO (-1 for polling mode) */
    int spi_clock_mhz;             /**< SPI clock speed in MHz (default: 12) */
    int spi_host;                  /**< SPI host device (default: SPI2_HOST) */
} egos_ethernet_config_t;
#endif

#ifdef CONFIG_EGOS_STATUS_LED_ENABLED
/**
 * Status LED pin configuration.
 *
 * With CONFIG_EGOS_STATUS_LED_MONO set, the board has a single LED rather
 * than an RGB one: only red_pin is used and the other two are ignored.
 * Colour collapses to brightness, so the patterns still carry the state.
 */
typedef struct {
    int red_pin;                    /**< Red channel GPIO, or the only GPIO when mono */
    int green_pin;                  /**< Green channel GPIO. Ignored when mono. */
    int blue_pin;                   /**< Blue channel GPIO. Ignored when mono. */
} egos_led_config_t;
#endif

/** Top-level SDK configuration */
typedef struct {
    const char *module_prefix;      /**< Prefix for auto-generated module ID (e.g., "my-buttons") */

    egos_wifi_config_t wifi;        /**< WiFi configuration */
    egos_mqtt_config_t mqtt;        /**< MQTT broker configuration */

#ifdef CONFIG_EGOS_ETHERNET_ENABLED
    egos_ethernet_config_t ethernet; /**< W5500 Ethernet configuration */
#endif

#ifdef CONFIG_EGOS_STATUS_LED_ENABLED
    egos_led_config_t status_led;   /**< RGB LED pin configuration */
#endif

    const egos_device_t *devices;   /**< Array of device definitions */
    size_t device_count;            /**< Number of devices in the array */

    egos_output_command_cb_t on_output_command;  /**< Output command callback (optional) */
    egos_configuration_cb_t on_configuration;    /**< Configuration update callback (optional) */
    egos_connected_cb_t on_connected;            /**< Connected callback (optional) */
    egos_disconnected_cb_t on_disconnected;      /**< Disconnected callback (optional) */

    void *user_data;                /**< Passed to all callbacks */
} egos_config_t;

/**
 * Default configuration macro. Provides sensible defaults for the EGOS network.
 *
 * WiFi: ssid="egos", password="egosegos"
 * MQTT: broker="egos.local", port=1883, keepalive=15s
 *
 * Usage:
 *   egos_config_t config = EGOS_CONFIG_DEFAULT();
 *   config.module_prefix = "my-module";
 *   config.devices = my_devices;
 *   config.device_count = ARRAY_SIZE(my_devices);
 */
#define EGOS_CONFIG_DEFAULT() { \
    .module_prefix = "module", \
    .wifi = { \
        .ssid = "egos", \
        .password = "egosegos", \
        .max_retry = 10, \
        .connect_timeout_ms = 7000, \
    }, \
    .mqtt = { \
        .broker_hostname = "egos.local", \
        .broker_ip = NULL, \
        .broker_port = 1883, \
        .keepalive_sec = 15, \
        .connect_timeout_ms = 10000, \
    }, \
    .devices = NULL, \
    .device_count = 0, \
    .on_output_command = NULL, \
    .on_configuration = NULL, \
    .on_connected = NULL, \
    .on_disconnected = NULL, \
    .user_data = NULL, \
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

/**
 * Initialize the EGOS SDK.
 * Validates configuration and generates the module ID from the ESP32 MAC address.
 * Must be called before egos_start().
 *
 * @param config  SDK configuration (copied internally)
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if config is invalid
 */
esp_err_t egos_init(const egos_config_t *config);

/**
 * Start the EGOS SDK.
 * Spawns the connection manager task which handles WiFi, MQTT, and device
 * registration automatically. Non-blocking — returns immediately.
 *
 * @return ESP_OK on success
 */
esp_err_t egos_start(void);

/**
 * Stop the EGOS SDK.
 * Disconnects MQTT and WiFi, stops the connection manager task, and frees resources.
 *
 * @return ESP_OK on success
 */
esp_err_t egos_stop(void);

/**
 * Publish an input event from a device.
 *
 * Publishes to: {moduleId}/device/{deviceId}/input
 * Payload: {"command":"<command>","parameters":{<params_json>}}
 *
 * @param device_id   Device ID (e.g., "button1")
 * @param command     Command name (e.g., "changed", "reading")
 * @param params_json JSON string for parameters (e.g., "{\"pressed\":true}")
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if not connected
 */
esp_err_t egos_publish_input(const char *device_id, const char *command, const char *params_json);

/**
 * Publish a state update for a device.
 *
 * Publishes to: {moduleId}/device/{deviceId}/state
 * Payload: the provided state_json string
 *
 * @param device_id  Device ID (e.g., "led1")
 * @param state_json JSON string for device state
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if not connected
 */
esp_err_t egos_publish_state(const char *device_id, const char *state_json);

/**
 * Publish a module-level telemetry payload.
 *
 * Publishes to: {moduleId}/system
 *
 * For readings that belong to the board rather than to any one device -
 * board temperature, per-port current, fault flags, uptime. Device-specific
 * state belongs in egos_publish_state() so the controller can attribute it.
 *
 * @param state_json JSON string for the module payload
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if not connected
 */
esp_err_t egos_publish_system(const char *state_json);

/**
 * Subscribe to a module-defined topic under {moduleId}/.
 *
 * For side channels that are not device traffic - a bring-up console, a
 * diagnostic trigger. The SDK subscribes on connect and re-subscribes on
 * every reconnect, and hands the raw payload to the callback without
 * parsing it.
 *
 * Protocol topics are matched first, so registering a suffix that collides
 * with one (e.g. "output") leaves the protocol behaviour intact and the
 * callback simply never fires.
 *
 * Call before egos_start() where possible; a later call still works and
 * subscribes immediately if MQTT is already up.
 *
 * @param topic_suffix  Suffix after "{moduleId}/", e.g. "bringup" (max 23 chars)
 * @param cb            Handler for messages on that topic
 * @return ESP_OK, ESP_ERR_NO_MEM if EGOS_MAX_CUSTOM_TOPICS is exhausted,
 *         ESP_ERR_INVALID_SIZE if the suffix is too long
 */
esp_err_t egos_subscribe(const char *topic_suffix, egos_message_cb_t cb);

/**
 * Get the auto-generated module ID.
 * Format: "{prefix}-{MAC}" where MAC is the 12-char hex of the ESP32 MAC address.
 *
 * @return Module ID string, or NULL if not yet initialized
 */
const char *egos_get_module_id(void);

/**
 * Check if the SDK is fully connected (MQTT broker connected).
 *
 * @return true if connected and ready to publish
 */
bool egos_is_connected(void);

/**
 * Briefly flash the status LED to acknowledge a local input event.
 *
 * Modules with physical inputs use this so a button press is visibly
 * acknowledged even when the module is offline and the event cannot be
 * published. The LED returns to showing connection state afterwards.
 *
 * No-op when CONFIG_EGOS_STATUS_LED_ENABLED is off, so callers do not need
 * to guard it.
 */
void egos_indicate_input(void);

/**
 * Bring the status LED up early, before egos_start().
 *
 * The SDK normally initialises the LED as part of egos_start(). A module
 * that puts safety-critical hardware into a known state before going near
 * the network needs the LED working during that window, so it can show a
 * fault raised by its own bring-up. Calling this first makes that possible;
 * the SDK's later initialisation becomes a no-op.
 *
 * Requires egos_init() to have run, since the pin configuration comes from
 * the config. Safe to call more than once. No-op when
 * CONFIG_EGOS_STATUS_LED_ENABLED is off.
 */
esp_err_t egos_status_led_begin(void);

/**
 * Latch or clear a module fault on the status LED.
 *
 * While a fault is latched the LED shows the error pattern and connection
 * state changes underneath are recorded but not displayed, so a board that
 * has tripped its safety layer cannot show a reassuring green while faulted.
 * Clearing the fault restores whatever the connection state had become.
 *
 * For modules with a safety or fault layer of their own. No-op when
 * CONFIG_EGOS_STATUS_LED_ENABLED is off, so callers need not guard it.
 *
 * @param active true to latch the fault, false to clear it
 */
void egos_indicate_fault(bool active);

/* --------------------------------------------------------------------------
 * Per-device persisted settings
 *
 * For anything a module must remember per device across reboots: a motor's
 * direction inversion and ramp time, a servo's pulse limits, a sensor's
 * calibration.
 *
 * Stored as opaque blobs keyed by device id. The SDK deliberately does not
 * define the shapes - a motor's settings belong to the module that has motors,
 * and putting them here would mean an SDK change for every new device type.
 * The module owns the struct and its schema version; the SDK owns persistence.
 *
 * Because the SDK cannot know whether a blob written by an earlier firmware
 * still matches the current struct, callers should carry a version field and
 * check it:
 *
 *     motor_cfg_t cfg;
 *     if (egos_settings_load("motor1", &cfg, sizeof(cfg)) != ESP_OK ||
 *         cfg.version != MOTOR_CFG_VERSION) {
 *         cfg = motor_cfg_defaults();
 *         egos_settings_save("motor1", &cfg, sizeof(cfg));
 *     }
 * -------------------------------------------------------------------------- */

/**
 * Persist a device's settings.
 *
 * @param device_id  Device id, as registered and as the controller addresses it
 * @param data       Settings struct to store
 * @param len        sizeof that struct
 * @return ESP_OK on success
 */
esp_err_t egos_settings_save(const char *device_id, const void *data, size_t len);

/**
 * Load a device's settings.
 *
 * @return ESP_OK on success; ESP_ERR_NVS_NOT_FOUND if never saved;
 *         ESP_ERR_INVALID_SIZE if the stored blob is a different size to len,
 *         which means the struct changed shape and the caller should fall back
 *         to defaults rather than trust the contents
 */
esp_err_t egos_settings_load(const char *device_id, void *data, size_t len);

/** Erase one device's settings. Succeeds if there were none. */
esp_err_t egos_settings_erase(const char *device_id);

/** Erase every device's settings. Intended for a factory reset. */
esp_err_t egos_settings_erase_all(void);

#ifdef __cplusplus
}
#endif

#endif /* EGOS_SDK_H */
