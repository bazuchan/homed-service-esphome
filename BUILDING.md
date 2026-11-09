# Building

Requires Qt5 (with MQTT module) development libraries. [homed-service-common](https://github.com/u236/homed-service-common) is cloned automatically if not present.

```sh
./build.sh
```

This clones `homed-service-common` into `../homed-common` (sibling of the repo directory) if the directory does not exist, then runs `qmake` and `make`. The resulting binary is `homed-esphome`.

## Qt regression tests

```sh
python3 tools/test-entity-cache.py
```

Requires Python 3, g++, Qt5 Core development files and `moc` (Debian-style paths). An optional argument selects a Qt installation root instead of `/usr`. The tests use the production discovery builder, state handler, protobuf codec, database code and homed-common expose classes. They check JSON/disk round-trips, endpoint IDs, HA requests, legacy-cache migration, omitted proto3 values, missing readings, name conflicts, retained-topic cleanup on rename and sub-device display names. They do not start MQTT or connect to ESPHome devices.

## Debian package

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

