# MQTT commands

All commands are published to `homed/command/esphome` as a retained JSON message. The `prefix` matches the `mqtt/prefix` config value (default: `homed`).

## addDevice

Adds a device and immediately connects to it.

```json
{"action": "addDevice", "host": "192.168.1.100", "key": "<base64-encoded 32-byte noise key>", "port": 6053, "name": "my-device", "discovery": false, "cloud": false}
```

`port` defaults to `6053`. `name` is optional — defaults to the sanitized hostname. The key is the `api_encryption.key` value from the ESPHome device config (base64, 32 bytes).

`discovery` and `cloud` are optional and default to `true` when omitted from an MQTT command. The web add-device form starts with both checkboxes checked and sends their values explicitly. `discovery` controls Home Assistant discovery. `cloud` is saved per device and published in the service status for HOMEd Cloud. Newly discovered sub-devices inherit their parent's cloud flag; subsequent edits are independent.

Device names must be unique and must not match another device's host/address. A conflicting explicit name is rejected with a `nameDuplicate` event; a conflicting host is rejected with `idDuplicate`.

## removeDevice

Disconnects and removes a device. `device` can be the device name or hostname.

```json
{"action": "removeDevice", "device": "my-device"}
```

## updateDevice

Updates device properties. Changing `active` may stop or resume the parent connection; other edits do not restart an established connection.

```json
{"action": "updateDevice", "device": "my-device", "name": "new-name", "active": true, "discovery": true, "cloud": true}
```

All fields except `action` and `device` are optional. An omitted, empty or whitespace-only `name` preserves the current name. Omitting a flag preserves its current value. `discovery = false` clears the device Home Assistant discovery configs (local MQTT topics keep working). `active = false` blocks commands and state publication and reports the device offline. For a parent device it closes the TCP connection, stops retries and makes its sub-devices unavailable without changing their own flags. Disabling a sub-device leaves the shared connection and siblings running. `active = true` resumes parent connection attempts, or re-enables a sub-device when its parent is active and connected.

A duplicate name rejects the entire update, including flag changes. When `mqtt/names = true`, renaming clears the old retained expose, availability and state topics, then republishes under the new name. With `mqtt/names = false`, the address-based topics stay unchanged. Successful updates emit `updated` and are saved immediately.

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
| `switch` (config/diagnostic, on the shared topic) | `{"<objectId>": true}` / `{"<objectId>": false}` / `{"<objectId>": "toggle"}` |
| `light` | `{"status": "on"}`, `{"level": 128}`, `{"color": [255, 0, 0]}`, `{"colorTemperature": 370}` |
| `cover` | `{"cover": "open"}` / `{"cover": "close"}` / `{"cover": "stop"}`, `{"position": 50}` |
| `climate` | `{"systemMode": "heat"}`, `{"targetTemperature": 21.5}`, `{"fanMode": "auto"}`, `{"operationMode": "eco"}` |
| `lock` | `{"status": "off"}` (lock) / `{"status": "on"}` (unlock) / `{"status": "toggle"}` |
| `select` | `{"<objectId>": "option name"}` |
| `number` | `{"<objectId>": 42.5}` |
| `button` | `{"<objectId>": true}` |

`systemMode` values: `off`, `heat`, `cool`, `heat_cool`, `dry`, `auto`, `fan` (ESPHome's `FAN_ONLY` mode — not `fan_only`, that's only used in Home Assistant's own UI). `fanMode`/`operationMode` accept either one of ESPHome's standard tokens (`fanMode`: `on`, `off`, `auto`, `low`, `medium`, `high`, `middle`, `focus`, `diffuse`, `quiet`; `operationMode`: `none`, `home`, `away`, `boost`, `comfort`, `eco`, `sleep`, `activity`) or any custom fan mode/preset name the device itself advertises.

Switch `toggle` inverts the last reported state: `status` for special switches, or the entity's `objectId` for config/diagnostic switches. If that state is unknown, the existing fallback is to turn on; explicit on/off commands do not depend on cached state.

Entity states are published the same way: special entities to their own `homed/fd/esphome/<device>/<N>` topic, everything else merged into one JSON object on `homed/fd/esphome/<device>`.

## Expose topic

`homed/expose/esphome/<device>` describes what a device has, published as `{"common": {...}, "<N>": {...}, ...}`:

- `"common"` — `{"items": [...], "options": {...}}`, one entry per objectId-named entity (same shape every other HOMEd service uses).
- `"<N>"` — one entry per special entity (light/cover/climate/lock/non-config switch), `{"items": [...], "options": {"name": "...", ...}}`. `N` is a small stable integer assigned on first discovery and persisted in the device database; if the entity disappears from the ESPHome device it's not published here anymore, but `N` stays reserved for it (matched back up by name) in case it reappears.
