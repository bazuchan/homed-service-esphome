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
mkdir -p "$STAGING/lib/systemd/system"

cp homed-esphome "$STAGING/usr/bin/"
cp deploy/data/etc/homed/homed-esphome.conf "$STAGING/etc/homed/"
cp deploy/systemd/homed-esphome.service "$STAGING/lib/systemd/system/"

sed "s/^Version:$/Version: ${VERSION}/; s/^Architecture:$/Architecture: ${ARCH}/" \
    deploy/apt/control > "$STAGING/DEBIAN/control"

cp deploy/apt/conffiles "$STAGING/DEBIAN/conffiles"
cp deploy/apt/postinst  "$STAGING/DEBIAN/postinst"
cp deploy/apt/prerm     "$STAGING/DEBIAN/prerm"
chmod 755 "$STAGING/DEBIAN/postinst" "$STAGING/DEBIAN/prerm"

dpkg-deb --build "$STAGING" "${PKG}.deb"
rm -rf "$STAGING"
echo "Built: ${PKG}.deb"
