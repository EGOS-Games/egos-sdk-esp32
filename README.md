# EGOS SDK for ESP32

A reusable ESP-IDF component for building EGOS hardware modules on ESP32. Handles WiFi, MQTT, device registration, and connection management so you can focus on your hardware logic.

## Quick Start

```c
#include "egos_sdk.h"

static const egos_device_t devices[] = {
    EGOS_DEVICE("button1", "button", "Button 1", NULL),
    EGOS_DEVICE("led1",    "led",    "Status LED", NULL),
};

static void on_command(const char *device_id, const char *command,
                       const cJSON *params, void *user_data) {
    // Handle output commands from the controller
}

void app_main(void) {
    egos_config_t config = EGOS_CONFIG_DEFAULT();
    config.module_prefix = "my-module";
    config.devices = devices;
    config.device_count = 2;
    config.on_output_command = on_command;

    egos_init(&config);
    egos_start();

    // Publish input events from your hardware logic:
    egos_publish_input("button1", "changed", "{\"pressed\":true}");
}
```

## Installation

Add as a git submodule to your ESP-IDF project:

```bash
cd my-esp32-project
git submodule add https://github.com/EGOS-Games/egos-sdk-esp32s3.git components/egos-sdk-esp32s3
```

Your project structure should look like:

```
my-module/
├── CMakeLists.txt
├── sdkconfig.defaults        # Optional Kconfig overrides
├── components/
│   └── egos-sdk-esp32s3/     # This SDK (git submodule)
└── main/
    ├── CMakeLists.txt        # REQUIRES egos-sdk-esp32s3
    └── main.c
```

In your `main/CMakeLists.txt`:

```cmake
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES egos-sdk-esp32s3
)
```

## Configuration

Start with `EGOS_CONFIG_DEFAULT()` and customize what you need:

```c
egos_config_t config = EGOS_CONFIG_DEFAULT();
```

### Default Values

| Field | Default | Description |
|-------|---------|-------------|
| `module_prefix` | `"module"` | Prefix for auto-generated module ID |
| `wifi.ssid` | `"egos"` | Default WiFi SSID |
| `wifi.password` | `"egosegos"` | Default WiFi password |
| `wifi.max_retry` | `10` | Max WiFi connection retries |
| `wifi.connect_timeout_ms` | `7000` | WiFi connection timeout |
| `mqtt.broker_hostname` | `"egos.local"` | MQTT broker hostname (mDNS) |
| `mqtt.broker_ip` | `NULL` | Direct broker IP (skips discovery) |
| `mqtt.broker_port` | `1883` | MQTT broker port |
| `mqtt.keepalive_sec` | `15` | MQTT keepalive interval |
| `mqtt.connect_timeout_ms` | `10000` | MQTT connection timeout |

### Module ID

The SDK auto-generates a unique module ID from the ESP32 MAC address:

```
{module_prefix}-{MAC_ADDRESS}
```

Example: `my-buttons-AABBCCDDEEFF`

This ID is used as the MQTT client ID and in all topic paths. Get it at runtime with `egos_get_module_id()`.

## Defining Devices

Use the `EGOS_DEVICE()` macro to define your devices:

```c
static const egos_device_t devices[] = {
    EGOS_DEVICE("button1", "button", "Button 1", NULL),
    EGOS_DEVICE("sensor1", "sensor", "Temperature", "{\"unit\":\"celsius\"}"),
    EGOS_DEVICE("motor1",  "motor",  "Door Motor", NULL),
};
```

Parameters:
- **id** — Unique device ID within the module (used in MQTT topics)
- **type** — Device type: `button`, `sensor`, `led`, `motor`, `neopixel`, `keypad`, `lcd`, `switch`, `dmx`, etc.
- **name** — Human-readable name shown in the EGOS Studio
- **config_json** — Optional JSON string for device-specific configuration, or `NULL`

Devices are automatically registered with the controller when MQTT connects.

## Publishing Events

### Input Events

Publish when device state changes (button press, sensor reading, etc.):

```c
// Button press
egos_publish_input("button1", "changed", "{\"pressed\":true}");

// Sensor reading
egos_publish_input("sensor1", "reading", "{\"temperature\":23.5}");

// Keypad input
egos_publish_input("keypad1", "keyPress", "{\"key\":\"5\"}");
```

The SDK builds the MQTT message: `{"command":"<command>","parameters":{<params>}}`

### State Updates

Publish the current state of a device:

```c
egos_publish_state("led1", "{\"power\":true,\"brightness\":100}");
```

Both functions return `ESP_ERR_INVALID_STATE` if not connected. Check `egos_is_connected()` first, or just fire-and-forget.

## Handling Commands

### Output Commands

