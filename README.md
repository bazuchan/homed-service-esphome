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

## MQTT commands

See [COMMANDS.md](COMMANDS.md) for the full list of commands and entity control payloads.

## Web UI

`homed-web` needs to be patched to add ESPHome support, after installing, run:

```sh
/usr/share/homed-web/esphome-patcher.sh
```

If [plugins branch](https://github.com/u236/homed-service-web/compare/plugins) gets merged and released, this patch step will no longer be needed.

## Releases

Prebuilt packages are available on the [Releases](https://github.com/bazuchan/homed-service-esphome/releases) page, covering the same platforms as [homed](https://wiki.homed.dev/) itself.

## Building

Requires Qt5 (with MQTT module) development libraries. [homed-service-common](https://github.com/u236/homed-service-common) is cloned automatically if not present.

```sh
./build.sh
```

This clones `homed-service-common` into `../homed-common` (sibling of the repo directory) if the directory does not exist, then runs `qmake` and `make`. The resulting binary is `homed-esphome`.

### Debian package

```sh
./build-deb.sh
```

Produces `homed-esphome_<version>_<arch>.deb` in the current directory. The package installs:

| Path | Content |
|------|---------|
| `/usr/bin/homed-esphome` | Service binary |
| `/lib/systemd/system/homed-esphome.service` | systemd unit (enabled and started on install) |
| `/etc/homed/homed-esphome.conf` | Default configuration (marked as conffile) |
| `/opt/homed-esphome/` | Runtime data directory |

Copy `/etc/homed/homed-esphome.conf` and adjust the MQTT settings before running.
