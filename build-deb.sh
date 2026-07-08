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

# Generate .changes file
DEB="${PKG}.deb"
SIZE=$(wc -c < "$DEB")
MD5=$(md5sum    "$DEB" | awk '{print $1}')
SHA1=$(sha1sum  "$DEB" | awk '{print $1}')
SHA256=$(sha256sum "$DEB" | awk '{print $1}')
DATE=$(date -R)
MAINTAINER=$(grep '^Maintainer:' deploy/apt/control | sed 's/Maintainer: //')
DESCRIPTION=$(grep '^Description:' deploy/apt/control | sed 's/Description: //')

cat > "${PKG}.changes" << EOF
Format: 1.8
Date: ${DATE}
Source: homed-service-esphome
Binary: homed-service-esphome
Architecture: ${ARCH}
Version: ${VERSION}
Distribution: unstable
Urgency: low
Maintainer: ${MAINTAINER}
Description:
 homed-service-esphome - ${DESCRIPTION}
Changes:
 homed-service-esphome (${VERSION}) unstable; urgency=low
 .
 * Package release
Checksums-Sha1:
 ${SHA1} ${SIZE} ${DEB}
Checksums-Sha256:
 ${SHA256} ${SIZE} ${DEB}
Files:
 ${MD5} ${SIZE} misc standard ${DEB}
EOF

echo "Built: ${DEB}"
echo "Built: ${PKG}.changes"
