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

## MQTT commands

See [COMMANDS.md](COMMANDS.md) for the full list of commands and entity control payloads.

## Web UI

`homed-web` needs to be patched to add ESPHome support, after installing, run:

```sh
/usr/share/homed-web/esphome-patcher.sh
```

If [homed-service-web#26](https://github.com/u236/homed-service-web/pull/26) gets merged upstream, this patch step will no longer be needed.

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
