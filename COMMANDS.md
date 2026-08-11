# MQTT commands

All commands are published to `homed/command/esphome` as a retained JSON message. The `prefix` matches the `mqtt/prefix` config value (default: `homed`).

## addDevice

Adds a device and immediately connects to it.

```json
{"action": "addDevice", "host": "192.168.1.100", "key": "<base64-encoded 32-byte noise key>", "port": 6053, "name": "my-device"}
```

`port` defaults to `6053`. `name` is optional — defaults to the sanitized hostname. The key is the `api_encryption.key` value from the ESPHome device config (base64, 32 bytes).

## removeDevice

Disconnects and removes a device. `device` can be the device name or hostname.

```json
{"action": "removeDevice", "device": "my-device"}
```

## updateDevice

Updates device properties without reconnecting.

```json
{"action": "updateDevice", "device": "my-device", "name": "new-name", "active": true, "discovery": true}
```

All fields except `device` are optional.

## getProperties

Re-publishes current entity states for a device.

```json
{"action": "getProperties", "device": "my-device"}
```

## restartService

```json
{"action": "restartService"}
```

## Controlling entities

Send to `homed/td/esphome/<device>/<endpointId>`:

| Entity type | Payload example |
|-------------|-----------------|
| `switch` | `{"status": "on"}` / `{"status": "off"}` / `{"status": "toggle"}` |
| `light` | `{"status": "on"}`, `{"level": 128}`, `{"color": [255, 0, 0]}`, `{"colorTemperature": 370}` |
| `cover` | `{"cover": "open"}` / `{"cover": "close"}` / `{"cover": "stop"}`, `{"position": 50}` |
| `climate` | `{"systemMode": "heat"}`, `{"targetTemperature": 21.5}`, `{"fanMode": "auto"}`, `{"operationMode": "eco"}` |
| `select` | `{"<objectId>": "option name"}` |
| `number` | `{"<objectId>": 42.5}` |
| `button` | `{"<objectId>": true}` |

`systemMode` values: `off`, `heat`, `cool`, `heat_cool`, `dry`, `auto`, `fan` (ESPHome's `FAN_ONLY` mode — not `fan_only`, that's only used in Home Assistant's own UI). `fanMode`/`operationMode` accept either one of ESPHome's standard tokens (`fanMode`: `on`, `off`, `auto`, `low`, `medium`, `high`, `middle`, `focus`, `diffuse`, `quiet`; `operationMode`: `none`, `home`, `away`, `boost`, `comfort`, `eco`, `sleep`, `activity`) or any custom fan mode/preset name the device itself advertises.

Entity states are published to `homed/fd/esphome/<device>/<endpointId>`.
