# HOMEd ESPHome

A [HOMEd](https://wiki.homed.dev/) service that integrates ESPHome devices into the HOMEd ecosystem via the ESPHome Native API. Device entities are exposed to MQTT in the same format as other HOMEd services.

Devices are added and removed at runtime via MQTT commands. On each connection the service discovers all exported entities from the device and updates the database automatically.

## Supported entities

| ESPHome type    | Description                                      |
|-----------------|---------------------------------------------------|
| `switch`        | On/off control, reports `status` (on/off)        |
| `binary_sensor` | Read-only boolean sensor (motion, contact, etc.) |
| `cover`         | Open/close/stop, position (if supported by the device) |
| `sensor`        | Numeric sensor (temperature, humidity, etc.)     |
| `text_sensor`   | String-valued sensor                             |
| `light`         | On/off, brightness, RGB color, color temperature |
| `climate`       | Mode, target temperature, fan mode, preset       |
| `lock`          | Lock/unlock, reports `status` (`off`=locked, `on`=unlocked) |
| `select`        | Dropdown selection with enumerated options       |
| `number`        | Numeric input with min/max/step constraints      |
| `button`        | Momentary trigger (press only)                   |

`cover` and `climate` entities without native ESPHome support for a given feature (e.g. tilt, dual-point target temperature, swing mode) fall back to sane defaults rather than being exposed — see the inline comments in `esphome.cpp` for exactly what's covered.

`switch` entities with `entity_category: config` or `entity_category: diagnostic` set in the ESPHome YAML aren't treated as one of a device's primary functions — they're exposed as a plain `toggle` control alongside the device's other simple exposes (`select`/`number`/`button`) instead of getting the full switch treatment (its own numbered slot, HA discovery as a `switch` entity, on/off/toggle status). Switches without an `entity_category` (the default) are unaffected.

## Sub-devices

ESPHome devices that group entities under [sub-devices](https://esphome.io/components/index.html#devices) are published as separate HOMEd devices rather than as endpoints of the parent, one per sub-device — address `<parent address>_<sub-device name>`, model `Subdev <sub-device name> of <parent name>`. They share the parent's connection (no separate TCP connection, no separate `addDevice`), and are (re)discovered automatically alongside it.

A sub-device that stops being reported (removed from the ESPHome config, or the parent simply offline) is never deleted on its own — it's left in the database exactly as last seen, so if it's ever reconfigured back it picks up its previous `objectId`/expose mapping again rather than starting over as a new device. Only an explicit `removeDevice` command deletes it — and removing a parent this way also removes its sub-devices, since they can't function without it.

## Web UI

~~`homed-web` needs to be patched to add ESPHome support, after installing, run: `/usr/share/homed-web/esphome-patcher.sh`~~

Since homed-web version 0.14 no patching is needed.

## Configuration

Edit `/etc/homed/homed-esphome.conf` and adjust the MQTT settings before running.

## Releases

Prebuilt packages are available on the [Releases](https://github.com/bazuchan/homed-service-esphome/releases) page, covering the same platforms as [homed](https://wiki.homed.dev/) itself.

## MQTT commands

See [COMMANDS.md](COMMANDS.md) for the full list of commands and entity control payloads.

## Building

See [BUILDING.md](BUILDING.md) for building instructions.
