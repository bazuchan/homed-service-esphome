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

`light`, `cover`, `climate`, `lock`, and any `switch` that isn't config/diagnostic-category are "special" entities: each gets its own stable numbered command topic, `homed/td/esphome/<device>/<N>` (the number is assigned on discovery and persists across reconnects/restarts — see the [expose topic](#expose-topic) section below). Everything else (`select`, `number`, `button`, `sensor`/`binary_sensor` read-only exposes, and config/diagnostic-category switches) shares one topic, `homed/td/esphome/<device>`, addressed by each entity's own `objectId`.

| Entity type | Payload example |
|-------------|-----------------|
| `switch` (special) | `{"status": "on"}` / `{"status": "off"}` / `{"status": "toggle"}` |
| `switch` (config/diagnostic, on the shared topic) | `{"<objectId>": true}` |
| `light` | `{"status": "on"}`, `{"level": 128}`, `{"color": [255, 0, 0]}`, `{"colorTemperature": 370}` |
| `cover` | `{"cover": "open"}` / `{"cover": "close"}` / `{"cover": "stop"}`, `{"position": 50}` |
| `climate` | `{"systemMode": "heat"}`, `{"targetTemperature": 21.5}`, `{"fanMode": "auto"}`, `{"operationMode": "eco"}` |
| `lock` | `{"status": "off"}` (lock) / `{"status": "on"}` (unlock) / `{"status": "toggle"}` |
| `select` | `{"<objectId>": "option name"}` |
| `number` | `{"<objectId>": 42.5}` |
| `button` | `{"<objectId>": true}` |

`systemMode` values: `off`, `heat`, `cool`, `heat_cool`, `dry`, `auto`, `fan` (ESPHome's `FAN_ONLY` mode — not `fan_only`, that's only used in Home Assistant's own UI). `fanMode`/`operationMode` accept either one of ESPHome's standard tokens (`fanMode`: `on`, `off`, `auto`, `low`, `medium`, `high`, `middle`, `focus`, `diffuse`, `quiet`; `operationMode`: `none`, `home`, `away`, `boost`, `comfort`, `eco`, `sleep`, `activity`) or any custom fan mode/preset name the device itself advertises.

Entity states are published the same way: special entities to their own `homed/fd/esphome/<device>/<N>` topic, everything else merged into one JSON object on `homed/fd/esphome/<device>`.

## Expose topic

`homed/expose/esphome/<device>` describes what a device has, published as `{"common": {...}, "<N>": {...}, ...}`:

- `"common"` — `{"items": [...], "options": {...}}`, one entry per objectId-named entity (same shape every other HOMEd service uses).
- `"<N>"` — one entry per special entity (light/cover/climate/lock/non-config switch), `{"name": "...", "items": [...], "options": {...}}`. `N` is a small stable integer assigned on first discovery and persisted in the device database; if the entity disappears from the ESPHome device it's not published here anymore, but `N` stays reserved for it (matched back up by name) in case it reappears.
