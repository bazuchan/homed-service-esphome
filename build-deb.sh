#!/bin/sh
set -e

./build.sh

VERSION=$(grep 'SERVICE_VERSION' controller.h | sed 's/.*"\(.*\)".*/\1/')
ARCH=$(dpkg --print-architecture)
PKG="homed-service-esphome_${VERSION}_${ARCH}"
STAGING="/tmp/${PKG}"

rm -rf "$STAGING"
mkdir -p "$STAGING/DEBIAN"
mkdir -p "$STAGING/usr/bin"
mkdir -p "$STAGING/etc/homed"
mkdir -p "$STAGING/opt/homed-esphome"

cp homed-esphome "$STAGING/usr/bin/"
cp deploy/data/etc/homed/homed-esphome.conf "$STAGING/etc/homed/"

cat > "$STAGING/DEBIAN/control" << EOF
Package: homed-service-esphome
Version: ${VERSION}
Architecture: ${ARCH}
Maintainer: drdm
Depends: libqt5core5a, libqt5network5, libssl3
Description: HOMEd service for ESPHome devices
 Integrates ESPHome devices into the HOMEd ecosystem via the ESPHome
 Native API using Noise_NNpsk0 encrypted protocol over TCP.
EOF

echo "/etc/homed/homed-esphome.conf" > "$STAGING/DEBIAN/conffiles"

dpkg-deb --build "$STAGING" "${PKG}.deb"
rm -rf "$STAGING"
echo "Built: ${PKG}.deb"