Register a callback to handle commands from the controller:

```c
static void on_command(const char *device_id, const char *command,
                       const cJSON *params, void *user_data) {
    if (strcmp(device_id, "led1") == 0 && strcmp(command, "setState") == 0) {
        cJSON *state = cJSON_GetObjectItem(params, "state");
        if (cJSON_IsString(state)) {
            bool on = strcmp(state->valuestring, "on") == 0;
            gpio_set_level(LED_PIN, on);
        }
    }

    if (strcmp(device_id, "motor1") == 0 && strcmp(command, "forward") == 0) {
        cJSON *speed = cJSON_GetObjectItem(params, "speed");
        if (cJSON_IsNumber(speed)) {
            motor_set_speed(speed->valueint);
        }
    }
}

config.on_output_command = on_command;
```

### Configuration Updates

Register a callback to handle device configuration changes:

```c
static void on_config(const char *device_id, const cJSON *config, void *user_data) {
    ESP_LOGI(TAG, "Config update for %s", device_id);
    // Apply new configuration
}

config.on_configuration = on_config;
```

### Lifecycle Callbacks

```c
config.on_connected = on_connected;       // Called when MQTT connects
config.on_disconnected = on_disconnected; // Called when MQTT disconnects
config.user_data = &my_app_state;         // Passed to all callbacks
```

## Optional Features

### W5500 Ethernet

Enable in `sdkconfig.defaults`:

```
CONFIG_EGOS_ETHERNET_ENABLED=y
```

Configure the SPI pins:

```c
egos_config_t config = EGOS_CONFIG_DEFAULT();
config.ethernet.mosi_pin = 11;
config.ethernet.miso_pin = 13;
config.ethernet.sclk_pin = 12;
config.ethernet.cs_pin = 10;
config.ethernet.int_pin = -1;        // -1 for polling mode
config.ethernet.spi_clock_mhz = 5;
config.ethernet.spi_host = SPI2_HOST;
```

When enabled, the connection manager tries Ethernet first with a 5-second timeout, then falls back to WiFi.

### RGB Status LED

Enable in `sdkconfig.defaults`:

```
CONFIG_EGOS_STATUS_LED_ENABLED=y
```

Configure the GPIO pins:

```c
config.status_led.red_pin = 41;
config.status_led.green_pin = 42;
config.status_led.blue_pin = 2;
```

**LED State Colors:**

| State | Color | Pattern |
|-------|-------|---------|
| Init | White | Pulse |
| WiFi connecting (default) | Blue | Blink |
| WiFi connecting (stored) | Cyan | Blink |
| Ethernet connecting | Cyan | Blink |
| MQTT connecting | Yellow | Blink |
| Connected (default creds) | Green | Solid |
| Connected (stored creds) | Green | Pulse |
| Error | Red | Fast blink |
| Credential update | Purple | Pulse |
| Network switching | Cyan | Fast blink |
| Rebooting | White | Fade out |

## MQTT Topics

All topics are relative to your module ID.

| Direction | Topic | QoS | Payload |
|-----------|-------|-----|---------|
| Publish | `module/register` | 1 | `[{"id":"...","type":"...","name":"..."}]` |
| Publish | `{moduleId}/device/{deviceId}/input` | 0 | `{"command":"...","parameters":{...}}` |
| Publish | `{moduleId}/device/{deviceId}/state` | 0 | Device state JSON |
| Subscribe | `{moduleId}/device/+/output` | 1 | `{"command":"...","parameters":{...}}` |
| Subscribe | `{moduleId}/device/+/configuration/set` | 1 | Configuration JSON |
| Subscribe | `{moduleId}/credentials/set` | 1 | `{"ssid":"...","password":"..."}` |
| Subscribe | `{moduleId}/credentials/reset` | 1 | (empty) |
| Last Will | `client/disconnect` | 1 | `{"id":"...","status":"disconnected"}` |

## Connection Behavior

The SDK manages connectivity automatically:

1. **Network connection** — Tries Ethernet first (if enabled, 5s timeout), then WiFi
2. **WiFi credentials** — Prefers stored NVS credentials, and falls back to the
   configured defaults if that network cannot be joined (see below)
3. **Broker discovery** — Direct IP > gateway IP (on EGOS network) > mDNS > hostname
4. **MQTT connection** — Connects with 10s timeout, auto-reconnects on disconnect
5. **Network fallback** — After 5 consecutive MQTT timeouts (~50s), switches to alternate network/credentials
6. **Credential cycling** — WiFi-only: toggles between stored and default. With Ethernet: cycles Ethernet > WiFi(default) > WiFi(stored)

### WiFi Credential Fallback

