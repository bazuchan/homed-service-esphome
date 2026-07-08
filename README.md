# HOMEd ESPHome

A [HOMEd](https://github.com/u236/homed-service-common) service that integrates ESPHome devices into the HOMEd ecosystem via the ESPHome Native API. Devices are connected over TCP using the Noise_NNpsk0_25519_ChaChaPoly_SHA256 encrypted protocol and their entities are exposed to MQTT in the same format as other HOMEd services.

Devices are added and removed at runtime via MQTT commands — no config file editing required. On each connection the service discovers all exported entities from the device and updates the database automatically.

## Supported entities

| ESPHome type    | Description                                      | Tested |
|-----------------|--------------------------------------------------|--------|
| `switch`        | On/off control, reports `status` (on/off)        | ✓      |
| `binary_sensor` | Read-only boolean sensor (motion, contact, etc.) |        |
| `sensor`        | Numeric sensor (temperature, humidity, etc.)     |        |
| `text_sensor`   | String-valued sensor                             |        |
| `light`         | On/off, brightness, RGB color, color temperature |        |
| `select`        | Dropdown selection with enumerated options       |        |
| `number`        | Numeric input with min/max/step constraints      |        |
| `button`        | Momentary trigger (press only)                   | ✓      |

## MQTT commands

All commands are published to `homed/command/esphome` as a retained JSON message. The `prefix` matches the `mqtt/prefix` config value (default: `homed`).

### addDevice

Adds a device and immediately connects to it.

```json
{"action": "addDevice", "host": "192.168.1.100", "key": "<base64-encoded 32-byte noise key>", "port": 6053, "name": "my-device"}
```

`port` defaults to `6053`. `name` is optional — defaults to the sanitized hostname. The key is the `api_encryption.key` value from the ESPHome device config (base64, 32 bytes).

### removeDevice

Disconnects and removes a device. `device` can be the device name or hostname.

```json
{"action": "removeDevice", "device": "my-device"}
```

### updateDevice

Updates device properties without reconnecting.

```json
{"action": "updateDevice", "device": "my-device", "name": "new-name", "active": true, "discovery": true}
```

All fields except `device` are optional.

### getProperties

Re-publishes current entity states for a device.

```json
{"action": "getProperties", "device": "my-device"}
```

### restartService

```json
{"action": "restartService"}
```

### Controlling entities

Send to `homed/td/esphome/<device>/<endpointId>`:

| Entity type | Payload example |
|-------------|-----------------|
| `switch` | `{"status": "on"}` / `{"status": "off"}` / `{"status": "toggle"}` |
| `light` | `{"status": "on"}`, `{"level": 128}`, `{"color": [255, 0, 0]}`, `{"colorTemperature": 370}` |
| `select` | `{"<objectId>": "option name"}` |
| `number` | `{"<objectId>": 42.5}` |
| `button` | `{"<objectId>": true}` |

Entity states are published to `homed/fd/esphome/<device>/<endpointId>`.

## Web UI

The web interface requires the forked [homed-service-web](https://github.com/bazuchan/homed-service-web), which adds ESPHome-specific device list, device info, and expose rendering support.

## Building

Requires Qt5 (with MQTT module) and OpenSSL development libraries. [homed-service-common](https://github.com/u236/homed-service-common) is cloned automatically if not present.

```sh
./build.sh
```

This clones `homed-service-common` into `../homed-common` (sibling of the repo directory) if the directory does not exist, then runs `qmake` and `make`. The resulting binary is `homed-esphome`.

### Debian package

```sh
./build-deb.sh
```

Produces `homed-service-esphome_<version>_<arch>.deb` in the current directory. The package installs:

| Path | Content |
|------|---------|
| `/usr/bin/homed-esphome` | Service binary |
| `/lib/systemd/system/homed-esphome.service` | systemd unit (enabled and started on install) |
| `/etc/homed/homed-esphome.conf` | Default configuration (marked as conffile) |
| `/opt/homed-esphome/` | Runtime data directory |

Copy `/etc/homed/homed-esphome.conf` and adjust the MQTT settings before running.