A module provisioned onto a site network keeps those credentials in NVS. If that
network later disappears — the module is moved, or the router is replaced — it
must reach the default network so the controller can push new credentials.
Otherwise it retries an absent SSID forever and is unreachable.

The credential source is therefore a **sticky preference**, not a fixed rule:

- It starts at **stored**, so a provisioned module always tries its own network first.
- On a successful connection it becomes whatever actually connected, so a module
  that has fallen back to the default network keeps preferring it across later
  drops rather than reverting to a network that is no longer there.
- On repeated failure it **alternates**, so the module can never end up pinned to
  a network that has gone away.

How quickly it alternates depends on why the attempt failed:

| Disconnect reason | Attempts before switching |
| --- | --- |
| 201 `NO_AP_FOUND` — the SSID is not in range | 1 (conclusive, switch at once) |
| anything else — wrong password, handshake timeout, AP still booting | 3 |

A missing SSID is also detected early: after two consecutive reason-201
disconnects the attempt is abandoned rather than running out the full
`max_retry` loop and connect timeout, so fallback takes roughly five seconds
instead of tens.

The preference lives in RAM only. A power cycle, or the reboot that follows new
credentials arriving, deliberately re-tries the stored network first. Stored
credentials are never erased by a failed connection — they are replaced only by
`{moduleId}/credentials/set`.

### WiFi Credential Provisioning

The EGOS controller can update module WiFi credentials over MQTT:

- **Set credentials**: Controller publishes `{"ssid":"MyWiFi","password":"MyPass"}` to `{moduleId}/credentials/set`
- **Reset to defaults**: Controller publishes to `{moduleId}/credentials/reset`

The module saves credentials to NVS and reboots. This allows deploying modules on the default EGOS network, then remotely switching them to a production WiFi network.

## API Reference

### Initialization

```c
esp_err_t egos_init(const egos_config_t *config);
```
Initialize the SDK. Validates config and generates module ID. Call before `egos_start()`.

```c
esp_err_t egos_start(void);
```
Start the connection manager. Non-blocking — spawns a FreeRTOS task that handles WiFi/MQTT lifecycle.

```c
esp_err_t egos_stop(void);
```
Stop the SDK. Disconnects MQTT/WiFi, stops the connection task, frees resources.

### Publishing

```c
esp_err_t egos_publish_input(const char *device_id, const char *command, const char *params_json);
```
Publish an input event. Returns `ESP_ERR_INVALID_STATE` if not connected.

```c
esp_err_t egos_publish_state(const char *device_id, const char *state_json);
```
Publish a state update. Returns `ESP_ERR_INVALID_STATE` if not connected.

### Status

```c
const char *egos_get_module_id(void);
```
Returns the auto-generated module ID, or `NULL` if not initialized.

```c
bool egos_is_connected(void);
```
Returns `true` if MQTT is connected and the module is operational.

## Example

See `examples/basic_buttons/` for a complete, buildable project. To build:

```bash
cd examples/basic_buttons
idf.py set-target esp32s3
idf.py build
idf.py flash monitor
```

## EGOS Module Protocol Guide (Build Without the SDK)

You don't need this SDK to build an EGOS module. Any device that speaks MQTT can be a module — Arduino, Raspberry Pi, Python script, Node.js app, or anything else. Here's the complete protocol.

### 1. Connect to the MQTT Broker

The EGOS controller runs an MQTT broker (Aedes). Connect to it on port **1883**. On the default EGOS network, the broker is the gateway (the Raspberry Pi running the controller). On other networks, the broker is discoverable via mDNS at `egos.local`.

**Your MQTT client ID must be your module ID.** The controller identifies modules by client ID.

### 2. Generate a Module ID

Your module ID must be unique. The convention is `{type}-{identifier}`, e.g.:
- `my-buttons-001`
- `sensor-module-AABBCCDDEEFF` (using MAC address)
- `custom-puzzle-room3`

### 3. Set a Last Will

Configure your MQTT client's Last Will and Testament so the controller detects disconnections:

- **Topic:** `client/disconnect`
- **Payload:** `{"id":"your-module-id","status":"disconnected"}`
- **QoS:** 1
- **Retain:** false

### 4. Register Your Devices

On connect, publish a JSON array of your devices:

- **Topic:** `module/register`
- **QoS:** 1
- **Payload:**

```json
[
  {
    "id": "button1",
    "type": "button",
    "name": "Button 1"
  },
  {
    "id": "led1",
    "type": "led",
    "name": "Status LED",
    "configuration": {
      "color": "red"
    }
  }
]
```

Each device needs:
- `id` — unique within your module (used in topic paths)
- `type` — device type: `button`, `sensor`, `led`, `motor`, `neopixel`, `keypad`, `lcd`, `switch`, `dmx`, `speaker`, `camera`, etc.
- `name` — human-readable name shown in EGOS Studio
- `configuration` — (optional) device-specific config object

### 5. Subscribe to Command Topics

Subscribe to receive commands from the controller:

```
{moduleId}/device/+/output                 — Output commands (QoS 1)
{moduleId}/device/+/configuration/set      — Configuration updates (QoS 1)
{moduleId}/credentials/set                 — WiFi credential provisioning (QoS 1)
{moduleId}/credentials/reset               — Reset credentials (QoS 1)
```

The `+` is an MQTT single-level wildcard matching any device ID.

### 6. Handle Output Commands

When the controller sends a command, you receive a message on `{moduleId}/device/{deviceId}/output`:

```json
{
  "command": "setState",
  "parameters": {
    "state": "on"
  }
}
```

Parse the `command` string and `parameters` object, then act on them. Common commands by device type:

| Device Type | Commands |
|-------------|----------|
| `led` | `setState` (state: "on"/"off") |
| `motor` | `forward`, `reverse`, `stop` (speed, duration) |
| `neopixel` | `setColor` (pixel, red, green, blue) |
| `lcd` | `write` (line, char, text), `clear` |
| `dmx` | `setChannel`, `setChannels`, `blackout`, `restore` |

### 7. Publish Input Events

When a device's state changes (button pressed, sensor reading, etc.), publish to:

- **Topic:** `{moduleId}/device/{deviceId}/input`
- **QoS:** 0
- **Payload:**

```json
{
  "command": "changed",
  "parameters": {
    "pressed": true
  }
}
```

Common input event patterns:

```json
// Button press/release
{"command": "changed", "parameters": {"pressed": true}}

// Sensor reading
{"command": "reading", "parameters": {"temperature": 23.5, "humidity": 65}}

// Keypad key press
{"command": "keyPress", "parameters": {"key": "5"}}

// QR/RFID scan
{"command": "scanned", "parameters": {"value": "ABC123"}}
```

### 8. Publish State Updates (Optional)

Publish current device state to:

- **Topic:** `{moduleId}/device/{deviceId}/state`
- **QoS:** 0
- **Payload:** Your device state as JSON

### 9. Handle Credential Provisioning (Optional)

If you want the controller to manage your WiFi credentials:

**On `{moduleId}/credentials/set`:**
```json
{"ssid": "MyWiFi", "password": "MyPassword"}
```
Save these credentials and reconnect to the new network.

**On `{moduleId}/credentials/reset`:**
Clear saved credentials and revert to defaults.

### Complete Topic Reference

| Direction | Topic | QoS | Payload |
|-----------|-------|-----|---------|
| Publish | `module/register` | 1 | Device array JSON |
| Publish | `{moduleId}/device/{deviceId}/input` | 0 | `{"command":"...","parameters":{...}}` |
| Publish | `{moduleId}/device/{deviceId}/state` | 0 | State JSON |
| Subscribe | `{moduleId}/device/+/output` | 1 | `{"command":"...","parameters":{...}}` |
| Subscribe | `{moduleId}/device/+/configuration/set` | 1 | Config JSON |
| Subscribe | `{moduleId}/credentials/set` | 1 | `{"ssid":"...","password":"..."}` |
| Subscribe | `{moduleId}/credentials/reset` | 1 | (empty) |
| Last Will | `client/disconnect` | 1 | `{"id":"...","status":"disconnected"}` |

### Minimal Python Example

```python
import json
import paho.mqtt.client as mqtt

MODULE_ID = "my-python-module-001"
DEVICES = [
    {"id": "sensor1", "type": "sensor", "name": "Temperature Sensor"}
]

def on_connect(client, userdata, flags, rc):
    # Register devices
    client.publish("module/register", json.dumps(DEVICES), qos=1)
    # Subscribe to commands
    client.subscribe(f"{MODULE_ID}/device/+/output", qos=1)

def on_message(client, userdata, msg):
    data = json.loads(msg.payload)
    print(f"Command: {data['command']}, Params: {data.get('parameters', {})}")

client = mqtt.Client(client_id=MODULE_ID)
client.will_set("client/disconnect",
                json.dumps({"id": MODULE_ID, "status": "disconnected"}), qos=1)
client.on_connect = on_connect
client.on_message = on_message
client.connect("egos.local", 1883)

# Publish a sensor reading
client.publish(f"{MODULE_ID}/device/sensor1/input",
               json.dumps({"command": "reading", "parameters": {"temperature": 22.5}}))

client.loop_forever()
```

## Requirements

- ESP-IDF v5.x
- ESP32-S3 (or compatible ESP32 variant)
